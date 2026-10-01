"""End-to-end check of the management page auth flow, including a simulated
container restart, against the real web.py served over HTTP.

密码模型在安全修复里变了：出厂不再有固定 `123456`。

  [A] 未设置 USBIP_WEB_PASSWORD：首次启动生成 12 位随机密码，写入
      `<配置目录>/initial-password.txt`，`mustChange=true`；此时除
      /api/session、/api/login、/api/change-password、/api/health、
      /api/devices 之外的写端点一律 403 must_change_password（客户端心跳
      /api/clients/heartbeat 必须继续可用，否则 Windows 客户端会被打死）。
  [B] 设置 USBIP_WEB_PASSWORD：密码由环境变量托管，passwordManaged=true、
      mustChange=false，页面上不能改密。
"""
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
PORT_A = 18099
PORT_B = 18100

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


class Server:
    """One `web.py` process with its own temp state dir and HTTP base URL."""

    def __init__(self, port, config_dir, extra_env=None):
        self.port = port
        self.base = f"http://127.0.0.1:{port}"
        self.config = Path(config_dir)
        self.env = dict(os.environ)
        self.env.pop("USBIP_WEB_PASSWORD", None)
        self.env.update({
            "USBIP_METADATA_FILE": str(self.config / "device-metadata.json"),
            "USBIP_AUTH_FILE": str(self.config / "auth.json"),
            "USBIP_MANAGED_FILE": str(self.config / "managed"),
            "USBIP_CLIENTS_FILE": str(self.config / "clients.json"),
            "USBIP_QUEUE_FILE": str(self.config / "queue.json"),
            "USBIP_NOTIFY_FILE": str(self.config / "queue-notify.json"),
            "USBIP_KICK_IDLE_SECONDS": "3600",
        })
        self.env.update(extra_env or {})
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [PY, str(SRV / "web.py"), "--host", "127.0.0.1", "--port", str(self.port)],
            cwd=str(SRV), env=self.env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        for _ in range(60):
            try:
                with urllib.request.urlopen(self.base + "/api/health", timeout=2):
                    return self
            except Exception:
                time.sleep(0.25)
        raise SystemExit("server did not come up")

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.proc = None

    def call(self, path, payload=None, token=None, method=None):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method or ("POST" if data else "GET"))
        if data:
            req.add_header("Content-Type", "application/json")
        if token:
            req.add_header("X-Admin-Token", token)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                return resp.status, json.loads(resp.read().decode())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read().decode())


# ---------------------------------------------------------------------------
print("[A] 无 USBIP_WEB_PASSWORD：随机初始密码 + 强制改密")
tmp_a = Path(tempfile.mkdtemp())
server = Server(PORT_A, tmp_a).start()

st, body = server.call("/api/session")
check("session 可读且未授权", st == 200 and body.get("authorized") is False, f"status={st}")
check("提示必须修改初始密码", body.get("mustChange") is True)
check("非环境变量托管", body.get("passwordManaged") is False)

password_file = tmp_a / "initial-password.txt"
initial = password_file.read_text(encoding="utf-8").strip() if password_file.exists() else ""
check("已生成 initial-password.txt", bool(initial), f"len={len(initial)}")
check("初始密码为 12 位随机密码", len(initial) == 12 and initial != "123456")
if os.name == "posix":
    check("initial-password.txt 权限为 0600", (password_file.stat().st_mode & 0o777) == 0o600,
          oct(password_file.stat().st_mode & 0o777))

st, body = server.call("/api/login", {"password": "123456"})
check("旧默认密码 123456 被拒", st == 401, f"status={st}")

print("[A2] 用初始密码登录")
st, body = server.call("/api/login", {"password": initial})
token = body.get("token", "")
check("初始密码可登录并返回签名令牌", st == 200 and body.get("ok") and token.count(".") == 1,
      f"token={token[:22]}…")
check("登录响应要求改密", body.get("mustChange") is True)

print("[A3] mustChange 服务端强制：写端点 403，白名单照常")
st, body = server.call("/api/devices/1-1/share", {}, token=token)
check("共享设备被 403 must_change_password",
      st == 403 and body.get("error") == "must_change_password", f"status={st} body={body}")
st, body = server.call("/api/devices/1-1/metadata", {"alias": "x", "remark": ""}, token=token)
check("改名称/备注被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
st, body = server.call("/api/devices/1-1/queue/clear", {}, token=token)
check("清空等待名单被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
st, body = server.call("/api/devices/1-1/queue/some-client", token=token, method="DELETE")
check("移出等待名单被 403", st == 403 and body.get("error") == "must_change_password", f"status={st}")
check("GET /api/health 不受影响", server.call("/api/health")[0] == 200)
st, body = server.call("/api/session", token=token)
check("GET /api/session 不受影响", st == 200 and body.get("authorized") is True)
check("GET /api/devices 不被 403", server.call("/api/devices", token=token)[0] != 403)
st, body = server.call("/api/clients/heartbeat", {
    "clientId": "windows-client-1", "clientName": "前台电脑", "busids": [], "dataPort": 5555,
})
check("客户端心跳不被 mustChange 拦截（否则已发布客户端会失效）",
      st == 200 and body.get("ok") is True, f"status={st} body={body}")

print("[A4] 改密后：旧令牌失效、写端点解禁")
st, body = server.call("/api/change-password", {"oldPassword": initial, "newPassword": "newpass123"}, token=token)
check("改密码成功", st == 200 and body.get("ok"), str(body))
check("旧令牌已失效", server.call("/api/session", token=token)[1].get("authorized") is False)
st, body = server.call("/api/login", {"password": "newpass123"})
token2 = body.get("token", "")
check("新密码可登录且不再要求改密", st == 200 and body.get("ok") and body.get("mustChange") is False)
st, body = server.call("/api/devices/1-1/share", {}, token=token2)
check("改密后写端点不再 403", st != 403 and body.get("error") != "must_change_password",
      f"status={st} body={body}")

print("[A5] 模拟容器重启（进程换一个，内存会话清零）")
server.stop()
server = Server(PORT_A, tmp_a).start()
check("重启后同一令牌仍然有效",
      server.call("/api/session", token=token2)[1].get("authorized") is True,
      "登录态不再因重建容器丢失")
server.stop()

# ---------------------------------------------------------------------------
print("[B] 设置 USBIP_WEB_PASSWORD：密码由环境变量托管")
tmp_b = Path(tempfile.mkdtemp())
env_password = "managed-by-env-2024"
server = Server(PORT_B, tmp_b, {"USBIP_WEB_PASSWORD": env_password}).start()

st, body = server.call("/api/session")
check("passwordManaged=true", st == 200 and body.get("passwordManaged") is True, str(body))
check("托管模式下不再要求改密", body.get("mustChange") is False)
check("托管模式下不生成 initial-password.txt", not (tmp_b / "initial-password.txt").exists())

st, body = server.call("/api/login", {"password": env_password})
token = body.get("token", "")
check("环境变量密码可登录", st == 200 and body.get("ok") is True and body.get("mustChange") is False,
      f"status={st} body={body}")
st, body = server.call("/api/login", {"password": "123456"})
check("旧默认密码 123456 被拒", st == 401, f"status={st}")
st, body = server.call("/api/login", {"password": env_password})
check("成功登录会清零失败计数", st == 200 and body.get("ok") is True, f"status={st}")

st, body = server.call("/api/change-password", {"oldPassword": env_password, "newPassword": "something-else"},
                       token=token)
check("托管模式下页面改密被拒（下次启动会被环境变量覆盖）",
      st == 400 and body.get("ok") is False, f"status={st} body={body}")

print("[B2] 环境变量变更后重启：auth.json 被同步")
server.stop()
server = Server(PORT_B, tmp_b, {"USBIP_WEB_PASSWORD": "rotated-password-9"}).start()
check("新环境变量密码可登录", server.call("/api/login", {"password": "rotated-password-9"})[0] == 200)
check("旧环境变量密码已失效", server.call("/api/login", {"password": env_password})[0] == 401)
server.stop()

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
