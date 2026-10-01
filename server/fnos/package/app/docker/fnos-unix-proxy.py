#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""飞牛统一网关 → 容器内中文管理页 的 Unix Socket 转发器。

飞牛 fnOS 的统一网关把 /app/usbip-share/* 的请求转发到应用 target 目录下的
app.sock，转发前会校验 NAS 登录态（所以小窗打开的管理页天然带登录保护）。

web.py 只认 `/` 与 `/api/*` 这类根路径，所以这里负责：

  * 把请求路径上的网关前缀剥掉再转发给 web.py；
  * 对 `/app/usbip-share`（没有结尾斜杠）返回 301 到带斜杠的形式 —— 页面里
    用的是相对链接，少了这个斜杠浏览器会把 `api/devices` 解析成 `/app/api/devices`；
  * 其余请求（含请求体）原样透传。

不使用 WebSocket，所以不做 Upgrade 处理。
"""
from __future__ import annotations

import os
import socket
import socketserver
import sys
import threading

PREFIX = os.environ.get("USBSHARE_GATEWAY_PREFIX", "/app/usbip-share").rstrip("/")
SOCKET_PATH = os.environ.get("USBSHARE_GATEWAY_SOCKET", "")
UPSTREAM_HOST = os.environ.get("USBSHARE_UPSTREAM_HOST", "127.0.0.1")
UPSTREAM_PORT = int(os.environ.get("USBSHARE_UPSTREAM_PORT", "8080"))

MAX_HEADER_BYTES = 64 * 1024
CLIENT_TIMEOUT_SECONDS = 30
UPSTREAM_CONNECT_TIMEOUT_SECONDS = 10

# 逐跳头部：转发时必须丢掉，否则连接复用语义会乱。
HOP_BY_HOP = {b"connection", b"keep-alive", b"proxy-connection", b"te", b"upgrade"}


def strip_prefix(target: str) -> tuple[str, str | None]:
    """返回 (转发给 web.py 的 target, 需要 301 时的 Location)。"""
    path, sep, query = target.partition("?")
    suffix = f"?{query}" if sep else ""

    if path == PREFIX:
        return target, f"{PREFIX}/{suffix}"
    if path.startswith(PREFIX + "/"):
        return path[len(PREFIX):] + suffix, None
    return target, None


def relay(src: socket.socket, dst: socket.socket) -> None:
    """src -> dst 单向转发；读到 EOF 时通知对端半关闭。"""
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


class GatewaySocketHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:  # noqa: N802 (socketserver API)
        client: socket.socket = self.request
        client.settimeout(CLIENT_TIMEOUT_SECONDS)

        head = b""
        try:
            while b"\r\n\r\n" not in head and len(head) < MAX_HEADER_BYTES:
                chunk = client.recv(4096)
                if not chunk:
                    return
                head += chunk
        except OSError:
            return

        if b"\r\n\r\n" not in head:
            return

        raw_head, _, body = head.partition(b"\r\n\r\n")
        lines = raw_head.split(b"\r\n")
        try:
            method, target, version = lines[0].decode("latin-1").split(" ", 2)
        except ValueError:
            return

        new_target, redirect = strip_prefix(target)
        if redirect is not None:
            response = (
                "HTTP/1.1 301 Moved Permanently\r\n"
                f"Location: {redirect}\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n"
            ).encode("latin-1")
            try:
                client.sendall(response)
            except OSError:
                pass
            return

        rebuilt = [f"{method} {new_target} {version}"]
        for line in lines[1:]:
            name = line.split(b":", 1)[0].strip().lower()
            if name in HOP_BY_HOP:
                continue
            rebuilt.append(line.decode("latin-1"))
        # web.py 是 HTTP/1.0、逐请求短连接，这里明确要求上游关连接。
        rebuilt.append("Connection: close")
        request = ("\r\n".join(rebuilt) + "\r\n\r\n").encode("latin-1") + body

        upstream = None
        try:
            upstream = socket.create_connection(
                (UPSTREAM_HOST, UPSTREAM_PORT), timeout=UPSTREAM_CONNECT_TIMEOUT_SECONDS
            )
            upstream.settimeout(None)
            upstream.sendall(request)
        except OSError:
            try:
                if upstream is not None:
                    upstream.close()
            finally:
                try:
                    client.sendall(
                        b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                    )
                except OSError:
                    pass
            return

        to_client = threading.Thread(target=relay, args=(upstream, client), daemon=True)
        to_client.start()
        relay(client, upstream)
        to_client.join(timeout=5)

        try:
            upstream.close()
        except OSError:
            pass


class GatewaySocketServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True

    def get_request(self):  # noqa: D102 (socketserver API)
        request, _ = super().get_request()
        return request, ""


def main() -> int:
    if not SOCKET_PATH:
        print("[usbip-share-gateway-socket] USBSHARE_GATEWAY_SOCKET is not set", file=sys.stderr)
        return 1

    directory = os.path.dirname(SOCKET_PATH)
    if directory:
        os.makedirs(directory, exist_ok=True)
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    server = GatewaySocketServer(SOCKET_PATH, GatewaySocketHandler)
    # 统一网关是宿主上的另一个进程，容器里跑的是 root，这里给足权限最省事；
    # socket 本身就在应用自己的 target 目录里，没有额外暴露面。
    os.chmod(SOCKET_PATH, 0o666)

    print(
        f"[usbip-share-gateway-socket] unix:{SOCKET_PATH} prefix={PREFIX} "
        f"-> http://{UPSTREAM_HOST}:{UPSTREAM_PORT}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        server.server_close()
        try:
            os.unlink(SOCKET_PATH)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
