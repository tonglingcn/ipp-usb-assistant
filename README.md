# IPP-USB 免驱助手 (IPP-USB Driverless Assistant)

面向 Deepin 25 的**免驱打印与扫描**原生管理工具，基于 DTK6 + Qt6 开发。
核心价值：把"IPP-USB 免驱"从命令行排查变成可视化的专业操作界面。

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

### 4. 设备诊断
一键采集排查外设问题所需的全部现场信息，并可导出为纯文本报告
（`~/Documents/ipp-usb-diag_<时间>.txt`），便于把问题现象连同环境信息
一起发给支持人员，省去来回追问。

报告包含 6 个部分：

1. 底层服务状态（ipp-usb / cups / avahi-daemon / saned）
2. IPP-over-USB 候选设备（按 ipp-usb 上游规则严格过滤）
3. ipp-usb 已接管设备（`ipp-usb check`）
4. DNS-SD 广播（`_ipp._tcp` 打印 / `_uscan._tcp` 扫描）
5. CUPS 打印队列
6. SANE 扫描设备

全部为只读操作，不需管理员权限。

### 5. 高级设置

提供两个**互相独立**的过滤维度，用于解决"厂商自带原生驱动"与"免驱"的共存问题。
界面上以两个子标签呈现：

| 子标签 | 作用 | 生效方式 |
|---|---|---|
| **ipp-usb 整机放行** | 整台设备不被 ipp-usb 接管 | 需重新插拔设备或重启服务 |
| **扫描通道排除** | 仅不走 eSCL 扫描，保留免驱打印 | 立即生效，无需重新插拔 |

#### 5.1 ipp-usb 整机放行（接管过滤）

写入 `/etc/ipp-usb/quirks/ipp-usb-driverless-assistant.conf`，按设备名称
（`iManufacturer + iProduct`）把整台设备加入 ipp-usb 黑名单。

- 生效后 ipp-usb 完全不接管该设备，**打印与扫描一并交还原厂驱动**
- 需重新插拔设备或重启 ipp-usb 服务
- 段名必须等于 ipp-usb 的 `MfgAndProduct`，程序已复刻其 `FixUp()` 算法自动计算

#### 5.2 扫描通道排除（sane-airscan blacklist）

写入 `/etc/sane.d/airscan.d/ipp-usb-assistant.conf` 的 `[blacklist]` 段，
只摘掉该设备的 airscan（eSCL）扫描，**IPP 免驱打印完全不受影响**。

- 适用于厂商提供原生 SANE 扫描驱动、但打印仍希望走免驱的一体机
- 规则支持 glob 通配符（如 `Pantum*`），由 sane-airscan 的 `fnmatch` 执行
- **立即生效**，无需重新插拔设备或重启服务
- 放在 `airscan.d/` 下是因为 `conf_load_from_dir()` 会遍历该目录，
  既不干扰用户自己的 `airscan.conf`，又能被可靠加载

**为什么需要 5.2**：ipp-usb 上游的 quirks 只有一个 `blacklist` 键，命中后整台设备
被放弃（`ErrBlackListed`），无法做到"只禁扫描、保留打印"。而 sane-airscan 提供按
设备的 `name` / `model` 过滤，且只作用于自动发现、与 CUPS 打印链路无关，
因此可实现这一粒度。

> **注意**：排除前程序会检查 `/etc/sane.d/dll.conf` 是否还有 airscan 之外的后端。
> 若没有，排除后该设备的扫描功能将完全不可用，此时会弹出确认提示。

## 编译与运行

```bash
# 依赖（Deepin 25）
sudo apt install build-essential cmake \
     qt6-base-dev qt6-tools-dev dtk6core-dev dtk6gui-dev dtk6widget-dev \
     libcups2-dev libsane-dev pkg-config

# 编译
cd examples/ipp-usb-assistant
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake
make -j4

# 运行（部分操作会通过 pkexec 申请管理员权限，用于添加打印机与改写 PPD）
./ipp-usb-assistant
```

一键脚本：`bash build.sh`

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

## 国际化与打包

- **国际化**：全部界面文案已接入 `tr()`，英文翻译见 `translations/ipp-usb-assistant_en.ts`
  （`make ipp-usb-assistant_lupdate` 更新、`lrelease` 生成 .qm）
- **应用图标**：SVG 源文件在 `resources/ipp-usb-assistant.svg`，
  多尺寸 PNG 在 `resources/icons/hicolor/`，安装后由桌面项 `Icon=ipp-usb-assistant` 引用
- **deb 打包**：

```bash
cd examples/ipp-usb-assistant
dpkg-buildpackage -us -uc -b   # 生成 ../ipp-usb-assistant_1.0.0_amd64.deb
```

## 自动构建（CI）

本仓库通过 `build.yml` 自主触发通用的 deepin 打包调度器
[buildpackage-deepin](https://github.com/tonglingcn/buildpackage-deepin)，
自动编译 **amd64 / arm64 / riscv64 / loong64** 四个架构的 deb，并做 `lintian` 检查。

### 触发方式

| 方式 | 操作 | 结果 |
|---|---|---|
| **打 tag** | 推送形如 `v1.0.1` 的 tag | 自动用 `crimson`（Deepin 25）构建四架构 deb |
| **手动** | Actions → build-deepin → Run workflow，可选 `codename` | 立即按所选代号（apricot/beige/crimson）构建 |

```bash
# 打 tag 触发示例
git tag v1.0.1
git push origin v1.0.1
```

### 前提配置

本仓库 `Settings → Secrets and variables → Actions → New repository secret` 需添加：

- **Name**：`DISPATCH_TOKEN`
- **Value**：一个带 `repo` + `workflow` 权限的 Personal Access Token
  （默认 `GITHUB_TOKEN` 不能跨仓库触发 workflow，因此必须单独配置）

### 获取产物

构建完成后，`buildpackage-deepin` 会自动在**本仓库的 [Releases](../../releases)** 页面创建对应 tag 的 Release，并将所有四架构 `.deb` 作为 **assets** 上传，无需再进 Actions 下载 artifact。

Release 标题即为 tag（如 `v1.0.0`），正文注明构建使用的 deepin 代号。每个架构通常包含：
- `ipp-usb-assistant_<version>_<arch>.deb`
- `ipp-usb-assistant-dbgsym_<version>_<arch>.deb`（如生成）

### 配置说明

1. **本仓库**（`ipp-usb-assistant`）`Settings → Secrets and variables → Actions → New repository secret`：
   - **Name**：`DISPATCH_TOKEN`
   - **Value**：带 `repo` + `workflow` 权限的 Personal Access Token
   （默认 `GITHUB_TOKEN` 不能跨仓库触发 workflow）

2. **打包调度器仓库**（`buildpackage-deepin`）同样需要配置同名 `DISPATCH_TOKEN`，
   因为上传 Release assets 到本仓库需要写入权限。同一个 PAT 存到两个仓库即可。

## 目录结构
```
src/
  main.cpp            应用入口 + 专业化样式
  mainwindow.{h,cpp}  四页式专业布局（环境/打印/扫描/高级设置）
  privileges.{h,cpp}  统一权限层：组检测 + 按操作类型决定提权策略
  envchecker.{h,cpp}  IPP-USB 环境能力检测
  printmanager.{h,cpp} driverless 发现 + lpadmin 添加队列 + 测试页
  ppdconfig.{h,cpp}    PPD 读取与改写（页面大小 + cupsBackSide 长边翻页）
  printerconfigdialog.{h,cpp} 驱动微调对话框（页面大小 + 长边翻页开关）
  scannermanager.{h,cpp} SANE/eSCL 扫描
  advancedsettings.{h,cpp} 高级设置（两个子标签）：
                           ipp-usb 整机放行 + 扫描通道排除
  diagnostics.{h,cpp}  设备诊断与报告导出
```

## 原理
详见 `docs/principles.md`（ipp-usb / sane-airscan 源码剖析）。
