#include "envchecker.h"
#include "privileges.h"

#include <QProcess>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <QHash>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

EnvChecker::EnvChecker(QObject *parent)
    : QObject(parent)
{
}

QString EnvChecker::unitStatus(const QString &unit)
{
    QProcess proc;
    proc.start("systemctl", {"is-active", unit});
    proc.waitForFinished(2000);
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    return out.isEmpty() ? "inactive" : out;
}

bool EnvChecker::unitInstalled(const QString &unit)
{
    QProcess proc;
    proc.start("systemctl", {"list-unit-files", unit + ".service"});
    if (!proc.waitForFinished(3000))
        return false;
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    return out.contains(unit + ".service");
}

bool EnvChecker::controlService(const QString &unit, const QString &action, QString &errMsg)
{
    // 管理系统服务必须 root，与 CUPS 队列管理不是一回事，用 Always。
    // pkexec 会弹出授权窗口等待用户输入密码，超时给足。
    const int code = Privileges::run(QStringLiteral("systemctl"),
                                     {action, unit},
                                     Privileges::Elevation::Always,
                                     nullptr, &errMsg, 120000);
    // execSync 以 code == -1 表示启动失败或超时，靠 err 内容区分二者
    if (code == -1) {
        errMsg = errMsg.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
                     ? tr("操作超时（120 秒）或授权窗口被关闭")
                     : tr("无法启动 pkexec（系统缺少授权组件）");
        return false;
    }
    if (code != 0) {
        errMsg = errMsg.trimmed();
        if (errMsg.isEmpty())
            errMsg = QString(tr("systemctl %1 %2 失败（exit %3）"))
                         .arg(action, unit).arg(code);
        return false;
    }
    return true;
}

bool EnvChecker::commandExists(const QString &cmd)
{
    QProcess proc;
    proc.start("which", {cmd});
    proc.waitForFinished(2000);
    return !QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed().isEmpty();
}

// 重要：不要改回 "ipp-usb status"！
//
// 上游 status.go:74-80 有 bug：
//     i := 0
//     for _, status := range statusTable { devs[i] = status }   // i 从未自增
// 多设备时 devs[1..] 保持 nil，紧接着的 sort.Slice 比较函数解引用
// devs[i].desc → panic → ipp-usb 进程崩溃 → systemd 重启。
//
// 实测日志（/var/log/ipp-usb/main.log）中每次崩溃前都是：
//     ctrlsock: GET /status
//     panic: runtime error: invalid memory address or nil pointer dereference
//     ...main.StatusFormat()  .../status.go:79
// 即只要有 ≥2 台 IPP-USB 设备，任何 "ipp-usb status" 调用都会打死守护进程，
// 表现为服务“一会儿运行、一会儿启动失败”。
//
// "ipp-usb check" 是独立进程、不走 ctrlsock，不会触发 StatusFormat()，
// 是安全的替代。两者输出格式基本一致，正则可复用。
static QStringList ippUsbDevices()
{
    // check 需要枚举 USB 设备，非 root 需要对 /dev/bus/usb/* 的读写权限；
    // udev 规则已把节点设为 root:lp 0664，故 lp 组成员可免提权。
    // 不满足时直接返回空，由 check() 回退到 lsusb，避免弹授权框打断用户。
    if (!Privileges::inGroup(QStringLiteral("lp")))
        return {};

    QStringList list;
    QProcess proc;
    proc.start(QStringLiteral("ipp-usb"), {QStringLiteral("check")});
    if (!proc.waitForFinished(15000))
        return list;

    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    // 匹配形如：
    //   1.   Bus 001 Device 004  232b:0f8e  "BM4240ADW series"   （ipp-usb check）
    //   1. Bus 001 Device 006  232b:0f8e  "BM4240ADW series"     （ipp-usb status）
    static const QRegularExpression re(
        R"(Bus\s+(\d+)\s+Device\s+(\d+)\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s+\"([^\"]+)\")");
    auto it = re.globalMatch(out);
    while (it.hasNext()) {
        const auto m = it.next();
        list.append(QString("Bus %1 Device %2  %3:%4  %5")
                        .arg(m.captured(1), m.captured(2),
                             m.captured(3), m.captured(4), m.captured(5)));
    }
    return list;
}

// 统计一个 lsusb -v 设备块中，满足 IPP-over-USB 条件的接口数。
//
// 规则对齐 ipp-usb 上游（vendor/ipp-usb-master/usbio_libusb.go）：
//   usbio_libusb.go:267  IsIppOverUsb() 要求 Class=7 / SubClass=1 / Proto=4
//   usbio_libusb.go:208  整台设备至少 2 个这样的接口
// 且每个接口必须同时具备 IN 与 OUT 端点（openUsbConn 前后各一）。
// 上游用 libusb 直接从描述符读取，这里只能解析 lsusb 文本，逻辑等价。
//
// 用途：过滤掉 USB Hub（Class 9）、键鼠（Class 3 HID）等无关设备，
// 避免它们出现在「ipp-usb 接管过滤」的放行列表里——对这类设备放行毫无意义。
static int countIppOverUsbInterfaces(const QString &block)
{
    // 每个 Interface Descriptor（含各 alt setting）单独统计
    static const QRegularExpression ifSplit(
        R"((?=^\s+Interface Descriptor:))",
        QRegularExpression::MultilineOption);
    static const QRegularExpression clsRe(R"(bInterfaceClass\s+7\b)");
    static const QRegularExpression subRe(R"(bInterfaceSubClass\s+1\b)");
    static const QRegularExpression protoRe(R"(bInterfaceProtocol\s+4\b)");
    // 形如：bEndpointAddress     0x81  EP 1 IN
    static const QRegularExpression epRe(
        R"(bEndpointAddress\s+0x[0-9a-fA-F]+\s+EP\s+\d+\s+(IN|OUT))");

    int count = 0;
    const QStringList ifBlocks = block.split(ifSplit, Qt::SkipEmptyParts);
    for (const QString &ifBlock : ifBlocks) {
        if (!clsRe.match(ifBlock).hasMatch())
            continue;
        if (!subRe.match(ifBlock).hasMatch())
            continue;
        if (!protoRe.match(ifBlock).hasMatch())
            continue;

        // 需要同时具备 IN 与 OUT 端点
        bool hasIn = false;
        bool hasOut = false;
        auto it = epRe.globalMatch(ifBlock);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.captured(1) == QLatin1String("IN"))
                hasIn = true;
            else
                hasOut = true;
        }
        if (hasIn && hasOut)
            ++count;
    }
    return count;
}

QStringList EnvChecker::lsusbImagingDevices()
{
    QStringList list;
    QProcess proc;
    proc.start("lsusb", {"-v"});
    if (!proc.waitForFinished(4000))
        return list;

    // 宽松判定用的类匹配。不用固定空格数的字符串包含判断，
    // 不同 lsusb / usbutils 版本的列宽可能不同。
    static const QRegularExpression printerRe(R"(bInterfaceClass\s+7\b)");
    static const QRegularExpression imagingRe(R"(bInterfaceClass\s+6\b)");

    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    // 按设备块分割：每个设备以 "Bus xxx Device xxx" 开始
    static const QRegularExpression devSplit(R"((?=Bus\s+\d+\s+Device\s+\d+))");
    const QStringList blocks = out.split(devSplit, Qt::SkipEmptyParts);

    for (const QString &block : blocks) {
        const bool hasPrinter = printerRe.match(block).hasMatch();
        const bool hasImaging = imagingRe.match(block).hasMatch();

        // lsusb 是否输出了接口描述符。普通用户读不到部分设备信息时，
        // 输出里不会有 bInterfaceClass，此时无法严格判定，需回退。
        const bool hasIfInfo = block.contains(QStringLiteral("bInterfaceClass"));

        if (hasIfInfo) {
            // 严格路径：必须是真正的 IPP-over-USB 设备（≥2 个 7/1/4 接口）。
            // Hub、键鼠、以及只有 1 个接口的打印机都会被过滤——
            // 它们本就不会被 ipp-usb 接管，放行毫无意义。
            if (countIppOverUsbInterfaces(block) < 2)
                continue;
        } else if (!hasPrinter && !hasImaging) {
            // 回退路径：信息缺失时保留打印机/影像类，避免误杀
            continue;
        }

        static const QRegularExpression busRe(R"(Bus\s+(\d+)\s+Device\s+(\d+))");
        static const QRegularExpression idRe(
            R"(idVendor\s+0x([0-9a-fA-F]+).+?idProduct\s+0x([0-9a-fA-F]+))",
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression prodRe(R"(iProduct\s+\d+\s+(.+))");
        static const QRegularExpression vendRe(R"(iManufacturer\s+\d+\s+(.+))");

        const auto busM = busRe.match(block);
        const auto idM = idRe.match(block);
        const auto prodM = prodRe.match(block);
        const auto vendM = vendRe.match(block);

        QString desc;
        if (vendM.hasMatch()) desc += vendM.captured(1).trimmed() + " ";
        if (prodM.hasMatch()) desc += prodM.captured(1).trimmed();
        desc = desc.trimmed();
        if (desc.isEmpty()) desc = hasPrinter ? "USB Printer" : "USB Imaging";

        QString id;
        if (idM.hasMatch())
            id = QString("%1:%2").arg(idM.captured(1), idM.captured(2));

        QString line;
        if (busM.hasMatch())
            line = QString("Bus %1 Device %2  %3  %4")
                       .arg(busM.captured(1), busM.captured(2), id, desc);
        else
            line = QString("%1  %2").arg(id, desc);
        list.append(line);
    }
    return list;
}

void EnvChecker::check()
{
    m_report.clear();
    m_supportedDevices.clear();
    m_services.clear();

    // 1) 三个关键服务（供交互卡片）
    struct Meta { const char *unit; const char *name; QString desc; };
    static const Meta metas[] = {
        { "ipp-usb",      "IPP-USB",       tr("USB 免驱打印/扫描协议桥（IPP over USB）") },
        { "avahi-daemon", "Avahi（mDNS）", tr("网络与 USB 设备的自动发现服务") },
        { "cups",         "CUPS",          tr("打印队列与任务管理系统") },
    };
    for (const auto &meta : metas) {
        ServiceInfo info;
        info.unit = meta.unit;
        info.name = meta.name;
        info.desc = meta.desc;
        info.installed = unitInstalled(info.unit);
        info.status = info.installed ? unitStatus(info.unit) : QStringLiteral("not-found");
        m_services.append(info);
    }

    // 按 unit 名查找，不依赖 m_services 的数组顺序。
    // 此前用 m_services[0]/[1]/[2] 硬编码下标，一旦调整 metas 顺序就会静默取错。
    auto serviceOf = [this](const QString &unit) -> const ServiceInfo * {
        for (const ServiceInfo &s : m_services) {
            if (s.unit == unit)
                return &s;
        }
        return nullptr;
    };
    auto isActive = [&serviceOf](const QString &unit) -> bool {
        const ServiceInfo *s = serviceOf(unit);
        return s && s->status == QStringLiteral("active");
    };

    // 注意：m_services 中 unit 字段为不带 .service 的短名（"ipp-usb"、
    // "avahi-daemon"、"cups"），isActive() 按 unit 短名查找，故这里不能
    // 带 .service 后缀，否则永远找不到对应服务，overall 会错误判为 Warn。
    const bool ippUsbOk = isActive(QStringLiteral("ipp-usb"));
    const bool avahiOk = isActive(QStringLiteral("avahi-daemon"));
    const bool cupsOk = isActive(QStringLiteral("cups"));
    const bool ippUsbBin = commandExists("ipp-usb");

    // 2) 文本报告（诊断/导出用）
    // 文案按 unit 名关联，不用数组下标。此前 okTails[i] 依赖 m_services 顺序，
    // 且当服务数量 > 3 时会越界。
    m_report.append(tr("== 免驱能力依赖服务 =="));
    auto tailFor = [](const QString &unit, bool active, bool installed) -> QString {
        if (active) {
            if (unit == QLatin1String("ipp-usb.service"))
                return tr("  ✓ 支持 USB 免驱打印");
            if (unit == QLatin1String("avahi-daemon.service"))
                return tr("  ✓ mDNS 发现就绪");
            return tr("  ✓ 打印队列管理就绪");
        }
        if (!installed)
            return tr("  ✗ 未安装（需 apt install）");
        if (unit == QLatin1String("ipp-usb.service"))
            return tr("  ✗ 未运行");
        if (unit == QLatin1String("avahi-daemon.service"))
            return tr("  ✗ 扫描/发现不可用");
        return tr("  ✗ 打印不可用");
    };
    for (const ServiceInfo &s : m_services) {
        const bool active = s.status == QLatin1String("active");
        m_report.append(QString("%1  %2  %3").arg(s.unit, s.status,
                                                  tailFor(s.unit, active, s.installed)));
    }
    m_report.append(QString(tr("ipp-usb 命令   : %1"))
                        .arg(ippUsbBin ? tr("已安装") : tr("未安装（需 apt install ipp-usb）")));

    // 3) USB 外设：优先用 ipp-usb check（非 lp 组时返回空），
    //    回退到 lsusb -v 的 Printer/Imaging 类。不可用 ipp-usb status，见上方说明。
    m_report.append("");
    m_report.append(tr("== 已连接 USB 外设（IPP-USB 候选） =="));

    m_supportedDevices = ippUsbDevices();
    if (m_supportedDevices.isEmpty() && commandExists("lsusb"))
        m_supportedDevices = lsusbImagingDevices();

    if (m_supportedDevices.isEmpty()) {
        m_report.append(tr("未检测到打印机/扫描类 USB 设备（插上设备后点「重新检测」）"));
    } else {
        for (const QString &dev : m_supportedDevices)
            m_report.append(dev);
    }

    // 4) 综合结论
    m_report.append("");
    if (ippUsbOk && avahiOk && cupsOk && !m_supportedDevices.isEmpty()) {
        m_overall = Status::Ok;
        m_summary = tr("环境就绪：已支持 IPP-USB 免驱");
    } else if (ippUsbOk && avahiOk && cupsOk) {
        m_overall = Status::Ok;
        m_summary = tr("环境就绪：已支持 IPP-USB 免驱");
    } else if (ippUsbBin || !m_supportedDevices.isEmpty()) {
        m_overall = Status::Warn;
        m_summary = tr("检测到外设但底层服务未完全就绪，请先修复依赖服务");
    } else {
        m_overall = Status::Error;
        m_summary = tr("未检测到 IPP-USB 能力：请安装 ipp-usb / avahi-daemon 并连接设备");
    }

    m_model.setStringList(m_report);
}

EnvChecker::CheckResult EnvChecker::collectCheckResult()
{
    // 使用线程内临时对象完成同步探测，后台任务不访问界面所持有的
    // EnvChecker，从而避免窗口关闭时悬空 this 和跨线程成员读写。
    EnvChecker checker;
    checker.check();

    CheckResult result;
    result.report = checker.model()->stringList();
    result.supportedDevices = checker.supportedDevices();
    result.services = checker.services();
    result.summary = checker.summary();
    result.overall = checker.overall();
    return result;
}

void EnvChecker::applyCheckResult(const CheckResult &result)
{
    m_report = result.report;
    m_supportedDevices = result.supportedDevices;
    m_services = result.services;
    m_summary = result.summary;
    m_overall = result.overall;
    m_model.setStringList(m_report);
}

void EnvChecker::checkAsync()
{
    if (m_checkRunning)
        return;

    m_checkRunning = true;
    auto *watcher = new QFutureWatcher<CheckResult>(this);
    connect(watcher, &QFutureWatcher<CheckResult>::finished, this, [this, watcher]() {
        applyCheckResult(watcher->result());
        m_checkRunning = false;
        watcher->deleteLater();
        emit basicCheckFinished();
    });
    watcher->setFuture(QtConcurrent::run([]() {
        return collectCheckResult();
    }));
}

// ---------- 联网检测 ----------
bool EnvChecker::checkOnline(int timeoutMs)
{
    // 先看是否有任意可达的网络接口（环回不算），排除"机器物理没插网线"的极端情况
    bool anyLink = false;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsLoopBack)
            && (iface.flags() & QNetworkInterface::IsUp)
            && (iface.flags() & QNetworkInterface::IsRunning)) {
            anyLink = true;
            break;
        }
    }
    if (!anyLink)
        return false;

    // 优先尝试探测系统软件源：Deepin/UOS/Debian 任一仓库可达即视为在线
    static const QStringList probes = {
        "https://packages.deepin.com/deepin/dists/stable/Release",
        "https://professional-packages.chinauos.com/repository/professional/dists/uos/InRelease",
        "https://deb.debian.org/debian/dists/bookworm/Release",
    };
    for (const QString &url : probes) {
        QProcess proc;
        proc.start("curl", {"-fsIL", "--max-time", QString::number(timeoutMs / 1000),
                                 "--connect-timeout", QString::number(timeoutMs / 1000),
                                 url});
        if (proc.waitForFinished(timeoutMs + 3000) && proc.exitCode() == 0)
            return true;
    }
    return false;
}

// ---------- 系统识别 + 仓库可达性 ----------
InstallInfo EnvChecker::collectInstallInfo()
{
    InstallInfo info;

    // 1) /etc/os-release
    QFile f("/etc/os-release");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        QHash<QString, QString> kv;
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.startsWith("#") || !line.contains("=")) continue;
            const int eq = line.indexOf('=');
            QString key = line.left(eq).trimmed();
            QString val = line.mid(eq + 1).trimmed();
            if (val.startsWith('"') && val.endsWith('"'))
                val = val.mid(1, val.length() - 2);
            kv.insert(key, val);
        }
        info.osId = kv.value("ID");
        info.osVersion = kv.value("VERSION_ID", kv.value("VERSION"));
        QString pretty = kv.value("PRETTY_NAME");
        if (pretty.isEmpty())
            pretty = kv.value("NAME");
        if (!info.osVersion.isEmpty() && !kv.value("VERSION").isEmpty()
            && kv.value("VERSION") != info.osVersion)
            pretty = QString("%1 %2").arg(pretty, info.osVersion);
        info.distroLabel = pretty.isEmpty()
                           ? tr("未知发行版")
                           : pretty;
    }

    // 2) 架构
    QProcess archProc;
    archProc.start("dpkg", {"--print-architecture"});
    if (archProc.waitForFinished(2000))
        info.arch = QString::fromLocal8Bit(archProc.readAllStandardOutput()).trimmed();

    // 3) 在线状态 + 包是否存在于已配置仓库
    info.online = checkOnline();

    // 包名按发行版差异：Deepin/UOS/Debian/Ubuntu 全部叫 ipp-usb
    info.packageName = "ipp-usb";

    if (info.online) {
        QProcess apt;
        apt.start("apt-cache", {"madison", info.packageName});
        if (apt.waitForFinished(4000)) {
            const QString out = QString::fromLocal8Bit(apt.readAllStandardOutput());
            // 输出形如："ipp-usb | 0.9.27-1+b1 | http://... bookworm/main amd64 Packages"
            static const QRegularExpression re("^[\\S]+\\s+\\|\\s+([\\S+~\\.\\-]+)\\s+\\|");
            auto m = re.match(out);
            if (m.hasMatch()) {
                info.aptSourceAvailable = true;
                info.packageVersion = m.captured(1);
            }
        }

        // 兜底：apt-cache search 看仓库是否能匹配到
        if (!info.aptSourceAvailable) {
            QProcess aptList;
            aptList.start("apt-cache", {"search", info.packageName});
            if (aptList.waitForFinished(4000)) {
                const QString out = QString::fromLocal8Bit(aptList.readAllStandardOutput());
                if (out.contains(info.packageName)) {
                    info.aptSourceAvailable = true;
                }
            }
        }
    }

    // 4) 离线下载页面（按发行版给真实仓库地址，避免给假链接）
    QString arch = info.arch;
    if (arch == "x86_64") arch = "amd64";
    if (arch == "aarch64") arch = "arm64";

    if (info.osId == "deepin") {
        // Deepin 25 (Beige) 主仓库，含 community
        info.repoUrl = QString("https://packages.deepin.com/deepin/pool/main/i/ipp-usb/");
        info.downloadTip = QString(tr("在联网的电脑上下载 .deb 后用 U 盘拷贝到本机，"
                                       "然后双击安装或执行 sudo dpkg -i ipp-usb_*.deb。"
                                       "仓库页：%1")).arg(info.repoUrl);
    } else if (info.osId == "uos") {
        info.repoUrl = QString("https://professional-packages.chinauos.com/repository/professional/%1/")
                           .arg(info.osVersion.isEmpty() ? "1050" : info.osVersion);
        info.downloadTip = QString(tr("到 UOS 商店或专业仓库下载 ipp-usb_%1.deb 后双击安装。"
                                       "仓库页：%1")).arg(info.repoUrl);
    } else {
        // Debian / Ubuntu 通用
        info.repoUrl = QString("https://packages.debian.org/%1/%2/ipp-usb")
                           .arg(info.osVersion.isEmpty() ? "bookworm" : info.osVersion,
                                arch);
        info.downloadTip = QString(tr("从 Debian 官方仓库或镜像站下载 ipp-usb_%1.deb 后"
                                       "使用 sudo dpkg -i 安装。详情：%2"))
                                .arg(arch, info.repoUrl);
    }

    // 5) 百度网盘统一离线下载地址（与发行版无关，内网用户可手动下载）
    info.baiduPanUrl = QString("https://pan.baidu.com/s/5W-hv5cEfyZKTl9dnAsn1mg");

    return info;
}

// ---------- 安装 ----------
int EnvChecker::installIppUsb(QString &errMsg)
{
    // 装包必须 root（Always）。超时给 10 分钟，慢源也能装完。
    QString out;
    const int code = Privileges::run(QStringLiteral("apt-get"),
                                     {QStringLiteral("install"), QStringLiteral("-y"),
                                      QStringLiteral("--no-install-recommends"),
                                      QStringLiteral("ipp-usb")},
                                     Privileges::Elevation::Always,
                                     &out, &errMsg, 600000);
    if (code == -1) {
        errMsg = errMsg.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
                     ? tr("安装超时（600 秒）或授权窗口被关闭")
                     : tr("无法启动 pkexec（系统缺少授权组件）");
        return -2;
    }
    if (code != 0) {
        errMsg = errMsg.trimmed();
        if (errMsg.isEmpty())
            errMsg = out.trimmed();
        if (errMsg.isEmpty())
            errMsg = QString(tr("apt-get install ipp-usb 失败（exit %1）")).arg(code);
        return code;
    }
    return 0;
}

int EnvChecker::uninstallIppUsb(QString &errMsg)
{
    // 卸载必须 root（Always）。--purge 同时删除配置文件，确保下一次安装是干净状态。
    // 卸载不会删除 /var/ipp-usb/dev/*.state（用户端口绑定状态），但会清除 service 单元，
    // 正是“启动失败反复无法恢复”时的兜底重置手段。
    QString out;
    const int code = Privileges::run(QStringLiteral("apt-get"),
                                     {QStringLiteral("remove"), QStringLiteral("--purge"),
                                      QStringLiteral("-y"), QStringLiteral("ipp-usb")},
                                     Privileges::Elevation::Always,
                                     &out, &errMsg, 300000);
    if (code == -1) {
        errMsg = errMsg.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
                     ? tr("卸载超时（300 秒）或授权窗口被关闭")
                     : tr("无法启动 pkexec（系统缺少授权组件）");
        return -2;
    }
    if (code != 0) {
        errMsg = errMsg.trimmed();
        if (errMsg.isEmpty())
            errMsg = out.trimmed();
        if (errMsg.isEmpty())
            errMsg = QString(tr("apt-get remove ipp-usb 失败（exit %1）")).arg(code);
        return code;
    }
    return 0;
}

// ---------- 一键自检（异步） ----------
void EnvChecker::checkWithInstallInfo()
{
    if (m_checkRunning)
        return;

    using FullResult = QPair<CheckResult, InstallInfo>;
    m_checkRunning = true;
    auto *watcher = new QFutureWatcher<FullResult>(this);
    connect(watcher, &QFutureWatcher<FullResult>::finished, this, [this, watcher]() {
        const FullResult result = watcher->result();
        applyCheckResult(result.first);
        m_checkRunning = false;
        watcher->deleteLater();
        emit checkFinished(result.second);
    });
    watcher->setFuture(QtConcurrent::run([]() -> FullResult {
        return qMakePair(collectCheckResult(), collectInstallInfo());
    }));
}

// ---------- 启动策略 ----------
QString EnvChecker::ippUsbPreset()
{
    QProcess proc;
    proc.start("systemctl", {"list-unit-files", "ipp-usb.service"});
    if (!proc.waitForFinished(2000))
        return QString();
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    // 输出形如："ipp-usb.service                          static          -"
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (!line.startsWith("ipp-usb.service")) continue;
        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2) return parts[1];
    }
    return QString();
}

bool EnvChecker::enableIppUsb(QString &errMsg)
{
    // 管理系统服务必须 root（Always）
    const int code = Privileges::run(QStringLiteral("systemctl"),
                                     {QStringLiteral("enable"), QStringLiteral("--now"),
                                      QStringLiteral("ipp-usb.service")},
                                     Privileges::Elevation::Always,
                                     nullptr, &errMsg, 60000);
    if (code == -1) {
        errMsg = errMsg.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
                     ? tr("操作超时（60 秒）或授权窗口被关闭")
                     : tr("无法启动 pkexec（系统缺少授权组件）");
        return false;
    }
    if (code != 0) {
        errMsg = errMsg.trimmed();
        if (errMsg.isEmpty())
            errMsg = QString(tr("systemctl enable --now ipp-usb.service 失败（exit %1）"))
                         .arg(code);
        return false;
    }
    return true;
}
