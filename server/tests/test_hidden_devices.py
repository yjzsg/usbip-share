"""隐藏设备（hide/unhide）：把指定硬件从设备列表与共享池里摘出去。

用例背景：用户把飞牛上的 USB 网卡（Realtek 0bda:8156）误点成「开始共享」，
一旦真的 bind 到 usbip-host，NAS 会直接掉网。隐藏功能必须做到：

  * 隐藏后匿名 `GET /api/devices` 看不到它（Windows 客户端因此完全看不到）；
  * 只有「带有效管理令牌」且「带 ?includeHidden=1」才看得到；
  * 隐藏一台正在共享的设备时先停止共享（`usbip unbind`，设备归还原驱动）；
  * 隐藏的设备不能再被 share（409，且不执行 bind）；
  * 隐藏标记绑定硬件（序列号/型号/同型号序号键），换插口后仍然生效；
  * 既有 metadata 迁移不丢 hidden。

    python tests/test_hidden_devices.py
"""
import json
import os
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
ADMIN_PASSWORD = "hidden-test-pass"
os.environ.pop("USBIP_WEB_TOKEN", None)
os.environ["USBIP_WEB_PASSWORD"] = ADMIN_PASSWORD
web.AUTH_FILE = root / "auth.json"
web.METADATA_FILE = root / "device-metadata.json"
web.MANAGED_FILE = root / "managed-busids"
web.CLIENTS_FILE = root / "clients.json"
web.QUEUE_FILE = root / "queue.json"
web.NOTIFY_FILE = root / "queue-notify.json"
web._ENV_PASSWORD_APPLIED = None

# 假 usbip 世界：`shared` 就是"当前导出的设备集合"，run_usbip 会真的改它，
# 这样"隐藏时有没有真的停止共享"可以直接断言，而不是只看调用记录。
shared: set[str] = set()
calls: list[tuple] = []


def fake_run_usbip(*args):
    calls.append(args)
    if args[:2] == ("list", "-l"):
        return 0, LISTING
    if args[:2] == ("bind", "-b"):
        shared.add(args[2])
        return 0, ""
    if args[:2] == ("unbind", "-b"):
        shared.discard(args[2])
        return 0, ""
    return 0, ""


web.run_usbip = fake_run_usbip
web.is_shared = lambda busid: busid in shared
web.load_auth()

httpd = ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
base = f"http://127.0.0.1:{port}"


def call(path, payload=None, token=None, method=None):
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


def busids(body):
    return sorted(str(device["busid"]) for device in body["devices"])


def device_of(body, busid):
    return next((device for device in body["devices"] if str(device["busid"]) == busid), None)


st, body = call("/api/login", {"password": ADMIN_PASSWORD})
token = body.get("token", "")
check("管理员登录成功", st == 200 and bool(token), str(body))

# ---------------------------------------------------------------------------
print("[A] 存储层：隐藏标记绑定硬件键")


def dev(busid, vidpid="0bda:8156", manufacturer="Realtek", product="USB 10/100/1000 LAN",
        fingerprint="RK", serial="", index=0, identity=None):
    return {
        "busid": busid, "vidpid": vidpid, "description": f"{manufacturer} : {product}",
        "serial": serial, "manufacturer": manufacturer, "product": product,
        "bcdDevice": "0100", "deviceClass": "00", "fingerprint": fingerprint,
        "identity": identity or f"path:/sys/bus/usb/devices/{busid}", "modelIndex": index,
    }


old_list_devices = web.list_devices
card = dev("2-7")
web.list_devices = lambda include_internal=False: ([dict(card)], None)
ok, message = web.set_device_hidden("2-7", True)
check("隐藏成功", ok and "已隐藏" in message, message)
check("写到了型号键（无序列号 → fingerprint 键）",
      web.read_metadata().get("fingerprint:RK", {}).get("hidden") == "true",
      str(web.read_metadata()))
check("device_hidden() 读到隐藏", web.device_hidden(card, {"RK": 1}, 0) is True)

moved = dev("3-1", identity="path:/sys/bus/usb/devices/3-1")
check("换到另一个 USB 口后仍然隐藏（按硬件而非端口）", web.device_hidden(moved, {"RK": 1}, 0) is True)
other = dev("3-1", vidpid="abcd:1234", manufacturer="Other", product="Thing", fingerprint="OTHER",
            identity="path:/sys/bus/usb/devices/3-1")
check("同一个口换插别的型号 → 不继承隐藏", web.device_hidden(other, {"OTHER": 1}, 0) is False)

print("[A2] 既有 metadata 迁移不丢 hidden")
web.METADATA_FILE = root / "migrate.json"
web.write_metadata({"device:0bda:8156|Realtek : USB 10/100/1000 LAN": {
    "alias": "网卡", "remark": "", "vidpid": "0bda:8156", "serial": "SN9",
    "fingerprint": "RK", "hidden": "true"}})
serial_card = dev("2-7", serial="SN9")
snap = web.migrate_metadata([serial_card], {"RK": 1})
check("旧型号级记录迁移到序列号键且保留 hidden",
      snap.get(web.serial_key(serial_card), {}).get("hidden") == "true"
      and snap.get(web.serial_key(serial_card), {}).get("alias") == "网卡",
      str(snap))
check("迁移后仍判定为隐藏", web.device_hidden(serial_card, {"RK": 1}, 0, snap) is True)

# 目标键已存在（设备已被单独命名）时，旧记录里的 hidden 也不能丢。
web.write_metadata({
    web.serial_key(serial_card): {"alias": "新名字", "remark": "", "vidpid": "0bda:8156",
                                  "serial": "SN9", "fingerprint": "RK"},
    "device:0bda:8156|Realtek : USB 10/100/1000 LAN": {
        "alias": "网卡", "remark": "", "vidpid": "0bda:8156", "serial": "SN9",
        "fingerprint": "RK", "hidden": "true"},
})
snap = web.migrate_metadata([serial_card], {"RK": 1})
check("目标键已存在时合并 hidden",
      snap.get(web.serial_key(serial_card), {}).get("hidden") == "true", str(snap))
check("已有名字未被旧记录覆盖", snap.get(web.serial_key(serial_card), {}).get("alias") == "新名字")

# ---------------------------------------------------------------------------
print("[B] 可见性：匿名 / 令牌 / includeHidden")
web.METADATA_FILE = root / "device-metadata.json"
web.write_metadata({})
web.list_devices = old_list_devices

st, body = call("/api/devices")
check("初始两台设备都可见", st == 200 and busids(body) == ["1-1", "2-7"], str(busids(body)))
check("每台设备都带 hidden 布尔字段",
      all(device.get("hidden") is False for device in body["devices"]))

st, body = call("/api/devices/2-7/hide", {}, token=token)
check("hide 返回 ok/message/devices", st == 200 and body.get("ok") is True and "devices" in body,
      f"status={st} body={body}")
check("hide 后默认响应里不再有该设备", busids(body) == ["1-1"], str(busids(body)))

st, body = call("/api/devices")
check("匿名 GET 看不到隐藏设备", st == 200 and busids(body) == ["1-1"], str(busids(body)))
st, body = call("/api/devices?includeHidden=1")
check("匿名带 includeHidden=1 仍然看不到", busids(body) == ["1-1"], str(busids(body)))
st, body = call("/api/devices?includeHidden=1", token="bogus.token")
check("无效令牌带 includeHidden=1 也看不到", busids(body) == ["1-1"], str(busids(body)))
st, body = call("/api/devices?includeHidden=1", token=token)
check("带令牌 + includeHidden=1 能看到隐藏设备", busids(body) == ["1-1", "2-7"], str(busids(body)))
hidden_card = device_of(body, "2-7")
check("隐藏设备 hidden=true", hidden_card is not None and hidden_card.get("hidden") is True)
for field in ("busid", "displayName", "alias", "remark", "shared", "connections",
              "currentHolder", "pendingQueue", "hidden"):
    check(f"隐藏设备字段完整 {field}", field in (hidden_card or {}))

# ---------------------------------------------------------------------------
print("[C] 隐藏一台正在共享的设备：先停止共享")
calls.clear()
shared.add("2-7")
st, body = call("/api/devices/2-7/unhide", {}, token=token)
check("先取消隐藏以便重做场景", st == 200 and body.get("ok") is True, str(body))
shared.add("2-7")
calls.clear()
st, body = call("/api/devices/2-7/hide", {}, token=token)
check("隐藏成功且 message 说明已停止共享",
      st == 200 and body.get("ok") is True and "已同时停止共享" in str(body.get("message")),
      str(body.get("message")))
check("调用了 usbip unbind（停止共享，设备归还原驱动）",
      ("unbind", "-b", "2-7") in calls, str(calls))
check("没有调用 usbip bind（否则等于把设备重新共享出去）",
      not any(call[:2] == ("bind", "-b") for call in calls), str(calls))
check("设备确实不再处于共享状态", "2-7" not in shared, str(shared))
st, body = call("/api/devices")
check("隐藏后默认列表里没有它", busids(body) == ["1-1"], str(busids(body)))

print("[D] 已隐藏设备不能再被共享")
calls.clear()
st, body = call("/api/devices/2-7/share", {}, token=token)
check("share 返回 409", st == 409, f"status={st} body={body}")
check("错误文案符合契约",
      body.get("error") == "该设备已隐藏，请先取消隐藏后再共享", str(body.get("error")))
check("没有执行 bind", not any(call[:2] == ("bind", "-b") for call in calls), str(calls))
check("设备仍未共享", "2-7" not in shared)

print("[E] unhide 后恢复可见、可以正常共享")
st, body = call("/api/devices/2-7/unhide", {}, token=token)
check("unhide 成功", st == 200 and body.get("ok") is True, str(body))
check("响应里设备已可见且 hidden=false",
      device_of(body, "2-7") is not None and device_of(body, "2-7").get("hidden") is False)
st, body = call("/api/devices")
check("匿名列表恢复可见", busids(body) == ["1-1", "2-7"], str(busids(body)))
calls.clear()
st, body = call("/api/devices/2-7/share", {}, token=token)
check("unhide 后可以共享", st == 200 and body.get("ok") is True, f"status={st} body={body}")
check("确实执行了 bind", ("bind", "-b", "2-7") in calls, str(calls))
st, body = call("/api/devices/2-7/unshare", {}, token=token)
check("清理：恢复未共享", st == 200 and body.get("ok") is True, str(body))

print("[F] 幂等与错误处理")
st, first = call("/api/devices/2-7/hide", {}, token=token)
calls.clear()
st, second = call("/api/devices/2-7/hide", {}, token=token)
check("重复 hide 都返回 ok", st == 200 and first.get("ok") is True and second.get("ok") is True, str(second))
check("重复 hide 不再触发 unbind/bind（设备已未共享）",
      not any(call[:2] in (("bind", "-b"), ("unbind", "-b")) for call in calls), str(calls))
st, body = call("/api/devices?includeHidden=1", token=token)
check("重复 hide 后仍然是隐藏状态",
      device_of(body, "2-7") is not None and device_of(body, "2-7").get("hidden") is True
      and busids(call("/api/devices")[1]) == ["1-1"], str(busids(body)))
st, first = call("/api/devices/2-7/unhide", {}, token=token)
st, second = call("/api/devices/2-7/unhide", {}, token=token)
check("重复 unhide 都返回 ok", st == 200 and first.get("ok") is True and second.get("ok") is True, str(second))
check("unhide 后记录里的标记已清掉",
      all(record.get("hidden", "") != "true" for record in web.read_metadata().values()),
      str(web.read_metadata()))
st, body = call("/api/devices/9-9/hide", {}, token=token)
check("不存在的设备 → 400", st == 400 and body.get("ok") is False, f"status={st} body={body}")
st, body = call("/api/devices/..%2Fetc/hide", {}, token=token)
check("非法 busid → 400", st == 400 and body.get("ok") is False, f"status={st} body={body}")

print("[G] 未登录与 mustChange 门同样覆盖 hide/unhide")
st, body = call("/api/devices/2-7/hide", {})
check("未登录 → 401", st == 401, f"status={st} body={body}")
check("未登录没有写任何隐藏标记",
      all(record.get("hidden", "") != "true" for record in web.read_metadata().values()),
      str(web.read_metadata()))

# 临时摘掉托管密码，构造一个 mustChange=true 的会话。
os.environ.pop("USBIP_WEB_PASSWORD", None)
original_auth = web.AUTH_FILE
web.AUTH_FILE = root / "auth-mustchange.json"
salt = "must-change-salt"
web.atomic_write_json(web.AUTH_FILE, {
    "version": 2, "salt": salt, "pwHash": web.hash_password(ADMIN_PASSWORD, salt), "mustChange": True})
web._ENV_PASSWORD_APPLIED = None
must_change_token = web.issue_session_token(web.load_auth())
st, body = call("/api/devices/2-7/hide", {}, token=must_change_token)
check("mustChange=true 时 hide 被 403 拦下",
      st == 403 and body.get("error") == "must_change_password", f"status={st} body={body}")
check("mustChange 拦截时没有写隐藏标记",
      all(record.get("hidden", "") != "true" for record in web.read_metadata().values()))
web.AUTH_FILE = original_auth
os.environ["USBIP_WEB_PASSWORD"] = ADMIN_PASSWORD
web._ENV_PASSWORD_APPLIED = None

httpd.shutdown()
httpd.server_close()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
