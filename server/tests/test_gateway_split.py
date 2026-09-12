"""单端口网关回归测试：按首字节分流 + 空闲长会话不能被拆断。

第二个用例针对一个真实踩过的坑：`socket.create_connection(timeout=...)` 会把超时
留在 socket 上，转发线程的 `recv` 于是会在空闲若干秒后抛超时并把连接拆掉——
USB/IP 会话表现为"没人操作也每 ~20 秒断一次"。这里用一个静置 12 秒后仍要能
正常收发的用例把它钉住。

用 Python 标准库即可运行，不需要真实 USB 设备：
    python tests/test_gateway_split.py
"""
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gateway  # noqa: E402

IDLE_SECONDS = 12.5
SETTLE = 0.35           # 假后端的静默窗口：攒够一小段再整段回显
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("  PASS " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


class EchoBackend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, tag: bytes):
        self.tag = tag
        super().__init__(("127.0.0.1", 0), EchoHandler)


class EchoHandler(socketserver.BaseRequestHandler):
    """静默窗口内把收到的字节攒起来整段回显,避免分片导致断言抖动。"""

    def handle(self):
        while True:
            buf = b""
            self.request.settimeout(SETTLE)
            try:
                while True:
                    data = self.request.recv(4096)
                    if not data:
                        return
                    buf += data
            except OSError:
                pass
            if buf:
                try:
                    self.request.sendall(self.server.tag + buf)
                except OSError:
                    return


def exchange(sock: socket.socket, payload: bytes, settle: float = 1.0) -> bytes:
    """在既有连接上发一段并收齐回声（读到静默为止）。"""
    sock.sendall(payload)
    sock.settimeout(settle)
    buf = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
    except OSError:
        pass
    return buf


def roundtrip(port: int, payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        return exchange(sock, payload)


usbip_backend = EchoBackend(b"USB:")
web_backend = EchoBackend(b"WEB:")
for backend in (usbip_backend, web_backend):
    threading.Thread(target=backend.serve_forever, daemon=True).start()

gw = gateway.Gateway(("127.0.0.1", 0), "127.0.0.1",
                     usbip_backend.server_address[1], web_backend.server_address[1])
threading.Thread(target=gw.serve_forever, daemon=True).start()
port = gw.server_address[1]

print("[1] 首字节分流")
payload = b"\x11\x01\x00\x00ping"
check("0x11(USB/IP 版本)走 usbipd", roundtrip(port, payload) == b"USB:" + payload,
      repr(roundtrip(port, payload)[:16]))
payload = b"GET /api/devices HTTP/1.1\r\n\r\n"
check("GET 走管理接口", roundtrip(port, payload) == b"WEB:" + payload,
      repr(roundtrip(port, payload)[:16]))
payload = b"POST /api/login HTTP/1.1\r\n\r\n"
check("POST 走管理接口", roundtrip(port, payload) == b"WEB:" + payload,
      repr(roundtrip(port, payload)[:16]))

print(f"[2] 空闲 {IDLE_SECONDS:g}s 的 USB/IP 长会话不能被拆断")
with socket.create_connection(("127.0.0.1", port), timeout=30) as sock:
    first = exchange(sock, b"\x11hello")
    check("连接建立正常", first == b"USB:\x11hello", repr(first[:16]))
    time.sleep(IDLE_SECONDS)
    alive = True
    try:
        after = exchange(sock, b"ping")
    except OSError as exc:
        alive, after = False, exc
    check("静置后仍可收发(转发层没有残留超时)",
          alive and after == b"USB:ping",
          repr(after[:24]) if isinstance(after, bytes) else str(after))

for backend in (usbip_backend, web_backend):
    backend.shutdown()
gw.shutdown()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
