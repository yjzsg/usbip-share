"""访问控制的信任边界 + 授权表健壮性回归。

两条真问题（都是"测试没覆盖到的交互缺陷"，不是既有用例的重复）：

  [A] PROXY 行的信任边界
      web.py 原来只凭"对端是回环地址"就采信网关补的 `PROXY TCP4 <源IP> ...` 行。
      但回环对端并不等于网关：在 host 网络部署 / 非网关部署里，web.py 的端口本身
      就可能被直连，任何能连上它的进程都能自己写一行 PROXY 头，声称自己是**任意
      已授权的来源 IP**，从而绕过共享访问密码（`GET /api/devices` 恢复 200），
      还能冒用别人的 IP 去登记占用方、抢占排队名额。
      修复：网关在 PROXY 行尾附一个由 `USBIP_PROXY_SECRET` 派生的校验标记，
      web.py 只认带正确标记的行；标记不对/缺失时**一个字节都不消费**，按直连处理。

  [B] 授权表损坏时的静默降级
      原来 `_read_access_grants()` 把"读失败/解析失败"和"表是空的"当成同一件事。
      授权/续期/吊销都是读-改-写，于是**一次读失败就会把整张授权表重写成只剩自己**：
      一个客户端授权成功，其他人的授权被静默抹掉，网关随后把它们全部拒绝。
      修复：写路径用 `_read_access_grants_strict()`，文件存在但读不出来时抛 OSError
      → HTTP 500 且**不写盘**（fail-closed，绝不拿猜测的表覆盖真实数据）。

  [C] 网关的授权表缓存
      原来只比对 mtime；有些文件系统 mtime 粒度粗（或时钟回拨）会让原子替换后的
      新文件 mtime 与旧值相同，缓存就永远返回旧结论 —— 一次 revoke 被静默忽略。
      修复：同时比对 inode/size，并每秒无条件重读一次兜底。

  [D] 半截 PROXY 行
      对端发 6 个字节 `PROXY ` 就不再说话时，web.py 原来会占着线程一直等到 15s 的
      连接超时。修复：只给"看这行头"设一个短超时，拿到/放弃后立刻恢复。

    python tests/test_access_boundary.py
"""
import contextlib
import io
import json
import os
import socket
import socketserver
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gateway  # noqa: E402
import web  # noqa: E402

SECRET = "boundary-test-secret"
AUTHORIZED_IP = "10.0.0.5"
OTHER_IP = "10.0.0.6"

results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


def reset_guard():
    web._AUTH_FAILURE_TIMES.clear()
    web._AUTH_LOCK_UNTIL.clear()
    web._AUTH_STRIKES.clear()
    web._AUTH_GUARD_SEEN.clear()


def _try(fn):
    """跑一段代码，返回它抛出的异常（None = 没抛）。"""
    try:
        fn()
        return None
    except Exception as exc:  # noqa: BLE001 - 测试用
        return exc


root = Path(tempfile.mkdtemp())
web.AUTH_FILE = root / "auth.json"
web.METADATA_FILE = root / "device-metadata.json"
web.MANAGED_FILE = root / "managed-busids"
web.CLIENTS_FILE = root / "clients.json"
web.QUEUE_FILE = root / "queue.json"
web.NOTIFY_FILE = root / "queue-notify.json"
web.ACCESS_FILE = root / "authorized-clients.json"
os.environ["USBIP_ACCESS_PASSWORD"] = "共享密码-边界"
os.environ.pop("USBIP_WEB_TOKEN", None)
os.environ["USBIP_PROXY_SECRET"] = SECRET
os.environ["USBIP_PROXY_WAIT_SECONDS"] = "1"
web.Handler.PROXY_LINE_WAIT_SECONDS = 1.0
reset_guard()
web._ENV_PASSWORD_APPLIED = None

web.run_usbip = lambda *args: (
    (0, " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n")
    if args[:2] == ("list", "-l") else (0, "ok")
)
web.is_shared = lambda busid: True
web.load_auth()
web.sync_access_file()

httpd = ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
PORT = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
BASE = f"http://127.0.0.1:{PORT}"

# 只有 AUTHORIZED_IP 被授权。
web.access_authorize(AUTHORIZED_IP, "legit", "合法客户端")
TOKEN = web.Handler.proxy_token(SECRET)


def raw_http(payload: bytes) -> tuple[int, bytes]:
    """裸 socket 发一段字节（可能带伪造的 PROXY 行），返回 (状态码, 原始响应)。"""
    with socket.create_connection(("127.0.0.1", PORT), timeout=15) as sock:
        sock.sendall(payload)
        raw = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw += chunk
    try:
        status = int(raw.split(b" ")[1])
    except (IndexError, ValueError):
        status = 0
    return status, raw


def proxy_line(address: str, token: str = "") -> bytes:
    tail = f" {token}" if token else ""
    return f"PROXY TCP4 {address} 127.0.0.1 40000 {PORT}{tail}\r\n".encode("ascii")


def get_devices(prefix: bytes) -> tuple[int, bytes]:
    return raw_http(prefix + b"GET /api/devices HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")


def heartbeat(prefix: bytes, client_id: str = "attacker") -> tuple[int, bytes]:
    body = json.dumps(
        {"clientId": client_id, "clientName": "攻击者", "busids": ["1-1"], "dataPort": 5555}
    ).encode()
    head = (b"POST /api/clients/heartbeat HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
            b"Content-Type: application/json\r\nContent-Length: " + str(len(body)).encode()
            + b"\r\n\r\n")
    return raw_http(prefix + head + body)


def clients_file_text() -> str:
    try:
        return web.CLIENTS_FILE.read_text(encoding="utf-8")
    except OSError:
        return ""


# ---------------------------------------------------------------------------
print("[A] PROXY 行的信任边界：直连伪造不再有效")

status, _ = get_devices(b"")
check("直连不带 PROXY 头 → 401（未授权来源，基线）", status == 401, f"status={status}")

status, _ = get_devices(proxy_line(AUTHORIZED_IP))
check("直连 + 6 段旧格式 PROXY 头（无标记）声称已授权 IP → 401",
      status == 401, f"status={status}")

status, _ = get_devices(proxy_line(AUTHORIZED_IP, "deadbeefdeadbeef"))
check("直连 + 7 段但标记错误 → 401", status == 401, f"status={status}")

status, _ = get_devices(proxy_line(OTHER_IP, TOKEN))
check("标记正确但声称的 IP 未授权 → 401（标记只证明来源是网关，不证明 IP 授权）",
      status == 401, f"status={status}")

status, _ = get_devices(proxy_line(AUTHORIZED_IP, TOKEN))
check("标记正确 + 已授权 IP → 200（网关那条路径没被修坏）", status == 200, f"status={status}")

before = clients_file_text()
status, _ = heartbeat(proxy_line(AUTHORIZED_IP))
after = clients_file_text()
check("心跳：直连伪造 PROXY 头声称已授权 IP → 401",
      status == 401, f"status={status}")
check("心跳：伪造被拒后没有登记 attacker（未授权不产生任何写操作）",
      "attacker" not in after, after[:120])
check("心跳：clients.json 与伪造前一致", before == after)
status, _ = heartbeat(proxy_line(AUTHORIZED_IP, TOKEN), client_id="legit-client")
check("心跳：带正确标记 + 已授权 IP → 200（正常客户端不受影响）",
      status == 200, f"status={status}")
check("心跳：带标记的合法调用确实登记了", "legit-client" in clients_file_text())

# 未配置秘密时退回旧行为（只要求对端是回环），否则"只升级 .py 不重建镜像"会直接不可用。
os.environ.pop("USBIP_PROXY_SECRET", None)
try:
    status, _ = get_devices(proxy_line(AUTHORIZED_IP))
    check("未设 USBIP_PROXY_SECRET：6 段旧格式仍被接受（向后兼容，与旧版一致）",
          status == 200, f"status={status}")
finally:
    os.environ["USBIP_PROXY_SECRET"] = SECRET

# 非法/畸形的 PROXY 行必须落到普通 HTTP 解析，而不是被当成合法行。
for label, line in (
    ("段数不足（5 段）", b"PROXY TCP4 10.0.0.5 127.0.0.1 40000\r\n"),
    ("协议字段非法", b"PROXY TCP9 10.0.0.5 127.0.0.1 40000 8080 " + TOKEN.encode() + b"\r\n"),
    ("源地址不是 IP", b"PROXY TCP4 not-an-ip 127.0.0.1 40000 8080 " + TOKEN.encode() + b"\r\n"),
    ("首段不是 PROXY", b"PROXYX TCP4 10.0.0.5 127.0.0.1 40000 8080 " + TOKEN.encode() + b"\r\n"),
):
    status, raw = get_devices(line)
    # 关键是不被当成"合法 PROXY 行"采信：要么 401（按直连处理），要么整行被
    # 当成畸形的 HTTP 请求行（400/501，连接可能直接断掉导致 status=0）。
    # 绝不能出现 200 或任何设备数据。
    check(f"畸形 PROXY 行（{label}）不被采信（401/400/无响应，绝无设备数据）",
          status in (0, 400, 401, 501) and b"devices" not in raw, f"status={status}")

print("[A2] 半截 PROXY 行不能占着连接等到 15s")
t0 = time.time()
with socket.create_connection(("127.0.0.1", PORT), timeout=20) as sock:
    sock.sendall(b"PROXY ")
    sock.settimeout(20)
    try:
        sock.recv(64)
    except OSError:
        pass
elapsed = time.time() - t0
check(f"只发 6 字节后静默：连接在 {elapsed:.1f}s 内结束（< 5s，而不是 15s）",
      elapsed < 5.0, f"elapsed={elapsed:.1f}s")

print("[A3] 网关端到端：真实网关生成的 PROXY 行必须被 web.py 认下来")


class EchoBackend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, tag: bytes):
        self.tag = tag
        super().__init__(("127.0.0.1", 0), EchoHandler)


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            try:
                data = self.request.recv(4096)
            except OSError:
                return
            if not data:
                return
            try:
                self.request.sendall(self.server.tag + data)
            except OSError:
                return


usbip_backend = EchoBackend(b"USB:")
threading.Thread(target=usbip_backend.serve_forever, daemon=True).start()

real_gw = gateway.Gateway(("127.0.0.1", 0), "127.0.0.1",
                          usbip_backend.server_address[1], PORT,
                          str(web.ACCESS_FILE), proxy_secret_value=SECRET)
threading.Thread(target=real_gw.serve_forever, daemon=True).start()


def gateway_get(path: str):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{real_gw.server_address[1]}{path}", timeout=10) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())


# 经网关的**匿名**调用：web.py 只看到网关（127.0.0.1），而授权表里是 10.0.0.5，
# 所以来源 IP 就是"网关自己"→ 未授权 → 401。证明整条链路仍然受访问控制约束。
status, body = gateway_get("/api/devices")
check("经真实网关的匿名调用：未授权来源 → 401 accessRequired",
      status == 401 and body.get("accessRequired") is True, f"status={status} body={body}")

# 真实网关补出来的那行 PROXY 必须能被 web.py 认下来（两边派生出的标记一致）。
# 断言方式是让网关"声称"一个**已授权**的地址：只有标记被采信时才会按这个地址
# 判定并放行；标记若被拒，web.py 会退回 socket 对端 127.0.0.1（未授权）→ 401。
web.access_authorize("10.0.0.7", "claimed", "被声称的地址")
original_proxy_header = gateway.proxy_header


def forged_proxy_header(address, source_port, local, secret="", _claim="10.0.0.7"):
    return original_proxy_header(_claim, source_port, local, secret)


gateway.proxy_header = forged_proxy_header
try:
    check("秘密一致：网关声称的已授权地址被采信 → 200（端到端链路正常）",
          gateway_get("/api/devices")[0] == 200)

    mismatched = gateway.Gateway(("127.0.0.1", 0), "127.0.0.1",
                                 usbip_backend.server_address[1], PORT,
                                 str(web.ACCESS_FILE), proxy_secret_value="different-secret")
    threading.Thread(target=mismatched.serve_forever, daemon=True).start()
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{mismatched.server_address[1]}/api/devices", timeout=10) as r:
            status = r.status
    except urllib.error.HTTPError as exc:
        status = exc.code
    check("秘密不一致：同一行 PROXY 被拒、退回未授权的对端 → 401（不静默放行）",
          status == 401, f"status={status}")
    mismatched.shutdown()
finally:
    gateway.proxy_header = original_proxy_header
web.access_revoke("10.0.0.7")

print("[B] 授权表损坏：写路径必须 fail-closed，绝不拿空表覆盖")

good_table = web.ACCESS_FILE.read_bytes()
web.ACCESS_FILE.write_text("{ 这不是 JSON", encoding="utf-8")
corrupt_bytes = web.ACCESS_FILE.read_bytes()

status, body = raw_http(
    proxy_line("10.0.0.9", TOKEN)
    + b"POST /api/access/authorize HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
      b"Content-Type: application/json\r\nContent-Length: "
    + str(len(json.dumps({"password": "共享密码-边界"}).encode())).encode()
    + b"\r\n\r\n" + json.dumps({"password": "共享密码-边界"}).encode()
)
check("授权表损坏时 authorize → 500（不是 200 后静默重写）",
      status == 500, f"status={status}")
check("授权表损坏时 authorize 没有改动文件（没有把表抹成只剩新条目）",
      web.ACCESS_FILE.read_bytes() == corrupt_bytes, web.ACCESS_FILE.read_text(encoding="utf-8")[:80])

check("读路径仍然把损坏表当空（access_grants 不抛异常）",
      web.access_grants() == {} and web.access_grant(AUTHORIZED_IP) is None)
check("access_revoke 在损坏表上抛 OSError（由调用方转 500）",
      isinstance(_try(lambda: web.access_revoke(None)), OSError))
check("access_authorize 在损坏表上抛 OSError",
      isinstance(_try(lambda: web.access_authorize("10.0.0.9", "x", "x")), OSError))
check("access_renew 在损坏表上抛 OSError",
      isinstance(_try(lambda: web.access_renew(AUTHORIZED_IP)), OSError))

web.ACCESS_FILE.write_bytes(good_table)
check("恢复合法授权表后读路径恢复正常", web.access_grant(AUTHORIZED_IP) is not None)

print("[B2] 空文件 / 缺 clients 字段：空文件按空表，类型错误则 fail-closed")
web.ACCESS_FILE.write_text("", encoding="utf-8")
check("空文件 → 空表（可以正常授权，不会 500）",
      web.access_authorize("10.0.0.9", "x", "x")["address"] == "10.0.0.9")
web.ACCESS_FILE.write_text('{"version": 1, "clients": "nope"}', encoding="utf-8")
check("clients 字段类型错误 → 抛 OSError（不静默重写）",
      isinstance(_try(lambda: web.access_authorize("10.0.0.8", "x", "x")), OSError))
web.ACCESS_FILE.write_bytes(good_table)

print("[C] 网关授权表缓存：mtime 不变也必须能察觉内容变化")

gate_root = Path(tempfile.mkdtemp())
gate_path = gate_root / "authorized-clients.json"
gate_path.write_text(json.dumps({"version": 1, "clients": {"1.2.3.4": {"expiresAt": time.time() + 600}}}),
                     encoding="utf-8")
gate = gateway.AccessGate(str(gate_path))
check("初始：表里的 IP 放行", gate.allows("1.2.3.4") is True)

# 模拟"原子替换后 mtime 没变"：把 mtime 钉死成同一个值，同时改内容。
pinned = gate_path.stat().st_mtime_ns
gate_path.write_text(json.dumps({"version": 1, "clients": {}}), encoding="utf-8")
os.utime(gate_path, ns=(pinned, pinned))
gateway.ACCESS_RELOAD_INTERVAL_SECONDS = 0.0
check("mtime 被钉死、内容已变 → 仍然察觉到 revoke（不再静默忽略）",
      gate.allows("1.2.3.4") is False, f"reloads={gate.reloads}")
gateway.ACCESS_RELOAD_INTERVAL_SECONDS = 1.0

# 每秒兜底：即使 stat 字段完全没变，也会定期重读。
pinned = gate_path.stat().st_mtime_ns
gate._refresh()
before_reloads = gate.reloads
gate_path.write_text(json.dumps({"version": 1, "clients": {"9.9.9.9": {"expiresAt": time.time() + 600}}}),
                     encoding="utf-8")
os.utime(gate_path, ns=(pinned, pinned))
gate._loaded_at -= 5.0  # 装作已经过了一个刷新周期
check("stat 字段全都没变时，周期兜底也会重读（9.9.9.9 生效）",
      gate.allows("9.9.9.9") is True and gate.reloads > before_reloads,
      f"reloads={gate.reloads}")

gate_path.write_text("{ 坏掉的 JSON", encoding="utf-8")
check("授权表损坏 → 网关拒绝一切（fail-closed）", gate.allows("9.9.9.9") is False)
gate_path.unlink()
check("文件消失 → 回到放行一切（关闭访问控制）",
      gate.allows("203.0.113.7") is True and gate.enabled() is False)

print("[D] 访问控制开启时，watchdog 的强制释放不经过 HTTP 门（内部调用）")
os.environ["USBIP_ACCESS_PASSWORD"] = "共享密码-边界"
web.sync_access_file()
check("访问控制确实开着", web.access_required() is True)
devices, error = web.list_devices()
check("内部 list_devices() 不受访问控制门影响", error is None and len(devices) == 1, f"error={error}")
check("匿名且未授权的来源仍被 HTTP 门挡住（门没有被整体关掉）",
      web.access_granted("127.0.0.1") is False)

# watchdog 的强制释放路径：force_release_allowed + kick_device 都是内部函数调用，
# 不经过 HTTP，因此不该受"来源 IP 未授权"影响。
shared_state = {"1-1"}


def fake_run_usbip_kick(*args):
    if args[:2] == ("list", "-l"):
        return 0, " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n"
    if args[:2] == ("unbind", "-b"):
        shared_state.discard(args[2])
        return 0, ""
    if args[:2] == ("bind", "-b"):
        shared_state.add(args[2])
        return 0, ""
    return 0, ""


web.run_usbip = fake_run_usbip_kick
web.is_shared = lambda busid: busid in shared_state
web.update_client({"clientId": "stale-peer", "clientName": "掉线客户端",
                   "busids": ["1-1"], "dataPort": 5555}, "10.0.0.5")
allowed, reason = web.force_release_allowed("1-1", "10.0.0.5")
check("force_release_allowed：持有者地址与该陈旧记录一致 → 允许释放",
      allowed is True, reason)
ok, message = web.kick_device("1-1")
check("kick_device 在访问控制开启时仍然工作（内部调用不经过访问门）",
      ok is True and "1-1" in shared_state, f"ok={ok} message={message}")

web.run_usbip = lambda *args: (
    (0, " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n")
    if args[:2] == ("list", "-l") else (0, "ok")
)
web.is_shared = lambda busid: True

real_gw.shutdown()
usbip_backend.shutdown()
httpd.shutdown()
httpd.server_close()
os.environ.pop("USBIP_ACCESS_PASSWORD", None)

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
