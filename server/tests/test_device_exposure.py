"""F7 回归：免鉴权的 GET /api/devices 不再回显客户端上报的 publicIp。

`GET /api/devices` 必须保持免鉴权（已发布的 Windows 客户端靠它列设备），但它
回显的 `connections[].publicIp` 是管理页才需要的信息，等于把局域网里各客户端的
公网出口地址白送给任何能访问该端口的人。修复后：没带有效 X-Admin-Token 的调用
拿到的数据里没有 publicIp；带令牌的管理页照旧拿到完整数据。

客户端消费的字段（clientId/name/address/alias/remark/shared/connections/
currentHolder/pendingQueue）必须一字不少 —— 这条是硬约束。

    python tests/test_device_exposure.py
"""
import json
import os
import sys
import tempfile
import threading
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web  # noqa: E402

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


LISTING = " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n"

root = Path(tempfile.mkdtemp())
os.environ.pop("USBIP_WEB_TOKEN", None)
os.environ.pop("USBIP_WEB_PASSWORD", None)
web.AUTH_FILE = root / "auth.json"
web.METADATA_FILE = root / "device-metadata.json"
web.MANAGED_FILE = root / "managed-busids"
web.CLIENTS_FILE = root / "clients.json"
web.QUEUE_FILE = root / "queue.json"
web.NOTIFY_FILE = root / "queue-notify.json"
web._ENV_PASSWORD_APPLIED = None

old_run_usbip = web.run_usbip
old_is_shared = web.is_shared
web.run_usbip = lambda *args: (0, LISTING) if args[:2] == ("list", "-l") else (0, "ok")
web.is_shared = lambda busid: True

auth = web.load_auth()
admin_token = web.issue_session_token(auth)

# 一个正在占用设备的客户端，带 publicIp 上报。
ok, message, _record, _notifications = web.update_client(
    {"clientId": "peer-1", "clientName": "前台电脑", "busids": ["1-1"],
     "publicIp": "8.8.8.8", "dataPort": 5555},
    "192.168.1.21",
)
assert ok, message
web.queue_append("1-1", "waiter-1")

httpd = ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
base = f"http://127.0.0.1:{port}"


def fetch_devices(token=None):
    request = urllib.request.Request(base + "/api/devices")
    if token:
        request.add_header("X-Admin-Token", token)
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


print("[A] 未认证调用：剥离 publicIp，其余字段照旧")
anonymous = fetch_devices()
device = anonymous["devices"][0]
check("设备列表可用", anonymous.get("ok") is True and len(anonymous["devices"]) == 1)
check("connections[].publicIp 已剥离",
      all("publicIp" not in connection for connection in device["connections"]), str(device["connections"]))
check("currentHolder.publicIp 已剥离",
      device["currentHolder"] is not None and "publicIp" not in device["currentHolder"],
      str(device["currentHolder"]))
for field in ("busid", "displayName", "alias", "remark", "shared", "connections",
              "currentHolder", "pendingQueue"):
    check(f"设备级字段保留 {field}", field in device)
for field in ("clientId", "name", "address"):
    check(f"connections[] 字段保留 {field}",
          device["connections"] and field in device["connections"][0])
for field in ("clientId", "name"):
    check(f"currentHolder 字段保留 {field}", field in device["currentHolder"])
check("pendingQueue 字段保留 clientId/name",
      device["pendingQueue"] == [{"clientId": "waiter-1", "name": "未命名客户端"}],
      str(device["pendingQueue"]))
check("未认证也能看到占用方地址（客户端依赖）",
      device["connections"][0]["address"] == "192.168.1.21")

print("[B] 认证调用：完整数据（管理页）")
authorized = fetch_devices(admin_token)
admin_device = authorized["devices"][0]
check("带令牌时 publicIp 可见",
      admin_device["connections"][0].get("publicIp") == "8.8.8.8"
      and admin_device["currentHolder"].get("publicIp") == "8.8.8.8",
      str(admin_device["connections"]))
check("无效令牌按未认证处理",
      all("publicIp" not in connection
          for connection in fetch_devices("bogus.token")["devices"][0]["connections"]))

print("[C] redact_public_ips 是纯拷贝，不改动原对象")
source = [{"busid": "1-1", "connections": [{"clientId": "c", "name": "n", "address": "a", "publicIp": "8.8.8.8"}],
           "currentHolder": None}]
source[0]["currentHolder"] = source[0]["connections"][0]   # list_devices() 的真实别名关系
redacted = web.redact_public_ips(source)
check("原始对象未被修改", "publicIp" in source[0]["connections"][0])
check("拷贝里 connections 与 currentHolder 都没有 publicIp",
      "publicIp" not in redacted[0]["connections"][0]
      and "publicIp" not in redacted[0]["currentHolder"])
check("别名被拆开（currentHolder 不再是原 connections 元素）",
      redacted[0]["currentHolder"] is not redacted[0]["connections"][0])
check("connections 不是列表时不炸", web.redact_public_ips([{"busid": "x", "connections": None}])[0]["connections"] is None)

httpd.shutdown()
httpd.server_close()
web.run_usbip = old_run_usbip
web.is_shared = old_is_shared

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
