# USB/IP 服务端（Docker Compose）

在 Linux NAS / 主机上把物理 USB 设备共享给局域网内的 Windows 客户端。
对外只需要**一个端口**：USB/IP 数据协议与中文管理页（含管理 API）共用它。

```text
USB 设备 → usbip-host（宿主内核）→ usbipd（容器内）→ 内部端口
                                        ↑
                            单端口分流网关 0.0.0.0:5555
                                        ↓
                        /api/*、网页  ←→  web.py（容器内 8080）
```

## 前置条件

| 项 | 要求 |
|---|---|
| 架构 | `x86_64`（镜像基于 Debian amd64） |
| 宿主内核 | 具备 `usbip-core` / `usbip-host` 模块，容器内会 `modprobe` 加载 |
| 权限 | 需要 `privileged: true`、`/dev/bus/usb` 与 `/lib/modules` 映射 |
| Docker | Docker Engine + Compose v2（`docker compose`） |

内核模块自检（在**宿主**上执行，不是容器里）：

```sh
uname -m
find /lib/modules/"$(uname -r)" -iname '*usbip*'
lsusb
```

也可以用 `scripts/host-check.sh`。

> 容器无法为宿主内核凭空增加模块。宿主内核若没有这两个模块，本方案不可用。

## 部署

```sh
# 1) 放到 NAS 上的任意目录，例如
cd /vol1/@appshare/usbip-share-server

# 2) 生成配置
cp .env.example .env

# 3) 构建并启动
docker compose up -d --build

# 4) 看日志确认网关与 usbipd 都起来了
docker compose logs --tail 30
```

日志里应能看到：

```text
[usbip-share] starting usbipd on internal TCP port 5556
[usbip-share-gateway] single port 0.0.0.0:5555 -> usbipd 127.0.0.1:5556 | web 127.0.0.1:8080
[usbip-share-web] Chinese management UI listening on 0.0.0.0:8080
```

然后浏览器打开 `http://<NAS-IP>:5555/`：

* 首次登录密码 `123456`，**登录后请立即修改**；
* 「开始共享 / 停止共享」控制哪些设备对外导出；
* 「编辑名称/备注」给设备起名（保存在 `./config/device-metadata.json`）；
* 「强制断开连接」可以踢掉当前占用设备的远程客户端。

## 配置项（.env）

| 变量 | 默认 | 说明 |
|---|---|---|
| `USBIP_HOST_PORT` | `5555` | 对外暴露的唯一端口 |
| `USBIP_PORT` | `5555` | 容器内 usbipd 的逻辑端口（网关按此转发） |
| `USBIP_BUSIDS` | 空 | 启动时自动共享的设备，如 `1-1,1-2.3` |
| `USBIP_LOAD_MODULE` | `true` | 启动时尝试 `modprobe usbip_host` |
| `USBIP_WEB_ENABLED` | `true` | 是否启用中文管理页 |
| `USBIP_WEB_TOKEN` | 空 | 设置后管理接口改用固定令牌（免密码登录流程） |
| `USBIP_KICK_IDLE_SECONDS` | `60` | 客户端停止心跳多久后自动释放其共享设备（避免关机后僵尸占用）|
| `USBIPD_DEBUG` | `false` | 打开 usbipd 调试日志 |

## 设备名称/备注是怎么保存的

名称不与 USB 端口绑定，而与硬件绑定，命名规则与优先级见仓库根 `README.md`。
元数据文件是 `./config/device-metadata.json`，**这个目录必须可写且要随容器迁移保留**，
里面还有登录密码散列（`auth.json`）——丢失会导致要重新设密码、设备名回到未命名状态。

服务端会在每次列举设备时做一次自愈式维护：把老版本按端口保存的名称升级到按硬件保存的键上、
清理已不再使用的旧键；**改写前会先备份**为 `device-metadata.json.bak`，并在日志逐条打印。

## 登录与会话

* 密码用 PBKDF2-SHA256 加盐散列保存在 `auth.json`。
* 登录后下发**签名令牌**（`过期时间戳.HMAC`，密钥由密码散列派生），浏览器存在 localStorage。
  由于令牌不依赖服务端内存，**重建容器 / 重启服务不会把你踢出登录**；
  修改密码会让此前所有令牌立即失效。
* 页面加载时会调用 `GET /api/session` 判断令牌是否仍然有效，只有真的无效才显示登录框。
* `GET /api/devices` 为兼容 Windows 客户端是**免密码**的（只读设备列表）；
  所有写操作（改名称/备注、共享/取消共享、强制断开、改密码）都需要登录令牌。

## HTTP 接口一览

| 方法 | 路径 | 需要登录 | 说明 |
|---|---|---|---|
| GET | `/` | 否 | 中文管理页 |
| GET | `/api/health` | 否 | 健康检查 |
| GET | `/api/session` | 否 | 返回 `authorized` / `mustChange` / `authDisabled` |
| GET | `/api/devices` | 否 | 设备列表（含名称、备注、识别依据、占用方） |
| POST | `/api/login` | 否 | `{password}` → `{token, mustChange}` |
| POST | `/api/change-password` | 是 | `{oldPassword, newPassword}` |
| POST | `/api/clients/heartbeat` | 否 | Windows 客户端登记占用方（心跳） |
| POST | `/api/devices/<busid>/metadata` | 是 | `{alias, remark}` |
| POST | `/api/devices/<busid>/share\|unshare\|kick` | 是 | 共享 / 取消共享 / 强制断开 |

## 测试

`tests/` 下是四套可直接运行的回归测试（不需要真实 USB 设备）：

```sh
python tests/test_metadata_keys.py     # 设备名称键规则（21 项）
python tests/test_auth_flow.py         # 登录/会话端到端，含模拟容器重启（10 项）
python tests/test_gateway_split.py     # 单端口分流 + 空闲长会话不被拆断（5 项，约 15s）
python tests/extract_inline.py && node tests/test_ui_boot.js   # 管理页启动逻辑（10 项）
```

前两套用临时目录承载状态文件，不会碰真实配置。

## 常见问题

**客户端列表里设备少了 / 网页显示已共享但客户端看不到**
网页状态读的是内核绑定，客户端列表读的是 usbipd 的协议导出。两者不一致时，
在网页上对该设备先「停止共享」再「开始共享」重建绑定；仍不行就看
`docker compose logs` 里绑定相关报错（部分设备驱动对重新绑定敏感）。

**连接反复断开**
先确认不是把 USB/IP 会话经过额外的转发层（本仓库的网关已修掉"转发超时导致空闲会话被拆"
的问题，如果你自建了其它代理请检查同类超时设置），再看 `USBIPD_DEBUG=true` 的日志。

**改完 `.env` 没生效**
compose 的环境变量在容器创建时注入，需要 `docker compose up -d` 重建容器。

## 许可

自有部分 MIT（见仓库根 `LICENSE`）；运行时依赖的许可与义务见仓库根 `THIRD-PARTY.md`。
