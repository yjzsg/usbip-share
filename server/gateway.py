#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""usbip-share 单端口分流网关 (Phase B).

对外只监听一个 TCP 端口(默认 5555),按每条连接的首字节把流量分流到容器内的两个服务:

  - USB/IP 协议(usbip header version 0x0111,首字节 0x11)→ usbipd(默认 127.0.0.1:5556)
  - HTTP(方法首字母 A-Z,如 GET/POST)→ 中文管理页 web.py(默认 127.0.0.1:8080)

整条 TCP 连接在建立时归队一次,之后双向透明转发;因此 USB/IP attach 长连接、
HTTP keep-alive、管理 API 与网页都走同一个对外端口。这样 NAS 只需对外开放一个端口,
不再需要单独的 18080 管理端口。
"""
import argparse
import os
import socket
import socketserver
import threading


def env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, ""))
    except ValueError:
        return default


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


def bridge(client: socket.socket, upstream_host: str, upstream_port: int, first: bytes) -> None:
    """把已消费首字节的连接接到 upstream,再双向转发。"""
    up = None
    try:
        up = socket.create_connection((upstream_host, upstream_port), timeout=10)
        # create_connection 会用 timeout=10 给 socket 设超时;必须清除!
        # 否则空闲的 USB/IP 长会话(>10s 无 URB 流量)会被 recv 超时错误拆断,
        # 表现为"每 ~20s 自动断开重连"。转发期两个方向都不允许超时。
        up.settimeout(None)
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
        try:
            first = client.recv(1)
        except OSError:
            return
        if not first:
            return
        if classify(first[0]) == "web":
            port = self.server.web_port
        else:
            port = self.server.usbip_port
        bridge(client, self.server.upstream_host, port, first)


class Gateway(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, upstream_host: str, usbip_port: int, web_port: int) -> None:
        self.upstream_host = upstream_host
        self.usbip_port = usbip_port
        self.web_port = web_port
        super().__init__(addr, GatewayHandler)


def main() -> None:
    parser = argparse.ArgumentParser(description="usbip-share single-port gateway")
    parser.add_argument("--host", default=os.environ.get("USBIP_GATEWAY_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=env_int("USBIP_PORT", 5555))
    parser.add_argument("--upstream-host", default="127.0.0.1")
    parser.add_argument("--usbip-port", type=int, default=env_int("USBIP_INNER_USBIPD_PORT", 5556))
    parser.add_argument("--web-port", type=int, default=env_int("USBIP_WEB_PORT", 8080))
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        raise SystemExit("gateway port out of range")
    if not 1 <= args.usbip_port <= 65535 or not 1 <= args.web_port <= 65535:
        raise SystemExit("upstream port out of range")

    srv = Gateway((args.host, args.port), args.upstream_host, args.usbip_port, args.web_port)
    print(
        f"[usbip-share-gateway] single port {args.host}:{args.port} -> "
        f"usbipd {args.upstream_host}:{args.usbip_port} | web {args.upstream_host}:{args.web_port}",
        flush=True,
    )
    try:
        srv.serve_forever(poll_interval=0.5)
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
