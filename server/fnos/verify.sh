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
