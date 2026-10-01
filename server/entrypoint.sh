#!/bin/sh
# USB/IP server entrypoint for Linux hosts.
# The host kernel must provide usbip-core and usbip-host.
set -eu

log() {
    printf '%s\n' "[usbip-share] $*" >&2
}

fail() {
    log "ERROR: $*"
    exit 1
}

: "${USBIP_PORT:=5555}"
: "${USBIP_INNER_USBIPD_PORT:=5556}"
: "${USBIP_BUSIDS:=}"
: "${USBIP_LOAD_MODULE:=true}"
: "${USBIP_UNBIND_ON_EXIT:=false}"
: "${USBIPD_DEBUG:=false}"
: "${USBIP_WEB_ENABLED:=true}"
: "${USBIP_WEB_HOST:=0.0.0.0}"
: "${USBIP_WEB_PORT:=8080}"
# 对外单端口网关的监听地址;默认 0.0.0.0 保持既有行为,可用环境变量收紧。
: "${USBIP_GATEWAY_HOST:=0.0.0.0}"
: "${USBIP_PIDFILE:=/run/usbip/usbipd.pid}"
: "${USBIP_MANAGED_FILE:=/run/usbip/managed-busids}"
# 共享访问密码的授权表:web.py 写,gateway.py 读。文件存在 = 已启用访问控制。
# USBIP_ACCESS_PASSWORD 为空(默认)时 web.py 会删掉它,网关放行一切。
: "${USBIP_ACCESS_FILE:=/run/usbip/authorized-clients.json}"
# PROXY 行的校验秘密:网关补的那行 PROXY v1 头本身没有认证,谁先连上 web.py 的
# 端口谁就能声称自己是任意来源 IP(web.py 只能看到对端是回环,分不清"网关"和
# "本机上任意进程")。两边用同一个秘密派生一个标记,web.py 只认带标记的行。
# 没显式设置时这里生成一个随机值;用户也可以自己指定(会覆盖)。
# 注意:这个变量必须同时传给 web.py 与 gateway.py —— 两个子进程都继承本脚本的
# 环境,所以 export 一次即可。改了它只换掉校验标记,不影响已经建立的授权。
: "${USBIP_PROXY_SECRET:=}"
if [ -z "$USBIP_PROXY_SECRET" ]; then
    USBIP_PROXY_SECRET=$(cat /proc/sys/kernel/random/uuid 2>/dev/null || true)
fi
if [ -z "$USBIP_PROXY_SECRET" ]; then
    # 极老的内核/受限容器没有 random/uuid:退化成"时间戳+pid"。这不是密码学随机,
    # 但攻击者要猜的只是"能不能直连 web 端口",暴露面本身就不该存在。
    USBIP_PROXY_SECRET="fallback-$$-$(date +%s%N 2>/dev/null || date +%s)"
fi
export USBIP_PROXY_SECRET

case "$USBIP_PORT" in
    ''|*[!0-9]*) fail "USBIP_PORT must be a number: $USBIP_PORT" ;;
esac
if [ "$USBIP_PORT" -lt 1 ] || [ "$USBIP_PORT" -gt 65535 ]; then
    fail "USBIP_PORT must be between 1 and 65535"
fi

case "$USBIP_INNER_USBIPD_PORT" in
    ''|*[!0-9]*) fail "USBIP_INNER_USBIPD_PORT must be a number: $USBIP_INNER_USBIPD_PORT" ;;
esac
if [ "$USBIP_INNER_USBIPD_PORT" -lt 1 ] || [ "$USBIP_INNER_USBIPD_PORT" -gt 65535 ]; then
    fail "USBIP_INNER_USBIPD_PORT must be between 1 and 65535"
fi

if [ "$USBIP_WEB_ENABLED" = "true" ] || [ "$USBIP_WEB_ENABLED" = "1" ]; then
    case "$USBIP_WEB_PORT" in
        ''|*[!0-9]*) fail "USBIP_WEB_PORT must be a number: $USBIP_WEB_PORT" ;;
    esac
    if [ "$USBIP_WEB_PORT" -lt 1 ] || [ "$USBIP_WEB_PORT" -gt 65535 ]; then
        fail "USBIP_WEB_PORT must be between 1 and 65535"
    fi
    if [ "$USBIP_INNER_USBIPD_PORT" = "$USBIP_WEB_PORT" ]; then
        fail "USBIP_INNER_USBIPD_PORT and USBIP_WEB_PORT must differ"
    fi
    command -v python3 >/dev/null 2>&1 || fail "python3 is missing; it is required for the Chinese management UI"
fi

if [ ! -d /dev/bus/usb ]; then
    fail "/dev/bus/usb is not available; map the host USB bus into the container"
fi

if ! command -v usbip >/dev/null 2>&1 || ! command -v usbipd >/dev/null 2>&1; then
    fail "usbip/usbipd is missing from the image"
fi

if [ "$USBIP_LOAD_MODULE" = "true" ] || [ "$USBIP_LOAD_MODULE" = "1" ]; then
    if ! grep -q '^usbip_host ' /proc/modules 2>/dev/null; then
        log "loading host kernel module usbip_host"
        modprobe usbip_core 2>/dev/null || true
        modprobe usbip_host || fail "cannot load usbip_host; verify the Linux kernel and /lib/modules mapping"
    fi
fi

# Sharing is deliberately opt-in. An empty list starts the server with no
# exported devices; the Chinese web page can then share devices one by one.
remember_managed() {
    mkdir -p "$(dirname "$USBIP_MANAGED_FILE")"
    if ! grep -Fqx "$1" "$USBIP_MANAGED_FILE" 2>/dev/null; then
        printf '%s\n' "$1" >> "$USBIP_MANAGED_FILE"
    fi
}

forget_managed() {
    mkdir -p "$(dirname "$USBIP_MANAGED_FILE")"
    [ -f "$USBIP_MANAGED_FILE" ] || return 0
    grep -Fvx "$1" "$USBIP_MANAGED_FILE" > "$USBIP_MANAGED_FILE.new" 2>/dev/null || true
    mv "$USBIP_MANAGED_FILE.new" "$USBIP_MANAGED_FILE" 2>/dev/null || true
}

# 共享一块承载宿主网络的 USB 网卡 = NAS 自己掉线(连远端 SSH 一起断,可能再也连不回来)。
# 用接口目录下的 net/ 判断是不是网卡:设备类不可靠(RTL8156 报的是私有类 0xff)。
# 只要有一个接口目录里存在 net/ 就拒绝;USBIP_ALLOW_NETDEV=true 才放行(危险开关)。
exports_network_interface() {
    [ -n "$(ls -d /sys/bus/usb/devices/"$1":*/net/* 2>/dev/null | head -n 1)" ]
}

netdev_guard_enabled() {
    [ "${USBIP_ALLOW_NETDEV:-false}" != "true" ]
}

# 默认路由是否还在:driver 无关的兜底判据(容器里没有 ip 命令,/proc/net/route 可读)。
default_route_present() {
    awk 'NR > 1 && $2 == "00000000" { found = 1 } END { exit !found }' /proc/net/route 2>/dev/null
}

# 本轮真正 bind 成功的 busid;回滚只针对本轮动作,不会去动别人。
ROUND_BOUND=""

bind_one() {
    busid="$1"
    case "$busid" in
        ''|*[!A-Za-z0-9_.:-]*) fail "invalid USB bus ID: $busid" ;;
    esac

    if netdev_guard_enabled && exports_network_interface "$busid"; then
        # return 0: 一个坏条目不能让整个容器起不来,但也绝不 bind。
        log "REFUSING to bind $busid: it exports a network interface; sharing it would drop the host off the network"
        return 0
    fi

    # A bound device appears as a symlink in the usbip-host driver directory.
    if [ -e "/sys/bus/usb/drivers/usbip-host/$busid" ]; then
        log "already bound: $busid"
        remember_managed "$busid"
        return 0
    fi

    log "binding USB device: $busid"
    usbip bind -b "$busid" || fail "cannot bind $busid; verify the bus ID and that no other driver owns it"
    ROUND_BOUND="$ROUND_BOUND $busid"
    remember_managed "$busid"
}

# USBIP_BUSIDS is a comma-separated list, e.g. "1-1,1-2.3".
if [ -n "$USBIP_BUSIDS" ]; then
    route_before=false
    if default_route_present; then
        route_before=true
    fi
    for busid in $(printf '%s' "$USBIP_BUSIDS" | tr ',' ' '); do
        bind_one "$busid"
    done
    # 兜底:网卡检测失效时(例如设备已被 usbip-host 绑着、net/ 已消失),
    # 用"默认路由还在不在"判断刚才是不是把宿主的网络搞断了。
    if [ "$route_before" = "true" ] && ! default_route_present; then
        log "ERROR: 绑定设备后宿主默认路由消失，已回滚；请检查是不是共享了宿主在用的 USB 网卡"
        for busid in $ROUND_BOUND; do
            log "rolling back: unbinding $busid"
            usbip unbind -b "$busid" >/dev/null 2>&1 || true
            # 别再留在 managed 列表里让下一次重启又绑一遍。
            forget_managed "$busid"
        done
        ROUND_BOUND=""
    fi
fi

WEB_PID=""
USBIPD_PID=""
GATEWAY_PID=""
cleanup() {
    trap - TERM INT HUP EXIT
    if [ -n "$GATEWAY_PID" ] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
        log "stopping single-port gateway (pid $GATEWAY_PID)"
        kill "$GATEWAY_PID" 2>/dev/null || true
    fi
    if [ -n "$WEB_PID" ] && kill -0 "$WEB_PID" 2>/dev/null; then
        log "stopping Chinese management UI (pid $WEB_PID)"
        kill "$WEB_PID" 2>/dev/null || true
    fi
    if [ -n "$USBIPD_PID" ] && kill -0 "$USBIPD_PID" 2>/dev/null; then
        log "stopping usbipd (pid $USBIPD_PID)"
        kill "$USBIPD_PID" 2>/dev/null || true
    fi

    if [ "$USBIP_UNBIND_ON_EXIT" = "true" ] || [ "$USBIP_UNBIND_ON_EXIT" = "1" ]; then
        if [ -f "$USBIP_MANAGED_FILE" ]; then
            while IFS= read -r busid || [ -n "$busid" ]; do
                [ -n "$busid" ] || continue
                if [ -e "/sys/bus/usb/drivers/usbip-host/$busid" ]; then
                    log "unbinding USB device: $busid"
                    usbip unbind -b "$busid" >/dev/null 2>&1 || true
                fi
            done < "$USBIP_MANAGED_FILE"
        fi
    fi
}
trap cleanup TERM INT HUP EXIT

mkdir -p "$(dirname "$USBIP_PIDFILE")"
rm -f "$USBIP_PIDFILE"

# usbipd 只监听容器内部端口;对外统一走下面的单端口网关。
set -- usbipd --daemon --tcp-port "$USBIP_INNER_USBIPD_PORT" --pid="$USBIP_PIDFILE"
if [ "$USBIPD_DEBUG" = "true" ] || [ "$USBIPD_DEBUG" = "1" ]; then
    set -- "$@" --debug
fi

log "starting usbipd on internal TCP port $USBIP_INNER_USBIPD_PORT"
"$@"

# usbipd forks in daemon mode. Wait for its pid file and then supervise it.
i=0
while [ ! -s "$USBIP_PIDFILE" ] && [ "$i" -lt 10 ]; do
    sleep 1
    i=$((i + 1))
done

[ -s "$USBIP_PIDFILE" ] || fail "usbipd did not create $USBIP_PIDFILE"
USBIPD_PID=$(sed -n '1p' "$USBIP_PIDFILE")
case "$USBIPD_PID" in
    ''|*[!0-9]*) fail "invalid usbipd pid file: $USBIP_PIDFILE" ;;
esac

if [ "$USBIP_WEB_ENABLED" = "true" ] || [ "$USBIP_WEB_ENABLED" = "1" ]; then
    log "starting Chinese management UI on TCP port $USBIP_WEB_PORT (internal)"
    USBIP_PORT="$USBIP_PORT" \
    USBIP_MANAGED_FILE="$USBIP_MANAGED_FILE" \
    USBIP_ACCESS_FILE="$USBIP_ACCESS_FILE" \
    python3 /opt/usbip-share/web.py --host "$USBIP_WEB_HOST" --port "$USBIP_WEB_PORT" &
    WEB_PID=$!
    sleep 1
    kill -0 "$WEB_PID" 2>/dev/null || fail "Chinese management UI did not start"
fi

# 单端口网关(Phase B):对外 $USBIP_PORT 一个端口,按首字节分流
# USB/IP(usbipd)与 HTTP(web.py/管理接口/网页)。
log "starting single-port gateway on TCP port $USBIP_PORT"
USBIP_PORT="$USBIP_PORT" \
USBIP_INNER_USBIPD_PORT="$USBIP_INNER_USBIPD_PORT" \
USBIP_WEB_PORT="$USBIP_WEB_PORT" \
USBIP_ACCESS_FILE="$USBIP_ACCESS_FILE" \
    python3 /opt/usbip-share/gateway.py --host "$USBIP_GATEWAY_HOST" --port "$USBIP_PORT" \
        --usbip-port "$USBIP_INNER_USBIPD_PORT" --web-port "$USBIP_WEB_PORT" \
        --access-file "$USBIP_ACCESS_FILE" &
GATEWAY_PID=$!
sleep 1
kill -0 "$GATEWAY_PID" 2>/dev/null || fail "single-port gateway did not start"

if [ -n "$ROUND_BOUND" ]; then
    log "USB/IP server is ready; initially exported bus IDs:$ROUND_BOUND"
elif [ -n "$USBIP_BUSIDS" ]; then
    # 一条都没绑上:要么被网卡拦截挡了,要么本来就已经处于绑定状态。
    log "USB/IP server is ready; USBIP_BUSIDS=$USBIP_BUSIDS produced no new exports (refused by the netdev guard or already bound)"
else
    log "USB/IP server is ready; no devices are shared yet"
fi
log "single external port: $USBIP_PORT (USB/IP + 管理接口/网页合一; 无需再单独开放管理端口)"

while kill -0 "$USBIPD_PID" 2>/dev/null && kill -0 "$GATEWAY_PID" 2>/dev/null && { [ -z "$WEB_PID" ] || kill -0 "$WEB_PID" 2>/dev/null; }; do
    sleep 5
done

if [ -n "$WEB_PID" ] && ! kill -0 "$WEB_PID" 2>/dev/null; then
    log "Chinese management UI exited unexpectedly"
elif ! kill -0 "$GATEWAY_PID" 2>/dev/null; then
    log "single-port gateway exited unexpectedly"
else
    log "usbipd exited unexpectedly"
fi
exit 1
