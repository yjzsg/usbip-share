# USB-SHARE 飞牛 fnOS 应用包

把 `../`（服务端源码）打包成飞牛应用：装好后在飞牛桌面点图标，就能在**小窗里打开中文管理页**；
设备共享、改名、备注、强制断开都在这个页面里点，不需要手填 busid；
服务端口、管理页密码、客户端失联释放时间等在「应用设置」里改。

```text
USB 设备 → 宿主内核 usbip-host → 容器内 usbipd（内部随机端口）
                                      ↑
                        单端口分流网关 0.0.0.0:<应用设置里的端口>
                                      ↓
                     /api/*、管理页 ←→ web.py（容器内 127.0.0.1:随机端口）
                                      ↑
                 飞牛统一网关 ── target/app.sock（桌面小窗走这条）

Windows 客户端 ──→ 宿主 <端口>（USB/IP 协议）
```

对外**只有一个端口**：Windows 客户端连它；管理页既可以直接用浏览器访问它，
也可以从飞牛桌面小窗（走统一网关、复用 NAS 登录态）打开。

## 目录结构

```text
server/fnos/
├── build.sh              # 同步源码 → 校验 → fnpack build（产物落在 dist/）
├── verify.sh             # 打包前的静态校验
├── dist/                 # 产物 usbip-share.fpk
└── package/              # fnpack 的输入（这个目录就是要打包的应用）
    ├── manifest
    ├── ICON.PNG / ICON_256.PNG
    ├── config/{privilege,resource}
    ├── wizard/{install,config,uninstall}
    ├── cmd/{main,lib-config.sh,install_*,upgrade_*,uninstall_*,config_*}
    └── app/
        ├── ui/{config,images/icon_{64,256}.png}
        └── docker/
            ├── docker-compose.yaml
            ├── Dockerfile                # 与 ../Dockerfile 的差异见文件头注释
            ├── fnos-env.sh               # 读应用设置 + 数据路径 + 挑内部空闲端口
            ├── entrypoint-fnos.sh        # 容器入口包装
            ├── healthcheck-fnos.sh       # 健康检查包装
            ├── fnos-unix-proxy.py        # 统一网关 Unix Socket 转发器（小窗入口）
            └── （web.py / gateway.py / index.html / entrypoint.sh /
                 healthcheck.sh / favicon.ico 由 build.sh 从 ../ 同步，已 gitignore）
```

## 构建

```sh
# 在飞牛宿主上（自带 /usr/local/bin/fnpack）
bash build.sh            # 同步 + 校验 + 打包 → dist/usbip-share.fpk
bash build.sh --check    # 只同步 + 校验，不打包
```

fnpack 也可以从官方静态站取：`https://static2.fnnas.com/fnpack/fnpack-1.2.3-linux-amd64`

## 安装 / 升级

```sh
cat > /tmp/wizard.env <<'EOF'
port=5555
admin_password=改成你自己的
restore_shared=true
load_module=true
unbind_on_exit=false
kick_idle_seconds=60
EOF

sudo appcenter-cli install-fpk dist/usbip-share.fpk --env /tmp/wizard.env -v 1
sudo appcenter-cli start usbip-share
sudo appcenter-cli status usbip-share
```

**升级和卸载必须走飞牛网页端「应用中心」**：`appcenter-cli` 对已安装应用会直接拒绝
（`Failed to uninstall usbip-share, please uninstall it from Web UI`），而且同 appname 的
`install-fpk` 在已安装时是空操作、不会升级。网页端「应用中心 → USB-SHARE → 手动安装 / 卸载」正常。

## 应用设置能改什么

| 字段 | 说明 |
|---|---|
| `port` | 对外服务端口（USB/IP + 管理页共用）。留空 = 保持当前值。桌面小窗走统一网关，不依赖这个端口。 |
| `admin_password` | 管理页密码。留空 = 不改。密码由环境变量托管，每次启动同步进 `auth.json`。 |
| `access_mode` | 共享访问密码：`disable`（关闭，任何客户端都能连）/ `enable`（启用或改密码）。选「启用」且密码框留空 = 沿用当前密码。 |
| `access_password` | 共享访问密码（6-64 位）。选「启用」时留空 = 沿用当前密码；选「关闭」时必须留空，否则保存会报错（见下面踩坑记录）。 |
| `restore_shared` | **重启后自动恢复上次共享的设备**。管理页每共享/取消一台设备都会维护 `<appdata>/managed-busids`，这个开关决定启动时要不要照它恢复。 |
| `kick_idle_seconds` | 客户端失联多久后释放其设备，`0` = 不自动释放。留空 = 保持当前值。 |
| `load_module` | 启动时 `modprobe usbip_host`。 |
| `unbind_on_exit` | 停止应用时把设备归还给原驱动。 |

布尔项在「应用设置」里是 radio，**表单会用已保存的值回填**（真机实测：`initValue` 是空串时表单里显示的仍是当前端口），所以 `initValue` 只表示"第一次的默认值"，不必写成「保持不变」这种占位语义。安装向导用 `enable`/`disable`、应用设置用 `true`/`false`，`usbshare_pick_tristate` 两套都认（还认 `keep`/`on`/`off`/`yes`/`no`/`1`/`0`）；取值写错会静默落回「当前值」——`verify.sh` 只放行词表里的取值。

> ⚠️ 这套设计成立的前提是上面那条"表单会回填当前值"。如果哪天出现"只是打开应用设置再保存，设置就被改回默认值"，说明回填没有发生，得把「保持不变」三态加回来（`lib-config.sh` 的 `usbshare_pick_tristate` 与 `access_mode` 分支一直保留 `keep` 支持）。

密码里出现空格、`$`、反引号都没问题：env 文件是**逐行解析**的（`fnos-env.sh` 的 `usbshare_load_env_file`），不经过 shell 展开。**不要**把 `<appconf>/usbip-share.env` 交给 `.`(source) 去读 —— 早期版本就是这么干的，密码里的空格会把整行拆开（容器直接以 127 退出起不来），`$`/反引号会被展开（密码被静默改掉，甚至在特权容器里执行命令）。

卸载时「删除全部数据」会删掉 `<appdata>/usbip-share` **和** `<appconf>/usbip-share/usbip-share.env`：后者含**明文**的管理页密码与共享访问密码，不删就等于把密码留在 NAS 上（fnOS 卸载后不会自己清理 `@appconf` / `@appdata`，本机几个已卸载应用的目录至今还在）。

保存后 `cmd/config_callback` 重写 `<appconf>/usbip-share.env` 并 `docker restart` 容器
（配置是挂载进去的文件，不需要重建容器）。

### 为什么没有「勾选设备」的选项

飞牛向导的 `checkbox` / `select` 选项是**打包时静态写死在 JSON 里**的，而 USB 设备是装完之后
才插上的，向导里没法枚举。所以默认行为改成：**在管理页点一次「开始共享」，以后重启自动恢复**。
想预置一批设备的话，手工往 `<appconf>/usbip-share.env` 里写 `USBIP_BUSIDS=1-1,1-2.3` 即可
（`USBIP_BUSIDS` 是 `usbshare_apply_wizard` 会原样保留的键，保存应用设置不会把它冲掉）。

## 关键设计决定

**1. 桌面入口用统一网关，不用端口入口。**
实测 `type: iframe` + `port: ${port}` 在飞牛桌面里弹得出窗口但页面打不开（这台机器上所有能用的
`type: iframe` 第三方应用——fn-seekbox、fn-deepseek-harness——都走统一网关；用 `port` 的
Sun-Panel / OpenList / Home-Assistant 全部是 `type: url` 新标签页）。
所以改成 `gatewayPrefix: /app/usbip-share` + `gatewaySocket: app.sock`：
容器里多跑一个 `fnos-unix-proxy.py`，在应用自己的 `target/app.sock` 上收请求、
剥掉网关前缀再转给 `web.py`。好处是顺带复用了 NAS 登录态。

**2. 端口不写进 docker-compose.yaml。**
compose 的变量替换发生在飞牛启动 compose 之前，无法确定它会不会把未知变量替换成空串。
所以端口等参数落在 `<appconf>/usbip-share.env`，由容器启动时的包装脚本读取。
（`TRIM_PKGETC` / `TRIM_PKGVAR` / `TRIM_APPDEST` 这类 `TRIM_*` 变量在 compose 里**是**会被替换的，已实测。）

**3. `network_mode: host` + 动态内部端口。**
USB/IP 需要宿主上稳定、可预期的端口，走宿主网络少一层映射。代价是容器里 bind 的每个端口都是
宿主端口，所以 usbipd 与 web.py 的内部端口在容器启动时现挑空闲端口（写进
`<appdata>/internal-ports.env`，healthcheck 读同一份）——写死 8080 会被宿主上别的应用占掉。

**4. `run-as: package` + `join-groups: ["docker"]`。**
生命周期脚本只需要 `docker restart` / `docker inspect`，给包用户 docker 组权限即可，不需要 root
（飞牛第三方应用上架本来就拒绝 root 权限）。实测包用户确实被加进了 docker 组。

**5. 管理页同时支持两种访问路径。**
`index.html` 里的 `/api/*` 是根路径写法，走网关时会打到 NAS 域名的 `/api` 上。
页面里加了一段前缀探测：路径以 `/app/usbip-share` 开头时给 `fetch` 统一补前缀。
静态资源（favicon）也改成了相对路径。两条路径都能用。

## 踩坑记录（真机实测，官方文档没写）

| 现象 | 根因 | 处理 |
|---|---|---|
| `File "install" is not valid due to JSON format or content validation failure` | **`switch` 的 `initValue` 写成布尔 `true`/`false` 会校验失败**，必须写字符串 `"true"`/`"false"`。官方文档示例是布尔值。 | `verify.sh` 强制检查所有 `initValue` 必须是字符串，且每个 step 必须有 `stepTitle`。 |
| 同上，且 JSON 明显合法 | **`fnpack` 打包失败时仍返回 exit 0**，用退出码判断会得到假阳性（我在这上面浪费了一轮排查）。 | 一律用输出里的 `Packing successfully` / 是否产出 `.fpk` 判断。 |
| `Parse manifest file ... failed: key-value delimiter not found` | `manifest` 是键值格式，**值不能跨行**。 | `verify.sh` 检查 manifest 每一行都含 `=`。 |
| `mkdir /tmp/fnpack.*/app/docker: permission denied` | 通过 SMB/Windows 共享创建的文件权限位是 `0000`，fnpack 复制目录时沿用源权限。 | `build.sh` 打包前统一 chmod，并一律复制到本地临时目录再打包。 |
| 内容 md5 一致但 fnpack 时而报 JSON 非法 | 共享目录对刚由 SMB 写入的文件存在内容可见性抖动。 | 打包在本地临时目录里做并在那里重建文件；失败自动重试 3 次。 |
| 容器反复重启，`OSError: [Errno 98] Address already in use` | host 网络下 web.py 绑 `0.0.0.0:8080`，与宿主 seafile 冲突。 | 内部端口动态挑选 + web.py 只绑 `127.0.0.1`。 |
| `Failed to launch app. error code 12005` + `service "server" has no container to start` | **compose 项目名必须等于 appname 小写**（Home-Assistant 就是 `home-assistant`）。手动 `docker compose up` 时项目名会取目录名（`docker`），应用中心就找不到容器。 | compose 里显式写 `name: usbip-share`。 |
| `appcenter-cli uninstall` 被拒 | 飞牛限制：已安装应用只能从网页端卸载。 | 升级/卸载走网页端，文档已写明。 |
| `appcenter-cli start` 报 `error code 10500` 但应用其实在跑 | 安装后应用中心会自动拉起容器，紧接着的 `start` 撞上了。 | 以 `appcenter-cli status` / 容器状态为准。 |
| `type: iframe` + `port: ${port}` 弹窗白屏 | 端口入口在桌面 iframe 里不可用。 | 改用统一网关入口。 |
| 密码里带空格 → 容器反复重启，日志只有一行 `123456: not found`；密码里带 `$`/反引号 → 密码被静默改掉、反引号里的命令在特权容器里被执行 | env 文件是用 `.`(source) + `set -a` 读的，shell 把值当代码解析：`USBIP_WEB_PASSWORD=abc 123456` 里 `123456` 变成了要执行的命令（赋值也不再生效），`pa$$word` 被展开成 `pa<PID>word` | `fnos-env.sh` 改成逐行解析（`usbshare_load_env_file`），不 source / 不 eval；`verify.sh` 里加了防回归检查 |
| 安装向导里填了共享访问密码，但没把默认的「不启用」改成「启用」→ 密码被静默丢掉，访问控制根本没生效，界面看不出异常 | `access_mode` 的 radio 默认值是 `disable`，而脚本把 `disable` 当成"明确关闭并清空密码"。1.3.1 的变更说明声称已修，其实只修了「应用设置」那一侧 | 矛盾输入（选了关闭却填了密码）改成报错中止，不再静默丢值 |
| 卸载时勾了「删除全部数据」，明文的管理页/共享访问密码仍留在 NAS 上 | `uninstall_callback` 只删了 `@appdata`，`@appconf/usbip-share.env`（明文密码）没动；fnOS 卸载后不会清理 `@appconf`/`@appdata`（本机已卸载的 cloud-idphoto / StirlingPDF / xiaomusic / Lucky / Cloudflare-QT 目录至今还在） | 一并删除 env 文件与 `.bak`，空了就删目录 |
| 应用端口设成 32768-60999 里的值（例如 45000）→ 容器永久重启循环，重试也不自愈 | 内部端口用 `bind(0)` 现挑，内核只会从临时端口范围里取；挑端口时对外端口还没被监听，可能正好被挑中，随后网关 `EADDRINUSE`，而 `/data/internal-ports.env` 里存的还是那个值 | 挑内部端口时显式排除 `USBIP_PORT` |
| 用 Windows 编辑器手工改 env 文件后，`USBIP_LOAD_MODULE=true` 之类的值不生效 | CRLF：值末尾带了 `\r`，`[ "$X" = "true" ]` 判不等 | 解析器剥掉行尾 CR，`usbshare_current` 同样处理 |

## 安全护栏（USB 网卡 / 隐藏设备）

**为什么要有这个**：这台测试机的唯一在用网卡就是一块 USB 网卡（Realtek RTL8156，busid `2-7`，
承载默认路由）。把它共享出去 = NAS 自己掉线，而且远端 SSH 一起断，用户可能再也连不回来。
实测日志（两次掉网都是这么来的）：

```text
21:28:09 usbip-host 2-7: usbip-host: register new device   ← 误点「开始共享」
21:29:18 usbip-host 2-7: USB disconnect                     ← 掉网约 1.5 分钟
21:42:13 usbip-host 2-7: usbip-host: register new device   ← 容器重启时被「自动恢复共享」又绑了一次
```

### 三道防线

| # | 位置 | 规则 |
|---|---|---|
| 1 | `web.py` `mutate_device('share')` | 设备导出网络接口（`/sys/bus/usb/devices/<busid>:*/net/*` 非空）→ 409 拒绝，**不执行 bind** |
| 2 | `web.py` `kick_device()` | 同上。「强制断开」是除 share 外唯一会 `bind` 的地方（它 unbind 完再 bind 回去），不拦的话点它同样会掉网 |
| 3 | `entrypoint.sh` `bind_one()` | 启动时自动共享（`USBIP_BUSIDS` / 「重启后自动恢复上次共享的设备」）同样拦截，只 `log REFUSING` + 跳过，**不 fail**，一个坏条目不会让容器起不来 |

**兜底**：`entrypoint.sh` 在启动绑定循环前后各查一次 `/proc/net/route` 的默认路由；若"之前有、之后没了"，
就把本轮 bind 过的设备全部 `unbind`、并从 `managed-busids` 里摘掉，然后打 ERROR 日志。这一条与驱动无关，
即使判据 1/3 都漏了（例如设备已被 usbip-host 绑着、`net/` 目录已经消失）也能自己把网抢回来。

**判据为什么用 `net/` 而不是设备类**：这块 RTL8156 的 `bInterfaceClass` 是 `ff`（厂商私有类），
按设备类根本认不出来；而 `/sys/bus/usb/devices/2-7:1.0/net/enxc84d44294124` 是可靠的。

**逃生阀**：`USBIP_ALLOW_NETDEV=true` 会关掉 1/2/3 三道判据（危险，默认关）。飞牛应用里它没有暴露在
「应用设置」中；真要开，手工往 `<appconf>/usbip-share.env` 里加一行 `USBIP_ALLOW_NETDEV=true`
（`fnos-env.sh` 会把该文件里每个变量名合法的键都导出，所以这样是生效的），然后重启应用。
**注意**：`usbshare_write_env` 每次都按固定的键集合重写这个文件，所以**任何一次「应用设置」保存都会
冲掉这类手工加的键**（只有 `USBIP_BUSIDS` 例外，它会被原样带过去）。改完设置记得回头再补上。

### 隐藏设备

管理页每行有「隐藏」按钮，勾选筛选栏的「已隐藏的设备 / 显示出来」可以看到并取消隐藏。

- 隐藏标记存在 `device-metadata.json` 里、**绑定硬件**（和名称/备注同一套键），换 USB 口也仍然隐藏。
- 隐藏的设备**默认不出现在 `/api/devices`**（Windows 客户端因此也看不到、点不到）；
  只有「带有效管理员令牌 + `includeHidden=1`」才返回。
- 隐藏一块正在共享的设备会**先停止共享**（只 `unbind`，把设备还给原驱动），并在返回消息里说明。
- 已隐藏的设备调 `share` 会被 409 拒绝。
- 导出网络接口的设备在页面上标「系统网卡·不可共享」，共享按钮置灰 —— 这比隐藏更透明，
  服务端本来也会拒。

## 已知限制

- **内部 usbipd 端口会监听在 `0.0.0.0`**（usbipd 没有绑定地址选项），即宿主上会多一个随机高位端口
  暴露原始 USB/IP 协议。它和 5555 上的网关是同一个协议、同样的能力，不额外放宽权限；
  但如果要对外做端口白名单，只放行应用设置里的那个端口即可。
- **管理页是明文 HTTP**，和 USB/IP 一样只在可信局域网内使用，不要把端口暴露到公网。
- **`<appconf>/usbip-share.env` 里是明文密码**（管理页密码、共享访问密码），文件权限 0600、属主是包用户；
  `@appdata/auth.json` 里存的只是散列。备份/迁移/把存储空间交给别人之前留意这个文件
  （卸载时勾「删除全部数据」会一并删掉它）。
- **容器是 `privileged: true`**：usbip-host 是宿主内核驱动，容器要 `modprobe` 并直接 bind/unbind
  设备。与本项目原本的 docker compose 部署一致。上架飞牛应用中心前需先向飞牛确认这一条是否允许。
- **应用设置里改端口会短暂中断正在使用的 USB/IP 会话**（容器重启）。
- **`app/ui/config` 里的 `gatewayPrefix` 写死了 `/app/usbip-share`**，`index.html` 里的前缀探测
  也认这个字符串；改 `appname` 时要一起改。

## 真机验收清单

- [x] `fnpack build` 产出 `dist/usbip-share.fpk`
- [x] `appcenter-cli install-fpk` 安装成功；`config/privilege` 的 docker 组生效
- [x] 容器 `running`，`docker inspect` 项目名为 `usbip-share`
- [x] 应用设置端口生效：5555 监听，`/api/health` 200
- [x] `cmd/main status` 返回 0
- [x] 管理页登录成功，`/api/session` 返回 `passwordManaged: true`
- [x] 设备列表、共享/取消共享 API 正常，宿主 `/sys/bus/usb/drivers/usbip-host/` 随之变化
- [x] 改端口（应用设置 → 5556）：env 落盘、容器自动重启、5556 生效
- [x] 统一网关 socket：`/app/usbip-share` 301、`/app/usbip-share/` 200 HTML、
      `/api/*` 与 `/favicon.ico` 均可访问
- [x] 重启后自动恢复上次共享的设备（`managed-busids` → `USBIP_BUSIDS`）
- [x] USB 网卡拦截：故意把 `2-7` 塞进 `USBIP_BUSIDS` 重启 → 日志出现
      `REFUSING to bind 2-7: it exports a network interface`、`produced no new exports`，
      默认路由与网卡驱动均未受影响，`2-7` 也没被记进 `managed-busids`
- [x] `share 2-7` 返回 409 且未执行 bind；`/api/devices` 里 `2-7` 的 `networkInterfaces`
      为 `['enxc84d44294124']`，其余设备为空数组
- [x] `/api/devices` 返回生效中的 `kickIdleSeconds`，页面按它渲染失联释放时长
- [x] 密码链：用带空格的密码 `pa ss word 123` / `cli ent pw` 走一遍 `lib-config.sh → env 文件 →
      容器`，容器 `healthy`，`/api/login` 200、`/api/access/authorize` 200（对照：旧版 source 读法
      在同一个文件上会 `ss: not found` 直接退出）
- [x] 安装向导里「选了不启用却填了密码」→ `install_callback` 返回 1 并给出提示，不再静默丢密码
- [x] 配置文件丢失但 `/data/config.log` 存在 → 容器拒绝启动（exit 1）并打印原因；从未配置过的
      实例仍按原样带兜底值启动（只有 WARNING）
- [x] 卸载 `wizard_delete_data=true` → `@appdata` 与 `@appconf/usbip-share.env` 都被删除
- [x] 飞牛桌面小窗实际打开效果（用户已确认可用）
- [ ] Windows 客户端连新端口做一次真实 attach
- [ ] 默认路由回滚（防线 3 的兜底）只做了单元测试，**未做真机断电式验证** —— 触发它需要真的
      把宿主网卡绑走，风险是 NAS 离线且远端无法恢复。真要做请在现场、并准备一个宿主侧的
      看门狗（测试脚本里的做法：每秒检查 `/sys/bus/usb/drivers/usbip-host/<busid>` 存在就解绑）。
