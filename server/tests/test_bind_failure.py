"""bind_one() 的容错：一台设备绑不上，不能把整个服务端拖进重启循环。

真机事故（2026-10-20）：算王1（1-8.2）被拔掉后仍留在 /data/managed-busids 里，
容器每次启动都执行 `usbip bind -b 1-8.2`，失败后 `fail` → exit 1 → 无限重启，
5555 全程不监听，所有客户端一起掉线。修复前用户只能看到「服务端启动不了」。

本用例把 entrypoint.sh 里的 bind_one() 原样抠出来，用桩函数在真实 shell 里跑，
确认三种绑不上的情况都只是跳过：
  * 设备不在总线上（拔掉了 / 换了端口）
  * 设备在，但被别的驱动占着（usbip bind 返回非 0）
  * busid 本身非法

    python tests/test_bind_failure.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "entrypoint.sh"

results = []


def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


def extract_function(text, name):
    """从脚本里按大括号配对抠出一个函数，避免整份脚本的顶层代码被执行。"""
    start = text.index(name + "() {")
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[start:j + 1]
    raise AssertionError("unbalanced braces in " + name)


src = SCRIPT.read_text(encoding="utf-8")
body = extract_function(src, "bind_one")

tmp = Path(tempfile.mkdtemp())
sysfs = tmp / "sys"
sysfs.mkdir()
(sysfs / "1-8.3").mkdir()          # 在总线上，能绑
(sysfs / "1-8.4").mkdir()          # 在总线上，但被别的驱动占用

# 测试机（Windows）上没有 /sys，把硬编码的 sysfs 前缀换成临时目录。
body = body.replace("/sys/bus/usb/devices", sysfs.as_posix())

harness = r"""
set -u
ROUND_BOUND=""
ROUND_MISSING=""
log() { printf 'LOG %s\n' "$*" >> "$LOG"; }
fail() { printf 'FAILCALL %s\n' "$*" >> "$LOG"; exit 99; }
remember_managed() { printf 'MANAGED %s\n' "$1" >> "$LOG"; }
forget_managed() { :; }
netdev_guard_enabled() { return 0; }
exports_network_interface() { [ "$1" = "2-7" ]; }
usbip() {
    # usbip bind -b <busid>
    case "${3:-}" in
        1-8.3) return 0 ;;
        1-8.4) return 1 ;;
    esac
    return 0
}

""" + body + r"""

: > "$LOG"
bind_one "1-8.2"; printf 'R1=%s MISSING=[%s]\n' "$?" "$ROUND_MISSING" >> "$LOG"
bind_one "1-8.3"; printf 'R2=%s BOUND=[%s]\n' "$?" "$ROUND_BOUND" >> "$LOG"
bind_one "1-8.4"; printf 'R3=%s\n' "$?" >> "$LOG"
bind_one "bad id!"; printf 'R4=%s\n' "$?" >> "$LOG"
"""

sh = tmp / "harness.sh"
sh.write_text(harness, encoding="utf-8")
log = tmp / "out.log"
proc = subprocess.run(
    ["sh", str(sh)], capture_output=True, text=True,
    env=dict(os.environ, LOG=log.as_posix()),
)
out = log.read_text(encoding="utf-8") if log.exists() else ""

print("[A] 脚本能跑完，没有被 fail 中止")
check("shell 退出码为 0", proc.returncode == 0, f"rc={proc.returncode} stderr={proc.stderr.strip()[:200]}")
check("全程没有调用 fail", "FAILCALL" not in out, out.strip()[:200])

print("[B] 设备不在总线上：跳过，但记进 ROUND_MISSING")
check("返回 0", "R1=0" in out, out.strip()[:200])
check("记入 ROUND_MISSING", "MISSING=[ 1-8.2]" in out, out.strip()[:200])
check("日志里有提示", "1-8.2 is not on the bus" in out, out.strip()[:300])
check("没有去 remember_managed", "MANAGED 1-8.2" not in out, out.strip()[:200])

print("[C] 设备在总线上：正常绑定")
check("返回 0", "R2=0" in out, out.strip()[:200])
check("记入 ROUND_BOUND", "BOUND=[ 1-8.3]" in out, out.strip()[:200])

print("[D] 设备在但绑不上（被别的驱动占用）：跳过，不中止")
check("返回 0", "R3=0" in out, out.strip()[:200])
check("日志里有提示", "cannot bind 1-8.4" in out, out.strip()[:300])

print("[E] busid 非法：跳过，不中止")
check("返回 0", "R4=0" in out, out.strip()[:200])
check("日志里有提示", "invalid USB bus ID" in out, out.strip()[:300])

passed = sum(results)
total = len(results)
print(f"\n{passed}/{total} 通过")
sys.exit(0 if passed == total else 1)
