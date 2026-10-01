#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""usbip-share 单端口分流网关 (Phase B).

对外只监听一个 TCP 端口(默认 5555),按每条连接的首字节把流量分流到容器内的两个服务:

  - USB/IP 协议(usbip header version 0x0111,首字节 0x11)→ usbipd(默认 127.0.0.1:5556)
  - HTTP(方法首字母 A-Z,如 GET/POST)→ 中文管理页 web.py(默认 127.0.0.1:8080)

整条 TCP 连接在建立时归队一次,之后双向透明转发;因此 USB/IP attach 长连接、
HTTP keep-alive、管理 API 与网页都走同一个对外端口。这样 NAS 只需对外开放一个端口,
不再需要单独的 18080 管理端口。

共享访问密码(可选)只作用在 USB/IP 分支上:授权表文件存在时,只有来源 IP 在表里
且未过期的连接才会被转发给 usbipd,其余立即关闭。文件不存在 = 未启用访问控制,
放行一切 —— 也就是本文件在引入该功能之前的行为,默认路径上逐字节不变。
"""
import argparse
import json
import os
import socket
import socketserver
import threading
import time


DEFAULT_ACCESS_FILE = "/run/usbip/authorized-clients.json"
# 同一个 IP 被拒的日志最多这么久打一条:客户端被拦后会不停重试,不能刷屏。
DENY_LOG_INTERVAL_SECONDS = 60.0
# 拒绝日志表的上限,防止有人拿大量伪造源地址把内存撑大。
DENY_LOG_MAX_ENTRIES = 4096
# 与 web.py 里 Handler.PROXY_LINE_PREFIX 保持一致。
PROXY_LINE_PREFIX = "PROXY "

_DENY_LOG_LOCK = threading.Lock()
_DENY_LOGGED: dict[str, float] = {}


def env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, ""))
    except ValueError:
        return default


class AccessGate:
    """授权表(Access Table)只读视图,带 mtime 缓存。

    文件就是开关:

      * 文件不存在 → 访问控制关闭,``allows()`` 恒为 True(默认,行为不变);
      * 文件存在   → 只有表里且未过期的地址放行。空表 = 全部拒绝。

    每个连接都读一次盘既慢又没必要,所以按 ``st_mtime_ns`` 缓存;文件被 web.py
    原子替换后 mtime 必然变化,下一次连接就会重新加载。文件消失(关闭访问控制)
    也会被察觉 —— 那一步必须每次 stat,否则永远发现不了"开关被关掉"。
    """

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = str(path)
        self._mtime_ns: int | None = None
        self._loaded = False
        self._entries: dict[str, float] = {}
        # 测试用:实际重新加载的次数,用来证明缓存真的生效。
        self.reloads = 0

    def _refresh(self) -> None:
        try:
            stat = os.stat(self.path)
        except OSError:
            if self._loaded:
                self._loaded = False
                self._entries = {}
                self._mtime_ns = None
                self.reloads += 1
                print(
                    f"[usbip-share-gateway] access file {self.path} is gone: "
                    "shared-access password disabled, every client is allowed",
                    flush=True,
                )
            return
        if self._loaded and self._mtime_ns == stat.st_mtime_ns:
            return
        was_loaded = self._loaded
        self._mtime_ns = stat.st_mtime_ns
        self._loaded = True
        self._entries = self._parse()
        self.reloads += 1
        if not was_loaded:
            print(
                f"[usbip-share-gateway] shared-access password enabled "
                f"({len(self._entries)} authorized source IPs)",
                flush=True,
            )

    def _parse(self) -> dict[str, float]:
        """{address: expiresAt}; unreadable/corrupt file means "deny all"."""
        try:
            with open(self.path, "r", encoding="utf-8") as stream:
                raw = json.load(stream)
        except (OSError, ValueError) as exc:
            print(
                f"[usbip-share-gateway] cannot read {self.path} ({exc}); "
                "refusing every USB/IP client until it is readable again",
                flush=True,
            )
            return {}
        clients = raw.get("clients", {}) if isinstance(raw, dict) else {}
        if not isinstance(clients, dict):
            return {}
        entries: dict[str, float] = {}
        for address, record in clients.items():
            if not isinstance(address, str) or not isinstance(record, dict):
                continue
            try:
                entries[address] = float(record.get("expiresAt", 0))
            except (TypeError, ValueError):
                continue
        return entries

    def enabled(self) -> bool:
        self._refresh()
        return self._loaded

    def allows(self, address: str, now: float | None = None) -> bool:
        self._refresh()
        if not self._loaded:
            return True  # 未启用访问控制:放行一切
        expires_at = self._entries.get(address)
        if expires_at is None:
            return False
        return expires_at > (time.time() if now is None else now)


def log_denied(address: str) -> bool:
    """按 IP 节流地打印一行拒绝日志;返回是否真的打印了(便于测试)。"""
    now = time.time()
    with _DENY_LOG_LOCK:
        if now - _DENY_LOGGED.get(address, 0.0) < DENY_LOG_INTERVAL_SECONDS:
            return False
        _DENY_LOGGED[address] = now
        if len(_DENY_LOGGED) > DENY_LOG_MAX_ENTRIES:
            for key in [item for item, seen in _DENY_LOGGED.items()
                        if now - seen > DENY_LOG_INTERVAL_SECONDS]:
                _DENY_LOGGED.pop(key, None)
    print(
        f"[usbip-share-gateway] refused USB/IP connection from {address}: "
        "not in the authorized client list (or its authorization expired)",
        flush=True,
    )
    return True


def proxy_header(address: str, source_port: int, local: object) -> bytes:
    """PROXY v1 头:把真实来源 IP 告诉 web.py。

    网关只是 TCP 转发,web.py 看到的对端永远是网关自己,按来源 IP 做访问控制就
    无从谈起。这行头只在**访问控制开启时**才补(见 GatewayHandler.handle),所以
    没设密码的老部署(以及不认识这行头的老 web.py)行为完全不变。
    """
    family = "TCP6" if ":" in address else "TCP4"
    destination, destination_port = "0.0.0.0", 0
    if isinstance(local, tuple) and len(local) >= 2:
        destination, destination_port = str(local[0]), int(local[1])
    return f"PROXY {family} {address} {destination} {source_port} {destination_port}\r\n".encode(
        "ascii", "replace"
    )


# 首字节的等待上限:连上却一个字节都不发的连接(慢速连接攻击)不能永远占着线程。
# 只作用于第一个字节,转发阶段两个方向都不设超时(见 bridge())。
FIRST_BYTE_TIMEOUT_SECONDS = 10.0


def classify(first_byte: int) -> str:
    """A-Z 开头视为 HTTP 方法,其余(含 0x11 usbip version)视为 USB/IP。"""
    if 0x41 <= first_byte <= 0x5A:  # 'A'..'Z'
        return "web"
    return "usbip"


def relay(src: socket.socket, dst: socket.socket) -> None:
    """src -> dst 单向转发;本方向读到 EOF(或出错)时通知对端半关闭。"""
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def bridge(client: socket.socket, upstream_host: str, upstream_port: int, first: bytes,
           prefix: bytes = b"") -> None:
    """把已消费首字节的连接接到 upstream,再双向转发。

    ``prefix`` 是在转发客户端数据之前先写给 upstream 的固定前缀(目前只有
    HTTP 分支用的 PROXY v1 头);USB/IP 分支始终为空,数据流逐字节不变。
    """
    up = None
    try:
        up = socket.create_connection((upstream_host, upstream_port), timeout=10)
        # create_connection 会用 timeout=10 给 socket 设超时;必须清除!
        # 否则空闲的 USB/IP 长会话(>10s 无 URB 流量)会被 recv 超时错误拆断,
        # 表现为"每 ~20s 自动断开重连"。转发期两个方向都不允许超时。
        up.settimeout(None)
        if prefix:
            up.sendall(prefix)
        up.sendall(first)  # 首字节原样补回
    except OSError:
        try:
            if up is not None:
                up.close()
        finally:
            client.close()
        return

    t1 = threading.Thread(target=relay, args=(client, up), daemon=True)
    t2 = threading.Thread(target=relay, args=(up, client), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    try:
        up.close()
    except OSError:
        pass
    try:
        client.close()
    except OSError:
        pass


class GatewayHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        client: socket.socket = self.request
        # 只给"等第一个字节"设超时,拿到之后必须清掉:否则转发线程的 recv 会在
        # 空闲 10s 后抛超时,USB/IP 长会话又会被拆断(见 bridge() 里的同类注释)。
        try:
            client.settimeout(FIRST_BYTE_TIMEOUT_SECONDS)
            first = client.recv(1)
        except OSError:
            # 包含 socket.timeout(TimeoutError):慢速连接直接丢弃,不打印栈。
            return
        finally:
            try:
                client.settimeout(None)
            except OSError:
                pass
        if not first:
            return
        address = self.client_address[0] if self.client_address else ""
        if classify(first[0]) == "web":
            prefix = b""
            if self.server.access.enabled():
                # 访问控制开启时才补 PROXY 头(见 proxy_header 的说明):
                # 关闭时这条路径与旧版逐字节一致。
                try:
                    local = client.getsockname()
                except OSError:
                    local = None
                source_port = self.client_address[1] if len(self.client_address) > 1 else 0
                prefix = proxy_header(address, source_port, local)
            bridge(client, self.server.upstream_host, self.server.web_port, first, prefix)
            return
        # USB/IP 分支:授权表存在时,只有授权过且未过期的来源 IP 能连到 usbipd。
        # 首字节已经读掉了,这里直接关闭连接(客户端会看到连接被断开)。
        if not self.server.access.allows(address):
            log_denied(address)
            try:
                client.close()
            except OSError:
                pass
            return
        bridge(client, self.server.upstream_host, self.server.usbip_port, first)


class Gateway(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, upstream_host: str, usbip_port: int, web_port: int,
                 access_file: str = DEFAULT_ACCESS_FILE) -> None:
        self.upstream_host = upstream_host
        self.usbip_port = usbip_port
        self.web_port = web_port
        self.access = AccessGate(access_file)
        super().__init__(addr, GatewayHandler)


def main() -> None:
    parser = argparse.ArgumentParser(description="usbip-share single-port gateway")
    parser.add_argument("--host", default=os.environ.get("USBIP_GATEWAY_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=env_int("USBIP_PORT", 5555))
    parser.add_argument("--upstream-host", default="127.0.0.1")
    parser.add_argument("--usbip-port", type=int, default=env_int("USBIP_INNER_USBIPD_PORT", 5556))
    parser.add_argument("--web-port", type=int, default=env_int("USBIP_WEB_PORT", 8080))
    parser.add_argument("--access-file", default=os.environ.get("USBIP_ACCESS_FILE", DEFAULT_ACCESS_FILE),
                        help="authorized-clients.json written by web.py; missing file = no access control")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        raise SystemExit("gateway port out of range")
    if not 1 <= args.usbip_port <= 65535 or not 1 <= args.web_port <= 65535:
        raise SystemExit("upstream port out of range")

    srv = Gateway((args.host, args.port), args.upstream_host, args.usbip_port, args.web_port,
                  args.access_file)
    print(
        f"[usbip-share-gateway] single port {args.host}:{args.port} -> "
        f"usbipd {args.upstream_host}:{args.usbip_port} | web {args.upstream_host}:{args.web_port}",
        flush=True,
    )
    print(
        f"[usbip-share-gateway] access file {args.access_file}: "
        + ("shared-access password enabled, unauthorized source IPs are refused"
           if srv.access.enabled() else "no shared-access password, every client is allowed"),
        flush=True,
    )
    try:
        srv.serve_forever(poll_interval=0.5)
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
