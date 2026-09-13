# usbip-share

把 Linux NAS / 主机上的 USB 设备（加密狗、绘图锁、网银 U 盾、串口设备等）通过内核
**USB/IP** 协议共享给局域网内的 Windows 电脑使用，并附带：

* **服务端**：Docker Compose 部署，USB/IP 数据与管理接口**共用一个端口**，内置中文管理页；
* **客户端**：基于 [usbip-win2](https://github.com/vadimgrn/usbip-win2) 的中文 Windows 客户端
  （驱动 + 用户态程序），可选管理接口增强（显示服务器端设备名称、备注、当前占用方）。

```text
物理 USB 设备
      │
      ▼
 Linux NAS / 主机（usbipd，内核 usbip-host 驱动）
      │  TCP 5555（USB/IP 协议 + 管理 HTTP 共用一个端口）
      ▼
 Windows 客户端（usbip-win2 驱动 + 中文界面）
      │
      ▼
 Windows 应用（算量软件、加密锁客户端等）
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `server/` | 服务端：Dockerfile、compose、单端口分流网关、中文管理页、设备名称/备注元数据维护逻辑与自动化测试 |
| `client/` | Windows 客户端：上游 BSD 许可声明 + 本分支相对基线的完整改动补丁 + 构建说明 |

## 致谢

本项目 Windows 客户端基于 **[usbip-win2](https://github.com/vadimgrn/usbip-win2)**，
由 **[Vadym Hrynchyshyn](https://github.com/vadimgrn)** 原创并以 BSD 2-Clause 许可开源。

`vadimgrn/usbip-win2` 把 Linux 内核的 USB/IP 协议带到 Windows，并附带了本仓库
修改的基线 (`74f5a7fa`)。`server/` 之外的所有客户端改动都在 `client/changes.patch`
里，方便与上游同步演进。

**没有原作者把 USB/IP 在 Windows 上跑通，就没有这个项目**——衷心感谢
Vadym Hrynchyshyn 在 USB/IP Windows 支持上的持续贡献。

## 快速开始（服务端）

```sh
cd server
cp .env.example .env      # 按需修改端口等
docker compose up -d --build
```

然后浏览器打开 `http://<NAS-IP>:5555/`：首次登录密码为 `123456`，登录后请立即修改。
设备共享、名称与备注都在这个页面操作。详细步骤见 [`server/README.md`](server/README.md)。

## 快速开始（客户端）

* **用预编译版本**：到 [Releases](https://github.com/yjzsg/usbip-share/releases) 下载，
  两个版本任选其一，sha256 在对应 Release 页标注：
  * `USB-SHARE-Setup-0.9.8.0.exe`（约 22 MB，含驱动 + 单文件客户端 + 中文管理页安装器）；
  * `USB-SHARE-Client-0.9.8.0.zip`（约 2.6 MB，仅单文件客户端，需先装过驱动）。
* **自己构建**：客户端是 `usbip-win2` 的修改版，按 [`client/README.md`](client/README.md)
  应用 `client/changes.patch` 后用 Visual Studio 构建。

## 设备名称是怎么认出来的

服务端把「名称/备注」和**硬件本身**绑定，而不是和 USB 端口绑定，所以在同一个口上换设备、
或把同一个设备换到另一个口，显示的名称都不会串。取键优先级：

| 优先级 | 识别依据 | 适用情况 |
|---|---|---|
| 1 | 设备序列号 | 设备自带唯一序列号 |
| 2 | 设备型号指纹（VID/PID + 厂商 + 产品 + bcdDevice + 设备类）| 该型号当前只接入 1 台 |
| 3 | 同型号组内序号（按 Bus ID 顺序）| 同型号多台且**没有**序列号 |

管理页的「识别依据」列会直接告诉你每台设备当前用的是哪一条。第 3 种情况下，
同型号无序列号的多台设备**互换插口会让序号互换**——这是硬件信息不足时的固有限制，
页面会对这种设备给出提示。

## 客户端的实时状态

### 状态列显示什么

设备列表的「状态」列同时把**本机驱动层**和**服务端元数据**合并成一段文案，避免
「看着像空闲但其实被别人被占」这种误连。常见取值：

| 显示 | 含义 |
|---|---|
| 已连接 (N) | 本机已挂载，N = 同时持有该设备的客户端总数（含本机） |
| 他人占用 (N) | 本机未连，服务端记录被别人持有，N = 持有方数 |
| 排队中 | 本机已申请排队，等服务端设备空出来时自动 attach |
| 空闲 | 服务端已共享且当前没人持有 |
| 挂载中 | 本机正在连接（驱动 `connecting`，文案比「连接中」更友好） |
| 断开中 | 本机正在断开 |
| 未连接 | 本机未连、服务端管理接口暂时没拉到这台设备 |

`update_from_registry()` / `on_device_state()` / `mgmt_apply_event()` 都会重新计算
并按需刷文案，避免在「驱动 unplugged → meta busy」切换时出现一闪而过的「未连接」。

### 被占用的设备对其他客户端可见 + 排队

服务端在 `/api/devices` 响应里返回每台设备的 `connections`（所有持有方）和
`pendingQueue`（等待队列）。客户端拿到后会在设备列表里：

* 「占用方」列：把当前所有持有方拼成「clientA / clientB」式的串，本机那台标记为
  「本机」避免误识；
* 「排队」列：把队列里等候的客户端按 FIFO 顺序展示；
* 状态列：若本机已加入排队而设备仍被占用，显示「排队中」而非「未连接」。

双击被占用的设备会调用 `POST /api/clients/heartbeat` 的 `enqueueBusids` 字段声明
排队意愿，每拍心跳都重复声明，服务端按 `clientId` 去重并维持顺序；设备空出来时
服务端在该 client 心跳响应里下发 `queueNotifications`，客户端解析后真正触发
attach——保留 GUI 线程的同条 attach 路径，不引入旁路。

## 安全提示

* USB/IP 协议**本身没有加密与认证**，请只在可信局域网内使用，不要把数据端口暴露到公网。
* 管理页目前仅支持「密码 + 本地存储的登录令牌」，同样不要暴露到公网；
  只读设备列表为了兼容 Windows 客户端是免密码的，写操作需要登录。
* 服务端容器需要 `privileged: true` 才能加载宿主内核模块并绑定 USB 设备，请自行评估风险。

## 许可

* 本仓库自有部分（`server/` 等）采用 [MIT 许可](LICENSE)。
* Windows 客户端是 `usbip-win2` 的修改版，其原始版权与许可条款见
  [`client/LICENSE.txt`](client/LICENSE.txt)（BSD 2-Clause），修改与再分发请一并保留该声明。
* 其余第三方组件（wxWidgets、内核 usbip 等）的许可与义务见 [THIRD-PARTY.md](THIRD-PARTY.md)。

## 免责声明

本项目与上述任何第三方软件、硬件厂商均无隶属或合作关系；文档中出现的他方名称仅用于
说明来源与技术兼容性，不代表其认可或背书。请自行确认在你的使用场景下的合规性。
