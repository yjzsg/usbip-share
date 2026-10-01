"""网卡保护：绝不把承载宿主网络的 USB 网卡共享出去（会直接把 NAS 搞掉线）。

真机事故（飞牛 NAS 上唯一的在用网卡就是一块 USB 网卡）：

    21:28:09 usbip-host 2-7: register new device   ← 误点「开始共享」
    21:29:18 usbip-host 2-7: USB disconnect        ← 掉网约 1.5 分钟
    21:42:13 usbip-host 2-7: register new device   ← 重启后被自动恢复共享又绑一次
    21:46:12 usbip-host 2-7: USB disconnect        ← 又断一次

检测手段（真机验证过）：`bInterfaceClass` 是私有类 0xff，靠设备类判断不行；
但网卡的接口目录下一定有 `net/`（例如 `2-7:1.0/net/enxc84d44294124`），
加密狗没有。本用例 monkeypatch `web.SYSFS_USB_DEVICES` 造出这种结构。

    python tests/test_netdev_guard.py
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web  # noqa: E402

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


LISTING = (
    " - busid 1-1 (1241:e001)\n"
    "      LYFdog : Sample\n"
    " - busid 2-7 (0bda:8156)\n"
    "      Realtek : USB 10/100/1000 LAN\n"
)

root = Path(tempfile.mkdtemp())
sysfs = root / "sysfs"
sysfs.mkdir()

# sysfs 里接口目录叫 "<busid>:1.0"，但 Windows 文件名不允许含 ':'，
# 所以这里把 glob 模板换成 '-' 分隔符，造的还是真实目录结构。
SEP = ":"
if os.name == "nt":
    web.USB_INTERFACE_DIR_GLOB = "{busid}-*"
    SEP = "-"
NET_DIR = sysfs / f"2-7{SEP}1.0" / "net"

# 2-7 是网卡（接口目录下有 net/），1-8.2 是加密狗（没有 net/）。
(NET_DIR / "enxc84d44294124").mkdir(parents=True)
(sysfs / f"1-8.2{SEP}1.0").mkdir(parents=True)
# 一个"接口目录其实是普通文件"的畸形条目：读它会抛 OSError，必须当作非网卡。
(sysfs / f"3-3{SEP}1.0").write_text("not a directory", encoding="utf-8")
# net/ 是普通文件（不是目录）：iterdir() 会抛 NotADirectoryError。
(sysfs / f"4-4{SEP}1.0").mkdir(parents=True)
(sysfs / f"4-4{SEP}1.0" / "net").write_text("not a directory", encoding="utf-8")

web.SYSFS_USB_DEVICES = sysfs

print("[A] device_network_interfaces() 检测")
check("网卡：拿到接口名", web.device_network_interfaces("2-7") == ["enxc84d44294124"],
      str(web.device_network_interfaces("2-7")))
check("非网卡：空列表", web.device_network_interfaces("1-8.2") == [])
check("设备不存在：空列表", web.device_network_interfaces("9-9") == [])
check("接口目录是文件（读不了）：当作非网卡，不抛异常", web.device_network_interfaces("3-3") == [])
check("net/ 不是目录（iterdir 抛 OSError）：当作非网卡", web.device_network_interfaces("4-4") == [])
check("非法 busid（含通配符）：直接空列表", web.device_network_interfaces("2-*") == []
      and web.device_network_interfaces("") == [] and web.device_network_interfaces(None) == [])
missing = web.SYSFS_USB_DEVICES
web.SYSFS_USB_DEVICES = root / "does-not-exist"
check("sysfs 整个不存在：空列表", web.device_network_interfaces("2-7") == [])
web.SYSFS_USB_DEVICES = missing

# ---------------------------------------------------------------------------
os.environ.pop("USBIP_WEB_PASSWORD", None)
os.environ["USBIP_WEB_TOKEN"] = "netdev-admin-token"
os.environ.pop("USBIP_ALLOW_NETDEV", None)
web.AUTH_FILE = root / "auth.json"
web.METADATA_FILE = root / "device-metadata.json"
web.MANAGED_FILE = root / "managed-busids"
web.CLIENTS_FILE = root / "clients.json"
web.QUEUE_FILE = root / "queue.json"
web.NOTIFY_FILE = root / "queue-notify.json"

shared: set[str] = set()
calls: list[tuple] = []


def set_netdir(present: bool) -> None:
    """模拟内核行为：设备被 usbip-host 绑定后，它的 netdev 就消失了。"""
    if present:
        (NET_DIR / "enxc84d44294124").mkdir(parents=True, exist_ok=True)
    else:
        shutil.rmtree(NET_DIR, ignore_errors=True)


def fake_run_usbip(*args):
    calls.append(args)
    if args[:2] == ("list", "-l"):
        return 0, LISTING
    if args[:2] == ("bind", "-b"):
        shared.add(args[2])
        if args[2] == "2-7":
            set_netdir(False)
        return 0, ""
    if args[:2] == ("unbind", "-b"):
        shared.discard(args[2])
        if args[2] == "2-7":
            set_netdir(True)
        return 0, ""
    return 0, ""


web.run_usbip = fake_run_usbip
web.is_shared = lambda busid: busid in shared

httpd = ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
base = f"http://127.0.0.1:{port}"
TOKEN = "netdev-admin-token"


def call(path, payload=None, token=TOKEN, method=None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(base + path, data=data, method=method or ("POST" if data else "GET"))
    if data:
        request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("X-Admin-Token", token)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, json.loads(response.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())


def binds():
    return [call_ for call_ in calls if call_[:2] == ("bind", "-b")]


def device_of(body, busid):
    return next((device for device in body.get("devices", []) if str(device["busid"]) == busid), None)


print("[B] list_devices() 暴露 networkInterfaces")
st, body = call("/api/devices")
check("GET /api/devices 正常", st == 200 and body.get("ok") is True, f"status={st}")
check("网卡字段非空", device_of(body, "2-7")["networkInterfaces"] == ["enxc84d44294124"],
      str(device_of(body, "2-7").get("networkInterfaces")))
check("非网卡字段为空数组", device_of(body, "1-1")["networkInterfaces"] == [])
check("顶层带生效中的 kickIdleSeconds",
      body.get("kickIdleSeconds") == web.KICK_IDLE_SECONDS and isinstance(body.get("kickIdleSeconds"), int),
      str(body.get("kickIdleSeconds")))

print("[C] 共享网卡被拒绝（409，且没有 bind）")
calls.clear()
st, body = call("/api/devices/2-7/share", {})
check("返回 409", st == 409, f"status={st} body={body}")
check("文案含「USB 网卡」且带接口名",
      "USB 网卡" in str(body.get("error")) and "enxc84d44294124" in str(body.get("error")),
      str(body.get("error")))
check("没有执行 bind", not binds(), str(calls))
check("设备仍未共享", "2-7" not in shared)

print("[D] 非网卡正常共享")
calls.clear()
st, body = call("/api/devices/1-1/share", {})
check("共享成功", st == 200 and body.get("ok") is True, f"status={st} body={body}")
check("执行了 bind", ("bind", "-b", "1-1") in calls, str(calls))
st, body = call("/api/devices/1-1/unshare", {})
check("清理：停止共享", st == 200 and body.get("ok") is True)

print("[E] 逃生阀 USBIP_ALLOW_NETDEV=true")
os.environ["USBIP_ALLOW_NETDEV"] = "true"
calls.clear()
st, body = call("/api/devices/2-7/share", {})
check("允许共享", st == 200 and body.get("ok") is True, f"status={st} body={body}")
check("执行了 bind", ("bind", "-b", "2-7") in calls, str(calls))
check("共享后 net/ 已消失（真机上就是掉网那一刻）",
      web.device_network_interfaces("2-7") == [], str(web.device_network_interfaces("2-7")))

print("[F] 网卡仍能识别出 net/ 时，强制断开也不能重新 bind")
# 关掉逃生阀（[E] 刚把它打开），否则这道防线本来就会被显式放行。
os.environ.pop("USBIP_ALLOW_NETDEV", None)
check("逃生阀已关闭", web.netdev_sharing_allowed() is False)
# 这是"部分状态"下的防线：设备被标成共享、但 net/ 还在（多接口设备等）。
set_netdir(True)
shared.add("2-7")
calls.clear()
st, body = call("/api/devices/2-7/kick", {})
check("kick 返回 409", st == 409, f"status={st} body={body}")
check("文案指向「停止共享」", "停止共享" in str(body.get("error")), str(body.get("error")))
check("没有执行 unbind/bind", not calls or all(call_[:2] == ("list", "-l") for call_ in calls), str(calls))
check("设备仍在共享（没有被偷偷改状态）", "2-7" in shared)
calls.clear()
st, body = call("/api/devices/2-7/unshare", {})
check("「停止共享」仍然可用（只 unbind，归还驱动）",
      st == 200 and ("unbind", "-b", "2-7") in calls and not binds(), str(calls))
check("停止共享后网络接口回来了", web.device_network_interfaces("2-7") == ["enxc84d44294124"])

print("[F2] 已知盲区（记录在案）：网卡已被绑定时 net/ 已消失，无法再判定")
# 真机上"设备已被 usbip-host 绑着"时 net/ 目录不存在，net/ 判据失效 ——
# 这条路径只能靠 entrypoint.sh 的默认路由回滚兜底，用例把现状钉住以免被误解。
set_netdir(False)
shared.add("2-7")
calls.clear()
st, body = call("/api/devices/2-7/kick", {})
check("此时 kick 会照常执行 unbind+bind（无法识别为网卡）",
      st == 200 and ("bind", "-b", "2-7") in calls, f"status={st} calls={calls}")
calls.clear()
st, body = call("/api/devices/2-7/unshare", {})
check("清理：回到未共享", st == 200 and ("unbind", "-b", "2-7") in calls, str(calls))

print("[G] 隐藏一块网卡仍然只 unbind、不 bind")
shared.add("2-7")
calls.clear()
st, body = call("/api/devices/2-7/hide", {})
check("hide 成功", st == 200 and body.get("ok") is True, f"status={st} body={body}")
check("hide 调用了 unbind", ("unbind", "-b", "2-7") in calls, str(calls))
check("hide 没有调用 bind", not binds(), str(calls))
check("hide 后设备不再共享", "2-7" not in shared)
check("hide 后默认列表里没有它", device_of(call("/api/devices")[1], "2-7") is None)
st, body = call("/api/devices/2-7/unhide", {})
check("unhide 恢复正常", st == 200 and body.get("ok") is True)

print("[H] 只读/未登录路径不受影响")
st, body = call("/api/devices", token=None)
check("匿名仍可列设备", st == 200 and len(body.get("devices", [])) == 2, f"status={st}")
check("匿名也能看到 networkInterfaces（客户端只读，不消费）",
      device_of(body, "2-7")["networkInterfaces"] == ["enxc84d44294124"])
calls.clear()
st, body = call("/api/devices/2-7/share", {}, token=None)
check("未登录 share → 401（先鉴权再判网卡）", st == 401, f"status={st}")
check("未登录没有 bind", not binds(), str(calls))

httpd.shutdown()
httpd.server_close()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
