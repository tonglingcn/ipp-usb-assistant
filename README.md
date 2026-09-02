# IPP-USB 免驱助手 (IPP-USB Driverless Assistant)

面向 deepin / UOS 的**免驱打印与扫描**原生管理工具。
核心价值：把「IPP-USB 免驱」从命令行排查变成可视化的专业操作界面。

[![Release](https://img.shields.io/github/v/release/tonglingcn/ipp-usb-assistant?label=release)](https://github.com/tonglingcn/ipp-usb-assistant/releases)
[![Build](https://github.com/tonglingcn/ipp-usb-assistant/actions/workflows/build-deb.yml/badge.svg)](https://github.com/tonglingcn/ipp-usb-assistant/actions/workflows/build-deb.yml)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue)](./LICENSE)
[![Platform](https://img.shields.io/badge/platform-deepin%2025%20%7C%20UOS%2025%20%7C%20UOS%2020-brightgreen)](https://www.deepin.org/)
[![Qt](https://img.shields.io/badge/Qt-5.11%20%7C%206.x-blue)](https://www.qt.io/)

## 平台支持

一套代码同时支持 **Qt 6 / DTK 6** 与 **Qt 5 / DTK 5**，由 `CMakeLists.txt` 自动探测无需手动指定：

| 平台 | Qt / DTK | 构建方式 | 打包元数据 |
|---|---|---|---|
| **deepin 25 / UOS 25** | Qt 6 + DTK 6 | `./build.sh` | `debian/` |
| **UOS 20** | Qt 5.11 + DTK 5.6 | `./build-uos.sh` | `debian-uos/` |

版本差异全部集中在 `src/qtcompat.h` 里用编译期宏抹平，业务代码只写统一接口，
不再出现 `#if QT_VERSION` 之类的平台判断散落各处。

## 下载安装（推荐）

[Releases](https://github.com/tonglingcn/ipp-usb-assistant/releases) 提供
**deepin 25 / UOS 25**（Qt6）的预编译 deb，含 **amd64 / arm64 / loong64** 三架构：

```bash
# 以 amd64 为例，其他架构替换包名中的架构字段
sudo apt install ./ipp-usb-assistant_1.0.1_amd64.deb
```

| 文件 | 说明 |
|---|---|
| `ipp-usb-assistant_1.0.1_<arch>.deb` | 主包（约 216–239 KB） |
| `ipp-usb-assistant-dbgsym_1.0.1_<arch>.deb` | 调试符号包，普通用户无需下载 |

安装后可从启动器搜索「IPP-USB」打开。

> **UOS 20 用户**：Release 里的 deb 是 Qt6 版本，不适用于 UOS 20。
> 请用下方[从源码构建](#从源码构建)的 `./build-uos.sh` 构建 Qt5 版本。

## 功能概览

### 1. 环境检测（IPP-USB 能力）

进入应用首页即自动检测当前系统是否具备免驱能力：

- `ipp-usb` 服务是否 active、二进制是否安装
- `avahi-daemon`（mDNS，扫描/发现硬性前提）是否运行
- `cups` 打印服务是否运行
- 已连接的 USB 外设中是否存在打印 / 成像类设备（支持 IPP-USB 的候选）

侧边栏实时显示环境结论（就绪 / 部分就绪 / 不可用），未就绪时给出修复指引。

### 2. 打印管理

- **发现设备**：`driverless` 列出支持 IPP-USB 的打印机（ipp:// URI），并合并 CUPS 已配置队列
- **添加打印机**：对发现的 driverless URI，通过 `pkexec lpadmin -p <队列> -v <URI> -m driverless -E` 建立 CUPS 队列（免厂商 PPD）
- **驱动微调（重点）**：针对免驱生成的 PPD 做关键修正：
  - 默认页面大小（`*DefaultPageSize`）
  - **双面长边翻页实机修正**（`*cupsBackSide: Normal` ↔ `Rotated`）
    > 在已测试的一批设备上，`DuplexNoTumble` / `DuplexTumble` 都只能得到短边双面；将 `*cupsBackSide` 改为 `Rotated` 后可通过旋转背面图像得到长边翻页效果。该选项属于针对这些机型的兼容策略，修改后会重启队列使 CUPS 重新加载 PPD。
- **打印测试页**：`lp -d <队列> -o media=a4 /usr/share/cups/data/testprint`

### 3. 扫描管理

基于 SANE + `sane-airscan` (eSCL)，发现 USB eSCL 扫描仪并扫描。

- 支持在设备列表中选择目标扫描仪
- 可调扫描参数：分辨率（75/150/300/600 DPI）与色彩模式（彩色/灰度/黑白）
- 扫描结果内嵌预览，可一键打开原图（保存至图片目录）

USB eSCL 扫描依赖 `ipp-usb` 与 `avahi-daemon` 的 mDNS 回环发现（通常需 15–20 秒）。

### 4. 高级设置

提供两个**互相独立**的过滤维度，用于解决「厂商自带原生驱动」与「免驱」的共存问题。
界面上以两个子标签呈现：

| 子标签 | 作用 | 生效方式 |
|---|---|---|
| **ipp-usb 整机放行** | 整台设备不被 ipp-usb 接管 | 需重新插拔设备或重启服务 |
| **扫描通道排除** | 仅不走 eSCL 扫描，保留免驱打印 | 立即生效，无需重新插拔 |

#### 4.1 ipp-usb 整机放行（接管过滤）

写入 `/etc/ipp-usb/quirks/ipp-usb-driverless-assistant.conf`，按设备名称
（`iManufacturer + iProduct`）把整台设备加入 ipp-usb 黑名单。

- 生效后 ipp-usb 完全不接管该设备，**打印与扫描一并交还原厂驱动**
- 需重新插拔设备或重启 ipp-usb 服务
- 段名必须等于 ipp-usb 的 `MfgAndProduct`，程序已复刻其 `FixUp()` 算法自动计算

#### 4.2 扫描通道排除（sane-airscan blacklist）

写入 `/etc/sane.d/airscan.d/ipp-usb-assistant.conf` 的 `[blacklist]` 段，
只摘掉该设备的 airscan（eSCL）扫描，**IPP 免驱打印完全不受影响**。

- 适用于厂商提供原生 SANE 扫描驱动、但打印仍希望走免驱的一体机
- 规则支持 glob 通配符（如 `Pantum*`），由 sane-airscan 的 `fnmatch` 执行
- **立即生效**，无需重新插拔设备或重启服务
- 放在 `airscan.d/` 下是因为 `conf_load_from_dir()` 会遍历该目录，
  既不干扰用户自己的 `airscan.conf`，又能被可靠加载

**为什么需要 4.2**：ipp-usb 上游的 quirks 只有一个 `blacklist` 键，命中后整台设备
被放弃（`ErrBlackListed`），无法做到「只禁扫描、保留打印」。而 sane-airscan 提供按
设备的 `name` / `model` 过滤，且只作用于自动发现、与 CUPS 打印链路无关，
因此可实现这一粒度。

> **注意**：排除前程序会检查 `/etc/sane.d/dll.conf` 是否还有 airscan 之外的后端。
> 若没有，排除后该设备的扫描功能将完全不可用，此时会弹出确认提示。

## 从源码构建

### deepin 25 / UOS 25（Qt 6 + DTK 6）

```bash
sudo apt install build-essential cmake debhelper \
     qt6-base-dev qt6-tools-dev \
     libdtk6core-dev libdtk6gui-dev libdtk6widget-dev \
     libcups2-dev libsane-dev pkg-config

./build.sh                # 仅编译
./build.sh --deb          # 编译并打包 deb
./build.sh --clean        # 清理
```

### UOS 20（Qt 5.11 + DTK 5.6）

```bash
sudo apt install build-essential cmake debhelper \
     qtbase5-dev qttools5-dev \
     libdtkcore-dev libdtkgui-dev libdtkwidget-dev \
     libcups2-dev libsane-dev pkg-config

./build-uos.sh            # 仅编译
./build-uos.sh --deb      # 编译并打包 deb（使用 debian-uos/ 元数据）
./build-uos.sh --clean    # 清理
```

两个脚本都会**先单独跑完 AUTOMOC 再整体并行编译**，规避 clean 后首次构建时
`mocs_compilation.cpp.o` 抢跑导致的「moc_*.cpp 不存在」偶发失败。

编译产物均在 `build/ipp-usb-assistant`，两个脚本共用 `build/` 目录，
切换平台前请先 `--clean`。

## 权限模型

由 `src/privileges.{h,cpp}` 统一管理，按**操作需要什么权限**决策，而非按调用模块：

| 类别 | 覆盖操作 | 策略 |
|---|---|---|
| `Never` | `lpinfo`、`driverless`、`lpoptions`、`scanimage` 等只读探测 | 永不提权 |
| `Auto` | `lpadmin`、`cupsaccept`、`cupsenable`（CUPS 队列管理） | 在 `lpadmin` 组则直跑，否则自动 `pkexec` |
| `Always` | 写 `/etc/cups/ppd`、`systemctl`、`apt-get`、写 quirks 与 airscan 配置 | 必须 root |

要点：

- **写 PPD 即使身在 `lpadmin` 组也需 root**：`/etc/cups/ppd` 为 `root:lp 755`，组 `lp` 只有 `r-x`。
- **`ipp-usb check` 本身不需要 root**（上游 `main.go` 中 check/status 模式均免 root 检查），
  但非 root 枚举 libusb 设备需对 `/dev/bus/usb/*` 有读写权，而 udev 规则已将节点设为
  `root:lp 0664`，因此仅 `lp` 组成员可免提权。
- `Auto` 带一次**失败回退**：明明在 `lpadmin` 组却仍报权限不足时
  （例如管理员改过 CUPS 的 `SystemGroup`），自动改用 `pkexec` 重试一次。
- 启动时若检测到当前用户不在 `lpadmin` 组，环境检测页顶部会显示提示条并给出加组命令
  （`sudo usermod -aG lpadmin $USER`，需注销重登录生效）。

## Qt 版本兼容层

`src/qtcompat.h` 集中处理 Qt 5 / Qt 6 之间的 API 迁移，业务代码统一使用封装后的名字：

| 差异点 | Qt 5 | Qt 6 | 本项目统一为 |
|---|---|---|---|
| `QString::split` 空段过滤 | `QString::SkipEmptyParts`（< 5.14） | `Qt::SkipEmptyParts` | `kSkipEmptyParts` |
| `QButtonGroup` 按 id 点击 | `buttonClicked(int)`（< 5.15） | `idClicked(int)` | `IPP_USB_BUTTON_GROUP_ID_SIGNAL` |
| `QTextStream` UTF-8 | `setCodec("UTF-8")` | `setEncoding(QStringConverter::Utf8)` | `setUtf8Encoding()` |

`CMakeLists.txt` 的对应处理：

- 先 `find_package(Qt6 QUIET)`，未命中再 `REQUIRED` 要求 Qt5，DTK 大版本跟随
- **CUPS 探测带回退**：UOS 20 的 `libcups2-dev` 不提供 `cups.pc`，
  pkg-config 失败时回退到 `find_path(cups/cups.h)` + `find_library(cups)`
- **SANE 探测带回退**：优先 `sane-backends`，未命中再试 `sane`
- 翻译：Qt6 用 `qt_add_translations`，Qt5 用 `qt5_add_translation`
  并显式挂到 `ALL` 目标（否则 `.qm` 不会被生成）

## 国际化

- 全部界面文案已接入 `tr()`，翻译文件见 `translations/`
  （`make ipp-usb-assistant_lupdate` 更新、`lrelease` 生成 `.qm`）
- **应用图标**：SVG 源文件在 `resources/ipp-usb-assistant.svg`，
  多尺寸 PNG 在 `resources/icons/hicolor/`，安装后由桌面项 `Icon=ipp-usb-assistant` 引用

## 自动构建（CI）

仓库自带 `.github/workflows/build-deb.yml`，为 **deepin 25 / UOS 25**（Qt6）构建并发布，
**不依赖外部调度器或额外 PAT**，使用仓库自带的 `GITHUB_TOKEN` 发布 Release。

### 构建矩阵

| 架构 | Runner | 构建镜像 |
|---|---|---|
| amd64 | `ubuntu-24.04` | `linuxdeepin/deepin:crimson` |
| arm64 | `ubuntu-24.04-arm` | `linuxdeepin/deepin:crimson-arm64` |
| loong64 | `ubuntu-24.04` + QEMU | `linuxdeepin/deepin:crimson-loong64` |

在 deepin 官方容器内 `apt install` 依赖后 `cmake` → `make` → `dpkg-buildpackage`，
保证与目标系统库版本一致。

### 发版流程

推送形如 `v*` 的 tag 即自动构建并发布：

```bash
git tag v1.0.1
git push origin v1.0.1
```

流程：三架构并行构建 → 上传 artifact → 聚合生成 Release 并上传全部 `.deb`。
产物直接出现在 [Releases](https://github.com/tonglingcn/ipp-usb-assistant/releases)。

也可在 Actions → **build-deb** → **Run workflow** 手动触发（不带 tag 时只构建，不发 Release）。

### 构建耗时参考

| 架构 | 耗时 | 说明 |
|---|---|---|
| amd64 | 约 4 分钟 | 原生容器 |
| arm64 | 约 3 分钟 | ARM 原生 runner |
| loong64 | 约 28 分钟 | QEMU 模拟编译 Qt6，较慢但可用 |

> **UOS 20 未纳入 CI**：其 Qt5/DTK5 依赖来自 UOS 私有源，暂无公开容器镜像，
> 请用 `./build-uos.sh` 在本地构建。

## 目录结构

```
src/
  main.cpp                 应用入口 + 专业化样式
  mainwindow.{h,cpp}       四页式布局（环境/打印/扫描/高级设置）
  qtcompat.h               Qt5 / Qt6 兼容层
  privileges.{h,cpp}       统一权限层：组检测 + 按操作类型决定提权策略
  envchecker.{h,cpp}       IPP-USB 环境能力检测
  printmanager.{h,cpp}     driverless 发现 + lpadmin 添加队列 + 测试页
  ppdconfig.{h,cpp}        PPD 读取与改写（页面大小 + cupsBackSide 长边翻页）
  printerconfigdialog.{h,cpp}  驱动微调对话框
  printpropertiesdialog.{h,cpp} 打印属性对话框
  addprinterdialog.{h,cpp}  添加打印机对话框
  scannermanager.{h,cpp}   SANE/eSCL 扫描
  advancedsettings.{h,cpp} 高级设置：ipp-usb 整机放行 + 扫描通道排除
  themehelper/focusstyle/twolineitemdelegate  界面细节
debian/                    deepin 25 / UOS 25 打包元数据（Qt6）
debian-uos/                UOS 20 打包元数据（Qt5）
build.sh / build-uos.sh    双平台构建脚本
docs/principles.md         ipp-usb / sane-airscan 源码剖析
```

## 原理

详见 `docs/principles.md`（ipp-usb / sane-airscan 源码剖析）。

## 参与开发

提交前建议本地跑通：

```bash
bash build.sh                  # 编译（Qt6 平台）
dpkg-buildpackage -us -uc -b   # 验证打包
```

如要新增功能，请沿用现有约定：

- 需要提权的操作统一走 `src/privileges.{h,cpp}`，不要自行拼接 `pkexec`
- 界面文案一律使用 `tr()`，并同步更新 `translations/`
- 遇到 Qt 5 / Qt 6 的 API 差异，**加到 `src/qtcompat.h`**，
  不要在业务代码里散落 `#if QT_VERSION` 判断
- 改动涉及 IPP-USB / sane-airscan 行为时，同步更新 `docs/principles.md`

## 许可

本项目基于 **GPL-3.0** 发布，详见 [LICENSE](./LICENSE)。
