# 第三方组件与许可义务

本仓库包含的代码分两类，义务不同，请分别对待。

## 1. 客户端：usbip-win2（BSD 2-Clause）

`client/` 下的改动基于 **[usbip-win2](https://github.com/vadimgrn/usbip-win2)**
（作者 Vadym Hrynchyshyn），采用 **BSD 2-Clause** 许可——允许修改、改名、闭源再分发。
许可全文见 [`client/LICENSE.txt`](client/LICENSE.txt)。

使用该代码时**必须满足**的两条：

1. **源码分发**保留原始版权声明、条款列表与免责声明（本仓库已通过 `client/LICENSE.txt` 满足）；
2. **二进制分发**（例如把编译出的 `USB-IP-中文客户端.exe` 发给别人）在其文档或随附材料中
   复现上述版权声明、条款与免责声明。

BSD 2-Clause 不授予商标权：请勿使用原作者姓名或项目名称为你的发行版本背书。

## 2. 客户端依赖：wxWidgets

客户端用户态程序通过 vcpkg 依赖 **wxWidgets 3.3.3**（`userspace/wusbip/vcpkg.json`），
采用 **wxWindows Library Licence**：以 LGPL 为基础，但附带**静态链接例外**——
静态链接进可执行文件时无需公开自有源码；相应义务是随二进制提供该许可文本与声明。
wxWidgets 会带入若干图形相关的传递依赖（如 zlib、libpng、libjpeg-turbo、libtiff），
各自许可随 vcpkg 安装目录提供。

## 3. 服务端运行时：内核 usbip / usbip-utils（GPL-2.0）

服务端容器基于 `debian:12-slim`，并通过 `apt-get install usbip` 安装用户态工具；
真正的数据通道由**宿主 Linux 内核**的 `usbip-core` / `usbip-host` 模块提供。

* 本仓库**不包含**这些 GPL-2.0 二进制，也不把它们打包进发布物——它们由 Debian 仓库与
  宿主内核提供，属运行时依赖。
* **如果你把构建好的整个容器镜像对外分发**（镜像里含 usbip 用户态二进制），
  则需按 GPL-2.0 履行相应义务（提供对应源码或获取方式）。发行方是你，不是本仓库。

## 4. 其他

* 管理页、分流网关、元数据逻辑等自有代码采用 MIT（见 [`LICENSE`](LICENSE)）。
* 文档中出现的第三方产品名称仅用于说明来源或技术兼容性，不表示任何隶属、认可或背书关系。
