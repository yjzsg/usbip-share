# 飞牛 fnOS 版镜像。
#
# 与 server/Dockerfile 的差异只有三处，其余必须保持一致：
#   1. 多复制 fnOS 包装脚本（fnos-env.sh / entrypoint-fnos.sh / healthcheck-fnos.sh）；
#   2. ENTRYPOINT / HEALTHCHECK 指向包装脚本——它们会先读取「应用设置」再调用原脚本；
#   3. 数据目录固定为挂载进来的 /data（@appdata）。
# server/fnos/verify.sh 会比对两者的 apt 包与应用文件清单，防止漂移。

FROM debian:12-slim

ARG DEBIAN_FRONTEND=noninteractive

# usbip supplies usbipd/usbip; kmod supplies modprobe/lsmod;
# python3 runs the small Chinese management page without third-party packages.
RUN apt-get update \
    && apt-get install -y --no-install-recommends usbip kmod python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/usbip-share-entrypoint
COPY healthcheck.sh /usr/local/bin/usbip-share-healthcheck
COPY web.py /opt/usbip-share/web.py
COPY gateway.py /opt/usbip-share/gateway.py
COPY index.html /opt/usbip-share/index.html
COPY favicon.ico /opt/usbip-share/favicon.ico

# fnOS 包装层
COPY fnos-env.sh /usr/local/bin/usbip-share-fnos-env.sh
COPY entrypoint-fnos.sh /usr/local/bin/usbip-share-entrypoint-fnos
COPY healthcheck-fnos.sh /usr/local/bin/usbip-share-healthcheck-fnos
# 飞牛统一网关的 Unix Socket 转发器（小窗入口用它访问管理页）
COPY fnos-unix-proxy.py /opt/usbip-share/fnos-unix-proxy.py

RUN chmod 0755 /usr/local/bin/usbip-share-entrypoint /usr/local/bin/usbip-share-entrypoint-fnos \
        /usr/local/bin/usbip-share-healthcheck /usr/local/bin/usbip-share-healthcheck-fnos \
        /opt/usbip-share/fnos-unix-proxy.py \
    && chmod 0644 /usr/local/bin/usbip-share-fnos-env.sh \
    && mkdir -p /etc/usbip-share /data /appdest /opt/usbip-share

EXPOSE 5555/tcp

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD ["/usr/local/bin/usbip-share-healthcheck-fnos"]

ENTRYPOINT ["/usr/local/bin/usbip-share-entrypoint-fnos"]
