#!/bin/sh
# 飞牛 fnOS 应用的容器入口。按顺序做三件事，然后交给原 entrypoint：
#   1. 读应用设置、挑好内部空闲端口；
#   2. 恢复上次共享的设备（可选）；
#   3. 起一个 Unix Socket 转发器，供飞牛统一网关（桌面小窗入口）访问管理页。
#
# 原 entrypoint（usbip-share-entrypoint）在 server/entrypoint.sh，
# 负责 modprobe、usbipd、web.py、单端口网关与进程监管。
set -eu

# shellcheck disable=SC1091
. /usr/local/bin/usbip-share-fnos-env.sh

mkdir -p /data

# 容器用 host 网络，内部端口就是宿主端口，所以不能写死：
#   * 沿用上一次挑好的端口（已记在 /data/internal-ports.env），前提是它们仍然空闲；
#   * 否则重新挑，并写回文件，让 healthcheck 读到同一组。
#
# 另外必须避开对外端口 USBIP_PORT：挑端口的时候网关还没起来，对外端口在
# 宿主上此刻是空闲的，而 pick_free_port 只会从内核临时端口范围（本机
# 32768-60999，见 /proc/sys/net/ipv4/ip_local_port_range）里取——用户把
# 应用端口设成这个区间里的值（例如 45000）时就有可能撞上。撞上以后网关
# EADDRINUSE 起不来，容器退出；重启时 /data/internal-ports.env 里存的还是
# 那个值、又判定"空闲"，于是变成永远起不来的重启循环。这里显式排除。
if [ -n "${USBIP_INNER_USBIPD_PORT}" ] && [ "${USBIP_INNER_USBIPD_PORT}" != "${USBIP_PORT}" ] \
    && usbshare_port_free "${USBIP_INNER_USBIPD_PORT}"; then
    :
else
    USBIP_INNER_USBIPD_PORT=$(usbshare_pick_free_port)
    printf '%s\n' "[usbip-share] picked internal usbipd port ${USBIP_INNER_USBIPD_PORT}" >&2
fi

if [ -n "${USBIP_WEB_PORT}" ] && [ "${USBIP_WEB_PORT}" != "${USBIP_PORT}" ] \
    && [ "${USBIP_WEB_PORT}" != "${USBIP_INNER_USBIPD_PORT}" ] \
    && usbshare_port_free "${USBIP_WEB_PORT}"; then
    :
else
    USBIP_WEB_PORT=$(usbshare_pick_free_port)
    printf '%s\n' "[usbip-share] picked internal web port ${USBIP_WEB_PORT}" >&2
fi

# pick_free_port 有极小概率正好挑中对外端口（或与 usbipd 端口相同），这里兜住。
while [ "${USBIP_INNER_USBIPD_PORT}" = "${USBIP_PORT}" ]; do
    USBIP_INNER_USBIPD_PORT=$(usbshare_pick_free_port)
done
while [ "${USBIP_WEB_PORT}" = "${USBIP_PORT}" ] || [ "${USBIP_WEB_PORT}" = "${USBIP_INNER_USBIPD_PORT}" ]; do
    USBIP_WEB_PORT=$(usbshare_pick_free_port)
done

umask 077
{
    printf '# 由 entrypoint-fnos.sh 在容器启动时写入，healthcheck 读取同一份。\n'
    printf 'USBIP_INNER_USBIPD_PORT=%s\n' "${USBIP_INNER_USBIPD_PORT}"
    printf 'USBIP_WEB_PORT=%s\n' "${USBIP_WEB_PORT}"
} >"${USBSHARE_PORTS_FILE}.tmp"
mv -f "${USBSHARE_PORTS_FILE}.tmp" "${USBSHARE_PORTS_FILE}"

# 重启后恢复上次共享的设备。
# 管理页每共享/取消一台设备都会维护 /data/managed-busids（web.py 的
# remember_managed/forget_managed），所以这份文件就是"当前应该共享哪些设备"的
# 真相。原 entrypoint 只认 USBIP_BUSIDS 这个环境变量，这里把文件内容填进去，
# 用户就不必在应用设置里手敲 busid —— 在管理页点一次「开始共享」就够。
if [ "${USBIP_RESTORE_SHARED:-true}" = "true" ] && [ -z "${USBIP_BUSIDS:-}" ] && [ -s /data/managed-busids ]; then
    USBIP_BUSIDS=$(tr '\n' ',' </data/managed-busids | sed -e 's/,,*/,/g' -e 's/,$//')
    printf '%s\n' "[usbip-share] restoring previously shared devices: ${USBIP_BUSIDS}" >&2
fi
export USBIP_BUSIDS

# 飞牛统一网关把 /app/usbip-share/* 转发到应用 target 目录下的 app.sock。
# 转发器是纯 Python 标准库实现，跟 web.py 一样不引入第三方依赖。
USBSHARE_GATEWAY_PREFIX="${USBSHARE_GATEWAY_PREFIX:-/app/usbip-share}"
USBSHARE_GATEWAY_SOCKET="${USBSHARE_GATEWAY_SOCKET:-/appdest/app.sock}"
USBSHARE_UPSTREAM_HOST="${USBSHARE_UPSTREAM_HOST:-127.0.0.1}"
USBSHARE_UPSTREAM_PORT="${USBIP_WEB_PORT}"
export USBSHARE_GATEWAY_PREFIX USBSHARE_GATEWAY_SOCKET USBSHARE_UPSTREAM_HOST USBSHARE_UPSTREAM_PORT

# 转发器挂了不影响 USB/IP 主功能，但也别让它静默死掉，所以外面套一层重试。
(
    while true; do
        python3 /opt/usbip-share/fnos-unix-proxy.py || true
        sleep 2
    done
) &

exec /usr/local/bin/usbip-share-entrypoint
