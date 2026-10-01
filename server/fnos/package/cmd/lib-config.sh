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

# 读取当前生效值；文件不存在或键为空时用 fallback。
#
# 返回码 2 = 文件存在但当前用户读不了。调用方必须中止，不能继续用空值覆盖 ——
# 否则一次属主/权限错乱就会静默把管理页密码和共享访问密码清空（实测踩过：
# 用 root 改过 env 文件后属主变成 root，包用户读不到，config_callback 就把两个密码都写空了）。
usbshare_current() {
    local key="$1" fallback="$2" value=""
    if [ -f "${USBSHARE_ENV_FILE}" ]; then
        if [ ! -r "${USBSHARE_ENV_FILE}" ]; then
            usbshare_log "ERROR: ${USBSHARE_ENV_FILE} 存在但当前用户读不了（属主或权限不对）。拒绝用空值覆盖它，以免静默清掉管理页密码与共享访问密码。"
            return 2
        fi
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

# 布尔字段的三态：keep（保持不变）/ enable（打开）/ disable（关闭）。
#
# 为什么不用飞牛的 switch：真机上实测 switch 的"打开"状态**传不到脚本里** ——
# 用户在安装向导把「停止应用时归还设备」打开，install_callback 收到的仍是空值，
# 于是落回兜底 false；访问密码的开关同理，导致填好的密码被丢弃。
# 现在向导里全部用 radio 显式选择，脚本这里按三态解析。
usbshare_pick_tristate() {
    local raw="$1" fallback="$2"
    case "$(printf '%s' "${raw}" | tr 'A-Z' 'a-z')" in
        on | true | enable | enabled | yes | 1) printf 'true' ;;
        off | false | disable | disabled | no | 0) printf 'false' ;;
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
    local tmp prev_owner

    mkdir -p "${USBSHARE_APPCONF}" 2>/dev/null || true
    # 记下旧文件的属主：管理员用 root 手改过 env 之后属主会变成 root，
    # 之后包用户就再也读不到它（见 usbshare_current 的说明）。
    prev_owner=""
    if [ -e "${USBSHARE_ENV_FILE}" ]; then
        prev_owner=$(stat -c '%u:%g' "${USBSHARE_ENV_FILE}" 2>/dev/null || true)
    fi

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
    if [ -n "${prev_owner}" ] && [ "$(id -u 2>/dev/null)" = "0" ]; then
        chown "${prev_owner}" "${tmp}" 2>/dev/null || true
    fi
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
    local w_access_mode="${access_mode:-}"
    local w_access_password="${access_password:-}"
    local new_port new_password new_busids new_load new_unbind new_restore new_kick
    local new_access_password cur_access_password password_state access_state
    local cur_port cur_password cur_busids cur_load cur_unbind cur_restore cur_kick

    w_port=$(usbshare_clean "${w_port}")
    w_password=$(usbshare_clean "${w_password}")
    w_kick=$(usbshare_clean "${w_kick}")
    w_access_mode=$(usbshare_clean "${w_access_mode}")
    w_access_password=$(usbshare_clean "${w_access_password}")

    # 先把当前值一次性读出来。任何一项读失败（文件存在但读不了）就中止 ——
    # 绝不能带着空值往下走，否则会把管理页密码/共享访问密码静默清空。
    cur_port=$(usbshare_current USBIP_PORT 5555) || return 1
    cur_password=$(usbshare_current USBIP_WEB_PASSWORD '') || return 1
    cur_busids=$(usbshare_current USBIP_BUSIDS '') || return 1
    cur_load=$(usbshare_current USBIP_LOAD_MODULE true) || return 1
    cur_unbind=$(usbshare_current USBIP_UNBIND_ON_EXIT false) || return 1
    cur_restore=$(usbshare_current USBIP_RESTORE_SHARED true) || return 1
    cur_kick=$(usbshare_current USBIP_KICK_IDLE_SECONDS 60) || return 1
    cur_access_password=$(usbshare_current USBIP_ACCESS_PASSWORD '') || return 1

    new_port=$(usbshare_pick_port "${w_port}" "${cur_port}")

    if [ -n "${w_password}" ]; then
        new_password="${w_password}"
    else
        new_password="${cur_password}"
    fi

    # busids 不再由向导收集：设备是装完之后才插上的，向导里没法列出来，
    # 让用户手敲 busid 也不合理。默认行为改成"重启后自动恢复上次共享的设备"
    # （见 entrypoint-fnos.sh 的 USBIP_RESTORE_SHARED）。仍可在 env 文件里手工
    # 写 USBIP_BUSIDS 预置一批设备，这里原样保留。
    new_busids="${cur_busids}"

    new_load=$(usbshare_pick_tristate "${w_load}" "${cur_load}")
    new_unbind=$(usbshare_pick_tristate "${w_unbind}" "${cur_unbind}")
    new_restore=$(usbshare_pick_tristate "${w_restore}" "${cur_restore}")
    new_kick=$(usbshare_pick_seconds "${w_kick}" "${cur_kick}")

    # 共享访问密码。
    #
    # 交互上踩过一个真实的坑：最初用「开关 access_enabled + 密码」两个字段，
    # 用户在应用设置里填了密码但没拨开关（或 fnOS 提交的是开关的 initValue=false），
    # 密码就被静默丢掉、访问控制根本没生效，界面上还看不出异常。
    # 现在改成显式三态，并且「选了启用却没给密码」会直接报错中止，不再静默降级：
    #   * access_mode=disable → 明确关闭，清空密码
    #   * access_mode=enable  → 用新填的密码；没填就沿用当前值；两者都没有则报错
    #   * 其余（keep / 安装向导未提供）→ 填了就启用，否则保持当前值
    case "$(printf '%s' "${w_access_mode}" | tr 'A-Z' 'a-z')" in
        disable | off | false)
            new_access_password=""
            ;;
        enable | on | true)
            if [ -n "${w_access_password}" ]; then
                new_access_password="${w_access_password}"
            elif [ -n "${cur_access_password}" ]; then
                new_access_password="${cur_access_password}"
            else
                usbshare_log "ERROR: 选择了启用共享访问密码，但没有填写密码（6-64 位）。"
                return 1
            fi
            ;;
        *)
            if [ -n "${w_access_password}" ]; then
                new_access_password="${w_access_password}"
            else
                new_access_password="${cur_access_password}"
            fi
            ;;
    esac

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
