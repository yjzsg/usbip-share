"""F5 回归：clients.json / managed-busids 的并发读-改-写不丢更新。

修复前：

  * `read_clients()` 的"过期清理 + 整体重写"在锁外做，两个并发调用会各自读到同一份
    快照，后写的那个把先写进去的记录整体覆盖掉（丢客户端）；
  * `write_managed()` 用固定的 `<path>.tmp` 临时名，两个并发写会互相覆盖/踩掉对方的
    临时文件，甚至把半截内容发布出去。

本用例用多线程并发打这两个路径，断言一条记录都不丢、目录里没有残留临时文件。

    python tests/test_state_race.py
"""
import json
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web  # noqa: E402

results = []
def check(name, cond, detail=""):
    results.append(bool(cond))
    print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""))


root = Path(tempfile.mkdtemp())
web.CLIENTS_FILE = root / "clients.json"
web.QUEUE_FILE = root / "queue.json"
web.NOTIFY_FILE = root / "queue-notify.json"
web.MANAGED_FILE = root / "managed-busids"

THREADS = 16
ROUNDS = 4
READERS = 4
READ_CALLS = 60


def heartbeat(client_id):
    payload = {
        "clientId": client_id,
        "clientName": client_id + "机",
        "busids": [],
        "dataPort": 5555,
    }
    ok, message, _record, _notifications = web.update_client(payload, "192.168.1.10")
    assert ok, message


print(f"[A] 读侧清理与写侧心跳并发（{THREADS} 写线程 × {ROUNDS}，{READERS} 个读线程）")

# 关键：留一条已过期记录，让 read_clients() 每次都必须走"清理 + 整体重写"分支。
# 修复前这个读-改-写在锁外，会把并发写进去的新记录整体覆盖掉。
web.CLIENTS_FILE.write_text(
    json.dumps({"version": 1, "clients": {"ghost": {
        "clientId": "ghost", "name": "已离线", "address": "", "publicIp": "",
        "dataPort": 5555, "busids": [], "lastSeen": int(time.time()) - web.CLIENT_TTL_SECONDS - 120,
    }}}),
    encoding="utf-8",
)


def beat(index):
    for round_index in range(ROUNDS):
        heartbeat(f"client-{index}-{round_index}")


def read_loop():
    for _ in range(READ_CALLS):
        web.read_clients()


readers = [threading.Thread(target=read_loop) for _ in range(READERS)]
threads = [threading.Thread(target=beat, args=(index,)) for index in range(THREADS)]
for thread in readers + threads:
    thread.start()
for thread in readers + threads:
    thread.join()

expected = {f"client-{index}-{round_index}" for index in range(THREADS) for round_index in range(ROUNDS)}
stored = {str(item["clientId"]) for item in web.read_clients()}
check("并发心跳一条不丢（读侧清理不再覆盖写侧）", stored == expected,
      f"got={len(stored)} want={len(expected)}")
check("过期记录被清理掉", "ghost" not in stored)
check("clients.json 仍是合法 JSON 且包含全部记录",
      len(json.loads(web.CLIENTS_FILE.read_text(encoding="utf-8"))["clients"]) == len(expected))

print(f"[B] {THREADS} 线程并发 remember_managed")
managed_expected = {f"1-{index}.{round_index}" for index in range(THREADS) for round_index in range(ROUNDS)}


def manage(index):
    for round_index in range(ROUNDS):
        web.remember_managed(f"1-{index}.{round_index}")


threads = [threading.Thread(target=manage, args=(index,)) for index in range(THREADS)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()

check("并发写入 managed-busids 一条不丢", web.read_managed() == managed_expected,
      f"got={len(web.read_managed())} want={len(managed_expected)}")
leftovers = [path.name for path in root.iterdir() if path.name.endswith(".tmp")]
check("没有残留的临时文件（唯一临时名 + 清理）", not leftovers, str(leftovers))

print("[C] 并发 forget 与 read 不产生半截文件")
threads = [threading.Thread(target=web.forget_managed, args=(f"1-{index}.0",)) for index in range(THREADS)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
check("forget 后剩余集合正确",
      web.read_managed() == managed_expected - {f"1-{index}.0" for index in range(THREADS)},
      str(sorted(web.read_managed())[:5]))

print()
print("总计:", sum(results), "/", len(results), "通过")
sys.exit(0 if all(results) else 1)
