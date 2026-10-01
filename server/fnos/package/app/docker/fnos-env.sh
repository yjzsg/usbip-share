#!/bin/sh
# 飞牛应用包装层：把「应用设置」里的值注入环境，并固定容器内的数据路径。
#
# 被 entrypoint-fnos.sh 与 healthcheck-fnos.sh 共用（两者都是独立进程，
# 谁也拿不到对方的运行时 export，所以各自 source 一次）。
#
# 为什么不用 docker-compose 的 environment/env_file：
#   compose 的变量替换发生在 fnOS 启动 compose 之前，我们无法确定它会不会
#   把未知变量替换成空串；而 healthcheck 又在全新进程里跑。让容器自己读挂载
#   进来的配置文件，行为完全可控。
#
# 为什么内部端口是动态的：
#   这个容器用 network_mode: host（USB/IP 需要宿主上稳定可预期的端口），
#   所以容器里 bind 的每个端口都是宿主端口。8080 这种常用端口在飞牛上极可能
#   被别的应用占着（本机就是 seafile），写死会直接导致容器起不来。
#   所以 usbipd 与 web.py 的内部端口在容器启动时现挑空闲端口，记在 /data 下，
#   healthcheck 再读同一份。
set -eu

USBSHARE_CONF="${USBSHARE_CONF:-/etc/usbip-share/usbip-share.env}"
USBSHARE_PORTS_FILE="${USBSHARE_PORTS_FILE:-/data/internal-ports.env}"

if [ -f "${USBSHARE_CONF}" ]; then
    set -a
    # shellcheck disable=SC1090
    . "${USBSHARE_CONF}"
    set +a
else
    printf '%s\n' "[usbip-share] WARNING: ${USBSHARE_CONF} not found; falling back to defaults" >&2
fi

# 对外端口：USB/IP 数据协议与中文管理页共用的那一个。这是唯一对外暴露的端口。
USBIP_PORT=${USBIP_PORT:-5555}
USBIP_GATEWAY_HOST=${USBIP_GATEWAY_HOST:-0.0.0.0}

# 内部端口：上一次启动时挑好的空闲端口（entrypoint 写，healthcheck 读）。
# 允许在应用设置里显式指定（USBIP_INNER_USBIPD_PORT / USBIP_WEB_PORT）来覆盖。
if [ -f "${USBSHARE_PORTS_FILE}" ]; then
    # shellcheck disable=SC1090
    . "${USBSHARE_PORTS_FILE}"
fi
USBIP_INNER_USBIPD_PORT=${USBIP_INNER_USBIPD_PORT:-}
USBIP_WEB_PORT=${USBIP_WEB_PORT:-}

# web.py 只服务容器内部的单端口网关，绑回环即可，不要暴露到局域网。
USBIP_WEB_ENABLED=${USBIP_WEB_ENABLED:-true}
USBIP_WEB_HOST=${USBIP_WEB_HOST:-127.0.0.1}

# 用户数据落在挂载进来的 @appdata（卸载时保留）。
USBIP_AUTH_FILE=${USBIP_AUTH_FILE:-/data/auth.json}
USBIP_INITIAL_PASSWORD_FILE=${USBIP_INITIAL_PASSWORD_FILE:-/data/initial-password.txt}
USBIP_METADATA_FILE=${USBIP_METADATA_FILE:-/data/device-metadata.json}
USBIP_CLIENTS_FILE=${USBIP_CLIENTS_FILE:-/data/clients.json}
USBIP_QUEUE_FILE=${USBIP_QUEUE_FILE:-/data/queue.json}
USBIP_NOTIFY_FILE=${USBIP_NOTIFY_FILE:-/data/queue-notify.json}
USBIP_PIDFILE=${USBIP_PIDFILE:-/data/usbipd.pid}
USBIP_MANAGED_FILE=${USBIP_MANAGED_FILE:-/data/managed-busids}
# 共享访问密码的授权表：web.py 写、单端口网关读。放在 @appdata 里，
# 但 web.py 启动时总会先写一张空表，所以重启/换密码都会清空所有授权。
USBIP_ACCESS_FILE=${USBIP_ACCESS_FILE:-/data/authorized-clients.json}

# 某个端口在宿主上是否可用（用 0.0.0.0 试绑，比只试回环严格）。
usbshare_port_free() {
    python3 - "$1" <<'PY'
import socket
import sys

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    sock.bind(("0.0.0.0", int(sys.argv[1])))
except OSError:
    raise SystemExit(1)
finally:
    sock.close()
PY
}

# 挑一个当前空闲的端口。
usbshare_pick_free_port() {
    python3 - <<'PY'
import socket

sock = socket.socket()
sock.bind(("0.0.0.0", 0))
print(sock.getsockname()[1])
sock.close()
PY
}

export USBIP_PORT USBIP_GATEWAY_HOST USBIP_INNER_USBIPD_PORT USBIP_WEB_PORT \
    USBIP_WEB_ENABLED USBIP_WEB_HOST USBIP_AUTH_FILE USBIP_INITIAL_PASSWORD_FILE \
    USBIP_METADATA_FILE USBIP_CLIENTS_FILE USBIP_QUEUE_FILE USBIP_NOTIFY_FILE \
    USBIP_PIDFILE USBIP_MANAGED_FILE USBIP_ACCESS_FILE USBSHARE_PORTS_FILE
