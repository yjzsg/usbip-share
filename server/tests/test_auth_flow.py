"""End-to-end check of the management page auth flow, including a simulated
container restart, against the real web.py served over HTTP."""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

PY = sys.executable
SRV = Path(__file__).resolve().parent.parent
PORT = 18099
BASE = f"http://127.0.0.1:{PORT}"

tmp = Path(tempfile.mkdtemp())
env = dict(os.environ)
env.update({
    "USBIP_METADATA_FILE": str(tmp / "device-metadata.json"),
    "USBIP_AUTH_FILE": str(tmp / "auth.json"),
    "USBIP_MANAGED_FILE": str(tmp / "managed"),
    "USBIP_CLIENTS_FILE": str(tmp / "clients.json"),
    "USBIP_KICK_IDLE_SECONDS": "3600",
})

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))

def call(path, payload=None, token=None, method=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method or ("POST" if data else "GET"))
    if data:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Admin-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode())

def start():
    proc = subprocess.Popen([PY, str(SRV / "web.py"), "--host", "127.0.0.1", "--port", str(PORT)],
                            cwd=str(SRV), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        try:
            with urllib.request.urlopen(BASE + "/api/health", timeout=2):
                return proc
        except Exception:
            time.sleep(0.25)
    raise SystemExit("server did not come up")

print("[1] 首次访问：未登录 + 要求改默认密码")
proc = start()
st, body = call("/api/session")
check("session 可读且未授权", st == 200 and body.get("authorized") is False, f"status={st}")
check("提示必须修改默认密码", body.get("mustChange") is True)

print("[2] 登录拿令牌")
st, body = call("/api/login", {"password": "123456"})
token = body.get("token", "")
check("登录成功并返回签名令牌", st == 200 and body.get("ok") and token.count(".") == 1, f"token={token[:22]}…")
check("错误密码被拒", call("/api/login", {"password": "wrong"})[0] == 401)

print("[3] 携带令牌：页面应直接进入设备列表（模拟刷新）")
st, body = call("/api/session", token=token)
check("刷新后仍视为已登录", body.get("authorized") is True, f"authorized={body.get('authorized')}")
check("无令牌视为未登录", call("/api/session")[1].get("authorized") is False)

print("[4] 模拟容器重启（进程换一个，内存会话清零）")
proc.terminate()
proc.wait(timeout=10)
proc = start()
st, body = call("/api/session", token=token)
check("重启后同一令牌仍然有效", body.get("authorized") is True, "登录态不再因重建容器丢失")

print("[5] 修改密码后旧令牌立即失效")
st, body = call("/api/change-password", {"oldPassword": "123456", "newPassword": "newpass123"}, token=token)
check("改密码成功", st == 200 and body.get("ok"), str(body))
check("旧令牌已失效", call("/api/session", token=token)[1].get("authorized") is False)
st, body = call("/api/login", {"password": "newpass123"})
check("新密码可登录", body.get("ok") and body.get("mustChange") is False)

proc.terminate()
proc.wait(timeout=10)
print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
