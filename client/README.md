# Windows 客户端（usbip-win2 修改版）

中文界面的 Windows USB/IP 客户端，基于
**[usbip-win2](https://github.com/vadimgrn/usbip-win2)**（作者 Vadym Hrynchyshyn，**BSD 2-Clause**）
修改而来。原始许可全文见同目录 [`LICENSE.txt`](LICENSE.txt)——修改与再分发必须保留它。

## 本目录内容

| 文件 | 说明 |
|---|---|
| `LICENSE.txt` | 上游原始许可（BSD 2-Clause），必须随源码与二进制保留 |
| `changes.patch` | 本分支相对基线提交（`74f5a7fa`）的**完整差异**，共 150 个文件、约 9.2k 行 |

## 改了什么

* **界面汉化**：菜单、列名、提示、关于对话框等改为中文；
* **中文列**：设备列表新增「服务器备注 / 占用方 / 共享状态」等列，并支持本机备注；
  备注列默认宽度收敛到 180，不再吞屏宽；
* **管理接口集成**（可选）：向服务端的 `/api/clients/heartbeat` 上报本机正在使用的设备，
  从而在服务器管理页显示"当前占用方"；同时读取服务端保存的设备名称与备注。
  不配置管理端口也能正常连接设备，只是少了名称/占用方信息；
* **状态列 union 渲染**：把「本机驱动层状态」与「服务端 meta 的远端语义」合并成一段文案，
  消除「未连接」的歧义——本机 plugged 优先不叠 meta（避免自我回环），他人占用 /
  排队中 / 空闲 / 挂载中 / 断开中 各走对应分支。「已连接 (N)」「他人占用 (N)」
  后缀告诉用户同时持有这台设备的客户端数；`make_device_columns`、
  `on_device_state`、`on_load`、`on_reload`、`add_mgmt_only_devices`、
  `mgmt_apply_event` 都走 union，首次加载就显示正确文案，避免闪烁；
* **被占用的设备对其他客户端可见 + 排队**：服务端 `/api/devices` 返回每台设备的
  `connections` 与 `pendingQueue`，客户端把它们组装成「占用方 / 排队」两列；
  双击被占用的设备会通过 `enqueueBusids` 声明排队意愿，服务端在设备空出来时
  在心跳响应里下发 `queueNotifications`，客户端解析后真正触发 attach——保留 GUI
  线程的同条 attach 路径；
* **公网 IP 鲁棒上报**：`fetch_public_ip()` 依次试 5 个独立服务
  （api64.ipify.org / api.ipify.org / ifconfig.co / icanhazip.com / ifconfig.me），
  每个失败点都 `wxLogVerbose` 输出 Win32 last-error 便于排查；首次失败后每 10 分钟
  重试一次，直到拿到 IP 才停止重试——应对公司代理 / DNS 污染 / 临时防火墙；
* **连接语义**：双击 = 连接一次；勾选 Auto（自动）= 断开后自动重连；
  断开/断开全部/启动时会先停止驱动层残留的自动重连，避免"手动断开却被自动拉回"；
  `attach_persistent_devices` 会先取驱动已挂载清单跳过已挂设备，避免连接 →
  已连接 → 断开的三态循环；
* **单实例**：命名 mutex `Local\USB-SHARE-Client-SingleInstance` + `RegisterWindowMessageW`
  在 `OnInit` 前抢锁；抢不到时通过广播把已有窗口拉到前台，不让用户开多个；
* **托盘常驻 + 默认配置**：启动即有托盘图标，最小化与关闭都收进托盘，退出走托盘菜单；
  首次启动默认勾选「最小化到托盘」与「开机自动启动」，配置直接落注册表保证耐久；
* **驱动侧修复**：完成 URB 时回填实际传输字节数等问题（详见提交记录与补丁）。

> 原先注释中出现过的服务端品牌字样已随服务端改名一并清理，本目录不再引用任何第三方厂商名称。

## 构建

上游项目用 Visual Studio + vcpkg 构建，先准备环境：

1. 安装 Visual Studio 2022（含"使用 C++ 的桌面开发"与 WDK，用于驱动部分）；
2. 克隆上游仓库并检出适配 v0.9.8.0 的基线：

   ```sh
   git clone https://github.com/vadimgrn/usbip-win2.git
   cd usbip-win2
   git checkout 74f5a7fa        # changes.patch 的基线
   git apply /path/to/changes.patch
   ```

3. 按上游 `README.md` 的说明执行 `bootstrap.bat` 拉取 vcpkg 依赖（本项目锁定 wxWidgets 3.3.3），
   然后用 `usbip_win2.slnx` 构建。

补丁是文本 diff，可直接查看每处改动；如果它无法干净应用（上游基线变动），
可以按文件路径手工比对——补丁里每个文件的开头都有完整路径。

## 使用要点

* 服务器地址填 `IP` 或域名，**不要带 `http://` 或 `:端口`**；端口默认 `5555`；
* 管理端口可留空或填与数据端口相同（本服务端两个功能共用 `5555`）；
* **不要**把加密锁类设备（尤其装了同厂商本地驱动的）用于远程共享测试——
  本机既有驱动与 USB/IP 在设备栈上叠加可能导致系统不稳定甚至重启，
  这类设备建议本机直插使用。

## 许可与义务

修改与分发本客户端请遵守 [`LICENSE.txt`](LICENSE.txt)（BSD 2-Clause）：

* 分发**源码**时保留版权声明、条款列表与免责声明；
* 分发**二进制**（例如编译好的 `USB-IP-中文客户端.exe`）时，在其文档或随附材料中复现同样内容；
* 不得使用原作者名义为你的发行版本背书。

客户端静态链接的 wxWidgets 及其传递依赖的许可与义务见仓库根 [`THIRD-PARTY.md`](../THIRD-PARTY.md)。
