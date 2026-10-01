#!/bin/bash
# 构建 USB-SHARE 的飞牛 fnOS 应用包（.fpk）。
#
# 用法：
#   bash build.sh            # 同步源码 + 校验 + fnpack build
#   bash build.sh --check    # 只同步 + 校验，不打包
#
# 需要 fnpack（https://developer.fnnas.com/docs/cli/fnpack/）。
# 飞牛系统自带 /usr/local/bin/fnpack；官方静态二进制：
#   https://static2.fnnas.com/fnpack/fnpack-1.2.3-linux-amd64
#
# 为什么要有这个脚本：
#   1. 包里的运行时文件（web.py、gateway.py、index.html、entrypoint.sh、
#      healthcheck.sh、favicon.ico）是 server/ 下的同一份代码，由这里同步进来，
#      避免"改了 server/ 但 fpk 里还是旧代码"。同步目标已加入 .gitignore。
#   2. 这个仓库通常放在飞牛的共享目录里（\\NAS\工作区\...）。从 Windows 侧通过
#      SMB 写入的文件有两个坑，都会让 fnpack 直接失败：
#        - 权限位是 0000（Windows ACL 与 POSIX mode 不同步）
#          → "mkdir /tmp/fnpack.*/app/docker: permission denied"
#        - 本地进程短时间内可能读到不一致的内容
#          → 'File "install" is not valid due to JSON format or content validation failure'
#      所以打包一律在本地临时目录里做，并在那里重建文件；重试 3 次兜住抖动。

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER_DIR="$(cd "${HERE}/.." && pwd)"
PKG_DIR="${HERE}/package"
DOCKER_DIR="${PKG_DIR}/app/docker"
DIST_DIR="${HERE}/dist"

# 从 server/ 同步进包内的运行时文件。Dockerfile、fnos-env.sh、entrypoint-fnos.sh、
# healthcheck-fnos.sh 是本包自有的，不在同步列表里，绝不会被覆盖。
SYNC_FILES=(
    entrypoint.sh
    healthcheck.sh
    web.py
    gateway.py
    index.html
    favicon.ico
)

log() { printf '[build] %s\n' "$*"; }
fail() { printf '[build] ERROR: %s\n' "$*" >&2; exit 1; }

# 在 $1 这棵树里：本地重建每个文件（绕开 SMB 内容不一致），再规范化权限。
normalize_tree() {
    local root="$1"
    local f tmp

    find "${root}" -type f -print0 | while IFS= read -r -d '' f; do
        tmp="${f}.rebuild.$$"
        if cat "${f}" >"${tmp}" 2>/dev/null; then
            mv -f "${tmp}" "${f}" 2>/dev/null || rm -f "${tmp}"
        else
            rm -f "${tmp}"
        fi
    done

    find "${root}" -type d -exec chmod 0755 {} + 2>/dev/null || true
    find "${root}" -type f -exec chmod 0644 {} + 2>/dev/null || true
    chmod 0755 "${root}"/cmd/* 2>/dev/null || true
    chmod 0644 "${root}/cmd/lib-config.sh" 2>/dev/null || true
    chmod 0755 "${root}"/app/docker/*.sh 2>/dev/null || true
}

dump_diagnostics() {
    local root="$1"
    printf '[build] --- 诊断：%s\n' "${root}"
    find "${root}" -maxdepth 2 -printf '%M %s %p\n' 2>/dev/null | sort | head -40
    if command -v python3 >/dev/null 2>&1; then
        python3 - "${root}" <<'PY' || true
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for rel in ("wizard/install", "wizard/config", "wizard/uninstall", "app/ui/config",
            "config/privilege", "config/resource"):
    path = root / rel
    if not path.is_file():
        print(f"  {rel}: 缺失")
        continue
    raw = path.read_bytes()
    try:
        json.loads(raw.decode("utf-8"))
        print(f"  {rel}: {len(raw)}B JSON 合法")
    except Exception as exc:  # noqa: BLE001
        print(f"  {rel}: {len(raw)}B JSON 非法 -> {exc}")
PY
    fi
}

pack_once() {
    local build_dir rc=0 produced

    build_dir="$(mktemp -d "${TMPDIR:-/tmp}/usbip-share-fpk.XXXXXX")" || return 1
    if ! cp -a "${PKG_DIR}/." "${build_dir}/"; then
        rm -rf "${build_dir}"
        return 1
    fi
    rm -f "${build_dir}"/*.fpk

    normalize_tree "${build_dir}"

    ( cd "${build_dir}" && fnpack build ) || rc=$?

    if [ "${rc}" -eq 0 ]; then
        produced=$(find "${build_dir}" -maxdepth 1 -name '*.fpk' -print -quit)
        if [ -n "${produced}" ]; then
            mkdir -p "${DIST_DIR}"
            cp -f "${produced}" "${DIST_DIR}/" || rc=1
            # 共享目录里新写的文件权限位可能还是 0000，显式给读权限。
            chmod 0644 "${DIST_DIR}"/*.fpk 2>/dev/null || true
        else
            printf '[build] ERROR: fnpack 报成功但没有产出 .fpk\n' >&2
            rc=1
        fi
    else
        dump_diagnostics "${build_dir}"
    fi

    rm -rf "${build_dir}"
    return "${rc}"
}

log "server 源码目录: ${SERVER_DIR}"
log "应用包目录:      ${PKG_DIR}"

[ -f "${SERVER_DIR}/web.py" ] || fail "${SERVER_DIR}/web.py 不存在，SERVER_DIR 解析错了？"

mkdir -p "${DOCKER_DIR}"

for f in "${SYNC_FILES[@]}"; do
    [ -f "${SERVER_DIR}/${f}" ] || fail "缺少 ${SERVER_DIR}/${f}"
    cp -f "${SERVER_DIR}/${f}" "${DOCKER_DIR}/${f}"
    log "synced ${f}"
done

# 共享目录里的可执行位要显式给（verify.sh 会检查）。
chmod 0755 "${PKG_DIR}"/cmd/* 2>/dev/null || true
chmod 0644 "${PKG_DIR}/cmd/lib-config.sh" 2>/dev/null || true
chmod 0755 "${DOCKER_DIR}"/*.sh 2>/dev/null || true

log "校验包结构"
bash "${HERE}/verify.sh"

if [ "${1:-}" = "--check" ]; then
    log "--check：跳过打包"
    exit 0
fi

command -v fnpack >/dev/null 2>&1 || fail "找不到 fnpack，见 https://developer.fnnas.com/docs/cli/fnpack/"

log "fnpack build（在本地临时目录里打包）"
attempt=1
while [ "${attempt}" -le 3 ]; do
    if pack_once; then
        log "打包成功（第 ${attempt} 次尝试）"
        log "产物："
        ls -la "${DIST_DIR}"/*.fpk 2>/dev/null || true
        exit 0
    fi
    log "第 ${attempt} 次打包失败"
    attempt=$((attempt + 1))
    [ "${attempt}" -le 3 ] && sleep 2
done

fail "打包失败。若上面诊断显示 wizard/*.json 合法，多半是共享目录内容抖动：稍等几秒重跑 build.sh；仍失败就确认 package/ 下文件的真实内容。"
