#!/bin/sh
# 入口脚本「绝不 bind USB 网卡」的回归测试。
#
# 真机事故：USBIP_BUSIDS（或 fnOS 从 managed-busids 自动恢复出来的列表）里混进
# 2-7 这块承载宿主默认路由的 USB 网卡 → 自动共享 → NAS 掉网，连远端 SSH 一起断。
# 这里把 entrypoint.sh 里的 bind_one() / exports_network_interface() /
# netdev_guard_enabled() / default_route_present() / remember_managed() /
# forget_managed() 原样抽出来跑（只把 sysfs 根、usbip-host 驱动目录、路由表换成
# 临时路径；Windows 文件名不能含 ':'，接口目录分隔符随之换成 '-'），断言：
#
#   * 有 net/ 的网卡：绝不调用 usbip bind（哪怕它已经处于绑定状态）
#   * USBIP_ALLOW_NETDEV=true 时才放行
#   * 非网卡：照常 bind
#   * 默认路由判据：读不到/只有表头都算"没有默认路由"（安全方向，不会误回滚）
#
# 用法（任何有 POSIX sh 的机器，容器内也可以）：
#     sh tests/test_entrypoint_netdev_guard.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ENTRYPOINT="$HERE/../entrypoint.sh"
[ -f "$ENTRYPOINT" ] || { echo "找不到 entrypoint.sh: $ENTRYPOINT" >&2; exit 1; }

TMP=$(mktemp -d 2>/dev/null) || { echo "SKIP: 无法创建临时目录"; exit 0; }
trap 'rm -rf "$TMP"' EXIT INT TERM

SYSFS="$TMP/sysfs"
DRIVERS="$TMP/usbip-host"
ROUTE="$TMP/route"
USBIP_MANAGED_FILE="$TMP/managed-busids"
USBIP_LOG="$TMP/usbip.log"
export SYSFS DRIVERS ROUTE USBIP_MANAGED_FILE USBIP_LOG

# Windows/MSYS 下文件名不能含 ':'，接口目录分隔符换成 '-'。
SEP=":"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) SEP="-" ;;
esac

extract() {
    awk -v fn="$1" '
        $0 ~ "^" fn "\\(\\) \\{" { inside = 1 }
        inside { print }
        inside && $0 == "}" { exit }
    ' "$ENTRYPOINT"
}

{
    for fn in log fail remember_managed forget_managed exports_network_interface \
              netdev_guard_enabled default_route_present bind_one; do
        body=$(extract "$fn")
        [ -n "$body" ] || { echo "FAIL 无法从 entrypoint.sh 抽出 $fn()" >&2; exit 1; }
        printf '%s\n\n' "$body"
    done
} > "$TMP/funcs.sh"

# 换成临时路径（只改路径与分隔符，逻辑一字不动）。
sed -e "s|/sys/bus/usb/devices|$SYSFS|g" \
    -e "s|/sys/bus/usb/drivers/usbip-host|$DRIVERS|g" \
    -e "s|/proc/net/route|$ROUTE|g" \
    "$TMP/funcs.sh" > "$TMP/funcs-rewritten.sh"
if [ "$SEP" != ":" ]; then
    sed 's|":\*/net/\*|"-*/net/*|' "$TMP/funcs-rewritten.sh" > "$TMP/funcs-final.sh"
else
    cp "$TMP/funcs-rewritten.sh" "$TMP/funcs-final.sh"
fi

mkdir -p "$SYSFS/2-7${SEP}1.0/net/enxc84d44294124" "$SYSFS/1-8.2${SEP}1.0" "$DRIVERS"
: > "$USBIP_LOG"

results=0
failed=0
check() {
    if [ "$2" = "1" ]; then
        echo "  PASS $1"
        results=$((results + 1))
    else
        echo "  FAIL $1"
        results=$((results + 1))
        failed=$((failed + 1))
    fi
}

# 让 bind_one 看到真实的日志/失败行为，但不真的退出容器。
log() { printf '%s\n' "[test] $*" >&2; }
fail() { log "ERROR: $*"; exit 1; }
# 假 usbip：只记录调用。
usbip() { printf '%s\n' "$*" >> "$USBIP_LOG"; }
ROUND_BOUND=""

# shellcheck source=/dev/null
. "$TMP/funcs-final.sh"

usbip_calls() { cat "$USBIP_LOG" 2>/dev/null; }

echo "[A] 网卡检测（net/ 判据）"
if exports_network_interface 2-7; then check "2-7 有 net/ -> 是网卡" "1"; else check "2-7 有 net/ -> 是网卡" "0"; fi
if exports_network_interface 1-8.2; then check "1-8.2 无 net/ -> 不是网卡" "0"; else check "1-8.2 无 net/ -> 不是网卡" "1"; fi
if exports_network_interface 9-9; then check "不存在的 busid -> 不是网卡(不报错)" "0"; else check "不存在的 busid -> 不是网卡(不报错)" "1"; fi

echo "[B] bind_one：网卡绝不 bind"
USBIP_ALLOW_NETDEV=false
: > "$USBIP_LOG"
bind_one 2-7
if [ -s "$USBIP_LOG" ]; then check "网卡被拦截，没有调用 usbip" "0"; else check "网卡被拦截，没有调用 usbip" "1"; fi
if [ -s "$USBIP_MANAGED_FILE" ]; then check "被拦截的网卡没有进 managed-busids" "0"; else check "被拦截的网卡没有进 managed-busids" "1"; fi

echo "[C] 已处于绑定状态的网卡同样被拦截"
mkdir -p "$DRIVERS/2-7"
: > "$USBIP_LOG"
bind_one 2-7
if [ -s "$USBIP_LOG" ]; then check "已绑定的网卡仍然不调用 usbip" "0"; else check "已绑定的网卡仍然不调用 usbip" "1"; fi
rmdir "$DRIVERS/2-7"

echo "[D] 非网卡照常 bind"
: > "$USBIP_LOG"
bind_one 1-8.2
if grep -q '^bind -b 1-8.2$' "$USBIP_LOG"; then check "非网卡执行了 usbip bind" "1"; else check "非网卡执行了 usbip bind" "0"; fi
if grep -Fqx '1-8.2' "$USBIP_MANAGED_FILE"; then check "非网卡记入 managed-busids" "1"; else check "非网卡记入 managed-busids" "0"; fi
if [ "$ROUND_BOUND" = " 1-8.2" ]; then check "非网卡记入 ROUND_BOUND（回滚用）" "1"; else check "非网卡记入 ROUND_BOUND（回滚用）" "0"; fi

echo "[E] 逃生阀 USBIP_ALLOW_NETDEV=true"
USBIP_ALLOW_NETDEV=true
: > "$USBIP_LOG"
bind_one 2-7
if grep -q '^bind -b 2-7$' "$USBIP_LOG"; then check "逃生阀打开后允许 bind" "1"; else check "逃生阀打开后允许 bind" "0"; fi
USBIP_ALLOW_NETDEV=false
: > "$USBIP_LOG"
bind_one 2-7
if [ -s "$USBIP_LOG" ]; then check "逃生阀关闭后重新拦截" "0"; else check "逃生阀关闭后重新拦截" "1"; fi

echo "[F] 非法 busid 仍然 fail（不会静默放过）"
if ( bind_one '../etc' ) >/dev/null 2>&1; then check "非法 busid 触发 fail" "0"; else check "非法 busid 触发 fail" "1"; fi

echo "[G] 默认路由判据"
printf 'Iface\tDestination\tGateway\neth0\t00000000\t0102A8C0\neth1\t0002A8C0\t00000000\n' > "$ROUTE"
if default_route_present; then check "有默认路由 -> true" "1"; else check "有默认路由 -> true" "0"; fi
printf 'Iface\tDestination\tGateway\neth0\t0002A8C0\t00000000\n' > "$ROUTE"
if default_route_present; then check "无默认路由 -> false" "0"; else check "无默认路由 -> false" "1"; fi
printf 'Iface\tDestination\tGateway\n' > "$ROUTE"
if default_route_present; then check "只有表头 -> false" "0"; else check "只有表头 -> false" "1"; fi
rm -f "$ROUTE"
if default_route_present; then check "路由表读不到 -> false（安全方向）" "0"; else check "路由表读不到 -> false（安全方向）" "1"; fi

echo "[H] forget_managed（回滚时把网卡从 managed 列表里摘掉）"
: > "$USBIP_MANAGED_FILE"
printf '2-7\n1-1\n' >> "$USBIP_MANAGED_FILE"
forget_managed 2-7
if [ "$(cat "$USBIP_MANAGED_FILE")" = "1-1" ]; then check "只删目标行" "1"; else check "只删目标行" "0"; fi
forget_managed 9-9
if [ "$(cat "$USBIP_MANAGED_FILE")" = "1-1" ]; then check "删不存在的条目不破坏文件" "1"; else check "删不存在的条目不破坏文件" "0"; fi

echo
echo "总计: $((results - failed)) / $results 通过"
[ "$failed" -eq 0 ] || exit 1
exit 0
