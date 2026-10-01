#!/bin/sh
# 飞牛应用包装层：把「应用设置」里的值注入环境，并固定容器内的数据路径。
#
# 被 entrypoint-fnos.sh 与 healthcheck-fnos.sh 共用（两者都是独立进程，
# 谁也拿不到对方的运行时 export，所以各自解析一次配置文件）。
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

# 逐行解析 key=value，**绝不用 `.`(source) / eval 去执行这个文件**。
#
# 为什么：env 文件里的值是原样写入的（见 cmd/lib-config.sh 的 usbshare_write_env），
# 而向导允许 6-64 位的任意字符密码。真机实测（bash/dash 都一样）：
#   USBIP_WEB_PASSWORD=abc 123456 → "123456: not found"，变量根本没被赋值；
#   USBIP_WEB_PASSWORD=pa$$word   → 被展开成 pa<PID>word，密码被静默改掉；
#   USBIP_WEB_PASSWORD=a`id`b     → 反引号里的命令在特权容器里真的被执行了。
# 更要命的是本文件带 set -e，那条 "not found" 会让入口脚本直接以 127 退出，
# 容器反复重启且日志里只有一行 "123456: not found"。
# 这里只做"取第一个 = 之后的整行原文"，不做任何展开。
#
# 文件里每一个变量名合法的行都会被导出，所以「手工往 env 文件里加一行
# USBIP_ALLOW_NETDEV=true」这种逃生阀用法（见 README）照旧生效。
usbshare_load_env_file() {
    _usbshare_env_file="$1"
    [ -f "${_usbshare_env_file}" ] || return 0
    _usbshare_env_cr=$(printf '\r')
    while IFS= read -r _usbshare_env_line || [ -n "${_usbshare_env_line}" ]; do
        # Windows 编辑器（README 鼓励手工编辑这个文件）会写成 CRLF。末尾的 CR
        # 会让 "true"/"5555" 这类值静默匹配失败（例如 USBIP_LOAD_MODULE=true\r
        # 不会被当成 true），所以统一剥掉。
        _usbshare_env_line=${_usbshare_env_line%"${_usbshare_env_cr}"}
        case "${_usbshare_env_line}" in
            '' | '#'*) continue ;;
        esac
        case "${_usbshare_env_line}" in
            *=*) ;;
            *)
                printf '%s\n' "[usbip-share] WARNING: ${_usbshare_env_file}: 忽略无法解析的行: ${_usbshare_env_line}" >&2
                continue
                ;;
        esac
        _usbshare_env_key=$(printf '%s' "${_usbshare_env_line%%=*}" | tr -d '[:blank:]')
        _usbshare_env_value=${_usbshare_env_line#*=}
        case "${_usbshare_env_key}" in
            '' | [0-9]* | *[!A-Za-z0-9_]*)
                printf '%s\n' "[usbip-share] WARNING: ${_usbshare_env_file}: 忽略变量名非法的行: ${_usbshare_env_line}" >&2
                continue
                ;;
        esac
        export "${_usbshare_env_key}=${_usbshare_env_value}"
    done <"${_usbshare_env_file}"
    return 0
}

# 配置文件缺失/读不了都不能"带默认值继续跑"：那等于把访问密码静默改成"不启用"、
# 把端口退回 5555，而界面上看不出任何异常（fail-open）。
if [ ! -e "${USBSHARE_CONF}" ]; then
    # 正常情况下 cmd/install_callback 会在容器第一次启动前写好这个文件。
    # 用 <appdata>/config.log 判断"这台机器上确实配置过"（install_callback 一定会
    # 写它）：是的话拒绝启动，而不是静默降级。
    if [ -e /data/config.log ]; then
        printf '%s\n' "[usbip-share] ERROR: ${USBSHARE_CONF} 不存在，但 /data/config.log 说明本应用已经配置过。拒绝用默认值（无共享访问密码、端口 5555）启动；请在飞牛「应用中心 → USB-SHARE → 应用设置」里保存一次以重建该文件。" >&2
        exit 1
    fi
    printf '%s\n' "[usbip-share] WARNING: ${USBSHARE_CONF} not found; falling back to defaults" >&2
elif [ ! -r "${USBSHARE_CONF}" ]; then
    printf '%s\n' "[usbip-share] ERROR: ${USBSHARE_CONF} 存在但读不了（属主或权限不对）。拒绝用默认值启动——那会静默关掉共享访问密码。请修正属主/权限后重试。" >&2
    exit 1
fi
usbshare_load_env_file "${USBSHARE_CONF}"

# 对外端口：USB/IP 数据协议与中文管理页共用的那一个。这是唯一对外暴露的端口。
USBIP_PORT=${USBIP_PORT:-5555}
USBIP_GATEWAY_HOST=${USBIP_GATEWAY_HOST:-0.0.0.0}

# 内部端口：上一次启动时挑好的空闲端口（entrypoint 写，healthcheck 读）。
# 允许在应用设置里显式指定（USBIP_INNER_USBIPD_PORT / USBIP_WEB_PORT）来覆盖。
usbshare_load_env_file "${USBSHARE_PORTS_FILE}"
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
