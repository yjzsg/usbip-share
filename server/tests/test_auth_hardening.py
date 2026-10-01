"""F3/F4 回归：随机初始密码、env 托管密码、mustChange 强制、登录限速、令牌解析健壮性。

覆盖：
  [A] 无 USBIP_WEB_PASSWORD：12 位随机密码 + initial-password.txt(0600) + mustChange
  [B] mustChange=true 时写端点 403 must_change_password，白名单与客户端心跳不受影响
  [C] 登录限速：按来源 IP 滑动窗口、5 次失败锁定 429 + Retry-After、成功清零、
      指数退避（上限 15 分钟）、过期清理不泄漏内存
  [D] USBIP_WEB_PASSWORD：passwordManaged=true、mustChange=false、散列同步且盐保持
      （重启后已签发令牌仍有效）、页面改密被拒
  [E] valid_token：长度上限 + 解析异常一律返回 False，绝不抛出

    python tests/test_auth_hardening.py
"""
import json
import os
import sys
import tempfile
import threading
import time
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


def reset_guard():
    web._AUTH_FAILURE_TIMES.clear()
    web._AUTH_LOCK_UNTIL.clear()
    web._AUTH_STRIKES.clear()
    web._AUTH_GUARD_SEEN.clear()


def use_config(root: Path, env_password: str | None = None, token: str | None = None):
    """Point every store at `root` and set/clear the auth environment."""
    web.AUTH_FILE = root / "auth.json"
    web.METADATA_FILE = root / "device-metadata.json"
    web.MANAGED_FILE = root / "managed-busids"
    web.CLIENTS_FILE = root / "clients.json"
    web.QUEUE_FILE = root / "queue.json"
    web.NOTIFY_FILE = root / "queue-notify.json"
    web.SESSIONS.clear()
    reset_guard()
    web._ENV_PASSWORD_APPLIED = None
    for name, value in (("USBIP_WEB_PASSWORD", env_password), ("USBIP_WEB_TOKEN", token)):
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value


# ---------------------------------------------------------------------------
print("[A] 无环境变量：随机初始密码 + initial-password.txt")
root_a = Path(tempfile.mkdtemp())
use_config(root_a)
auth = web.load_auth()
password_file = root_a / "initial-password.txt"
initial = password_file.read_text(encoding="utf-8").strip() if password_file.exists() else ""
check("生成了 initial-password.txt", bool(initial), f"len={len(initial)}")
check("密码为 12 位", len(initial) == web.INITIAL_PASSWORD_LENGTH == 12, f"len={len(initial)}")
check("初始密码不是 123456", initial != "123456")
check("auth.json 里没有明文密码", initial not in (root_a / "auth.json").read_text(encoding="utf-8"))
if os.name == "posix":
    mode = password_file.stat().st_mode & 0o777
    check("initial-password.txt 权限 0600", mode == 0o600, oct(mode))
check("mustChange=True", auth.get("mustChange") is True)
check("旧默认密码 123456 无法登录", web.verify_login("123456")[0] is False)
ok, token, must_change = web.verify_login(initial)
check("初始密码可登录且要求改密", ok and must_change is True and token.count(".") == 1)
salt_before = str(auth.get("salt", ""))
web._ENV_PASSWORD_APPLIED = None
web.load_auth()
check("已存在的 auth.json 不会被重新生成（密码稳定）",
      json.loads((root_a / "auth.json").read_text(encoding="utf-8"))["salt"] == salt_before)

# ---------------------------------------------------------------------------
print("[B] mustChange 服务端强制（真实 HTTP）")
root_b = Path(tempfile.mkdtemp())
use_config(root_b)
web.load_auth()   # 冷启动：生成随机密码 + initial-password.txt + mustChange=true
initial = (root_b / "initial-password.txt").read_text(encoding="utf-8").strip()
check("冷启动密码来自 initial-password.txt", len(initial) == 12, f"len={len(initial)}")

# The device layer is stubbed: this test is about the auth gate, not usbip.
old_run_usbip = web.run_usbip
old_is_shared = web.is_shared
web.run_usbip = lambda *args: (0, " - busid 1-1 (1241:e001)\n      LYFdog : Sample\n") if args[:2] == ("list", "-l") else (0, "ok")
web.is_shared = lambda busid: True

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
            return response.status, json.loads(response.read().decode()), dict(response.headers)
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode()), dict(exc.headers)


st, body, _ = call("/api/session")
check("session 返回 mustChange/passwordManaged",
      st == 200 and body.get("mustChange") is True and body.get("passwordManaged") is False, str(body))
st, body, _ = call("/api/login", {"password": initial})
session_token = body.get("token", "")
check("初始密码登录成功", st == 200 and bool(session_token), str(body))

st, body, _ = call("/api/devices/1-1/share", {}, token=session_token)
check("share 被 403 must_change_password",
      st == 403 and body.get("error") == "must_change_password", f"status={st} body={body}")
st, body, _ = call("/api/devices/1-1/metadata", {"alias": "a", "remark": ""}, token=session_token)
check("metadata 被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
st, body, _ = call("/api/devices/1-1/queue/clear", {}, token=session_token)
check("queue/clear 被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
st, body, _ = call("/api/devices/1-1/queue/abc", token=session_token, method="DELETE")
check("DELETE queue 被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
check("GET /api/health 放行", call("/api/health")[0] == 200)
check("GET /api/session 放行", call("/api/session", token=session_token)[0] == 200)
check("GET /api/devices 放行", call("/api/devices", token=session_token)[0] == 200)
st, body, _ = call("/api/clients/heartbeat",
                   {"clientId": "c1", "clientName": "前台电脑", "busids": [], "dataPort": 5555})
check("客户端心跳放行（Windows 客户端不被 mustChange 打死）", st == 200 and body.get("ok") is True,
      f"status={st} body={body}")

st, body, _ = call("/api/change-password", {"oldPassword": initial, "newPassword": "brand-new-pass"}, token=session_token)
check("改密端点放行", st == 200 and body.get("ok"), str(body))
st, body, _ = call("/api/devices/1-1/share", {}, token=session_token)
check("旧令牌改密后失效（401）", st == 401, f"status={st}")
st, body, _ = call("/api/login", {"password": "brand-new-pass"})
session_token = body.get("token", "")
check("新密码登录后 mustChange=False", st == 200 and body.get("mustChange") is False, str(body))
st, body, _ = call("/api/devices/1-1/share", {}, token=session_token)
check("改密后写端点不再 403", st == 200 and body.get("ok") is True, f"status={st} body={body}")

print("[C] 登录限速（真实 HTTP，同一来源 IP）")
reset_guard()
for attempt in range(1, 5):
    st, body, _ = call("/api/login", {"password": "wrong"})
    check(f"第 {attempt} 次错误密码返回 401", st == 401 and body.get("error") == "密码不正确", f"status={st}")
st, body, headers = call("/api/login", {"password": "wrong"})
check("第 5 次失败返回 429", st == 429, f"status={st} body={body}")
retry_after = int(headers.get("Retry-After", "0") or 0)
check("429 带 Retry-After 且不超过 15 分钟",
      web.AUTH_GUARD_BASE_LOCK_SECONDS <= retry_after <= web.AUTH_GUARD_MAX_LOCK_SECONDS,
      f"Retry-After={retry_after}")
st, body, _ = call("/api/login", {"password": "brand-new-pass"})
check("锁定期间正确密码同样被拒（429）", st == 429, f"status={st} body={body}")

print("[C2] 成功登录清零计数 / 指数退避 / 过期清理")
reset_guard()
ip = "203.0.113.10"
for _ in range(4):
    web.note_auth_failure(ip)
check("4 次失败未锁定", web.auth_retry_after(ip) == 0)
web.note_auth_success(ip)
for _ in range(4):
    web.note_auth_failure(ip)
check("成功清零后再次 4 次失败仍未锁定", web.auth_retry_after(ip) == 0)

reset_guard()
for _ in range(4):
    web.note_auth_failure(ip)
first = web.note_auth_failure(ip)
check("第 1 次锁定 30s", first == web.AUTH_GUARD_BASE_LOCK_SECONDS == 30, f"{first}s")
check("锁定期间 auth_retry_after > 0", web.auth_retry_after(ip) > 0)
web._AUTH_LOCK_UNTIL.pop(ip)          # 模拟锁到期
for _ in range(4):
    web.note_auth_failure(ip)
second = web.note_auth_failure(ip)
check("第 2 次锁定翻倍 60s", second == 60, f"{second}s")
web._AUTH_LOCK_UNTIL.pop(ip)
for _ in range(4):
    web.note_auth_failure(ip)
third = web.note_auth_failure(ip)
check("第 3 次锁定 120s", third == 120, f"{third}s")
web._AUTH_STRIKES[ip] = 20
web._AUTH_LOCK_UNTIL.pop(ip)
for _ in range(4):
    web.note_auth_failure(ip)
capped = web.note_auth_failure(ip)
check("锁定时长上限 15 分钟", capped == web.AUTH_GUARD_MAX_LOCK_SECONDS == 900, f"{capped}s")

old_ip = "203.0.113.11"
web._AUTH_GUARD_SEEN[old_ip] = time.time()
web._AUTH_FAILURE_TIMES[old_ip] = [time.time() - web.AUTH_GUARD_WINDOW_SECONDS - 60] * 4
check("滑动窗口外的失败不再计数", web.note_auth_failure(old_ip) == 0)

stale_ip = "203.0.113.12"
web._AUTH_GUARD_SEEN[stale_ip] = time.time() - web.AUTH_GUARD_IDLE_SECONDS - 60
web._AUTH_FAILURE_TIMES[stale_ip] = [time.time()]
web._AUTH_LOCK_UNTIL[stale_ip] = time.time() + 5
web._AUTH_STRIKES[stale_ip] = 3
web.auth_retry_after(stale_ip)
check("长期无活动条目被清理（不泄漏内存）",
      stale_ip not in web._AUTH_GUARD_SEEN and stale_ip not in web._AUTH_FAILURE_TIMES
      and stale_ip not in web._AUTH_LOCK_UNTIL and stale_ip not in web._AUTH_STRIKES)

# ---------------------------------------------------------------------------
print("[D] USBIP_WEB_PASSWORD 托管密码")
root_d = Path(tempfile.mkdtemp())
env_password = "managed-password-1"
use_config(root_d, env_password=env_password)
auth = web.load_auth()
check("password_managed()=True", web.password_managed() is True)
check("auth.json 标记 managedByEnv", auth.get("managedByEnv") is True)
check("mustChange=False", auth.get("mustChange") is False)
check("环境变量密码可登录", web.verify_login(env_password)[0] is True)
check("不再生成 initial-password.txt", not (root_d / "initial-password.txt").exists())
managed_token = web.issue_session_token(auth)
web._ENV_PASSWORD_APPLIED = None      # 模拟容器重启：重新走一遍同步
auth_after = web.load_auth()
check("重启后盐与散列不变（令牌不失效）",
      auth_after.get("salt") == auth.get("salt") and auth_after.get("pwHash") == auth.get("pwHash"))
check("重启后旧令牌仍有效", web.valid_token(managed_token) is True)
check("旧默认密码 123456 被拒", web.verify_login("123456")[0] is False)

# 环境变量轮换：散列必须被同步，旧令牌立即失效。
os.environ["USBIP_WEB_PASSWORD"] = "managed-password-2"
web._ENV_PASSWORD_APPLIED = None
auth_rotated = web.load_auth()
check("轮换后新密码可登录", web.verify_login("managed-password-2")[0] is True)
check("轮换后旧密码失效", web.verify_login(env_password)[0] is False)
check("轮换后旧令牌失效", web.valid_token(managed_token) is False)

print("[D2] 托管模式下 /api/session 与改密端点")
use_config(root_d, env_password="managed-password-2")
web.load_auth()
st, body, _ = call("/api/session")
check("passwordManaged=true", body.get("passwordManaged") is True and body.get("mustChange") is False, str(body))
st, body, _ = call("/api/login", {"password": "managed-password-2"})
managed_session = body.get("token", "")
check("托管密码登录成功", st == 200 and bool(managed_session), str(body))
st, body, _ = call("/api/change-password",
                   {"oldPassword": "managed-password-2", "newPassword": "whatever-123"}, token=managed_session)
check("托管模式页面改密被拒 400", st == 400 and body.get("ok") is False, f"status={st} body={body}")

httpd.shutdown()
httpd.server_close()
web.run_usbip = old_run_usbip
web.is_shared = old_is_shared

# ---------------------------------------------------------------------------
print("[E] valid_token 解析健壮性")
root_e = Path(tempfile.mkdtemp())
use_config(root_e)
auth = web.load_auth()
valid = web.issue_session_token(auth)
check("正常令牌仍然有效", web.valid_token(valid) is True)
check("超长令牌直接 False（长度上限 512）", web.valid_token("x" * 100000) is False)
check("512 位以上数字令牌不抛异常", web.valid_token("9" * 600 + "." + "a" * 64) is False)
check("400 位数字 + 假签名不抛异常", web.valid_token("9" * 400 + "." + "f" * 64) is False)
check("非字符串令牌不抛异常", web.valid_token(None) is False and web.valid_token(b"bytes") is False)
check("空令牌与乱码令牌无效", web.valid_token("") is False and web.valid_token("garbage") is False)
check("缺签名的数字令牌无效", web.valid_token("9999999999.") is False)

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
