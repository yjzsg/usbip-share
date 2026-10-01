"""F1/F2 回归：免鉴权的 goodbye 不能碰设备；watchdog 不能反向踢掉真实持有者。

威胁模型（已确认的攻击链）：

  * `GET /api/devices` 免鉴权，且回显 `connections[].clientId`；
  * `POST /api/clients/heartbeat` 免鉴权，请求体里带 `clientId` 和 `shutdown`。

修复前：伪造 `{"clientId":"<受害ID>","shutdown":true}` → `handle_client_goodbye`
→ `kick_device()` → `usbip unbind` + `bind`，正在使用的 USB/IP 会话被强行拆断，
可循环复现（持续 DoS）。同类问题：伪造心跳的 `busids` 会让 watchdog 在 5 个周期后
按陈旧记录去踢真正持有该设备的人。

本用例把 `run_usbip` 换成记录器，断言"没有任何 unbind/bind 调用"。

    python tests/test_shutdown_no_kick.py
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
        # 这台设备"存在且已共享"。
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
        web._WATCHDOG_MISSED_RUNS.clear()
        self.temp.cleanup()

    def heartbeat(self, client_id, address, **extra):
        payload = {"clientId": client_id, "clientName": client_id + "机", "busids": [], "dataPort": 5555}
        payload.update(extra)
        ok, message, record, notifications = web.update_client(payload, address)
        assert ok, message
        return record, notifications

    def age_record(self, client_id, seconds):
        """Make one client's heartbeat look `seconds` old."""
        raw = json.loads(web.CLIENTS_FILE.read_text(encoding="utf-8"))
        raw["clients"][client_id]["lastSeen"] = int(time.time() - seconds)
        web.CLIENTS_FILE.write_text(json.dumps(raw), encoding="utf-8")


print("[F1] 伪造 shutdown 心跳不得触发 unbind/bind")
box = Sandbox()
box.heartbeat("victim-1", "192.168.1.50", busids=["1-1"])
# 攻击者只读 GET /api/devices 就能拿到 victim-1，然后伪造它的告别心跳。
ok, message, record, _ = web.update_client(
    {"clientId": "victim-1", "clientName": "伪造", "busids": ["1-1"], "dataPort": 5555, "shutdown": True},
    "10.0.0.66",
)
check("伪造 goodbye 仍返回成功（客户端退出路径不受影响）", ok, message)
check("没有 usbip unbind/bind 调用", not box.usbip_calls("unbind", "bind"),
      f"calls={box.calls}")
check("受害者记录被释放（只释放声明）", web.read_clients() == [])

# 设备真的还能被管理员强制断开（功能没被删掉，只是不再免鉴权触发）。
box.calls.clear()
ok, message = web.kick_device("1-1")
check("管理员强制断开仍会 unbind+bind", ok and box.usbip_calls("unbind", "bind"), message)

print("[F1b] 直接调用 handle_client_goodbye 同样不碰设备")
box.calls.clear()
box.heartbeat("victim-2", "192.168.1.51", busids=["1-1"])
web.handle_client_goodbye("victim-2", ["1-1"])
check("handle_client_goodbye 不调 usbip", not box.calls, f"calls={box.calls}")
check("handle_client_goodbye 清掉记录", [item["clientId"] for item in web.read_clients()] == [])
box.close()

print("[F2] watchdog：陈旧记录的 busids 不能反向踢掉真实持有者")
box = Sandbox()
web.MISSED_RUN_THRESHOLD = 1
box.heartbeat("holder", "192.168.1.50", busids=["1-1"])
box.heartbeat("attacker", "10.0.0.66", busids=["1-1"])
box.age_record("attacker", 600)
box.calls.clear()
web.watchdog_sweep()
check("真实持有者仍在册", [item["clientId"] for item in web.read_clients()] == ["holder"])
check("陈旧记录已被清除", "attacker" not in json.loads(web.CLIENTS_FILE.read_text(encoding="utf-8"))["clients"])
check("没有对真实持有者 kick", not box.usbip_calls("unbind", "bind"), f"calls={box.calls}")

print("[F2b] watchdog：确实没人持有时才真的释放")
web._WATCHDOG_MISSED_RUNS.clear()
web.handle_client_goodbye("holder", ["1-1"])   # 持有者自己也走了（只释放声明）
box.heartbeat("stale", "192.168.1.50", busids=["1-1"])
box.age_record("stale", 600)
box.calls.clear()
web.watchdog_sweep()
check("无活跃持有者时按陈旧记录释放", bool(box.usbip_calls("unbind", "bind")), f"calls={box.calls}")

print("[F2c] watchdog：同一地址重新登记（同一台机器换了 clientId）仍可释放")
web._WATCHDOG_MISSED_RUNS.clear()
box.heartbeat("stale-2", "192.168.1.50", busids=["1-1"])
box.heartbeat("fresh-same-host", "192.168.1.50", busids=["1-1"])
box.age_record("stale-2", 600)
box.calls.clear()
web.watchdog_sweep()
check("同地址持有者可被释放", bool(box.usbip_calls("unbind", "bind")), f"calls={box.calls}")

print("[F2d] watchdog：设备未共享时不 unbind")
web._WATCHDOG_MISSED_RUNS.clear()
web.is_shared = lambda busid: False
box.heartbeat("stale-3", "192.168.1.60", busids=["1-1"])
box.age_record("stale-3", 600)
box.calls.clear()
web.watchdog_sweep()
check("未共享设备不释放", not box.usbip_calls("unbind", "bind"), f"calls={box.calls}")
box.close()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
