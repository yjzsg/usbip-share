"""共享访问密码（访问控制）回归：API 门 + 授权表 + 网关判定 + 端到端。

USB/IP 协议没有认证字段，所以这个功能是「访问密码 + 源 IP 授权窗口」：
客户端先用密码调 POST /api/access/authorize 换取自己**来源 IP** 的授权，
之后它发起的 USB/IP 数据连接才会被 gateway.py 放行。

覆盖：
  [A] 未设 USBIP_ACCESS_PASSWORD：向后兼容锚点 —— /api/access 返回 required=false，
      匿名 GET /api/devices 与心跳行为与现在完全一致，且不产生授权表文件
  [B] 设了密码：未授权来源 IP 的 GET /api/devices / 心跳 → 401 + accessRequired，
      心跳不做任何写操作；带管理员令牌的管理页不受影响
  [C] POST /api/access/authorize：密码错 → 403 且不入表；连续错 → 429（复用登录限速）
  [D] 密码对 → 200，授权表落盘且结构正确
  [E] 授权后：心跳与设备列表恢复，心跳续期（到期时间往后推）
  [F] 过期后重新变回 401
  [G] GET /api/access/clients 与 POST /api/access/clients/revoke：单个 / 全部吊销
  [H] 关闭访问控制：授权表文件被删除，一切恢复放行
  [I] 网关判定：放行 / 拒绝 / 无文件放行 / 过期拒绝 / mtime 缓存 / 拒绝日志节流
  [J] 网关端到端：未授权来源的 USB/IP 连接不被转发；授权后才转发；
      访问控制开启时 HTTP 连接带上 PROXY 头（关闭时不带，默认路径逐字节不变）

    python tests/test_access_gate.py
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

TOKEN = "access-gate-admin-token"
CLIENT_A = "203.0.113.7"
CLIENT_B = "203.0.113.8"
# 网关与 web.py 共享的 PROXY 校验秘密:web.py 只认带正确标记的 PROXY 行
# (信任边界见 tests/test_access_boundary.py)。本用例里 gateway_call() 模拟网关,
# 所以它必须像真网关一样把标记附上。
PROXY_SECRET = "access-gate-proxy-secret"

results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


def reset_guard():
    web._AUTH_FAILURE_TIMES.clear()
    web._AUTH_LOCK_UNTIL.clear()
    web._AUTH_STRIKES.clear()
    web._AUTH_GUARD_SEEN.clear()


def use_config(root: Path):
    web.AUTH_FILE = root / "auth.json"
    web.METADATA_FILE = root / "device-metadata.json"
    web.MANAGED_FILE = root / "managed-busids"
    web.CLIENTS_FILE = root / "clients.json"
    web.QUEUE_FILE = root / "queue.json"
    web.NOTIFY_FILE = root / "queue-notify.json"
    web.ACCESS_FILE = root / "authorized-clients.json"
    web.SESSIONS.clear()
    reset_guard()
    web._ENV_PASSWORD_APPLIED = None
    os.environ["USBIP_WEB_TOKEN"] = TOKEN
    os.environ["USBIP_PROXY_SECRET"] = PROXY_SECRET


def set_access_env(password: str, ttl: str | None = None):
    if password:
        os.environ["USBIP_ACCESS_PASSWORD"] = password
    else:
        os.environ.pop("USBIP_ACCESS_PASSWORD", None)
    if ttl is None:
        os.environ.pop("USBIP_ACCESS_TTL_SECONDS", None)
    else:
        os.environ["USBIP_ACCESS_TTL_SECONDS"] = ttl


def read_grants_file(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


# ---------------------------------------------------------------------------
# 真实 HTTP：直连（无 PROXY 头，等价于"管理页/健康检查"那条路径）与
# 经网关（带 PROXY 头，等价于 Windows 客户端的真实路径）。
httpd = ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
PORT = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
BASE = f"http://127.0.0.1:{PORT}"


def direct_call(path, payload=None, token=None, method=None):
    """不带头部直连 web.py —— 对端就是 127.0.0.1（旧行为）。"""
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(BASE + path, data=data, method=method or ("POST" if data else "GET"))
    if data:
        request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("X-Admin-Token", token)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, json.loads(response.read().decode()), dict(response.headers)
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode()), dict(exc.headers)


def gateway_call(peer, path, payload=None, token=None, method=None):
    """用带校验标记的 PROXY v1 头模拟任意来源 IP 经单端口网关进来的请求。

    标记是必须的:web.py 只在 PROXY 行带着由 ``USBIP_PROXY_SECRET`` 派生的标记时
    才采信这行头(否则谁都能直连 web 端口冒充授权来源)。真网关会自己补上它。
    """
    body = json.dumps(payload).encode("utf-8") if payload is not None else b""
    method = method or ("POST" if body else "GET")
    head = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1", "Connection: close"]
    if body:
        head.append("Content-Type: application/json")
    if token:
        head.append(f"X-Admin-Token: {token}")
    head.append(f"Content-Length: {len(body)}")
    request = ("\r\n".join(head) + "\r\n\r\n").encode("utf-8") + body
    prefix = f"PROXY TCP4 {peer} 127.0.0.1 40000 {PORT} {web.Handler.proxy_token(PROXY_SECRET)}\r\n".encode("ascii")
    with socket.create_connection(("127.0.0.1", PORT), timeout=10) as sock:
        sock.sendall(prefix + request)
        raw = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw += chunk
    head, _, payload_raw = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name:
            headers[name.strip()] = value.strip()
    try:
        return status, json.loads(payload_raw.decode("utf-8")), headers
    except ValueError:
        return status, {}, headers


# 设备层打桩：本测试只关心访问控制这道门，不碰真实 usbip。
old_run_usbip = web.run_usbip
old_is_shared = web.is_shared
web.run_usbip = lambda *args: (0, " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n") if args[:2] == ("list", "-l") else (0, "ok")
web.is_shared = lambda busid: True

root = Path(tempfile.mkdtemp())
use_config(root)
web.load_auth()

HEARTBEAT = {"clientId": "c-a", "clientName": "前台电脑", "busids": [], "dataPort": 5555}

# ---------------------------------------------------------------------------
print("[A] 未设 USBIP_ACCESS_PASSWORD：向后兼容锚点")
set_access_env("")
web.sync_access_file()
check("access_required() = False", web.access_required() is False)
check("授权表文件不存在", not web.ACCESS_FILE.exists(), str(web.ACCESS_FILE))

st, body, _ = direct_call("/api/access")
check("直连 GET /api/access → 200 required=false",
      st == 200 and body.get("ok") is True and body.get("required") is False, str(body))
st, body, _ = gateway_call(CLIENT_A, "/api/access")
check("任意来源 IP 的 GET /api/access 都免鉴权、无副作用",
      st == 200 and body.get("required") is False and not web.ACCESS_FILE.exists(), str(body))

st, body, _ = direct_call("/api/devices")
check("匿名 GET /api/devices 行为不变（200 + 设备列表 + usbipPort）",
      st == 200 and body.get("ok") is True and len(body.get("devices", [])) == 1
      and body.get("usbipPort") == 5555 and "accessRequired" not in body, f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/devices")
check("任意来源 IP 的匿名 GET /api/devices 同样 200（没有密码就没有门）",
      st == 200 and body.get("ok") is True, f"status={st} body={body}")

st, body, _ = direct_call("/api/clients/heartbeat", HEARTBEAT)
check("匿名心跳行为不变（200 + 登记成功）",
      st == 200 and body.get("ok") is True and body.get("client", {}).get("clientId") == "c-a", f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-anon"})
check("经网关的匿名心跳同样 200，且 address 记录为真实来源 IP",
      st == 200 and body.get("client", {}).get("address") == CLIENT_A, f"status={st} body={body}")

st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": "whatever"})
check("未启用访问控制时 authorize 幂等成功（客户端可直接放行）",
      st == 200 and body.get("authorized") is True and body.get("required") is False, str(body))
check("幂等 authorize 不会写出授权表文件", not web.ACCESS_FILE.exists())

st, body, _ = gateway_call(CLIENT_A, "/api/access/clients")
check("管理端点仍需要管理员令牌（401）", st == 401, f"status={st}")

# ---------------------------------------------------------------------------
print("[B] 设了密码：未授权来源 IP 被挡住，管理页不受影响")
ACCESS_PASSWORD = "共享密码-1"
set_access_env(ACCESS_PASSWORD)
web.sync_access_file()
check("授权表文件已落盘（空表 = 先全拒）",
      web.ACCESS_FILE.exists() and read_grants_file(web.ACCESS_FILE).get("clients") == {}, "")

st, body, _ = gateway_call(CLIENT_A, "/api/access")
check("GET /api/access → required=true", st == 200 and body.get("required") is True, str(body))

st, body, _ = gateway_call(CLIENT_A, "/api/devices")
check("未授权 IP 的 GET /api/devices → 401 accessRequired",
      st == 401 and body.get("ok") is False and body.get("accessRequired") is True
      and body.get("error") == "需要访问密码", f"status={st} body={body}")

before_clients = read_grants_file(web.CLIENTS_FILE)
st, body, _ = gateway_call(CLIENT_A, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-blocked"})
check("未授权 IP 的心跳 → 401 accessRequired",
      st == 401 and body.get("accessRequired") is True, f"status={st} body={body}")
after_clients = read_grants_file(web.CLIENTS_FILE)
check("未授权心跳不做任何写操作（clients.json 无变化）",
      json.dumps(before_clients, sort_keys=True) == json.dumps(after_clients, sort_keys=True),
      f"before={bool(before_clients)} after={bool(after_clients)}")
check("未授权心跳没有登记该 clientId",
      "c-blocked" not in json.dumps(after_clients, ensure_ascii=False))

st, body, _ = gateway_call(CLIENT_A, "/api/devices", token=TOKEN)
check("带管理员令牌的 GET /api/devices → 200（管理页不受影响）",
      st == 200 and body.get("ok") is True and len(body.get("devices", [])) == 1, f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/devices?includeHidden=1", token=TOKEN)
check("带令牌 + includeHidden=1 仍正常", st == 200 and body.get("ok") is True, f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/session")
check("GET /api/session 行为不变", st == 200 and body.get("ok") is True, f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/health")
check("GET /api/health 行为不变", st == 200 and body.get("ok") is True, f"status={st}")

# ---------------------------------------------------------------------------
print("[C] POST /api/access/authorize：密码错 → 403 且不入表；连续错 → 429")
reset_guard()
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": "wrong-1", "clientId": "c-a"})
check("密码错 → 403 + 访问密码不正确",
      st == 403 and body.get("ok") is False and body.get("error") == "访问密码不正确", f"status={st} body={body}")
check("密码错不会把 IP 加进授权表",
      read_grants_file(web.ACCESS_FILE).get("clients") == {}, str(read_grants_file(web.ACCESS_FILE)))
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": ""})
check("空密码 → 400（不消耗限速次数）",
      st == 400 and body.get("error") == "请输入访问密码", f"status={st} body={body}")

for attempt in (2, 3, 4):
    st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": f"wrong-{attempt}"})
    check(f"第 {attempt} 次错误密码仍是 403", st == 403, f"status={st}")
st, body, headers = gateway_call(CLIENT_A, "/api/access/authorize", {"password": "wrong-5"})
retry_after = int(headers.get("Retry-After", "0") or 0)
check("第 5 次失败 → 429（复用登录限速）", st == 429, f"status={st} body={body}")
check("429 带 Retry-After 且不超过 15 分钟",
      web.AUTH_GUARD_BASE_LOCK_SECONDS <= retry_after <= web.AUTH_GUARD_MAX_LOCK_SECONDS,
      f"Retry-After={retry_after}")
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": ACCESS_PASSWORD})
check("锁定期间正确密码同样被拒（429）", st == 429, f"status={st} body={body}")
check("限速按来源 IP 记账，别的 IP 不受影响",
      web.auth_retry_after(CLIENT_B) == 0 and web.auth_retry_after(CLIENT_A) > 0)

# ---------------------------------------------------------------------------
print("[D] 密码正确 → 200 + 授权表落盘")
reset_guard()
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize",
                           {"password": ACCESS_PASSWORD, "clientId": "c-a", "clientName": "前台电脑"})
check("authorize → 200 authorized/required/expiresInSeconds",
      st == 200 and body.get("ok") is True and body.get("authorized") is True
      and body.get("required") is True and body.get("expiresInSeconds") == 43200
      and bool(body.get("message")), f"status={st} body={body}")

payload = read_grants_file(web.ACCESS_FILE)
check("授权表文件结构正确（version/clients）",
      payload.get("version") == 1 and isinstance(payload.get("clients"), dict)
      and CLIENT_A in payload.get("clients", {}), json.dumps(payload, ensure_ascii=False)[:200])
record = payload.get("clients", {}).get(CLIENT_A, {})
check("记录里有 clientId/clientName/authorizedAt/expiresAt",
      record.get("clientId") == "c-a" and record.get("clientName") == "前台电脑"
      and isinstance(record.get("authorizedAt"), int) and isinstance(record.get("expiresAt"), int)
      and record.get("expiresAt") - record.get("authorizedAt") == 43200, str(record))
check("授权表是原子写出来的合法 JSON（无 .tmp 残留）",
      not list(root.glob("*.tmp")) and not list(root.glob(".*.tmp")), str(list(root.iterdir())))

st, body, _ = gateway_call(CLIENT_A, "/api/access/clients", token=TOKEN)
entry = (body.get("clients") or [{}])[0]
check("管理接口列出该 IP（address/clientId/expiresInSeconds）",
      st == 200 and body.get("required") is True and entry.get("address") == CLIENT_A
      and entry.get("clientId") == "c-a" and 0 < entry.get("expiresInSeconds", 0) <= 43200,
      f"status={st} body={body}")

# ---------------------------------------------------------------------------
print("[E] 授权后：心跳与设备列表恢复，且心跳续期")
st, body, _ = gateway_call(CLIENT_A, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-a"})
check("授权后心跳恢复正常（200 + ok）",
      st == 200 and body.get("ok") is True, f"status={st} body={body}")
st, body, _ = gateway_call(CLIENT_A, "/api/devices")
check("授权后 GET /api/devices 恢复（200）", st == 200 and body.get("ok") is True, f"status={st}")

first_expiry = read_grants_file(web.ACCESS_FILE)["clients"][CLIENT_A]["expiresAt"]
time.sleep(1.2)
st, body, _ = gateway_call(CLIENT_A, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-a"})
second_expiry = read_grants_file(web.ACCESS_FILE)["clients"][CLIENT_A]["expiresAt"]
check("心跳会把到期时间往后推（续期）",
      second_expiry > first_expiry, f"{first_expiry} -> {second_expiry}")
check("续期后的到期时间 = 现在 + TTL",
      abs(second_expiry - (int(time.time()) + 43200)) <= 2, f"expiresAt={second_expiry}")

set_access_env(ACCESS_PASSWORD, ttl="120")
check("TTL 可用 USBIP_ACCESS_TTL_SECONDS 覆盖", web.access_ttl_seconds() == 120)
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": ACCESS_PASSWORD})
check("authorize 响应里的 expiresInSeconds 跟随 TTL",
      st == 200 and body.get("expiresInSeconds") == 120, str(body))
set_access_env(ACCESS_PASSWORD)
check("TTL 非法值回落到默认 43200", web.access_ttl_seconds() == 43200)

# ---------------------------------------------------------------------------
print("[F] 过期后重新变回 401")
grants = read_grants_file(web.ACCESS_FILE)
grants["clients"][CLIENT_A]["expiresAt"] = int(time.time()) - 5
web.ACCESS_FILE.write_text(json.dumps(grants, ensure_ascii=False), encoding="utf-8")
st, body, _ = gateway_call(CLIENT_A, "/api/devices")
check("过期后 GET /api/devices → 401 accessRequired",
      st == 401 and body.get("accessRequired") is True, f"status={st} body={body}")
st, body, _ = gateway_call(CLIENT_A, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-a"})
check("过期后心跳 → 401（不会自我续期）", st == 401, f"status={st} body={body}")
st, body, _ = gateway_call(CLIENT_A, "/api/access/clients", token=TOKEN)
check("过期条目不再出现在管理列表里", st == 200 and body.get("clients") == [], str(body))
check("过期后授权表文件仍然存在（访问控制没被关掉）", web.ACCESS_FILE.exists())

# ---------------------------------------------------------------------------
print("[G] revoke：单个 / 全部")
for peer, cid in ((CLIENT_A, "c-a"), (CLIENT_B, "c-b")):
    gateway_call(peer, "/api/access/authorize", {"password": ACCESS_PASSWORD, "clientId": cid, "clientName": cid})
check("两个来源 IP 都已授权",
      sorted(read_grants_file(web.ACCESS_FILE)["clients"]) == sorted([CLIENT_A, CLIENT_B]))

st, body, _ = gateway_call(CLIENT_A, "/api/access/clients/revoke", {"address": CLIENT_B}, token=TOKEN)
check("吊销单个 → revoked=1", st == 200 and body.get("revoked") == 1, f"status={st} body={body}")
check("被吊销的 IP 立即变回 401", gateway_call(CLIENT_B, "/api/devices")[0] == 401)
check("未受影响的 IP 仍然 200", gateway_call(CLIENT_A, "/api/devices")[0] == 200)

st, body, _ = gateway_call(CLIENT_A, "/api/access/clients/revoke", {}, token=TOKEN)
check("不带 address → 吊销全部（revoked=1）",
      st == 200 and body.get("revoked") == 1 and body.get("clients") == [], f"status={st} body={body}")
check("全部吊销后所有 IP 都是 401",
      gateway_call(CLIENT_A, "/api/devices")[0] == 401 and gateway_call(CLIENT_B, "/api/devices")[0] == 401)
check("全部吊销后授权表文件仍存在且为空（网关必须继续拦）",
      web.ACCESS_FILE.exists() and read_grants_file(web.ACCESS_FILE).get("clients") == {})

check("revoke 不带管理员令牌 → 401",
      gateway_call(CLIENT_A, "/api/access/clients/revoke", {"address": CLIENT_A})[0] == 401)
check("revoke 不存在的地址 → 200 且 revoked=0",
      gateway_call(CLIENT_A, "/api/access/clients/revoke", {"address": "198.51.100.9"}, token=TOKEN)[1].get("revoked") == 0)

old_pending = web.Handler.must_change_pending
web.Handler.must_change_pending = lambda self: True
st, body, _ = gateway_call(CLIENT_A, "/api/access/clients", token=TOKEN)
check("mustChange 未改密时 GET /api/access/clients → 403",
      st == 403 and body.get("error") == "must_change_password", f"status={st} body={body}")
st, body, _ = gateway_call(CLIENT_A, "/api/access/clients/revoke", {}, token=TOKEN)
check("mustChange 未改密时 revoke → 403",
      st == 403 and body.get("error") == "must_change_password", f"status={st} body={body}")
web.Handler.must_change_pending = old_pending

# ---------------------------------------------------------------------------
print("[H] 关闭访问控制：授权表被删除，一切恢复放行")
gateway_call(CLIENT_A, "/api/access/authorize", {"password": ACCESS_PASSWORD, "clientId": "c-a"})
check("关闭前：授权表里有条目", CLIENT_A in read_grants_file(web.ACCESS_FILE).get("clients", {}))
set_access_env("")
web.sync_access_file()
check("清空密码后授权表文件被删除", not web.ACCESS_FILE.exists())
st, body, _ = gateway_call(CLIENT_A, "/api/devices")
check("任意来源 IP 的 GET /api/devices 恢复 200",
      st == 200 and body.get("ok") is True, f"status={st}")
st, body, _ = gateway_call(CLIENT_B, "/api/clients/heartbeat", {**HEARTBEAT, "clientId": "c-open"})
check("任意来源 IP 的心跳恢复 200", st == 200 and body.get("ok") is True, f"status={st}")
st, body, _ = gateway_call(CLIENT_A, "/api/access")
check("GET /api/access 回到 required=false", st == 200 and body.get("required") is False, str(body))
st, body, _ = gateway_call(CLIENT_A, "/api/access/authorize", {"password": "x"})
check("关闭后 authorize 幂等成功且不重建文件",
      st == 200 and body.get("required") is False and not web.ACCESS_FILE.exists(), str(body))

# ---------------------------------------------------------------------------
print("[I] 网关判定函数：放行 / 拒绝 / 无文件放行 / 过期拒绝 / 缓存与重读")
gate_root = Path(tempfile.mkdtemp())
gate_path = gate_root / "authorized-clients.json"
now = time.time()


def write_gate_table(entries: dict, bump_mtime: bool = True):
    payload = {"version": 1, "clients": {addr: {"expiresAt": exp} for addr, exp in entries.items()}}
    if bump_mtime and gate_path.exists():
        previous = gate_path.stat().st_mtime_ns
        gate_path.write_text(json.dumps(payload), encoding="utf-8")
        os.utime(gate_path, ns=(previous + 5_000_000_000, previous + 5_000_000_000))
    else:
        gate_path.write_text(json.dumps(payload), encoding="utf-8")


gate = gateway.AccessGate(str(gate_path))
check("文件不存在 → 放行一切（访问控制关闭）",
      gate.allows("203.0.113.7") is True and gate.enabled() is False)

write_gate_table({"1.2.3.4": now + 600})
check("表里没有的 IP → 拒绝", gate.allows("203.0.113.7") is False)
check("表里的 IP → 放行", gate.allows("1.2.3.4") is True)
check("已过期的条目 → 拒绝", gate.allows("1.2.3.4", now=now + 900) is False)
write_gate_table({})
check("空表（访问控制已开启但没人授权）→ 全部拒绝，不是放行",
      gate.allows("1.2.3.4") is False and gate.enabled() is True)

write_gate_table({"1.2.3.4": now + 600})
check("重新加载后回到放行", gate.allows("1.2.3.4") is True)
# 缓存：mtime 未变 → 不重新读盘，旧结论仍然生效（reloads 不增长）。
reloads_before = gate.reloads
pinned = gate_path.stat().st_mtime_ns
gate_path.write_text(json.dumps({"version": 1, "clients": {"9.9.9.9": {"expiresAt": now + 600}}}),
                     encoding="utf-8")
os.utime(gate_path, ns=(pinned, pinned))
check("mtime 未变 → 命中缓存，旧结论仍然生效（reloads 不增长）",
      gate.allows("1.2.3.4") is True and gate.allows("9.9.9.9") is False
      and gate.reloads == reloads_before, f"reloads={gate.reloads}")
# 变更检测：mtime 变了（原子替换后正常就是这样）→ 立刻重读。
os.utime(gate_path, ns=(pinned + 5_000_000_000, pinned + 5_000_000_000))
check("mtime 变化 → 重新加载（reloads +1）",
      gate.allows("9.9.9.9") is True and gate.allows("1.2.3.4") is False
      and gate.reloads == reloads_before + 1, f"reloads={gate.reloads}")
# 兜底：即使 stat 字段全都没变（有些文件系统 mtime 粒度粗 / 时钟回拨），也必须
# 在一个刷新周期后读到新内容 —— 否则一次 revoke 会被静默忽略（fail-open）。
pinned = gate_path.stat().st_mtime_ns
gate_path.write_text(json.dumps({"version": 1, "clients": {"7.7.7.7": {"expiresAt": now + 600}}}),
                     encoding="utf-8")
os.utime(gate_path, ns=(pinned, pinned))
gate._loaded_at -= gateway.ACCESS_RELOAD_INTERVAL_SECONDS + 1
check("stat 字段全未变时，周期兜底仍会重读（revoke 不会被静默忽略）",
      gate.allows("7.7.7.7") is True and gate.allows("9.9.9.9") is False,
      f"reloads={gate.reloads}")
gate_path.unlink()
check("文件被删除 → 恢复放行一切（关闭访问控制）",
      gate.allows("203.0.113.7") is True and gate.enabled() is False)
gate_path.write_text("{ this is not json", encoding="utf-8")
check("文件损坏 → 拒绝一切（文件存在就是启用，宁可拒绝）",
      gate.allows("203.0.113.7") is False)

with contextlib.redirect_stdout(io.StringIO()):
    proxy_v4 = gateway.proxy_header("192.168.1.21", 54321, ("192.168.1.10", 5555))
    proxy_v6 = gateway.proxy_header("fd00::1", 1234, ("fd00::2", 5555))
    proxy_signed = gateway.proxy_header("192.168.1.21", 54321, ("192.168.1.10", 5555), PROXY_SECRET)
    gateway._DENY_LOGGED.clear()
    first_deny = gateway.log_denied("203.0.113.9")
    second_deny = gateway.log_denied("203.0.113.9")
    other_deny = gateway.log_denied("203.0.113.10")
check("PROXY 头（IPv4）格式正确",
      proxy_v4 == b"PROXY TCP4 192.168.1.21 192.168.1.10 54321 5555\r\n", repr(proxy_v4))
check("PROXY 头（IPv6）用 TCP6",
      proxy_v6 == b"PROXY TCP6 fd00::1 fd00::2 1234 5555\r\n", repr(proxy_v6))
check("PROXY 头带秘密时附上校验标记（与 web.py 派生的一致）",
      proxy_signed == (
          b"PROXY TCP4 192.168.1.21 192.168.1.10 54321 5555 "
          + web.Handler.proxy_token(PROXY_SECRET).encode("ascii") + b"\r\n"),
      repr(proxy_signed))
check("秘密为空时退回 6 段旧格式（向后兼容）",
      gateway.proxy_token("") == "" and gateway.proxy_header("1.1.1.1", 1, ("2.2.2.2", 2), "").count(b" ") == 5)
check("拒绝日志：首次打印", first_deny is True)
check("拒绝日志：同一 IP 节流（不再刷屏）", second_deny is False)
check("拒绝日志：不同 IP 各打一次", other_deny is True)

# ---------------------------------------------------------------------------
print("[J] 网关端到端：未授权来源的 USB/IP 连接不被转发")


class EchoBackend(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, tag: bytes):
        self.tag = tag
        self.hits = 0
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


usbip_backend = EchoBackend(b"")
web_backend = EchoBackend(b"WEB:")
for backend in (usbip_backend, web_backend):
    threading.Thread(target=backend.serve_forever, daemon=True).start()

e2e_root = Path(tempfile.mkdtemp())
e2e_file = e2e_root / "authorized-clients.json"
gw = gateway.Gateway(("127.0.0.1", 0), "127.0.0.1",
                     usbip_backend.server_address[1], web_backend.server_address[1],
                     str(e2e_file))
threading.Thread(target=gw.serve_forever, daemon=True).start()
gw_port = gw.server_address[1]


def usbip_roundtrip(payload: bytes = b"\x11hello", settle: float = 2.0) -> tuple[bool, bytes]:
    """返回 (是否被放行, 收到的字节)。连接被立即关闭也算"拒绝"。

    网关会先补回首字节再转发剩余部分,所以回声可能分成几段,这里攒够再判断。
    """
    with socket.create_connection(("127.0.0.1", gw_port), timeout=10) as sock:
        sock.sendall(payload)
        sock.settimeout(settle)
        data = b""
        try:
            while len(data) < len(payload):
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        except OSError:
            pass
        return bool(data), data


with contextlib.redirect_stdout(io.StringIO()):
    allowed, data = usbip_roundtrip()
check("无授权表 → USB/IP 连接照常转发（默认行为不变）",
      allowed and data == b"\x11hello", repr(data[:16]))

e2e_file.write_text(json.dumps({"version": 1, "clients": {}}), encoding="utf-8")
with contextlib.redirect_stdout(io.StringIO()):
    allowed, data = usbip_roundtrip()
check("有授权表（空表）→ 未授权来源的连接被拒绝，后端收不到任何东西",
      not allowed and data == b"", repr(data[:16]))

e2e_file.write_text(json.dumps({"version": 1, "clients": {"127.0.0.1": {"expiresAt": time.time() + 600}}}),
                    encoding="utf-8")
os.utime(e2e_file, ns=(time.time_ns() + 5_000_000_000, time.time_ns() + 5_000_000_000))
with contextlib.redirect_stdout(io.StringIO()):
    allowed, data = usbip_roundtrip()
check("授权后同一来源 IP 的连接恢复转发", allowed and data == b"\x11hello", repr(data[:16]))

e2e_file.write_text(json.dumps({"version": 1, "clients": {"127.0.0.1": {"expiresAt": time.time() - 5}}}),
                    encoding="utf-8")
os.utime(e2e_file, ns=(time.time_ns() + 9_000_000_000, time.time_ns() + 9_000_000_000))
with contextlib.redirect_stdout(io.StringIO()):
    allowed, data = usbip_roundtrip()
check("授权过期后同一来源 IP 又被拒绝", not allowed and data == b"", repr(data[:16]))


def web_roundtrip(payload: bytes = b"GET /api/access HTTP/1.1\r\n\r\n") -> bytes:
    """读回声到静默为止；把后端的 tag 去掉就是它实际收到的字节。"""
    with socket.create_connection(("127.0.0.1", gw_port), timeout=10) as sock:
        sock.sendall(payload)
        sock.settimeout(0.6)
        data = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        except OSError:
            pass
        return data.replace(b"WEB:", b"")


REQUEST = b"GET /api/access HTTP/1.1\r\n\r\n"
e2e_file.write_text(json.dumps({"version": 1, "clients": {}}), encoding="utf-8")
os.utime(e2e_file, ns=(time.time_ns() + 12_000_000_000, time.time_ns() + 12_000_000_000))
with contextlib.redirect_stdout(io.StringIO()):
    http_reply = web_roundtrip(REQUEST)
check("访问控制开启时，HTTP 连接带上带校验标记的 PROXY 头（web.py 才能知道真实来源 IP）",
      http_reply.startswith(b"PROXY TCP4 127.0.0.1 ")
      and http_reply.endswith(REQUEST)
      and http_reply.split(b"\r\n")[0].endswith(
          web.Handler.proxy_token(PROXY_SECRET).encode("ascii")),
      repr(http_reply[:80]))

e2e_file.unlink()
with contextlib.redirect_stdout(io.StringIO()):
    http_reply = web_roundtrip(REQUEST)
check("访问控制关闭时，HTTP 连接逐字节不变（没有 PROXY 头）",
      http_reply == REQUEST, repr(http_reply[:60]))

# 真网关 + 真 web.py：网关注入的 PROXY 头必须能被 web.py 正确吃掉，
# 而且真实 HTTP 请求（而不是裸 socket）仍然照常解析 —— 如果 web.py 没消费这行头，
# 请求行会变成 "PROXY TCP4 ..."，这里就会拿到 400 而不是 200/401。
real_gw = gateway.Gateway(("127.0.0.1", 0), "127.0.0.1",
                          usbip_backend.server_address[1], PORT, str(e2e_file),
                          proxy_secret_value=PROXY_SECRET)
threading.Thread(target=real_gw.serve_forever, daemon=True).start()
real_port = real_gw.server_address[1]
set_access_env(ACCESS_PASSWORD)
web.sync_access_file()


def real_gateway_call(path):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{real_port}{path}", timeout=10) as response:
            return response.status, json.loads(response.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())


with contextlib.redirect_stdout(io.StringIO()):
    status, payload = real_gateway_call("/api/access")
check("真实 HTTP 客户端经网关访问 web.py：PROXY 头被正确消费，响应正常",
      status == 200 and payload.get("ok") is True and payload.get("required") is True,
      f"status={status} body={payload}")
with contextlib.redirect_stdout(io.StringIO()):
    status, payload = real_gateway_call("/api/devices")
check("经网关的匿名 GET /api/devices 也走访问控制（未授权的 127.0.0.1 → 401）",
      status == 401 and payload.get("accessRequired") is True, f"status={status} body={payload}")
set_access_env("")
web.sync_access_file()
real_gw.shutdown()

for backend in (usbip_backend, web_backend):
    backend.shutdown()
gw.shutdown()
httpd.shutdown()
httpd.server_close()
web.run_usbip = old_run_usbip
web.is_shared = old_is_shared

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
