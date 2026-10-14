"""回归：应用设置里的「客户端失联释放时间」必须真的控制 watchdog。

背景（真机日志发现的缺陷）：`USBIP_KICK_IDLE_SECONDS` 原先只用于
`CLIENT_TTL_SECONDS`（列表过滤）和回给前端的 `kickIdleSeconds`（页面显示），
watchdog 真正决定要不要强制释放时用的是写死的冗余窗口
（HEARTBEAT_SAFE_GAP_SECONDS + MISSED_RUN_THRESHOLD 个周期 ≈ 65 秒）。
后果：操作者把该值改成 180 或 0 都不起作用，设备照样在约 65 秒后被摘掉 ——
真机上 12 天内因此踢了 4 次，表现为「设备莫名其妙断掉」。

修复后由 watchdog_release_grace_seconds() 统一决定：
  * 0  → 永不自动释放（只记日志）；
  * >0 → 取「设置值」与「冗余窗口」的较大者。

    python tests/test_watchdog_grace.py
"""
import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web  # noqa: E402

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


class Sandbox:
    """Redirect every store and stub out the parts that need real USB hardware."""

    def __init__(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.old = {name: getattr(web, name) for name in
                    ("CLIENTS_FILE", "QUEUE_FILE", "NOTIFY_FILE", "MANAGED_FILE")}
        web.CLIENTS_FILE = root / "clients.json"
        web.QUEUE_FILE = root / "queue.json"
        web.NOTIFY_FILE = root / "queue-notify.json"
        web.MANAGED_FILE = root / "managed-busids"
        self.old_is_shared = web.is_shared
        self.old_list_devices = web.list_devices
        self.old_run_usbip = web.run_usbip
        self.old_missed_runs = web.MISSED_RUN_THRESHOLD
        self.old_kick_idle = web.KICK_IDLE_SECONDS
        web.is_shared = lambda busid: True
        web.list_devices = lambda include_internal=False: ([{"busid": "1-1", "connections": []}], None)
        self.calls = []
        web.run_usbip = self.record_usbip

    def record_usbip(self, *args):
        self.calls.append(args)
        return 0, "ok"

    def usbip_calls(self, *names):
        return [call for call in self.calls if call and call[0] in names]

    def close(self):
        for name, value in self.old.items():
            setattr(web, name, value)
        web.is_shared = self.old_is_shared
        web.list_devices = self.old_list_devices
        web.run_usbip = self.old_run_usbip
        web.MISSED_RUN_THRESHOLD = self.old_missed_runs
        web.KICK_IDLE_SECONDS = self.old_kick_idle
        web._WATCHDOG_MISSED_RUNS.clear()
        self.temp.cleanup()

    def heartbeat(self, client_id, address, **extra):
        payload = {"clientId": client_id, "clientName": client_id + "机", "busids": [], "dataPort": 5555}
        payload.update(extra)
        ok, message, record, notifications = web.update_client(payload, address)
        assert ok, message
        return record, notifications

    def age_record(self, client_id, seconds):
        raw = json.loads(web.CLIENTS_FILE.read_text(encoding="utf-8"))
        raw["clients"][client_id]["lastSeen"] = int(time.time() - seconds)
        web.CLIENTS_FILE.write_text(json.dumps(raw), encoding="utf-8")

    def client_ids(self):
        raw = json.loads(web.CLIENTS_FILE.read_text(encoding="utf-8"))
        return sorted(raw.get("clients", {}))

    def reset(self):
        web._WATCHDOG_MISSED_RUNS.clear()
        self.calls.clear()


box = Sandbox()
box.heartbeat("probe", "192.168.1.50")
box.reset()

print("[A] watchdog_release_grace_seconds() 跟随设置")
web.MISSED_RUN_THRESHOLD = 5          # 生产默认
for idle, expect in ((0, 0.0), (60, 65.0), (600, 600.0), (10, 65.0), (65, 65.0)):
    web.KICK_IDLE_SECONDS = idle
    got = web.watchdog_release_grace_seconds()
    check(f"kick_idle={idle} → grace={expect:g}", got == expect, f"got={got:g}")
web.MISSED_RUN_THRESHOLD = 1
web.KICK_IDLE_SECONDS = 60
check("冗余窗口是下限（阈值调小时不会被设置值拉低）",
      web.watchdog_release_grace_seconds() == 60.0,
      f"got={web.watchdog_release_grace_seconds():g}")

print("[B] 设成 0 = 永不自动释放：记录保留、不 kick")
web.MISSED_RUN_THRESHOLD = 1
web.KICK_IDLE_SECONDS = 0
box.reset()
box.heartbeat("silent-zero", "192.168.1.50", busids=["1-1"])
box.age_record("silent-zero", 600)
web.watchdog_sweep()
check("没有 unbind/bind", not box.usbip_calls("unbind", "bind"), f"calls={box.calls}")
check("记录没有被清除", "silent-zero" in box.client_ids(), f"ids={box.client_ids()}")
web.handle_client_goodbye("silent-zero", ["1-1"])

print("[C] 设成 600 秒：静默 70 秒仍在宽限期内，不释放")
web.KICK_IDLE_SECONDS = 600
box.reset()
box.heartbeat("silent-short", "192.168.1.51", busids=["1-1"])
box.age_record("silent-short", 70)
web.watchdog_sweep()
check("没有 unbind/bind", not box.usbip_calls("unbind", "bind"), f"calls={box.calls}")
check("记录仍保留", "silent-short" in box.client_ids(), f"ids={box.client_ids()}")

print("[D] 设成 600 秒：静默 700 秒超过宽限期，释放")
web._WATCHDOG_MISSED_RUNS.clear()
box.age_record("silent-short", 700)
box.calls.clear()
web.watchdog_sweep()
check("执行了 unbind/bind", bool(box.usbip_calls("unbind", "bind")), f"calls={box.calls}")

print("[E] 默认 60 秒：静默 70 秒就释放（保持修复前的实际行为）")
web.KICK_IDLE_SECONDS = 60
box.reset()
box.heartbeat("silent-default", "192.168.1.52", busids=["1-1"])
box.age_record("silent-default", 70)
web.watchdog_sweep()
check("执行了 unbind/bind", bool(box.usbip_calls("unbind", "bind")), f"calls={box.calls}")

print("[F] 心跳正常时任何设置下都不动设备")
web.KICK_IDLE_SECONDS = 60
box.reset()
box.heartbeat("alive", "192.168.1.53", busids=["1-1"])
web.watchdog_sweep()
check("新鲜心跳不触发任何 usbip 调用", not box.usbip_calls("unbind", "bind"), f"calls={box.calls}")

box.close()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
