# USB-SHARE 服务端（Docker Compose）

在 Linux NAS / 主机上把物理 USB 设备共享给局域网内的 Windows 客户端。
对外只需要**一个端口**：USB/IP 数据协议与中文管理页（含管理 API）共用它。

```text
USB 设备 → usbip-host（宿主内核）→ usbipd（容器内）→ 内部端口
                                        ↑
                            单端口分流网关 0.0.0.0:5555
                                        ↓
                        /api/*、网页  ←→  web.py（容器内 8080）
```

## ⚠️ 不要共享承载宿主网络的 USB 网卡

**共享一块 USB 网卡 = 把 NAS 自己从网络上摘下来。** `usbip bind` 会把这块设备的驱动
从系统里抢走（`r8152`/`r8169` → `usbip-host`），它的网络接口随之消失；如果它正好承载
默认路由，NAS 会立刻掉线 —— **远端 SSH 也会一起断，可能再也连不回来**。
本仓库真机踩过一次：USB 网卡 `2-7`（Realtek RTL8156）被误点「开始共享」，约 50 秒后
`USB disconnect`，掉网 1.5 分钟；容器重启后又被"自动恢复共享"绑了第二次。

因此服务端**默认拒绝**共享任何导出了网络接口的设备（`net/` 判据，见下文），
两道防线 + 一道兜底：

1. 管理页/API：`POST /api/devices/<busid>/share` 返回
   `409 {"error": "这是一块 USB 网卡（网络接口 xxx），共享它会让 NAS 自己掉线，已拒绝共享。…"}`，
   连 `usbip bind` 都不会执行；「强制断开」对这类设备也拒绝（它会在断开后立刻重新共享）。
2. 启动自动共享：`entrypoint.sh` 的 `bind_one()` 在 `usbip bind` 之前做同样的判断，
   命中就只打一行 `REFUSING to bind …` 然后跳过（**不会**让容器启动失败）。
   `USBIP_BUSIDS` 与飞牛从 `managed-busids` 自动恢复出来的列表都走这条路径。
3. 兜底：启动绑定前后各查一次 `/proc/net/route` 的默认路由，**绑完默认路由消失就
   立刻回滚**本轮绑定过的所有设备（`usbip unbind` + 从 `managed-busids` 里摘掉），
   并打印 `ERROR: 绑定设备后宿主默认路由消失，已回滚…`。

> **逃生阀（危险）**：`USBIP_ALLOW_NETDEV=true` 会同时关掉上面第 1、2 道拦截。
> 只在这台机器确实还有别的路可走（第二块网卡、串口控制台）时才用。
> 注意它需要被传进容器：`docker-compose.yml` 的 `environment:` 与飞牛的
> `fnos-env.sh` 都是显式白名单，默认没有这一项，要手动加一行
> `USBIP_ALLOW_NETDEV: ${USBIP_ALLOW_NETDEV:-false}`。

**已知盲区**：设备**已经**被 `usbip-host` 绑着时，它的 `net/` 目录已经消失，
第 1、2 道拦截都认不出来（第 3 道只覆盖"启动绑定"这一轮）。
这种情况请用管理页的**「停止共享」**（只 `unbind`，把设备还给原驱动），
不要用「强制断开」（会重新 bind）。

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
[usbip-share-gateway] access file /run/usbip/authorized-clients.json: no shared-access password, every client is allowed
[usbip-share-web] Chinese management UI listening on 0.0.0.0:8080
```

（设了 `USBIP_ACCESS_PASSWORD` 时，上面第二行会变成
`shared-access password enabled, unauthorized source IPs are refused`，并且 `web.py` 会打印
`共享访问密码已启用…`。）

> **部署提示**：`web.py` 与 `gateway.py` 都是 bind mount，改完 `docker compose up -d`
> 重建容器即可生效；`entrypoint.sh` 仍然烧在镜像里（本次它把 `USBIP_ACCESS_FILE`
> 与 `USBIP_PROXY_SECRET` 传给两个进程，缺了它也不影响功能 —— 两个进程都会自己从同名
> 环境变量取默认值。**但 `USBIP_PROXY_SECRET` 只有新版 `entrypoint.sh` 会生成**，
> 所以只替换两个 `.py` 而不更新 `entrypoint.sh` 时，PROXY 行的校验标记是空的，
> 会退回旧行为并在日志里告警；要拿到完整的信任边界必须重建镜像或手工把新的
> `entrypoint.sh` 拷进容器）。

然后浏览器打开 `http://<NAS-IP>:5555/`：

* **首次启动的密码不再是固定的 `123456`**：未设置 `USBIP_WEB_PASSWORD` 时，容器会生成
  一个 12 位随机密码，写入 `./config/initial-password.txt`（权限 0600）并打印在
  `docker compose logs` 里。用它登录后**必须先修改密码**，否则除查看状态外的写操作
  一律返回 `403 {"ok": false, "error": "must_change_password"}`。改完即可删除该文件。
* 「开始共享 / 停止共享」控制哪些设备对外导出；
* 「编辑名称/备注」给设备起名（保存在 `./config/device-metadata.json`）；
* 「强制断开连接」可以踢掉当前占用设备的远程客户端。

## 配置项（.env）

| 变量 | 默认 | 说明 |
|---|---|---|
| `USBIP_HOST_PORT` | `5555` | 对外暴露的唯一端口 |
| `USBIP_PORT` | `5555` | 容器内 usbipd 的逻辑端口（网关按此转发） |
| `USBIP_GATEWAY_HOST` | `0.0.0.0` | 单端口网关的监听地址（改成具体网卡地址可缩小暴露面） |
| `USBIP_BUSIDS` | 空 | 启动时自动共享的设备，如 `1-1,1-2.3` |
| `USBIP_LOAD_MODULE` | `true` | 启动时尝试 `modprobe usbip_host` |
| `USBIP_WEB_ENABLED` | `true` | 是否启用中文管理页 |
| `USBIP_WEB_PASSWORD` | 空 | 空 = 首次启动生成随机密码并要求改密；非空 = 密码由环境变量托管（每次启动同步 `auth.json`，`mustChange=false`，页面上隐藏「修改密码」） |
| `USBIP_WEB_TOKEN` | 空 | 设置后管理接口改用固定令牌（免密码登录流程），并跳过「必须改密」限制 |
| `USBIP_ACCESS_PASSWORD` | 空 | **共享访问密码**。空 = 关闭访问控制（默认，行为与旧版完全一致）；非空 = 客户端必须先用它换取源 IP 授权才能连 USB/IP（见「共享访问密码」一节） |
| `USBIP_ACCESS_TTL_SECONDS` | `43200` | 授权有效期（秒），默认 12 小时；客户端每拍心跳都会续期 |
| `USBIP_ACCESS_FILE` | `/run/usbip/authorized-clients.json` | 授权表路径：`web.py` 写、`gateway.py` 读。**文件存在 = 访问控制开启**，删除即恢复放行 |
| `USBIP_PROXY_SECRET` | 启动时随机生成 | 网关与 `web.py` 共享的 PROXY 校验秘密（见「网关怎么知道真实来源 IP」）。由 `entrypoint.sh` 生成并导出，正常不用手设；显式设置会覆盖生成值。**为空 = 退回旧版"只看回环"的行为并在日志里告警** |
| `USBIP_PROXY_WAIT_SECONDS` | `2` | `web.py` 等网关那行 PROXY 头的最长时间（秒）。只作用于这一行，拿到/放弃后立刻恢复 15 秒连接超时 |
| `USBIP_KICK_IDLE_SECONDS` | `60` | 客户端停止心跳多久后自动释放其共享设备（避免关机后僵尸占用）|
| `USBIP_WATCHDOG_MISSED_RUNS` | `5` | 连续多少个 watchdog 周期无心跳才考虑释放（每周期 10s） |
| `USBIPD_DEBUG` | `false` | 打开 usbipd 调试日志 |

## 设备名称/备注是怎么保存的

名称不与 USB 端口绑定，而与硬件绑定，命名规则与优先级见仓库根 `README.md`。
元数据文件是 `./config/device-metadata.json`，**这个目录必须可写且要随容器迁移保留**，
里面还有登录密码散列（`auth.json`）和首次启动的随机密码
（`initial-password.txt`，改完密码后可以删掉）——丢失会导致要重新设密码、设备名回到未命名状态。

同一条记录里还存着隐藏标记（`"hidden": "true"`，见下文「隐藏设备」），
它和名称用同一套硬件键，所以换插口、容器重启都不会丢。

服务端会在每次列举设备时做一次自愈式维护：把老版本按端口保存的名称升级到按硬件保存的键上、
清理已不再使用的旧键；**改写前会先备份**为 `device-metadata.json.bak`，并在日志逐条打印。

## 登录与会话

* 密码用 PBKDF2-SHA256 加盐散列保存在 `auth.json`。
* **出厂没有默认密码**：`USBIP_WEB_PASSWORD` 未设置时，首次启动生成 12 位随机密码，
  写入 `<配置目录>/initial-password.txt`（0600）并在日志里打印，同时
  `mustChange=true`。设置 `USBIP_WEB_PASSWORD` 则由环境变量托管密码
  （`mustChange=false`，`GET /api/session` 的 `passwordManaged=true`，页面隐藏改密入口）。
* **`mustChange` 由服务端强制**（不只是页面弹窗）：未改密前，除
  `GET /api/session`、`POST /api/login`、`POST /api/change-password`、
  `GET /api/health`、`GET /api/devices` 之外的管理写操作一律返回
  `403 {"ok": false, "error": "must_change_password"}`。
  `POST /api/clients/register|heartbeat`（Windows 客户端心跳）不受该限制，
  否则未改密时客户端会整体不可用。使用 `USBIP_WEB_TOKEN` 固定令牌模式时跳过该限制。
* **登录 / 改密限速**：按来源 IP 做 5 分钟滑动窗口，连续 5 次失败后返回
  `429` 并带 `Retry-After`；锁定时长从 30s 起指数退避，上限 15 分钟；
  一次成功登录立即清零。状态只在内存里，随容器重启清空。
* 登录后下发**签名令牌**（`过期时间戳.HMAC`，密钥由密码散列派生），浏览器存在 localStorage。
  由于令牌不依赖服务端内存，**重建容器 / 重启服务不会把你踢出登录**；
  修改密码会让此前所有令牌立即失效。
* 页面加载时会调用 `GET /api/session` 判断令牌是否仍然有效，只有真的无效才显示登录框。
* `GET /api/devices` 为兼容 Windows 客户端是**免密码**的（只读设备列表）；
  所有写操作（改名称/备注、共享/取消共享、强制断开、改密码）都需要登录令牌。
  未携带有效 `X-Admin-Token` 的调用拿到的列表里**没有** `connections[].publicIp`
  / `currentHolder.publicIp`（客户端上报、仅管理页展示的字段）；带令牌的管理页拿到完整数据。

## 共享访问密码（访问控制）

USB/IP 协议本身**没有认证字段**，所以"要密码才能访问服务器"只能做成
**访问密码 + 源 IP 授权窗口**：客户端先用密码通过 HTTP API 换到授权，之后它发起的
USB/IP 数据连接才被放行。

```text
客户端                                 网关(5555)                    web.py
  │  GET  /api/access ────────────────────┼──────────────────────────► required?
  │  POST /api/access/authorize {password}─┼──────────────────────────► 校验密码
  │                                        │              写入 authorized-clients.json（源 IP + 到期时间）
  │  POST /api/clients/heartbeat ──────────┼──────────────────────────► 续期
  │  USB/IP attach ────────────────────────► 源 IP 在表里且未过期? ──► usbipd
```

* **默认关闭**：`USBIP_ACCESS_PASSWORD` 为空时，`web.py` 不写授权表文件，
  `gateway.py` 看到文件不存在就**放行一切** —— 行为与引入该功能之前逐字节一致，
  已发布的 Windows 客户端不受任何影响。
* **开关就是文件**：授权表存在 = 访问控制开启。因此"启用后先写一个空表"意味着
  **开启瞬间谁都不能连**，直到有人用密码授权为止；`revoke` 全部之后文件仍在（继续拦），
  只有关掉密码才会删掉文件。
* **授权单位是来源 IP**：记录 `clientId` / `clientName` / `authorizedAt` / `expiresAt`。
  续期由心跳完成（每拍一次），所以客户端只要还在跑就不会掉线；到期或吊销后，
  它的心跳与设备列表都会重新变回 `401 {"accessRequired": true}`。
* **限速**：`/api/access/authorize` 密码错返回 `403`，并**复用管理员登录那套按来源 IP 的
  限速**（5 分钟滑动窗口、连续 5 次后 30s 起指数退避、上限 15 分钟、成功即清零）。
* **管理页不受影响**：带有效 `X-Admin-Token` 的调用（以及 `/api/session`、`/api/login`、
  `/api/health`）不走这道门；`/api/access` 永远免鉴权、无副作用，供客户端探测。
* **网关只认新连接**：`gateway.py` 在连接建立时判定一次，所以吊销/过期**不会**踢掉
  已经建立的 USB/IP 会话（要断就用管理页的「强制断开连接」）。

### 网关怎么知道"真实来源 IP"

单端口网关是纯 TCP 转发，`web.py` 看到的对端永远是网关自己（127.0.0.1），
按来源 IP 做控制就无从谈起。因此**访问控制开启时**，网关在转发的 HTTP 连接前面补一行
`PROXY TCP4 <源IP> <本机IP> <源端口> <本机端口> <校验标记>\r\n`（PROXY protocol v1
的 6 个字段 + 一个自定义校验标记），`web.py` 只在这行头**同时**满足下面两点时才采信：

1. 对端是回环地址（挡掉从网络直连过来的伪造）；
2. 行尾的校验标记等于由 `USBIP_PROXY_SECRET` 派生出的值（挡掉**本机/同网络命名空间里
   的任意进程**直连 web 端口伪造）。

**为什么不能只靠第 1 条**：PROXY v1 头本身没有任何认证，"对端是回环"并不能证明对面
就是网关。在 host 网络部署、或有人把 `USBIP_WEB_HOST` 设成 `0.0.0.0`（`.env.example`
默认值就是这个）时，web.py 的端口可以被直连 —— 任何能连上它的进程只要自己写一行
`PROXY TCP4 <已授权IP> ...` 就能冒充授权来源，直接绕过共享访问密码，还能冒用别人的 IP
登记占用方、抢占排队名额。所以第 2 条是必须的。

* `USBIP_PROXY_SECRET` 由 `entrypoint.sh` 在容器启动时生成一个随机值并导出给
  `web.py` 与 `gateway.py`（两个进程都从环境变量读，不需要额外配置）。
  **它需要新的 `entrypoint.sh`**：只替换两个 `.py` 文件、没重建镜像时该变量为空，
  这时会退回"只看回环"的旧行为，两个进程都会在日志里打 `WARNING` 提示。
* 标记对不上时 `web.py` 会**一个字节都不消费**那行头，按直连处理（来源 = socket 对端），
  并打印一行 `rejected a PROXY line without a valid USBIP_PROXY_SECRET token`。
  所以正常请求不会被这行判断吃掉。
* 校验标记不是密码：它只用来证明"这行头是网关写的"，不承担鉴权职责，也不落盘。

关闭访问控制时**不补这行头**，所以默认路径上 HTTP 流量逐字节不变；网关是旧版、
或 `web.py` 不认识这行头的老组合也不会出现（授权表是 `web.py` 自己写的，
文件存在就说明它已经支持访问控制）。

**其它边界**：`web.py` 只在"等这行头"时设一个短超时（`USBIP_PROXY_WAIT_SECONDS`，
默认 2 秒），拿到或放弃后立刻恢复 15 秒的连接超时 —— 否则对端发 6 个字节 `PROXY `
就不再说话时，这个连接会占着线程一直等到 15 秒。

### 管理接口

| 方法 | 路径 | 需要登录 | 说明 |
|---|---|---|---|
| GET | `/api/access/clients` | 是 | `{ok, required, clients:[{address, clientId, clientName, authorizedAt, expiresAt, expiresInSeconds}]}` |
| POST | `/api/access/clients/revoke` | 是 | `{"address": "..."}` 吊销单个；`{}`（或不带 `address`）吊销全部。返回 `{ok, revoked, required, clients}` |

这两个端点和别的管理写端点一样受 `mustChange` 403 门保护（未改初始密码时不能用）。

## HTTP 接口一览

| 方法 | 路径 | 需要登录 | 说明 |
|---|---|---|---|
| GET | `/` | 否 | 中文管理页 |
| GET | `/api/health` | 否 | 健康检查 |
| GET | `/api/session` | 否 | 返回 `authorized` / `mustChange` / `passwordManaged` / `authDisabled` |
| GET | `/api/access` | 否 | 共享访问密码是否启用：`{ok, required}`。永远免鉴权、无副作用 |
| POST | `/api/access/authorize` | 否 | `{password, clientId, clientName}` → `{ok, authorized, required, expiresInSeconds, message}`；把**调用方源 IP** 加进授权表。密码错 403（5 次失败后 429），未启用访问控制时幂等返回 `required:false` |
| GET | `/api/devices` | 否 | 设备列表（含名称、备注、识别依据、占用方、等待名单、`hidden`、`networkInterfaces`；顶层另有 `kickIdleSeconds`。匿名调用不含 `publicIp`，且不含隐藏设备。**启用访问控制后，匿名且源 IP 未授权的调用返回 `401 {"accessRequired": true}`**，带管理令牌的管理页不受影响） |
| POST | `/api/login` | 否 | `{password}` → `{token, mustChange}`（5 次失败后 429） |
| POST | `/api/change-password` | 是 | `{oldPassword, newPassword}`（5 次失败后 429） |
| POST | `/api/clients/heartbeat` | 否 | Windows 客户端登记占用方 + 排队意愿（心跳）。**启用访问控制后，源 IP 未授权的调用返回 `401 {"accessRequired": true}` 且不做任何写操作**；已授权则正常处理并续期 |
| POST | `/api/devices/<busid>/metadata` | 是 | `{alias, remark}` |
| POST | `/api/devices/<busid>/share\|unshare\|kick` | 是 | 共享 / 取消共享 / 强制断开（网卡会被拒绝，见上文） |
| POST | `/api/devices/<busid>/hide` | 是 | 隐藏设备（正在共享时先停止共享） |
| POST | `/api/devices/<busid>/unhide` | 是 | 取消隐藏 |
| GET | `/api/devices/<busid>/queue` | 是 | 查看某台设备的等待名单 |
| POST | `/api/devices/<busid>/queue` | 是 | `{clientId}`：管理员把某客户端加进等待名单 |
| POST | `/api/devices/<busid>/queue/clear` | 是 | 清空等待名单，并给每位等待者发 `dropped` |
| DELETE | `/api/devices/<busid>/queue/<clientId>` | 是 | 把某客户端移出等待名单 |

标注「是」的写接口在 `mustChange=true` 时会返回 403，见上文。

### 网卡保护（networkInterfaces）

`GET /api/devices` 的每台设备都带 `networkInterfaces: [...]`：这台 USB 设备当前导出的
网络接口名（例如 `["enxc84d44294124"]`），非网卡一律是空数组。
判据是 sysfs 里接口目录下有没有 `net/`（`/sys/bus/usb/devices/<busid>:<n>.<m>/net/<ifname>`）——
**不能**用 `bInterfaceClass`：RTL8156 报的是私有类 `0xff`，不是 `0x02`。

* 该字段非空时，`share` 与 `kick` 都会返回 409 并拒绝执行（`USBIP_ALLOW_NETDEV=true` 除外）。
* 管理页可以用它显示「USB 网卡」标记，或者干脆引导用户用「隐藏设备」把它锁起来。
* sysfs 读不到（非 Linux、权限不足、设备已绑定导致 `net/` 消失）一律按"不是网卡"处理：
  检测出错绝不能把正常设备停用，真正危险的是 bind 本身。

`GET /api/devices` 顶层还有 `kickIdleSeconds`：当前生效的「客户端失联多久后自动释放设备」
（就是 `USBIP_KICK_IDLE_SECONDS` 的值，`0` 表示不自动释放）。管理页用它渲染真实秒数。

### 隐藏设备（hide / unhide）

**为什么需要**：把 NAS 内置的 USB 网卡（例如 Realtek `0bda:8156`）误点「开始共享」，
一旦真的 `bind` 到 usbip-host，NAS 会直接掉网。隐藏功能把这类设备从列表里摘掉，
并阻止它被共享出去（网卡另有一层硬拦截，见上一节）。

* `GET /api/devices` 的每台设备都带布尔字段 `hidden`。
* **已隐藏的设备默认不出现在 `devices` 数组里** —— Windows 客户端因此完全看不到它，
  既不会显示也不能连接。只有**同时**满足「带有效 `X-Admin-Token`」且
  「查询串含 `includeHidden=1`」时才返回隐藏设备。未认证调用者传 `includeHidden=1`
  依然看不到（管理页要显示隐藏设备时必须自己带上令牌）。同一条规则也适用于所有
  返回 `devices` 的写接口，所以管理页可以用任一响应直接重绘。
* `POST /api/devices/<busid>/hide`（需要登录）：返回
  `{"ok": true, "message": "...", "devices": [...]}`，结构与 `GET /api/devices` 一致。
  **如果设备正在共享，会先停止共享**（`usbip unbind`，设备归还原驱动），
  `message` 为「设备已隐藏，已同时停止共享」。重复 hide 幂等。
* `POST /api/devices/<busid>/unhide`（需要登录）：同样的返回结构，清除标记，幂等。
* 对已隐藏的设备调 `share` → `409` +
  `{"ok": false, "error": "该设备已隐藏，请先取消隐藏后再共享"}`，**不会执行 bind**。
* 隐藏标记与名称/备注存在同一条记录里（`device-metadata.json` 的 `"hidden": "true"`），
  并和名称一样**绑定硬件**（序列号 / 型号 / 同型号序号键），换 USB 插口后依然生效；
  既有记录的迁移不会丢掉该标记。

**注意**：隐藏是本服务的管理层约束，不是内核层封锁。若用 `USBIP_BUSIDS=2-7`
（或在容器里手工 `usbip bind -b 2-7`）显式导出，设备仍会被共享 ——
启动前请确认 `USBIP_BUSIDS` 里没有要隐藏的设备。隐藏状态本身是持久的
（存在 `./config/device-metadata.json`），重启后仍然隐藏。

### 排队（等待名单）

一台设备同一时刻只能被一个客户端使用。此前第二个客户端点「连接」只会得到
"设备忙"，而且那台设备在它的列表里可能根本不出现。现在：

* `GET /api/devices` 的每个设备都带 `currentHolder`（当前占用方，可能为 `null`）
  与 `pendingQueue`（等待名单，FIFO 顺序，含 `clientId` 和 `name`）。
* 客户端通过**心跳**声明排队意愿 —— 心跳体里加一个 `enqueueBusids` 数组。
  服务端只接受**当前存在且处于共享状态**的 busid，其余静默丢弃（客户端每拍心跳
  都会重发，所以不会卡住）；服务端按 `clientId` 去重、保持 FIFO，并在设备空出来时把
  `queueNotifications: [{"busid": "...", "action": "attach"}]` **一次性**塞进
  该客户端的心跳响应里。
* 客户端收到 `attach` 通知就真正发起连接；成功连接后，它下一次心跳里的
  `busids` 会让服务端自动把它从等待名单里摘掉（无需额外的"出队"请求）。
* 管理员取消共享（`unshare`）时，等待者会收到 `action: "dropped"`，提示设备已
  不可用，而不是静默消失。
* 等待名单与通知都持久化在 `/run/usbip/queue.json`、`/run/usbip/queue-notify.json`，
  容器重启不会丢掉正在排队的客户端。
* 同一个客户端在同一台设备的空闲窗口内只会被唤醒一次（内存里去重），避免它
  每拍心跳都重复声明意愿时被反复触发。

## 谁能释放设备（安全边界）

`POST /api/clients/heartbeat` 是**免鉴权**的（已发布的 Windows 客户端必须能在没有管理密码
的情况下登记占用方），因此它带来的任何效果都必须按"任何人可伪造"来设计：

* **心跳里的 `shutdown: true` 只释放声明，绝不碰设备。** 服务端只删掉该 `clientId`
  在 `clients.json` 里的记录、清掉它的排队位置和通知信箱；**不会**执行
  `usbip unbind` + `bind`。设备的真实释放交给：客户端 TCP 断开后 usbipd 自然回收、
  收紧后的 watchdog、以及管理员在页面上显式点「强制断开连接」。
* **watchdog 只在"确实没人在用"时才强制释放。** 一个连续 `USBIP_WATCHDOG_MISSED_RUNS`
  个周期没心跳的客户端，其 `busids` 声明会被丢弃；只有当该 busid **当前已共享**，
  且**没有活跃持有者、或活跃持有者的来源地址与这条陈旧记录相同**时，才会
  `unbind` + `bind`。陈旧记录声明的设备正被别人持有时，只丢弃声明，不踢人 ——
  否则伪造一次心跳就能让 watchdog 反过来踢掉真正的使用者。
* **匿名读接口不回显公网地址。** 未带有效 `X-Admin-Token` 的 `GET /api/devices`
  会剥掉 `connections[].publicIp` 与 `currentHolder.publicIp`。这是**部分缓解**：
  `clientId` / `name` / `address` 仍然可见（Windows 客户端用它们显示「他人占用 (addr)」，
  不能去掉），所以攻击者依然能读到 `clientId` 并伪造占用方登记 —— 但已无法借此
  拆断任何正在使用的 USB/IP 会话。

### 已知未修复项（明确记录，避免误解为"已经安全"）

| 项 | 说明 |
|---|---|
| 明文 HTTP 传令牌 | 管理页与 API 仍是 HTTP，令牌/TLS 未加；**必须**只在可信内网暴露，不要把端口映射到公网 |
| 共享访问密码也是明文 | `USBIP_ACCESS_PASSWORD` 同样走明文 HTTP，和上面的管理密码是同一类问题；不要把它当成能暴露到公网的口令 |
| 授权按来源 IP 记账 | NAT / 多出口后多台机器共用一个源 IP，会共享同一份授权（一台授权=它们都能连）；反过来吊销也会一起被吊销。这是"USB/IP 协议没有认证"的必然取舍 |
| 吊销不影响已建立的连接 | `gateway.py` 只在连接建立时判定一次，所以 `revoke` 之后已连上的 USB/IP 会话仍然保持，直到客户端自己断开；要立刻断开请用管理页「强制断开连接」 |
| 容器重启后需重新授权 | 授权表默认在 `/run/usbip`（tmpfs），且启动时总会写成空表：重启/改密码都会清空所有授权，客户端需要重新输一次密码 |
| 授权表损坏 = 全拒 | 网关读到损坏的授权表会拒绝一切 USB/IP 连接（宁可拒绝也不放行）。`web.py` 的写入是原子的，正常不会出现。**`web.py` 自己也 fail-closed**：授权表存在但读不出来时，`authorize` / `renew` / `revoke` 一律返回 `500` 且**不写盘**（绝不会把"读失败"当成"表是空的"，用一张只剩自己的表覆盖掉别人的授权）。恢复办法：修好或删掉授权表文件 |
| `privileged: true` | 容器仍以特权运行（访问宿主内核 usbip 模块所需），未改 |
| 心跳写放大 | 一次心跳可能触发多次全量 JSON 写 + fsync，未优化（启用访问控制后每次心跳还会多一次授权表写入） |
| 占用方身份未认证 | `clientId` 只是客户端自称，无法证明"我就是上次那台机器"；占用显示、排队位置因此可被伪造（不影响设备释放） |
| 已绑定的网卡无法识别 | 设备一旦被 `usbip-host` 绑定，`net/` 就消失了，网卡判据失效；这种状态只能用「停止共享」解（见文首 ⚠️ 小节） |
| 慢速连接 | `web.py` 的 HTTP 连接有 15s 读超时，网关首字节有 10s 超时；转发阶段仍不设超时（否则会拆断空闲 USB/IP 会话） |

## 测试

`tests/` 下是可直接运行的回归测试（不需要真实 USB 设备）：

```sh
python tests/test_metadata_keys.py     # 设备名称键规则（21 项）
python tests/test_auth_flow.py         # 登录/会话端到端：随机初始密码、mustChange、env 托管、模拟容器重启（30 项）
python tests/test_auth_hardening.py    # 随机密码文件、mustChange 403、登录限速、env 托管散列、令牌解析（60 项）
python tests/test_gateway_split.py     # 单端口分流 + 空闲长会话不被拆断（5 项，约 15s）
python tests/test_public_ip.py         # 心跳里的公网 IP 只接受可全局路由地址（5 项）
python tests/test_device_exposure.py   # 匿名 GET /api/devices 剥离 publicIp、客户端字段不变（24 项）
python tests/test_queue.py             # 排队：FIFO、自动出队、轮到你了、告别清理（13 项）
python tests/test_shutdown_no_kick.py  # 伪造 shutdown/陈旧 busids 不得触发 unbind+bind（12 项）
python tests/test_state_race.py        # clients.json / managed-busids 并发读写不丢更新（6 项）
python tests/test_hidden_devices.py    # 隐藏设备：可见性、includeHidden、先停共享、409、硬件键跟随（55 项）
python tests/test_netdev_guard.py      # 网卡保护：net/ 判据、share/kick 拒绝、逃生阀、networkInterfaces（40 项）
python tests/test_access_gate.py       # 共享访问密码：向后兼容锚点、API 门、授权表、限速、吊销、网关判定（90 项）
python tests/test_access_boundary.py   # 访问控制信任边界：PROXY 行伪造、授权表损坏 fail-closed、网关缓存（38 项）
sh     tests/test_entrypoint_netdev_guard.sh  # 入口脚本 bind_one 网卡拦截 + 默认路由判据（18 项）
node   tests/test_ui_boot.js           # 管理页：直接抽取 index.html 的内联脚本跑（81 项）
```

也可以用 `python -m unittest discover -s tests -p 'test_queue.py'` 只跑排队那一套。
除 `test_public_ip.py` / `test_queue.py` 外，其余都是"导入即执行"的脚本（不是 unittest
用例），所以 `unittest discover` 会把它们报成导入错误——直接按上面的方式单独运行即可。

除 `test_ui_boot.js` 外都用临时目录承载状态文件，不会碰真实配置；`test_ui_boot.js`
直接从 `index.html` 读取真实脚本，不存在"测试跑的是生成物、和页面不同步"的问题。

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

**忘了管理密码**
两种办法：① 在 `.env` 里设置 `USBIP_WEB_PASSWORD=<新密码>` 后 `docker compose up -d`，
容器启动时会把散列同步过去；② 删掉 `./config/auth.json` 与 `./config/initial-password.txt`
后重启，会重新生成一个随机初始密码（打印在日志里，同时要求改密）。
注意两种办法都会让此前所有登录令牌立即失效。

**页面提示"必须修改初始密码"（403 must_change_password）**
这是 `mustChange=true` 时的服务端强制：先用初始密码登录，页面会弹出改密框；
若已关闭弹窗，可点右上角「修改密码」。也可以在 `.env` 里用 `USBIP_WEB_PASSWORD` 固定密码。

**登录报 429 / 提示"尝试次数过多"**
按来源 IP 的登录限速：连续 5 次失败后锁定 30s，之后每次翻倍（上限 15 分钟）。
等 `Retry-After` 秒后重试；成功登录一次即清零。容器重启也会清空。
`POST /api/access/authorize` 输错**共享访问密码**走的是同一套限速（同一来源 IP）。

**客户端提示"需要访问密码"/连上但看不到设备**
说明服务端启用了 `USBIP_ACCESS_PASSWORD`。客户端要先用密码调
`POST /api/access/authorize` 换取授权（授权单位是它的**来源 IP**）；授权后心跳与
`GET /api/devices` 才会恢复。排查顺序：
① `GET /api/access` 看 `required` 是不是 true；
② 看 `docker compose logs` 里有没有 `refused USB/IP connection from <IP>`（被网关拒了）
或 `access authorized: address=<IP>`（授权成功，但可能和客户端实际出口 IP 不同，例如
客户端走了另一个网卡 / NAT）；
③ 授权有效期默认 12 小时且心跳续期，若客户端有心跳间隔超过 TTL 的休眠策略，
把 `USBIP_ACCESS_TTL_SECONDS` 调大。

**启用访问密码后，管理页要求重新登录 / 看不到设备**
管理页带 `X-Admin-Token` 时不受访问密码影响；但若令牌已过期（`/api/session` 返回
`authorized:false`），页面会拿到 `401 {"accessRequired": true}` —— 重新登录即可。
注意飞牛的统一网关是经 Unix socket 连 `web.py` 的，对端是 127.0.0.1，同样要靠登录令牌。

**某台设备在列表里"消失"了（客户端也看不到）**
多半是被隐藏了（防止 USB 网卡之类的设备被共享出去）。管理页勾选「显示隐藏设备」
（即带 `includeHidden=1`）就能看到它，点「取消隐藏」即可恢复；也可以直接调
`POST /api/devices/<busid>/unhide`。隐藏状态存在 `./config/device-metadata.json`，
用文本编辑器删掉对应记录里的 `"hidden": "true"` 也能恢复（改前建议先备份）。

**不小心把网卡共享出去了，NAS 已经掉网、SSH 连不上**
先用键盘/显示器在 NAS 本地控制台执行（busid 换成实际值）：
`usbip unbind -b 2-7`，网络会立刻恢复。恢复后确认它没有留在
`./config/../managed-busids` 或 `.env` 的 `USBIP_BUSIDS` 里，否则重启还会再绑一次；
本版本起这两处都有网卡拦截，正常不会再发生。

**点了「开始共享」报 409「这是一块 USB 网卡」**
这是保护，不是故障：共享它会直接让 NAS 掉线。确实要共享请在宿主上手工执行
`usbip bind -b <busid>`；若这台机器另有管理通道，也可以设 `USBIP_ALLOW_NETDEV=true`
（危险，见文首 ⚠️ 小节）后由页面操作。多数情况下你要的其实是「隐藏设备」。

## 许可

自有部分 MIT（见仓库根 `LICENSE`）；运行时依赖的许可与义务见仓库根 `THIRD-PARTY.md`。
