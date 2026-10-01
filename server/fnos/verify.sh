#!/bin/bash
# 校验飞牛应用包结构。build.sh 会在打包前调用它，也可以单独跑：
#   bash verify.sh
#
# 这里做的是"能在打包前抓住"的静态检查：字段是否齐、向导字段和服务端脚本
# 是否对得上、包里的 Dockerfile 有没有和 server/Dockerfile 漂移、图标尺寸对不对。
# 它不能替代真机安装测试。

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="${HERE}/package"
SERVER_DIR="$(cd "${HERE}/.." && pwd)"

FAILED=0
ok() { printf '  [ok]   %s\n' "$*"; }
bad() { printf '  [FAIL] %s\n' "$*"; FAILED=$((FAILED + 1)); }
warn() { printf '  [warn] %s\n' "$*"; }
section() { printf '\n== %s\n' "$*"; }

section "必需文件"
for f in manifest ICON.PNG ICON_256.PNG \
    config/privilege config/resource \
    app/ui/config app/ui/images/icon_64.png app/ui/images/icon_256.png \
    cmd/main cmd/lib-config.sh cmd/install_init cmd/install_callback \
    cmd/upgrade_init cmd/upgrade_callback cmd/uninstall_init cmd/uninstall_callback \
    cmd/config_init cmd/config_callback \
    app/docker/docker-compose.yaml app/docker/Dockerfile \
    app/docker/fnos-env.sh app/docker/entrypoint-fnos.sh app/docker/healthcheck-fnos.sh \
    app/docker/fnos-unix-proxy.py; do
    if [ -f "${PKG_DIR}/${f}" ]; then ok "${f}"; else bad "缺少 ${f}"; fi
done

section "Docker 构建上下文（由 build.sh 从 server/ 同步）"
for f in entrypoint.sh healthcheck.sh web.py gateway.py index.html favicon.ico; do
    if [ -f "${PKG_DIR}/app/docker/${f}" ]; then
        ok "app/docker/${f}"
    else
        bad "缺少 app/docker/${f} —— 请先跑 build.sh 同步"
    fi
done

section "向导字段 ↔ 服务端脚本 一致性"
if command -v python3 >/dev/null 2>&1; then
    python3 - "${PKG_DIR}" <<'PY'
import json
import pathlib
import sys

pkg = pathlib.Path(sys.argv[1])
fields = set()
for name in ("install", "config", "uninstall"):
    path = pkg / "wizard" / name
    if not path.is_file():
        print(f"  [warn] 缺少 wizard/{name}")
        continue
    try:
        steps = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        print(f"  [FAIL] wizard/{name} 不是合法 JSON: {exc}")
        sys.exit(1)
    if not isinstance(steps, list):
        print(f"  [FAIL] wizard/{name} 顶层必须是数组")
        sys.exit(1)
    names = []
    for idx, step in enumerate(steps):
        # fnpack 1.2.4 的实测约束（官方文档没写全，踩过）：
        #   * 每个 step 必须有非空 stepTitle
        #   * initValue 必须是字符串；switch 写布尔 true/false 会直接打包失败，
        #     报 "File \"install\" is not valid due to JSON format or content validation failure"
        if not step.get("stepTitle"):
            print(f"  [FAIL] wizard/{name} 第 {idx + 1} 个 step 缺少 stepTitle")
            sys.exit(1)
        for item in step.get("items", []):
            field = item.get("field")
            if "initValue" in item and not isinstance(item["initValue"], str):
                print(
                    f"  [FAIL] wizard/{name} 字段 {field!r} 的 initValue 必须是字符串"
                    f"（fnpack 1.2.4 不接受 {type(item['initValue']).__name__}），"
                    f"布尔开关请写 \"true\"/\"false\""
                )
                sys.exit(1)
            # 真机实测：switch 的"打开"状态传不到脚本（用户在安装向导打开
            # 「停止应用时归还设备」，install_callback 收到的仍是空值）。全部改用 radio。
            if item.get("type") == "switch":
                print(
                    f"  [FAIL] wizard/{name} 字段 {field!r} 用了 switch —— 真机实测 switch 的打开状态"
                    f"不会传到脚本里，请改用 radio"
                )
                sys.exit(1)
            # 真机实测：飞牛的表单会用**已保存的值**回填 —— 例：`port` 的 initValue 是空串，
            # 但表单里显示的是当前端口 5555。所以 initValue 只是"第一次"的默认值，
            # 不要写成「保持不变」那种占位语义（那样用户反而看不到当前设置）。
            # 这里只校验 initValue 是 options 里的合法取值，避免拼错导致表单没有选中项。
            options = item.get("options")
            if options and item.get("initValue") not in [o.get("value") for o in options]:
                print(
                    f"  [FAIL] wizard/{name} 字段 {field!r} 的 initValue={item.get('initValue')!r} "
                    f"不在 options 里 {[o.get('value') for o in options]}"
                )
                sys.exit(1)
            if field:
                names.append(field)
                fields.add(field)
    print(f"  [ok]   wizard/{name} 合法，字段={names}")

lib = (pkg / "cmd" / "lib-config.sh").read_text(encoding="utf-8")
missing = sorted(f for f in fields if f not in lib and f != "wizard_delete_data")
if missing:
    print(f"  [FAIL] 向导字段没有在 cmd/lib-config.sh 里被消费: {missing}")
    sys.exit(1)
print(f"  [ok]   向导字段都被 lib-config.sh 消费（除 wizard_delete_data）")
PY
    [ $? -ne 0 ] && FAILED=$((FAILED + 1))
else
    warn "没有 python3，跳过 JSON 校验"
fi

section "配置链一致性（词表 / 默认值 / 键集合 / 网关前缀）"
if command -v python3 >/dev/null 2>&1; then
    python3 - "${PKG_DIR}" <<'PY'
import json
import pathlib
import re
import sys

pkg = pathlib.Path(sys.argv[1])
failed = []


def ok(msg):
    print(f"  [ok]   {msg}")


def bad(msg):
    failed.append(msg)
    print(f"  [FAIL] {msg}")


def warn(msg):
    print(f"  [warn] {msg}")


def load_json(rel):
    return json.loads((pkg / rel).read_text(encoding="utf-8"))


def wizard_fields(name):
    out = {}
    for step in load_json(f"wizard/{name}"):
        for item in step.get("items", []):
            if item.get("field"):
                out[item["field"]] = item
    return out


install = wizard_fields("install")
config = wizard_fields("config")
uninstall = wizard_fields("uninstall")

lib = (pkg / "cmd" / "lib-config.sh").read_text(encoding="utf-8")
uninstall_cb = (pkg / "cmd" / "uninstall_callback").read_text(encoding="utf-8")
envsh = (pkg / "app" / "docker" / "fnos-env.sh").read_text(encoding="utf-8")
ep = (pkg / "app" / "docker" / "entrypoint-fnos.sh").read_text(encoding="utf-8")
compose = (pkg / "app" / "docker" / "docker-compose.yaml").read_text(encoding="utf-8")

# --- 1. 向导字段必须真的被当成环境变量读取 -------------------------------
# 只检查"字段名在 lib-config.sh 里出现过"是不够的：注释、函数名
# （比如 access_mode 撞上 usbshare_pick_... 之类）都能让它蒙混过关。
for name, fields, src, where in (
    ("install", install, lib, "cmd/lib-config.sh"),
    ("config", config, lib, "cmd/lib-config.sh"),
    ("uninstall", uninstall, uninstall_cb, "cmd/uninstall_callback"),
):
    for field in sorted(fields):
        if re.search(r"\$\{" + re.escape(field) + r"[:}]", src):
            ok(f"{name}: 字段 {field} 以 ${{{field}}} 的形式被 {where} 读取")
        else:
            bad(
                f"{name}: 字段 {field} 没有以 ${{{field}}} 的形式出现在 {where} 里"
                f"（名字出现在注释里不算）—— 用户在向导里填了它也不会有任何效果"
            )

# --- 2. 安装向导与应用设置的字段集合 ------------------------------------
only_install = sorted(set(install) - set(config))
only_config = sorted(set(config) - set(install))
if only_install:
    bad(f"这些字段只在安装向导里有、应用设置里没有，装好后用户再也改不了：{only_install}")
elif only_config:
    warn(f"这些字段只在应用设置里有（安装时无法设置）：{only_config}")
else:
    ok("wizard/install 与 wizard/config 的字段集合一致")

# --- 3. radio 取值词表 + initValue 必须是合法选项 -----------------------
# lib-config.sh 的 usbshare_pick_tristate / access_mode 分支只认这些值；
# 选项值写错（比如 "enabled"/"disbale"）会静默落回"当前值"，界面上看不出异常。
TRISTATE = {"keep", "enable", "disable", "on", "off", "true", "false", "yes", "no", "1", "0"}
for name, fields in (("install", install), ("config", config), ("uninstall", uninstall)):
    for field, item in sorted(fields.items()):
        if item.get("type") not in ("radio", "select"):
            continue
        values = [str(o.get("value")) for o in item.get("options", [])]
        unknown = [v for v in values if v.lower() not in TRISTATE]
        if unknown:
            bad(f"{name}: 字段 {field} 的选项取值 {unknown} 不在脚本认得的词表里 {sorted(TRISTATE)}")
        else:
            ok(f"{name}: 字段 {field} 的选项取值都在词表里 {values}")
        init = item.get("initValue")
        if init is not None and str(init) not in values:
            bad(f"{name}: 字段 {field} 的 initValue={init!r} 不是它的任何一个选项 {values}")

# --- 3b. 同一个字段在两个向导里的取值词表 ------------------------------
# 脚本两套写法都认，但同一个设置在「安装」和「应用设置」里写法不同，改的时候
# 很容易只改一处；这里只提醒，不拦。
for field in sorted(set(install) & set(config)):
    left, right = install[field], config[field]
    if left.get("type") != right.get("type"):
        warn(f"字段 {field} 在 install 里是 {left.get('type')}、在 config 里是 {right.get('type')}")
        continue
    if left.get("type") not in ("radio", "select"):
        continue
    left_values = [str(o.get("value")) for o in left.get("options", [])]
    right_values = [str(o.get("value")) for o in right.get("options", [])]
    if left_values == right_values:
        ok(f"字段 {field} 在两个向导里的选项取值一致 {left_values}")
    else:
        warn(
            f"字段 {field} 的选项取值两处写法不同：install={left_values} config={right_values}"
            f"（usbshare_pick_tristate 两套都认，但改的时候容易只改一处）"
        )

# --- 4. 默认值一致性：lib-config.sh 的兜底 ↔ 安装向导的 initValue --------
# 两边不一致时，安装时用户看到的默认值和"读不到配置时脚本用的默认值"会打架。
KEY_FIELD = {
    "USBIP_PORT": "port",
    "USBIP_WEB_PASSWORD": "admin_password",
    "USBIP_BUSIDS": None,  # 向导不再收集，由管理页维护
    "USBIP_LOAD_MODULE": "load_module",
    "USBIP_UNBIND_ON_EXIT": "unbind_on_exit",
    "USBIP_RESTORE_SHARED": "restore_shared",
    "USBIP_KICK_IDLE_SECONDS": "kick_idle_seconds",
    "USBIP_ACCESS_PASSWORD": "access_password",
}


def norm(value):
    value = (value or "").strip().strip("'\"")
    return {
        "true": "enable", "on": "enable", "1": "enable",
        "false": "disable", "off": "disable", "0": "disable",
    }.get(value.lower(), value)


fallbacks = {}
for match in re.finditer(r"usbshare_current\s+(USBIP_[A-Z_]+)\s+(\S*)", lib):
    fallbacks[match.group(1)] = match.group(2).rstrip(")").strip("'\"")

for key, field in sorted(KEY_FIELD.items()):
    if key not in fallbacks:
        bad(f"cmd/lib-config.sh 没有用 usbshare_current 读取 {key}（保存设置时会静默落回兜底值）")
        continue
    if field is None or field not in install or "initValue" not in install[field]:
        continue
    if norm(fallbacks[key]) != norm(install[field]["initValue"]):
        bad(
            f"默认值不一致：lib-config.sh 里 {key} 的兜底是 {fallbacks[key]!r}，"
            f"安装向导 {field} 的 initValue 是 {install[field]['initValue']!r}"
        )
    else:
        ok(f"默认值一致：{key} 兜底={fallbacks[key]!r} ↔ install/{field}={install[field]['initValue']!r}")

# --- 5. env 文件读写的键集合必须一致 ------------------------------------
written = set(re.findall(r"printf '(USBIP_[A-Z_]+)=%s", lib))
read_keys = set(fallbacks)
if written == read_keys:
    ok(f"env 文件读写的键集合一致：{sorted(written)}")
else:
    bad(
        f"env 文件读写的键对不上：只写不读={sorted(written - read_keys)}，"
        f"只读不写={sorted(read_keys - written)}"
        f"（只写不读的键，用户下一次保存设置就会被丢掉）"
    )

# --- 6. 写进 env 的键必须真的有人用 -------------------------------------
docker_text = "\n".join(
    p.read_text(encoding="utf-8", errors="replace")
    for p in sorted((pkg / "app" / "docker").iterdir())
    if p.is_file() and p.name not in ("Dockerfile", "docker-compose.yaml")
)
dead = sorted(k for k in written if k not in docker_text)
if dead:
    warn(f"这些键写进了 env 文件，但容器侧没有任何脚本/代码引用它们：{dead}")
else:
    ok("写进 env 文件的每个键在容器侧都有消费者")

# --- 7. 容器侧读配置文件的方式（防回归）--------------------------------
# 曾经用 `.`(source) + set -a 读：密码里有空格/$/反引号时会被 shell 拆分或
# 展开（实测容器以 127 退出起不来，或访问密码变成空值 = 访问控制静默失效）。
if "set -a" in envsh or re.search(r"^\s*\.\s+\"?\$\{?(USBSHARE_CONF|USBSHARE_PORTS_FILE)", envsh, re.M):
    bad(
        "app/docker/fnos-env.sh 又用 source/set -a 读配置文件了：密码里的空格/$/反引号"
        "会被 shell 拆分或展开，容器可能起不来，或者共享访问密码变成空值（访问控制静默失效）"
    )
elif "usbshare_load_env_file" not in envsh:
    bad("app/docker/fnos-env.sh 没有用 usbshare_load_env_file 逐行解析配置文件")
else:
    ok("fnos-env.sh 用逐行解析（不 source、不 eval）读配置文件")

# --- 8. 内部端口必须避开对外端口 ----------------------------------------
# 对外端口在挑内部端口时还没被监听，会被"挑空闲端口"挑中；随后网关
# EADDRINUSE 起不来，容器退出，重启后 /data/internal-ports.env 里还是那个值，
# 于是变成永远起不来的重启循环。
if re.search(r'!=\s*"\$\{USBIP_PORT\}"', ep):
    ok("entrypoint-fnos.sh 挑内部端口时排除了对外端口 USBIP_PORT")
else:
    bad(
        "entrypoint-fnos.sh 挑内部端口时没有排除对外端口 USBIP_PORT："
        "应用端口落在内核临时端口范围（本机 32768-60999）里时可能撞上，容器会永久重启循环"
    )

# --- 9. 统一网关：前缀 / socket / 挂载 ----------------------------------
manifest = {}
for line in (pkg / "manifest").read_text(encoding="utf-8").splitlines():
    if line.lstrip().startswith("#") or "=" not in line:
        continue
    key, _, value = line.partition("=")
    manifest[key.strip()] = value.strip()
uidir = manifest.get("desktop_uidir", "ui")
appname = manifest.get("appname", "")
launch = manifest.get("desktop_applaunchname", "")
ui = load_json(f"app/{uidir}/config")
entry = ui.get(".url", {}).get(launch, {})

prefix = entry.get("gatewayPrefix", "")
match = re.search(r'USBSHARE_GATEWAY_PREFIX="\$\{USBSHARE_GATEWAY_PREFIX:-([^}"]+)\}"', ep)
ep_prefix = match.group(1) if match else ""
if prefix and ep_prefix == prefix:
    ok(f"gatewayPrefix 一致：app/{uidir}/config={prefix} ↔ entrypoint-fnos.sh={ep_prefix}")
else:
    bad(
        f"gatewayPrefix 不一致：app/{uidir}/config={prefix!r}，"
        f"entrypoint-fnos.sh 的默认值={ep_prefix!r}（桌面小窗会 404）"
    )

match = re.search(r'USBSHARE_GATEWAY_SOCKET="\$\{USBSHARE_GATEWAY_SOCKET:-([^}"]+)\}"', ep)
if match:
    sock_dir = str(pathlib.PurePosixPath(match.group(1)).parent)
    if re.search(r"\$\{TRIM_APPDEST\}:" + re.escape(sock_dir) + r"(\s|$)", compose):
        ok(f"网关 socket 目录 {sock_dir} 已从 ${{TRIM_APPDEST}} 挂进容器")
    else:
        bad(f"网关 socket 放在 {sock_dir}，但 docker-compose.yaml 没有把 ${{TRIM_APPDEST}} 挂到那里")
else:
    warn("entrypoint-fnos.sh 里没找到 USBSHARE_GATEWAY_SOCKET 的默认值")

# --- 10. compose 项目名必须等于 appname --------------------------------
# 项目名不对，应用中心的 `docker compose start` 会报
# `service "server" has no container to start`（error code 12005）。
match = re.search(r"^name:\s*(\S+)\s*$", compose, re.M)
project = match.group(1) if match else ""
if project and project == appname:
    ok(f"docker-compose.yaml 的 name={project} 与 appname 一致")
else:
    bad(f"docker-compose.yaml 的 name 必须是 appname（{appname}），当前是 {project!r}")

raise SystemExit(1 if failed else 0)
PY
    [ $? -ne 0 ] && FAILED=$((FAILED + 1))
else
    warn "没有 python3，跳过配置链一致性校验"
fi

section "manifest"
if [ -f "${PKG_DIR}/manifest" ]; then
    # manifest 是键值格式（不是 JSON），值不能跨行；fnpack 会报
    # "key-value delimiter not found" 并直接打包失败。
    while IFS= read -r line; do
        case "${line}" in
            '' | '#'*) continue ;;
        esac
        if ! printf '%s' "${line}" | grep -q '='; then
            bad "manifest 有不是键值格式的行（值不能跨行）：${line}"
        fi
    done <"${PKG_DIR}/manifest"
    for key in appname version display_name source platform; do
        if grep -qE "^${key}[[:space:]]*=" "${PKG_DIR}/manifest"; then
            ok "manifest 有 ${key}"
        else
            bad "manifest 缺少 ${key}"
        fi
    done
    plat=$(sed -n 's/^platform[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest" | tr -d ' \r')
    case "${plat}" in
        x86 | arm | loongarch | risc-v | all) ok "platform=${plat}" ;;
        *) bad "platform 取值非法: '${plat}'" ;;
    esac
    uidir=$(sed -n 's/^desktop_uidir[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest" | tr -d ' \r')
    uidir=${uidir:-ui}
    if [ -d "${PKG_DIR}/app/${uidir}" ]; then ok "desktop_uidir=${uidir} 目录存在"; else bad "desktop_uidir=${uidir} 目录不存在"; fi
    launch=$(sed -n 's/^desktop_applaunchname[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest" | tr -d ' \r')
    if [ -n "${launch}" ]; then
        if grep -q "\"${launch}\"" "${PKG_DIR}/app/${uidir}/config"; then
            ok "desktop_applaunchname=${launch} 在入口配置里存在"
        else
            bad "desktop_applaunchname=${launch} 在 app/${uidir}/config 里找不到"
        fi
    else
        warn "manifest 没有 desktop_applaunchname"
    fi
    # changelog 必须提到当前版本，否则应用中心里"更新说明"与版本对不上
    # （升了版本号却忘了写这一版的说明，用户看到的还是上一版的）。
    ver=$(sed -n 's/^version[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest" | tr -d ' \r')
    clog=$(sed -n 's/^changelog[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest")
    if [ -z "${clog}" ]; then
        warn "manifest 没有 changelog"
    elif [ -n "${ver}" ] && ! printf '%s' "${clog}" | grep -q "${ver}"; then
        warn "manifest 的 changelog 里没有提到当前版本 ${ver}（应用中心里看到的更新说明会和版本号对不上）"
    else
        ok "changelog 提到了当前版本 ${ver}"
    fi
fi

section "图标尺寸"
if command -v python3 >/dev/null 2>&1; then
    python3 - "${PKG_DIR}" <<'PY'
import pathlib
import sys

try:
    from PIL import Image
except Exception:  # noqa: BLE001
    print("  [warn] 没有 Pillow，跳过图标尺寸校验")
    raise SystemExit(0)

pkg = pathlib.Path(sys.argv[1])
for rel, size in (
    ("ICON.PNG", 64),
    ("ICON_256.PNG", 256),
    ("app/ui/images/icon_64.png", 64),
    ("app/ui/images/icon_256.png", 256),
):
    path = pkg / rel
    if not path.is_file():
        continue
    with Image.open(path) as img:
        kb = path.stat().st_size / 1024
        if img.size != (size, size):
            print(f"  [FAIL] {rel} 尺寸是 {img.size}，应为 {size}x{size}")
            raise SystemExit(1)
        if kb > 1024:
            print(f"  [FAIL] {rel} 有 {kb:.0f} KB，超过 1024 KB")
            raise SystemExit(1)
        print(f"  [ok]   {rel} {img.size[0]}x{img.size[1]} {kb:.1f} KB")
PY
    [ $? -ne 0 ] && FAILED=$((FAILED + 1))
fi

section "cmd 脚本语法与可执行位"
for f in "${PKG_DIR}"/cmd/*; do
    [ -f "${f}" ] || continue
    name=$(basename "${f}")
    if bash -n "${f}" 2>/dev/null; then ok "${name} 语法通过"; else bad "${name} 语法错误"; fi
    if [ "${name}" = "lib-config.sh" ]; then continue; fi
    if [ -x "${f}" ]; then ok "${name} 可执行"; else bad "${name} 缺少可执行位"; fi
done

section "包装脚本语法（容器里 /bin/sh 就是 dash）"
for f in app/docker/fnos-env.sh app/docker/entrypoint-fnos.sh app/docker/healthcheck-fnos.sh; do
    if sh -n "${PKG_DIR}/${f}" 2>/dev/null; then
        ok "${f} sh 语法通过"
    else
        bad "${f} sh 语法错误（容器里由 dash 执行，写 bash 专有语法会直接起不来）"
    fi
done
if command -v python3 >/dev/null 2>&1; then
    # 用 ast.parse 而不是 py_compile：后者会在包目录里留下 __pycache__。
    if python3 - "${PKG_DIR}/app/docker/fnos-unix-proxy.py" <<'PY'
import ast
import pathlib
import sys

ast.parse(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
PY
    then
        ok "app/docker/fnos-unix-proxy.py 语法通过"
    else
        bad "app/docker/fnos-unix-proxy.py 语法错误"
    fi
fi

section "docker-compose 与 Dockerfile"
compose="${PKG_DIR}/app/docker/docker-compose.yaml"
if grep -qE '\{(host_port|container_port)\}' "${compose}"; then
    bad "docker-compose.yaml 里还有未替换的模板占位符"
else
    ok "docker-compose.yaml 没有模板占位符"
fi
if grep -q 'container_name' "${compose}"; then
    cname=$(sed -n 's/^[[:space:]]*container_name[[:space:]]*:[[:space:]]*//p' "${compose}" | head -n 1 | tr -d ' "\r')
    lib_cname=$(grep -oE 'USBSHARE_FALLBACK_CONTAINER="[^"]+"' "${PKG_DIR}/cmd/lib-config.sh" | head -n 1 | cut -d'"' -f2)
    if [ "${cname}" = "${lib_cname}" ]; then
        ok "container_name=${cname} 与 lib-config.sh 的回退名一致"
    else
        bad "container_name=${cname} 与 lib-config.sh 的 ${lib_cname} 不一致"
    fi
else
    warn "docker-compose.yaml 没有 container_name，cmd/main 的 status 会失效"
fi

# 镜像 tag 跟着 manifest.version 走，避免升级后还跑着旧镜像。
pkg_ver=$(sed -n 's/^version[[:space:]]*=[[:space:]]*//p' "${PKG_DIR}/manifest" | tr -d ' \r')
img_tag=$(sed -n 's/^[[:space:]]*image[[:space:]]*:[[:space:]]*//p' "${compose}" | head -n 1 | tr -d ' "\r' | sed 's/.*://')
if [ -n "${pkg_ver}" ] && [ "${img_tag}" = "${pkg_ver}" ]; then
    ok "compose 镜像 tag (${img_tag}) 与 manifest.version 一致"
else
    bad "compose 镜像 tag '${img_tag}' 与 manifest.version '${pkg_ver}' 不一致"
fi

# 统一网关入口必须指向真的会被创建的 socket。
ui_cfg="${PKG_DIR}/app/${uidir:-ui}/config"
if grep -q 'gatewaySocket' "${ui_cfg}"; then
    sock=$(sed -n 's/.*"gatewaySocket"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${ui_cfg}" | head -n 1)
    if [ -n "${sock}" ] && grep -q "${sock}" "${PKG_DIR}/app/docker/entrypoint-fnos.sh"; then
        ok "gatewaySocket=${sock} 在 entrypoint-fnos.sh 里被创建"
    else
        bad "gatewaySocket=${sock} 没有在 entrypoint-fnos.sh 里出现"
    fi
    if grep -q 'fnos-unix-proxy.py' "${PKG_DIR}/app/docker/Dockerfile"; then
        ok "转发器 fnos-unix-proxy.py 已 COPY 进镜像"
    else
        bad "fnos-unix-proxy.py 没有被 Dockerfile COPY"
    fi
fi

if command -v python3 >/dev/null 2>&1; then
    python3 - "${PKG_DIR}" "${SERVER_DIR}" <<'PY'
import pathlib
import re
import sys

pkg = pathlib.Path(sys.argv[1])
server = pathlib.Path(sys.argv[2])
docker_dir = pkg / "app" / "docker"


def apt_packages(text: str) -> set[str]:
    match = re.search(r"apt-get install[^\n]*?--no-install-recommends\s+([^\n&|]+)", text)
    if not match:
        return set()
    # 续行反斜杠会被一起匹配进来，这里丢掉。
    return {pkg for pkg in match.group(1).split() if pkg != "\\"}


pkg_df = (docker_dir / "Dockerfile").read_text(encoding="utf-8")
srv_df = (server / "Dockerfile").read_text(encoding="utf-8")

pkg_pkgs, srv_pkgs = apt_packages(pkg_df), apt_packages(srv_df)
if pkg_pkgs == srv_pkgs:
    print(f"  [ok]   apt 包与 server/Dockerfile 一致: {sorted(pkg_pkgs)}")
else:
    print(f"  [FAIL] apt 包漂移：包内={sorted(pkg_pkgs)} server={sorted(srv_pkgs)}")
    raise SystemExit(1)

missing = []
for line in pkg_df.splitlines():
    if not line.startswith("COPY "):
        continue
    parts = line.split()[1:]
    if len(parts) < 2:
        continue
    for src in parts[:-1]:
        if not (docker_dir / src).is_file():
            missing.append(src)
if missing:
    print(f"  [FAIL] Dockerfile COPY 的源文件不存在: {missing}")
    raise SystemExit(1)
print("  [ok]   Dockerfile 里 COPY 的源文件都在 app/docker/ 里")

for rel in ("entrypoint.sh", "healthcheck.sh", "web.py", "gateway.py", "index.html", "favicon.ico"):
    a, b = docker_dir / rel, server / rel
    if a.is_file() and b.is_file() and a.read_bytes() != b.read_bytes():
        print(f"  [FAIL] app/docker/{rel} 与 server/{rel} 内容不一致（请重跑 build.sh）")
        raise SystemExit(1)
print("  [ok]   同步进来的运行时文件与 server/ 逐字节一致")
PY
    [ $? -ne 0 ] && FAILED=$((FAILED + 1))
fi

printf '\n'
if [ "${FAILED}" -eq 0 ]; then
    printf '校验通过。\n'
    exit 0
fi
printf '校验失败：%d 项。\n' "${FAILED}"
exit 1
