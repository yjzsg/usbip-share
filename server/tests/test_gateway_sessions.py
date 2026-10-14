"""回归：网关解析 OP_REQ_IMPORT，补上客户端没上报的占用。

真机现象：两个客户端心跳里的 `busids` 一直是空的，服务端因此认为"没人占用"，
而客户端想连 1-8.2 时 usbipd 回 `Device busy (already exported)` —— 两边状态对不上。
网关本来就在中继这条连接，解析 OP_REQ_IMPORT 得到的是硬事实。

本用例覆盖：
  * parse_import_head 的三种状态（pending / import / other）与畸形输入；
  * SessionRegistry 写盘、清理、原子性；
  * relay_client_usbip 在嗅探的同时**逐字节转发**（观测绝不能改数据）；
  * connection_info 把网关观测到的会话补进来，且不与客户端声明重复。

    python tests/test_gateway_sessions.py
"""
import json
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gateway  # noqa: E402
import web  # noqa: E402

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


def import_message(busid: str) -> bytes:
    """构造一条 OP_REQ_IMPORT。

    请求体是 `op_import_request { char busid[32]; }`,不是 usbip_usb_device ——
    后者只出现在 OP_REP_IMPORT 回复里。所以整条消息 = 8 字节头 + 32 字节 busid。
    """
    busid_bytes = busid.encode("utf-8")[:31].ljust(gateway.USBIP_BUSID_SIZE, b"\0")
    assert len(busid_bytes) == 32
    return struct.pack(">HHI", gateway.USBIP_VERSION, gateway.OP_REQ_IMPORT, 0) + busid_bytes


print("[A] parse_import_head 的三种状态")
msg = import_message("1-8.2")
state, busid = gateway.parse_import_head(msg)
check("完整导入请求 → import 且 busid 正确", state == "import" and busid == "1-8.2", f"{state}/{busid}")
state, _ = gateway.parse_import_head(msg[:3])
check("字节不足 → pending", state == "pending", state)
state, _ = gateway.parse_import_head(msg[:gateway.USBIP_IMPORT_MIN_BYTES - 1])
check("差一个字节仍 pending", state == "pending", state)
state, _ = gateway.parse_import_head(msg[:gateway.USBIP_IMPORT_MIN_BYTES])
check("刚好够 → import", state == "import", state)

devlist = struct.pack(">HHI", gateway.USBIP_VERSION, 0x8005, 0) + b"\0" * 16
state, _ = gateway.parse_import_head(devlist)
check("OP_REQ_DEVLIST → other（放弃嗅探）", state == "other", state)

urb = struct.pack(">HHI", gateway.USBIP_VERSION, 0x0001, 0) + b"\0" * 32
state, _ = gateway.parse_import_head(urb)
check("URB 数据 → other", state == "other", state)

state, _ = gateway.parse_import_head(b"GET / HTTP/1.1\r\n")
check("HTTP 请求 → other", state == "other", state)

state, _ = gateway.parse_import_head(struct.pack(">HHI", 0x9999, gateway.OP_REQ_IMPORT, 0) + b"\0" * 300)
check("版本号不对 → other", state == "other", state)

print("[B] busid 的 NUL 结尾与边界")
short = struct.pack(">HHI", gateway.USBIP_VERSION, gateway.OP_REQ_IMPORT, 0) + b"1-8\0" + b"\0" * 28
state, busid = gateway.parse_import_head(short)
check("按第一个 NUL 截断", state == "import" and busid == "1-8", f"{state}/{busid}")
full32 = struct.pack(">HHI", gateway.USBIP_VERSION, gateway.OP_REQ_IMPORT, 0) + b"1-8.2".ljust(32, b"\0")
state, busid = gateway.parse_import_head(full32 + b"\xde\xad\xbe\xef" * 50)
check("后面跟着 URB 数据也不影响（只看固定 32 字节）",
      state == "import" and busid == "1-8.2", f"{state}/{busid}")

print("[C] SessionRegistry 写盘与清理")
temp = tempfile.TemporaryDirectory()
session_path = Path(temp.name) / "sub" / "usbip-sessions.json"
registry = gateway.SessionRegistry(session_path)
check("初始化就建好文件（目录不存在也能建）", session_path.exists())

token = registry.open_session()
registry.record_import(token, "1-8.2", "192.168.123.129", 51635)
payload = json.loads(session_path.read_text(encoding="utf-8"))
sessions = payload["sessions"]
check("记录已落盘", len(sessions) == 1 and sessions[0]["busid"] == "1-8.2", str(sessions))
check("带来源 IP 与端口",
      sessions[0]["clientIp"] == "192.168.123.129" and sessions[0]["clientPort"] == 51635,
      str(sessions[0]))

token2 = registry.open_session()
registry.record_import(token2, "1-8.3", "10.0.0.5", 40000)
sessions = json.loads(session_path.read_text(encoding="utf-8"))["sessions"]
check("第二条会话并存", len(sessions) == 2, str(sessions))

registry.close_session(token)
sessions = json.loads(session_path.read_text(encoding="utf-8"))["sessions"]
check("关掉一条后只剩另一条",
      len(sessions) == 1 and sessions[0]["busid"] == "1-8.3", str(sessions))

registry.record_import(registry.open_session(), "", "10.0.0.9", 1)
sessions = json.loads(session_path.read_text(encoding="utf-8"))["sessions"]
check("空 busid 不记录", len(sessions) == 1, str(sessions))

registry.close_session(token2)
sessions = json.loads(session_path.read_text(encoding="utf-8"))["sessions"]
check("全部关掉后是空表", sessions == [], str(sessions))
check("关闭不存在的 token 不报错", (registry.close_session(99999) or True))

print("[D] bridge：嗅探的同时逐字节转发，连接结束清理会话")
payload_sent = import_message("1-8.2") + b"\x01\x02\x03" * 400

# 起一个只收不发的假 upstream（就是 usbipd 的位置）。
upstream_listener = socket.socket()
upstream_listener.bind(("127.0.0.1", 0))
upstream_listener.listen(1)
upstream_port = upstream_listener.getsockname()[1]
collected = {}

def accept_once():
    conn, _ = upstream_listener.accept()
    conn.settimeout(5)
    data = bytearray()
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            # 收到就更新，不要等 EOF —— 否则要跟这里的 5 秒超时赛跑。
            collected["data"] = bytes(data)
    except OSError:
        pass
    collected["data"] = bytes(data)
    conn.close()

threading.Thread(target=accept_once, daemon=True).start()

client_a, client_b = socket.socketpair()
session_path2 = Path(temp.name) / "relay.json"
registry2 = gateway.SessionRegistry(session_path2)

bridge_thread = threading.Thread(
    target=gateway.bridge,
    args=(client_a, "127.0.0.1", upstream_port, payload_sent[:1]),
    kwargs={"registry": registry2, "client_address": ("192.168.1.77", 12345)},
    daemon=True,
)
bridge_thread.start()
client_b.sendall(payload_sent[1:])

deadline = time.time() + 5
while time.time() < deadline and not collected.get("data"):
    time.sleep(0.05)
check("转发内容与发送内容逐字节相同", collected.get("data") == payload_sent,
      f"sent={len(payload_sent)} got={len(collected.get('data') or b'')}")
check("嗅探到了 busid",
      json.loads(session_path2.read_text(encoding="utf-8"))["sessions"][0]["busid"] == "1-8.2")

client_b.close()
bridge_thread.join(timeout=5)
check("连接结束后 bridge 线程退出", not bridge_thread.is_alive())
check("连接结束后会话被清理",
      json.loads(session_path2.read_text(encoding="utf-8"))["sessions"] == [])

for sock in (client_a, upstream_listener):
    try:
        sock.close()
    except OSError:
        pass

print("[E] web.py 读取观测会话并合并进 connection_info")
old_session_file = web.SESSION_FILE
old_clients_file = web.CLIENTS_FILE
old_queue_file = web.QUEUE_FILE
old_notify_file = web.NOTIFY_FILE
web.SESSION_FILE = Path(temp.name) / "web-sessions.json"
web.CLIENTS_FILE = Path(temp.name) / "web-clients.json"
web.QUEUE_FILE = Path(temp.name) / "web-queue.json"
web.NOTIFY_FILE = Path(temp.name) / "web-notify.json"
try:
    check("文件不存在时返回空表（行为与旧版一致）", web.read_observed_sessions() == [])
    web.SESSION_FILE.write_text("{ this is not json", encoding="utf-8")
    check("文件损坏时返回空表，不抛异常", web.read_observed_sessions() == [])
    web.SESSION_FILE.write_text(json.dumps({"version": 1, "sessions": [
        {"busid": "1-8.2", "clientIp": "192.168.1.77", "clientPort": 1, "startedAt": 100},
        {"busid": "", "clientIp": "192.168.1.78", "clientPort": 2, "startedAt": 100},
        "垃圾",
    ]}), encoding="utf-8")
    check("过滤掉空 busid 与非字典项", len(web.read_observed_sessions()) == 1)

    # 没有任何客户端声明时，占用信息应该来自网关观测。
    web.CLIENTS_FILE.write_text(json.dumps({"version": 1, "clients": {}}), encoding="utf-8")
    holders = web.connection_info("1-8.2")
    check("客户端没上报时也能看到占用方",
          len(holders) == 1 and holders[0]["address"] == "192.168.1.77", str(holders))
    check("标记为观测来源", holders[0].get("observed") is True, str(holders[0]))
    check("没有占用的设备仍是空表", web.connection_info("1-8.4") == [])

    # 客户端已经声明过同一地址时不再重复列一遍。
    web.CLIENTS_FILE.write_text(json.dumps({"version": 1, "clients": {
        "cid-1": {"clientId": "cid-1", "name": "台式", "address": "192.168.1.77",
                  "busids": ["1-8.2"], "lastSeen": int(time.time())},
    }}), encoding="utf-8")
    holders = web.connection_info("1-8.2")
    check("同地址不重复列出", len(holders) == 1 and holders[0]["name"] == "台式", str(holders))
    check("客户端声明的不带 observed 标记", not holders[0].get("observed"), str(holders[0]))

    # 客户端声明了别的地址时，网关观测的这条要单独列出。
    web.CLIENTS_FILE.write_text(json.dumps({"version": 1, "clients": {
        "cid-2": {"clientId": "cid-2", "name": "笔记本", "address": "10.0.0.9",
                  "busids": ["1-8.2"], "lastSeen": int(time.time())},
    }}), encoding="utf-8")
    holders = web.connection_info("1-8.2")
    check("不同地址各自列出",
          len(holders) == 2 and {h["address"] for h in holders} == {"10.0.0.9", "192.168.1.77"},
          str(holders))
finally:
    web.SESSION_FILE = old_session_file
    web.CLIENTS_FILE = old_clients_file
    web.QUEUE_FILE = old_queue_file
    web.NOTIFY_FILE = old_notify_file
    temp.cleanup()

print("[F] main() 必须把 --session-file 传给 Gateway（曾经漏传过）")
probe = Path(temp.name) / "probe-sessions.json"
captured = {}
real_tcp_init = gateway.socketserver.ThreadingTCPServer.__init__
real_serve = gateway.Gateway.serve_forever
real_close = gateway.Gateway.server_close

def fake_tcp_init(self, addr, handler):      # 不真的监听端口
    captured["addr"] = addr

def fake_serve(self, poll_interval=0.5):     # 不进事件循环
    captured["session_path"] = self.sessions._path
    captured["access_path"] = self.access.path

def fake_close(self):                        # 没真的建 socket，别去 close
    pass

gateway.socketserver.ThreadingTCPServer.__init__ = fake_tcp_init
gateway.Gateway.serve_forever = fake_serve
gateway.Gateway.server_close = fake_close
saved_argv = sys.argv
sys.argv = ["gateway.py", "--port", "15555",
            "--access-file", "/tmp/probe-access.json",
            "--session-file", str(probe)]
try:
    gateway.main()
finally:
    sys.argv = saved_argv
    gateway.socketserver.ThreadingTCPServer.__init__ = real_tcp_init
    gateway.Gateway.serve_forever = real_serve
    gateway.Gateway.server_close = real_close

check("--session-file 真的传到了 SessionRegistry",
      captured.get("session_path") == str(probe), str(captured.get("session_path")))
check("--access-file 仍然照常传下去",
      captured.get("access_path") == "/tmp/probe-access.json", str(captured.get("access_path")))
check("会话表文件已在指定路径建好", probe.exists(), str(probe))

print("[G] 协议布局钉死：请求体是 op_import_request（8 字节头 + 32 字节 busid）")
check("常量偏移就是 8（不是 usbip_usb_device 里的 256+8）",
      gateway.USBIP_IMPORT_BUSID_OFFSET == 8, str(gateway.USBIP_IMPORT_BUSID_OFFSET))
check("整条消息 40 字节", len(import_message("1-8.2")) == 40, str(len(import_message("1-8.2"))))
check("busid 从第 8 字节开始",
      import_message("1-8.2")[8:14] == b"1-8.2\0", repr(import_message("1-8.2")[8:14]))

print("[H] 关闭访问控制时也要补 PROXY 头（否则服务端分不清谁在用哪台设备）")
# 用一个真的 loopback 连接跑一遍 GatewayHandler 的 web 分支，断言 usbipd/web 上游
# 收到的第一段就是 PROXY 行 —— 访问控制关闭（授权表不存在）时同样如此。
probe_listener = socket.socket()
probe_listener.bind(("127.0.0.1", 0))
probe_listener.listen(1)
probe_port = probe_listener.getsockname()[1]
seen = {}

def accept_probe():
    conn, _ = probe_listener.accept()
    conn.settimeout(2)
    data = bytearray()
    try:
        while b"GET /api/devices" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                break
            data += chunk
            seen["data"] = bytes(data)
    except OSError:
        pass
    seen["data"] = bytes(data)
    conn.close()

threading.Thread(target=accept_probe, daemon=True).start()

# 授权表不存在 = 访问控制关闭。
gateway_instance = gateway.Gateway.__new__(gateway.Gateway)
gateway_instance.upstream_host = "127.0.0.1"
gateway_instance.web_port = probe_port
gateway_instance.usbip_port = probe_port
gateway_instance.access = gateway.AccessGate(Path(temp.name) / "no-such-access.json")
gateway_instance.proxy_secret = "probe-secret"
gateway_instance.sessions = gateway.SessionRegistry(None)
check("前置：访问控制确实是关闭的", not gateway_instance.access.enabled())

handler = gateway.GatewayHandler.__new__(gateway.GatewayHandler)
client_end, server_end = socket.socketpair()
handler.request = server_end
handler.client_address = ("192.168.1.99", 43210)
handler.server = gateway_instance
client_end.sendall(b"GET /api/devices HTTP/1.1\r\nHost: x\r\n\r\n")
threading.Thread(target=handler.handle, daemon=True).start()

deadline = time.time() + 5
while time.time() < deadline and not seen.get("data"):
    time.sleep(0.05)
payload = seen.get("data") or b""
check("访问控制关闭时仍然补了 PROXY 头", payload.startswith(b"PROXY TCP4 192.168.1.99 "), repr(payload[:60]))
check("带上了校验标记（7 段）",
      len(payload.split(b"\r\n", 1)[0].split(b" ")) == 7, repr(payload.split(b"\r\n", 1)[0]))
check("HTTP 数据原样跟在后面", b"GET /api/devices HTTP/1.1" in payload, repr(payload[-60:]))

for sock in (client_end, server_end, probe_listener):
    try:
        sock.close()
    except OSError:
        pass

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
