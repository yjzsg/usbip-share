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
: "${USBIP_PIDFILE:=/run/usbip/usbipd.pid}"
: "${USBIP_MANAGED_FILE:=/run/usbip/managed-busids}"

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

bind_one() {
    busid="$1"
    case "$busid" in
        ''|*[!A-Za-z0-9_.:-]*) fail "invalid USB bus ID: $busid" ;;
    esac

    # A bound device appears as a symlink in the usbip-host driver directory.
    if [ -e "/sys/bus/usb/drivers/usbip-host/$busid" ]; then
        log "already bound: $busid"
        remember_managed "$busid"
        return 0
    fi

    log "binding USB device: $busid"
    usbip bind -b "$busid" || fail "cannot bind $busid; verify the bus ID and that no other driver owns it"
    remember_managed "$busid"
}

# USBIP_BUSIDS is a comma-separated list, e.g. "1-1,1-2.3".
for busid in $(printf '%s' "$USBIP_BUSIDS" | tr ',' ' '); do
    bind_one "$busid"
done

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
    python3 /opt/usbip-share/gateway.py --host 0.0.0.0 --port "$USBIP_PORT" \
        --usbip-port "$USBIP_INNER_USBIPD_PORT" --web-port "$USBIP_WEB_PORT" &
GATEWAY_PID=$!
sleep 1
kill -0 "$GATEWAY_PID" 2>/dev/null || fail "single-port gateway did not start"

if [ -n "$USBIP_BUSIDS" ]; then
    log "USB/IP server is ready; initially exported bus IDs: $USBIP_BUSIDS"
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
