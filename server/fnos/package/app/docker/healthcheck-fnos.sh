#!/bin/sh
# 飞牛 fnOS 版的健康检查入口。
#
# Docker 的 HEALTHCHECK 在全新进程里执行，看不到 entrypoint 的运行时 export，
# 所以这里自己读一次应用设置与内部端口文件，否则端口不是 5555 / 不是 8080 时
# 会被误判为不健康。
set -eu

# shellcheck disable=SC1091
. /usr/local/bin/usbip-share-fnos-env.sh

if [ -z "${USBIP_WEB_PORT}" ]; then
    # 内部端口文件还没写出来 = 容器主进程还没走到启动服务那一步。
    printf '%s\n' "[usbip-share] healthcheck: internal ports not allocated yet" >&2
    exit 1
fi

exec /usr/local/bin/usbip-share-healthcheck
