# 外设适配原理（基于 ipp-usb / sane-airscan 源码分析）

> 本文档基于对 `ipp-usb` 与 `sane-airscan` 上游源码的拆解，用于指导「IPP-USB 免驱助手」
> 在 Deepin 25 下的开发与排障。
>
> 上游源码仓库：
> - ipp-usb：https://github.com/OpenPrinting/ipp-usb
> - sane-airscan：https://github.com/alexpevzner/sane-airscan

## 一、整体数据流

```
USB 打印机/MFP
   │  (USB 接口，IPP-over-USB 协议)
   ▼
ipp-usb 守护进程  (systemd + udev 触发，监听 localhost:60000)
   │  1) IPP 打印端点   → /ipp/print
   │  2) eSCL 扫描端点  → /eSCL/ScannerCapabilities (仅 MFP 有)
   │  3) DNS-SD 广播：
   │       _ipp._tcp / _ipps._tcp        (打印)
   │       _uscan._tcp (eSCL)            (仅扫描/MFP)
   ▼
┌──────────────┬─────────────────────┐
▼              ▼                     ▼
CUPS          avahi-daemon        sane-airscan
(driverless)  (mDNS 本地广播)       (SANE 后端)
   │              │                     │
   ▼              ▼                     ▼
IPP-USB 免驱助手      IPP-USB 免驱助手设备发现      IPP-USB 免驱助手扫描
打印模块      诊断模块              管理模块
```

## 二、ipp-usb 关键原理

### 2.1 触发机制（udev）
- 规则文件 `systemd-udev/71-ipp-usb.rules`：
  `ENV{ID_USB_INTERFACES}=="*:070104:*"` 命中即启动
  `ipp-usb.service`（070104 = USB 打印类/子类01/协议04 = IPP over USB）。
- 纯打印类（07）但非 IPP 协议（如 0701**02**）的不触发。
- **IPP-USB 免驱助手诊断要点**：设备插上后若无 `ipp-usb.service` 实例，先查 udev 是否命中、
  `systemctl status ipp-usb`、以及内核是否枚举出对应 USB 接口。

### 2.2 运行模式（`main.go`）
- `udev` 模式：被 udev 拉起，当最后一个设备断开自动退出（无设备不常驻）。
- `check` 模式：枚举设备，需要 root（直接访问 USB）。
- `status` 模式：查看当前运行实例状态。
- **注意**：`ipp-usb` 实例数 = 当前连接的支持 IPP-USB 设备数。

### 2.3 扫描端点注册（`escl.go`）
- `EsclService()` 查询 `http://localhost:<port>/eSCL/ScannerCapabilities`。
- 解析 `scan:ColorMode` 等能力；成功则注册 `_uscan._tcp` DNS-SD 服务
  （TXT 含 `rs=eSCL`）。
- **关键结论（能力探测）**：
  - 只有**真正暴露扫描能力**的设备才会注册 `_uscan._tcp`。
  - 纯打印机 / 部分 MFP 只暴露打印 → 扫描仪列表中不会出现。
  - IPP-USB 免驱助手"扫描发现"为空时，应区分：
    - ipp-usb 未运行 / 未注册 `_uscan._tcp`（底层问题）
    - 设备本身不支持 eSCL 扫描（硬件能力问题，非软件缺陷）

## 三、sane-airscan 关键原理

### 3.1 SANE 接口（`airscan.c`）
- `sane_init` → `airscan_init` + `device_management_init`。
- `sane_get_devices(device_list, local_only)`：
  - **`local_only = SANE_TRUE` 时返回空列表**（airscan 设备本质为"非本地"
    网络发现设备，除非配置 `pretend_local`）。
  - **IPP-USB 免驱助手调用必须传 `SANE_FALSE`**，否则 USB eSCL 设备列不出来。
  - 设备列表来自 `zeroconf_device_list_get()`。

### 3.2 发现（`airscan-zeroconf.c`）
- 通过 mDNS（avahi）监听：
  - `_uscan._tcp` / `_uscans._tcp` → eSCL 协议
  - `ZEROCONF_WSD` → WSD 协议
- 设备标识形如 `airscan:eSCL:<model>@<host>`。
- **USB eSCL 实际是"本地回环网络发现"**：ipp-usb 在 `127.0.0.1` 广播
  `_uscan._tcp`，sane-airscan 的 avahi 在回环口收到 → 显示为 USB 扫描仪。
- **关键结论**：`avahi-daemon` 必须运行，否则 USB 扫描仪**无法被发现**。
  IPP-USB 免驱助手诊断项必须包含 `avahi-daemon` 状态。

### 3.3 eSCL 协议（`airscan-escl.c`）
- 走 HTTP + XML（PWG/HP eSCL 命名空间）。
- 支持 Platen（平板）、ADF（进纸器）、单/双面、彩色/灰度。
- 设备兼容性好（Brother/Canon/Epson/HP 等），但存在大量厂商
  **quirk**（如 `quirk_localhost`、retry on 404/410），说明现实设备
  对协议实现不严格 → IPP-USB 免驱助手扫描时不要假设设备完全合规。

## 四、对「IPP-USB 免驱助手」开发的落地指导

| 模块 | 依赖与服务 | 常见故障排查点 |
|------|-----------|---------------|
| 打印发现 | `driverless` + `CUPS` | ipp-usb 未运行 / udev 未命中 070104 |
| 打印测试页 | `lp` + CUPS 队列 | 用户未在 `lp` 组 / 队列未自动创建 |
| 扫描发现 | `sane-airscan` + `avahi-daemon` + `ipp-usb` | avahi 未运行 / 设备无 `_uscan._tcp` / 纯打印机 |
| 扫描成像 | `sane_start/read` 或 `scanimage` | 分辨率/格式不支持 / 设备忙 |
| 诊断 | `systemctl is-active` + `lsusb` | 逐项检查上述 4 个服务 |

### 必须检查的 4 个底层服务
1. `ipp-usb`        — USB→IPP/eSCL 桥接
2. `cups`           — 打印队列管理
3. `avahi-daemon`   — mDNS 发现（扫描发现的硬性前提）
4. `saned`          — SANE 网络扫描守护（本地 USB 一般不需，但网络扫描需）

### 权限
- 添加打印机队列、访问 USB 设备节点通常需 `lp` / `scanner` 组，
  或 polkit 提权。
- 建议IPP-USB 免驱助手在检测到权限不足时，明确提示用户加入用户组。

## 五、源码阅读索引
- ipp-usb: `main.go`(模式)、`pnp.go`(热插拔)、`escl.go`(扫描端点)、
  `usbio_libusb.go`(USB 传输)、`systemd-udev/71-ipp-usb.rules`(触发)
- sane-airscan: `airscan.c`(SANE API)、`airscan-zeroconf.c`(发现)、
  `airscan-escl.c`(eSCL 协议)、`airscan-device.c`(设备会话)
