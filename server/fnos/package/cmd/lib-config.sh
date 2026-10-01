#!/bin/bash
# USB-SHARE 飞牛应用：向导值 → 容器配置文件。
# 由 cmd/install_callback、cmd/upgrade_callback、cmd/config_callback、cmd/main 共用。
#
# 设计要点：
#   1. 容器不依赖 fnOS 对 docker-compose.yaml 的变量替换。端口这类由向导决定的参数
#      写在 <appconf>/usbip-share.env 里，容器启动时自己读（见 app/docker/entrypoint-fnos.sh），
#      所以 compose 文件是静态的，不需要为"端口可配置"做任何模板化。
#   2. 文本/密码字段留空 = 保持当前值；端口在 wizard/config 里是 required，不会为空。
#   3. 所有写入都是"临时文件 + mv"，避免容器读到半截文件。

USBSHARE_APPCONF="${TRIM_PKGETC:-/var/apps/${TRIM_APPNAME:-usbip-share}/etc}"
USBSHARE_VARDIR="${TRIM_PKGVAR:-/var/apps/${TRIM_APPNAME:-usbip-share}/var}"
USBSHARE_APPDEST="${TRIM_APPDEST:-/var/apps/${TRIM_APPNAME:-usbip-share}/target}"
USBSHARE_ENV_FILE="${USBSHARE_APPCONF}/usbip-share.env"
USBSHARE_LOG_FILE="${USBSHARE_VARDIR}/config.log"
USBSHARE_COMPOSE_FILE="${USBSHARE_APPDEST}/docker/docker-compose.yaml"
USBSHARE_FALLBACK_CONTAINER="usbip-share-server"

usbshare_log() {
    mkdir -p "${USBSHARE_VARDIR}" 2>/dev/null || true
    printf '%s [usbip-share] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"${USBSHARE_LOG_FILE}" 2>/dev/null || true
}

# 去掉回车换行与首尾空白：向导值会原样进 env 文件，多行内容会破坏解析。
usbshare_clean() {
    printf '%s' "$1" | tr -d '\r\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

# 读取当前生效值；文件不存在或值为空时用 fallback。
usbshare_current() {
    local key="$1" fallback="$2" value=""
    if [ -f "${USBSHARE_ENV_FILE}" ]; then
        value=$(sed -n "s/^${key}=//p" "${USBSHARE_ENV_FILE}" | tail -n 1)
    fi
    if [ -z "${value}" ]; then
        printf '%s' "${fallback}"
    else
        printf '%s' "${value}"
    fi
}

usbshare_valid_port() {
    case "$1" in
        '' | *[!0-9]*) return 1 ;;
    esac
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

# switch 字段在不同版本里可能是布尔或字符串；空/无法识别 = 保持当前值。
usbshare_pick_bool() {
    local raw="$1" fallback="$2"
    case "${raw}" in
        true | True | TRUE | 1 | yes | on) printf 'true' ;;
        false | False | FALSE | 0 | no | off) printf 'false' ;;
        *) printf '%s' "${fallback}" ;;
    esac
}

usbshare_pick_port() {
    if usbshare_valid_port "$1"; then printf '%s' "$1"; else printf '%s' "$2"; fi
}

usbshare_pick_seconds() {
    case "$1" in
        '') printf '%s' "$2" ;;
        *[!0-9]*) printf '%s' "$2" ;;
        *) printf '%s' "$1" ;;
    esac
}

usbshare_write_env() {
    local port="$1" password="$2" busids="$3" load_module="$4" unbind_on_exit="$5" kick_idle="$6"
    local restore_shared="$7" access_password="$8"
    local tmp

    mkdir -p "${USBSHARE_APPCONF}" 2>/dev/null || true
    tmp=$(mktemp "${USBSHARE_APPCONF}/.usbip-share.env.XXXXXX" 2>/dev/null) || return 1

    umask 077
    {
        printf '# 由 USB-SHARE 的 cmd/install_callback、cmd/config_callback 生成，请勿手工编辑。\n'
        printf '# 容器启动时由 entrypoint-fnos.sh 读取。\n'
        printf 'USBIP_PORT=%s\n' "${port}"
        printf 'USBIP_BUSIDS=%s\n' "${busids}"
        printf 'USBIP_LOAD_MODULE=%s\n' "${load_module}"
        printf 'USBIP_UNBIND_ON_EXIT=%s\n' "${unbind_on_exit}"
        printf 'USBIP_KICK_IDLE_SECONDS=%s\n' "${kick_idle}"
        printf 'USBIP_RESTORE_SHARED=%s\n' "${restore_shared}"
        printf 'USBIP_WEB_PASSWORD=%s\n' "${password}"
        printf '# 共享访问密码：非空时客户端必须先用它换取授权才能连 USB/IP 数据端口。\n'
        printf 'USBIP_ACCESS_PASSWORD=%s\n' "${access_password}"
    } >"${tmp}" 2>/dev/null || {
        rm -f "${tmp}"
        return 1
    }

    chmod 0600 "${tmp}" 2>/dev/null || true
    mv -f "${tmp}" "${USBSHARE_ENV_FILE}" 2>/dev/null || {
        rm -f "${tmp}"
        return 1
    }
    chmod 0600 "${USBSHARE_ENV_FILE}" 2>/dev/null || true
    return 0
}

# 把当前向导环境里的值合并进配置文件。留空/非法的字段沿用旧值。
usbshare_apply_wizard() {
    # 注意：向导字段是环境变量 port/admin_password/...，
    # 下面先快照再声明 local，否则 local 会遮蔽同名的全局向导值。
    local w_port="${port:-}"
    local w_password="${admin_password:-}"
    local w_load="${load_module:-}"
    local w_unbind="${unbind_on_exit:-}"
    local w_restore="${restore_shared:-}"
    local w_kick="${kick_idle_seconds:-}"
    local w_access_enabled="${access_enabled:-}"
    local w_access_password="${access_password:-}"
    local new_port new_password new_busids new_load new_unbind new_restore new_kick
    local new_access_password cur_access_password cur_access_enabled password_state access_state

    w_port=$(usbshare_clean "${w_port}")
    w_password=$(usbshare_clean "${w_password}")
    w_kick=$(usbshare_clean "${w_kick}")

    new_port=$(usbshare_pick_port "${w_port}" "$(usbshare_current USBIP_PORT 5555)")

    if [ -n "${w_password}" ]; then
        new_password="${w_password}"
    else
        new_password=$(usbshare_current USBIP_WEB_PASSWORD '')
    fi

    # busids 不再由向导收集：设备是装完之后才插上的，向导里没法列出来，
    # 让用户手敲 busid 也不合理。默认行为改成"重启后自动恢复上次共享的设备"
    # （见 entrypoint-fnos.sh 的 USBIP_RESTORE_SHARED）。仍可在 env 文件里手工
    # 写 USBIP_BUSIDS 预置一批设备，这里原样保留。
    new_busids=$(usbshare_current USBIP_BUSIDS '')

    new_load=$(usbshare_pick_bool "${w_load}" "$(usbshare_current USBIP_LOAD_MODULE true)")
    new_unbind=$(usbshare_pick_bool "${w_unbind}" "$(usbshare_current USBIP_UNBIND_ON_EXIT false)")
    new_restore=$(usbshare_pick_bool "${w_restore}" "$(usbshare_current USBIP_RESTORE_SHARED true)")
    new_kick=$(usbshare_pick_seconds "${w_kick}" "$(usbshare_current USBIP_KICK_IDLE_SECONDS 60)")

    # 共享访问密码。用一个显式开关决定"要不要启用"，而不是靠"留空=不变"——
    # 否则用户没有办法把它关掉（关掉 = 把密码清空）。
    cur_access_password=$(usbshare_current USBIP_ACCESS_PASSWORD '')
    if [ -n "${cur_access_password}" ]; then cur_access_enabled=true; else cur_access_enabled=false; fi
    if [ "$(usbshare_pick_bool "${w_access_enabled}" "${cur_access_enabled}")" = "false" ]; then
        new_access_password=""
    elif [ -n "$(usbshare_clean "${w_access_password}")" ]; then
        new_access_password=$(usbshare_clean "${w_access_password}")
    else
        new_access_password="${cur_access_password}"
    fi

    if ! usbshare_write_env "${new_port}" "${new_password}" "${new_busids}" "${new_load}" "${new_unbind}" "${new_kick}" "${new_restore}" "${new_access_password}"; then
        usbshare_log "ERROR: 无法写入 ${USBSHARE_ENV_FILE}"
        return 1
    fi

    if [ -n "${new_password}" ]; then password_state=set; else password_state=unset; fi
    if [ -n "${new_access_password}" ]; then access_state=set; else access_state=off; fi
    usbshare_log "config applied: port=${new_port} busids='${new_busids}' load_module=${new_load} unbind_on_exit=${new_unbind} restore_shared=${new_restore} kick_idle=${new_kick} admin_password=${password_state} access_password=${access_state}"
    return 0
}

usbshare_container_name() {
    local name=""
    if [ -f "${USBSHARE_COMPOSE_FILE}" ]; then
        name=$(sed -n 's/^[[:space:]]*container_name[[:space:]]*:[[:space:]]*//p' "${USBSHARE_COMPOSE_FILE}" | head -n 1 | tr -d '\r')
        name=${name%\"}
        name=${name#\"}
        name=${name%\'}
        name=${name#\'}
    fi
    if [ -n "${name}" ]; then
        printf '%s' "${name}"
    else
        printf '%s' "${USBSHARE_FALLBACK_CONTAINER}"
    fi
}

usbshare_container_running() {
    command -v docker >/dev/null 2>&1 || return 1
    [ "$(docker inspect -f '{{.State.Running}}' "$(usbshare_container_name)" 2>/dev/null)" = "true" ]
}

# 只改配置文件不需要重建容器：容器重启时会重新读挂载进来的 env 文件。
usbshare_restart_container() {
    local name
    name=$(usbshare_container_name)

    if ! command -v docker >/dev/null 2>&1; then
        usbshare_log "WARNING: 当前用户无法调用 docker（需要加入 docker 组），配置将在下次启动时生效"
        return 1
    fi

    if ! usbshare_container_running; then
        usbshare_log "container ${name} is not running; new configuration applies on next start"
        return 0
    fi

    if docker restart "${name}" >/dev/null 2>&1; then
        usbshare_log "container ${name} restarted to apply new configuration"
        return 0
    fi

    usbshare_log "WARNING: docker restart ${name} 失败，请在应用中心手动重启本应用"
    return 1
}
