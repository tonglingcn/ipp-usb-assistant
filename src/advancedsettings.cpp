#include "advancedsettings.h"
#include "privileges.h"
#include "envchecker.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QInputDialog>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QLabel>
#include <QTabWidget>
#include <QFrame>
#include <QFrame>
#include <QShowEvent>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <DMessageBox>

namespace {

constexpr char kQuirkPath[] = "/etc/ipp-usb/quirks/ipp-usb-driverless-assistant.conf";

// sane-airscan 配置目录。conf_load_from_dir() 会先读 <dir>/airscan.conf，
// 再 opendir 遍历 <dir>/airscan.d/ 下的全部文件（无后缀过滤），
// 因此放在 airscan.d/ 下既会被加载，又不会覆盖用户自己的 airscan.conf。
constexpr char kAirscanDir[] = "/etc/sane.d/airscan.d";
constexpr char kAirscanPath[] = "/etc/sane.d/airscan.d/ipp-usb-assistant.conf";

// 去掉 INI 值两侧的空白与成对引号：  "Xerox*"  ->  Xerox*
QString dequote(const QString &raw)
{
    QString v = raw.trimmed();
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"')
                          || (v.front() == '\'' && v.back() == '\'')))
        v = v.mid(1, v.size() - 2);
    return v.trimmed();
}

// 按 airscan 的配置格式加引号。含引号会破坏 INI 解析，直接拒绝。
QString quoteIniValue(const QString &value)
{
    return QStringLiteral("\"%1\"").arg(value);
}

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QString runCommand(const QString &program, const QStringList &args, int timeoutMs, int *exitCode = nullptr)
{
    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForStarted(timeoutMs)) {
        if (exitCode) *exitCode = -1;
        return QString();
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        if (exitCode) *exitCode = -1;
        return QString();
    }
    if (exitCode) *exitCode = proc.exitCode();
    return QString::fromUtf8(proc.readAllStandardOutput());
}

QString extractLsusbString(const QString &block, const QString &key)
{
    QRegularExpression re(key + R"(\s+\d+\s+(.*))");
    auto m = re.match(block);
    if (!m.hasMatch())
        return QString();
    QString v = m.captured(1).trimmed();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.mid(1, v.size() - 2);
    return v.trimmed();
}

// 复刻 ipp-usb 的 UsbDeviceInfo.FixUp() 算法：
//   MfgAndProduct = ProductName
//   if !strings.HasPrefix(ProductName, Manufacturer):
//       MfgAndProduct = Manufacturer + " " + ProductName
// 这是 ipp-usb 用于匹配 quirks 段名的真实键，必须与之一致，
// 否则写出的放行规则段名无法命中，blacklist 永不生效。
QString computeMfgAndProduct(const QString &manufacturer, const QString &product)
{
    const QString mfg = manufacturer.trimmed();
    const QString prod = product.trimmed();
    if (prod.startsWith(mfg, Qt::CaseInsensitive))
        return prod;
    if (!mfg.isEmpty() && !prod.isEmpty())
        return mfg + " " + prod;
    return prod.isEmpty() ? mfg : prod;
}

// 解析用户输入的 VID:PID。支持 "232b:5f20"、"232b 5f20"、
// "232b/5f20"、"232b-5f20"、"232b,5f20" 以及纯 "5f20"（仅 PID）形式。
// 返回 true 时 vid/pid 为有效 16 进制值（0~0xFFFF）。
bool parseVidPid(const QString &raw, int &vid, int &pid)
{
    vid = -1;
    pid = -1;
    QString s = raw.trimmed();
    if (s.isEmpty())
        return false;

    // 提取所有连续的十六进制 token（由非十六进制字符分隔）
    QRegularExpression re(QStringLiteral("([0-9a-fA-F]{1,4})"));
    QStringList tokens;
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        tokens.append(it.next().captured(1));
        if (tokens.size() == 2)
            break;
    }

    bool okV = false, okP = false;
    if (tokens.size() >= 2) {
        vid = tokens[0].toInt(&okV, 16);
        pid = tokens[1].toInt(&okP, 16);
    } else if (tokens.size() == 1) {
        // 仅 PID
        pid = tokens[0].toInt(&okP, 16);
    }
    return (vid >= 0 && vid <= 0xFFFF && pid >= 0 && pid <= 0xFFFF && okV && okP)
           || (vid == -1 && pid >= 0 && pid <= 0xFFFF && okP);
}

} // namespace

AdvancedSettings::AdvancedSettings(QWidget *parent)
    : QWidget(parent)
{
    // load() 末尾会一并加载扫描排除规则，此处无需重复调用。
    buildUi();
    load();
}

void AdvancedSettings::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_hasScanned)
        scanDevices();
}

void AdvancedSettings::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    auto *title = new DLabel(tr("高级设置"));
    title->setObjectName("pageTitle");
    auto *desc = new DLabel(tr(
        "当厂商提供原生驱动时，可用下面两种方式调整设备由谁接管。"
        "两者<span style='font-weight:600;'>互相独立</span>，可按设备分别设置。"));
    desc->setObjectName("pageSubtitle");
    desc->setWordWrap(true);
    desc->setTextFormat(Qt::RichText);

    layout->addWidget(title);
    layout->addWidget(desc);

    // 两个维度拆成子标签：原先上下堆叠导致单页塞进 2 个标题、2 张列表、
    // 2 组按钮与 2 段提示，滚动条很长且重点不明。拆开后每页只做一件事。
    auto *tabs = new QTabWidget;
    tabs->setObjectName("advancedTabs");
    tabs->setDocumentMode(true);   // 去掉边框留白，与 DTK 内容区更贴合
    tabs->addTab(buildIppUsbTab(), tr("ipp-usb 整机放行"));
    tabs->addTab(buildScanExcludeTab(), tr("扫描通道排除"));
    layout->addWidget(tabs, 1);
}

QWidget *AdvancedSettings::buildIppUsbTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 16, 0, 0);
    layout->setSpacing(12);

    auto *desc = new DLabel(tr(
        "把设备加入放行后，ipp-usb <span style='font-weight:600;'>完全不再接管它</span>，"
        "打印与扫描一并交还原厂驱动。适用于厂商自带完整驱动的一体机。"));
    desc->setObjectName("pageSubtitle");
    desc->setWordWrap(true);
    desc->setTextFormat(Qt::RichText);
    layout->addWidget(desc);

    // ---- 候选设备 ----
    // 列表已按 IPP-over-USB 规则过滤（≥2 个 7/1/4 接口），
    // 所以标题要明确是"候选"，而非笼统的"已连接设备"。
    auto *deviceTitle = new DLabel(tr("IPP-over-USB 候选设备"));
    deviceTitle->setObjectName("sectionTitle");
    layout->addWidget(deviceTitle);

    m_deviceStatus = new DLabel(tr("正在扫描已连接设备…"));
    m_deviceStatus->setObjectName("deviceStatusLabel");
    layout->addWidget(m_deviceStatus);

    m_deviceList = new QListWidget;
    m_deviceList->setObjectName("deviceList");
    m_deviceList->setSelectionMode(QAbstractItemView::NoSelection);
    // 候选设备通常只有 1~2 台，行高已压紧；列表框只保留单行高度，
    // 超过时通过滚动条查看。
    // 与扫描通道排除页的当前扫描设备列表保持相同高度（48~96），视觉上更协调。
    m_deviceList->setMinimumHeight(48);
    m_deviceList->setMaximumHeight(96);
    m_deviceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_deviceList);

    auto *deviceBtnRow = new QHBoxLayout;
    deviceBtnRow->setSpacing(12);
    m_btnRefreshDevices = new QPushButton(tr("刷新"));
    deviceBtnRow->addWidget(m_btnRefreshDevices);
    deviceBtnRow->addStretch();
    layout->addLayout(deviceBtnRow);

    auto *deviceHint = new DLabel(tr(
        "优先使用 ipp-usb 官方判定，无权限时自动回退到 lsusb，全程无需密码。"));
    deviceHint->setObjectName("deviceHintLabel");
    deviceHint->setWordWrap(true);
    layout->addWidget(deviceHint);

    // ---- 放行规则 ----
    auto *ruleTitle = new DLabel(tr("放行规则"));
    ruleTitle->setObjectName("sectionTitle");
    layout->addWidget(ruleTitle);

    m_status = new DLabel(tr("状态：等待加载配置…"));
    m_status->setObjectName("statusLabel");
    layout->addWidget(m_status);

    m_list = new QListWidget;
    m_list->setObjectName("ruleList");
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    // 放行规则列表与下方按钮行保持距离，高度适中。
    m_list->setMinimumHeight(80);
    m_list->setMaximumHeight(140);
    layout->addWidget(m_list);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(12);
    btnRow->setContentsMargins(0, 6, 0, 0);
    m_btnAdd = new QPushButton(tr("手动添加"));
    m_btnRemove = new QPushButton(tr("删除选中"));
    m_btnSave = new QPushButton(tr("保存并应用"));
    m_btnReload = new QPushButton(tr("重新加载"));
    btnRow->addWidget(m_btnAdd);
    btnRow->addWidget(m_btnRemove);
    btnRow->addStretch();
    btnRow->addWidget(m_btnReload);
    btnRow->addWidget(m_btnSave);
    layout->addLayout(btnRow);

    auto *hint = new DLabel(tr(
        "保存需管理员权限。生效需重新插拔设备，或在环境检测页重启 ipp-usb 服务。"));
    hint->setObjectName("hintLabel");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(m_btnAdd, &QPushButton::clicked, this, &AdvancedSettings::addRule);
    connect(m_btnRemove, &QPushButton::clicked, this, &AdvancedSettings::removeRule);
    connect(m_btnSave, &QPushButton::clicked, this, &AdvancedSettings::save);
    connect(m_btnReload, &QPushButton::clicked, this, &AdvancedSettings::load);
    connect(m_btnRefreshDevices, &QPushButton::clicked, this, &AdvancedSettings::scanDevices);

    return page;
}

QWidget *AdvancedSettings::buildScanExcludeTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 16, 0, 0);
    layout->setSpacing(12);

    auto *desc = new DLabel(tr(
        "只摘掉该设备的 eSCL 扫描，<span style='font-weight:600;'>IPP 免驱打印不受影响</span>。"
        "适用于厂商提供原生 SANE 扫描驱动、但打印仍希望走免驱的一体机。"));
    desc->setObjectName("pageSubtitle");
    desc->setWordWrap(true);
    desc->setTextFormat(Qt::RichText);
    layout->addWidget(desc);

    auto *why = new DLabel(tr(
        "说明：ipp-usb 官方黑名单是整机级，会同时禁用打印与扫描。"
        "此处改用 sane-airscan 的 [blacklist]，仅排除 eSCL 扫描发现，不影响 CUPS 打印。"));
    why->setObjectName("deviceHintLabel");
    why->setWordWrap(true);
    layout->addWidget(why);

    // ---- airscan 设备 ----
    auto *airscanTitle = new DLabel(tr("当前扫描设备"));
    airscanTitle->setObjectName("sectionTitle");
    layout->addWidget(airscanTitle);

    m_airscanStatus = new DLabel(tr("尚未检测，点击下方按钮开始"));
    m_airscanStatus->setObjectName("deviceStatusLabel");
    layout->addWidget(m_airscanStatus);

    m_airscanList = new QListWidget;
    m_airscanList->setObjectName("airscanDeviceList");
    m_airscanList->setSelectionMode(QAbstractItemView::NoSelection);
    // 当前扫描设备通常 1~2 个，与候选设备列表保持相同高度（48~96）。
    m_airscanList->setMinimumHeight(48);
    m_airscanList->setMaximumHeight(96);
    m_airscanList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_airscanList);

    auto *airscanBtnRow = new QHBoxLayout;
    airscanBtnRow->setSpacing(12);
    m_btnRefreshAirscan = new QPushButton(tr("检测"));
    airscanBtnRow->addWidget(m_btnRefreshAirscan);
    airscanBtnRow->addStretch();
    layout->addLayout(airscanBtnRow);

    // ---- 排除规则 ----
    auto *excludeTitle = new DLabel(tr("排除规则"));
    excludeTitle->setObjectName("sectionTitle");
    layout->addWidget(excludeTitle);

    m_scanExcludeStatus = new DLabel(tr("状态：等待加载配置…"));
    m_scanExcludeStatus->setObjectName("statusLabel");
    layout->addWidget(m_scanExcludeStatus);

    m_scanExcludeList = new QListWidget;
    m_scanExcludeList->setObjectName("scanExcludeList");
    m_scanExcludeList->setSelectionMode(QAbstractItemView::SingleSelection);
    // 排除规则列表与下方按钮行保持距离；高度与整页比例协调。
    m_scanExcludeList->setMinimumHeight(80);
    m_scanExcludeList->setMaximumHeight(140);
    layout->addWidget(m_scanExcludeList);

    auto *excludeBtnRow = new QHBoxLayout;
    excludeBtnRow->setSpacing(12);
    // 列表框与按钮行之间留出一点间距，避免视觉拥挤/重叠
    excludeBtnRow->setContentsMargins(0, 6, 0, 0);
    m_btnAddExclude = new QPushButton(tr("手动添加"));
    m_btnRemoveExclude = new QPushButton(tr("删除选中"));
    m_btnSaveExclude = new QPushButton(tr("保存并应用"));
    excludeBtnRow->addWidget(m_btnAddExclude);
    excludeBtnRow->addWidget(m_btnRemoveExclude);
    excludeBtnRow->addStretch();
    excludeBtnRow->addWidget(m_btnSaveExclude);
    layout->addLayout(excludeBtnRow);

    auto *excludeHint = new DLabel(tr(
        "支持通配符，例如 Pantum* 可一次排除同系列机型。保存后立即生效，"
        "无需重新插拔设备，打印队列不受影响。"));
    excludeHint->setObjectName("hintLabel");
    excludeHint->setWordWrap(true);
    layout->addWidget(excludeHint);

    connect(m_btnRefreshAirscan, &QPushButton::clicked, this, &AdvancedSettings::refreshAirscanDevices);
    connect(m_btnAddExclude, &QPushButton::clicked, this, &AdvancedSettings::addScanExclude);
    connect(m_btnRemoveExclude, &QPushButton::clicked, this, &AdvancedSettings::removeScanExclude);
    connect(m_btnSaveExclude, &QPushButton::clicked, this, &AdvancedSettings::saveScanExcludes);

    return page;
}

bool AdvancedSettings::parseIppUsbDevice(const QString &line, UsbDevice &dev)
{
    // ipp-usb check 输出示例：
    // 1.   Bus 001 Device 004  232b:0f8e  "BM4240ADW series"
    static const QRegularExpression re(
        R"(^\s*\d+\.\s+Bus\s+\d+\s+Device\s+\d+\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s+(.*)$)");
    auto m = re.match(line.trimmed());
    if (!m.hasMatch())
        return false;

    bool vok = false, pok = false;
    dev.vid = m.captured(1).toInt(&vok, 16);
    dev.pid = m.captured(2).toInt(&pok, 16);
    dev.model = m.captured(3).trimmed();
    // 去掉 ipp-usb check 输出两端可能存在的引号
    if (dev.model.size() >= 2 && dev.model.front() == '"' && dev.model.back() == '"')
        dev.model = dev.model.mid(1, dev.model.size() - 2);

    if (!vok || !pok || dev.model.isEmpty())
        return false;

    // 从 lsusb -v 获取 manufacturer + product，并按 ipp-usb 的
    // MfgAndProduct 算法重算 dev.model，确保与 quirks 匹配键一致。
    // 若 lsusb 取不到名称，则退回到 ipp-usb check 输出的 model。
    UsbDevice detail;
    if (queryDeviceName(dev.vid, dev.pid, detail) &&
        (!detail.manufacturer.isEmpty() || !detail.product.isEmpty())) {
        dev.manufacturer = detail.manufacturer;
        dev.product = detail.product;
        dev.model = computeMfgAndProduct(detail.manufacturer, detail.product);
    }
    return true;
}

// 解析 EnvChecker::lsusbImagingDevices() 的输出行，形如：
//   Bus 001 Device 008  232b:5f20  Pantum Ltd. M6600NW series
// 这里拿到的设备已通过 IPP-over-USB 严格判定，无需再过滤。
bool AdvancedSettings::parseEnvDeviceLine(const QString &line, UsbDevice &dev)
{
    static const QRegularExpression re(
        R"(^\s*Bus\s+\d+\s+Device\s+\d+\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s*(.*)$)");
    auto m = re.match(line.trimmed());
    if (!m.hasMatch())
        return false;

    bool vok = false, pok = false;
    const int vid = m.captured(1).toInt(&vok, 16);
    const int pid = m.captured(2).toInt(&pok, 16);
    if (!vok || !pok)
        return false;

    // 仍走 queryDeviceName 取准确的 iManufacturer / iProduct，
    // 放行规则的段名必须是 ipp-usb 的 MfgAndProduct，不能拿拼接串凑合。
    if (queryDeviceName(vid, pid, dev))
        return true;

    // 兜底：单设备 lsusb -v 可能权限不足，此时退化用行内描述，
    // 至少让用户看到设备并可选择放行，而不是整个列表变空。
    dev.vid = vid;
    dev.pid = pid;
    dev.manufacturer.clear();
    dev.product = m.captured(3).trimmed();
    dev.model = dev.product;
    return !dev.model.isEmpty();
}

bool AdvancedSettings::parseLsusbDevice(const QString &line, UsbDevice &dev)
{
    // lsusb 输出示例：Bus 001 Device 004: ID 232b:0f8e Brother Industries, Ltd
    static const QRegularExpression re(
        R"(^.*ID\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s+(.*)$)");
    auto m = re.match(line.trimmed());
    if (!m.hasMatch())
        return false;

    bool vok = false, pok = false;
    dev.vid = m.captured(1).toInt(&vok, 16);
    dev.pid = m.captured(2).toInt(&pok, 16);
    if (!vok || !pok)
        return false;

    return queryDeviceName(dev.vid, dev.pid, dev);
}

bool AdvancedSettings::queryDeviceName(int vid, int pid, UsbDevice &dev)
{
    QStringList args;
    args << "-d" << QStringLiteral("%1:%2")
                .arg(vid, 4, 16, QLatin1Char('0'))
                .arg(pid, 4, 16, QLatin1Char('0'))
         << "-v";
    int code = 0;
    QString out = runCommand("lsusb", args, 15000, &code);
    if (code != 0 || out.isEmpty())
        return false;

    dev.vid = vid;
    dev.pid = pid;
    dev.manufacturer = extractLsusbString(out, "iManufacturer");
    dev.product = extractLsusbString(out, "iProduct");

    if (!dev.manufacturer.isEmpty() && !dev.product.isEmpty()) {
        dev.model = computeMfgAndProduct(dev.manufacturer, dev.product);
    } else if (!dev.product.isEmpty()) {
        dev.model = dev.product;
    } else if (!dev.manufacturer.isEmpty()) {
        dev.model = dev.manufacturer;
    } else {
        // 回退到 lsusb 简短短描述
        QRegularExpression re(QStringLiteral(R"(ID\s+%1:%2\s+(.*)$)")
                                  .arg(QString::number(vid, 16).rightJustified(4, '0'))
                                  .arg(QString::number(pid, 16).rightJustified(4, '0')));
        auto m = re.match(out);
        dev.model = m.hasMatch() ? m.captured(1).trimmed() : QStringLiteral("USB %1:%2").arg(vid, 0, 16).arg(pid, 0, 16);
    }
    return true;
}

QString AdvancedSettings::formatRuleDisplay(const Rule &rule) const
{
    return QStringLiteral("%1&nbsp;&nbsp;<span style='color:#666;'>(%2:%3)</span>")
        .arg(rule.deviceName.toHtmlEscaped())
        .arg(QString::number(rule.vid, 16).rightJustified(4, '0'))
        .arg(QString::number(rule.pid, 16).rightJustified(4, '0'));
}

QString AdvancedSettings::formatQuirkSection(const QString &deviceName) const
{
    return QStringLiteral("[%1]\n  blacklist = true\n").arg(deviceName);
}

bool AdvancedSettings::isDeviceAllowed(const QString &deviceName) const
{
    for (const Rule &r : m_rules) {
        if (r.deviceName.compare(deviceName, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void AdvancedSettings::scanDevices()
{
    if (m_scanRunning)
        return;

    m_scanRunning = true;
    m_btnRefreshDevices->setEnabled(false);
    m_deviceStatus->setText(tr("正在扫描已连接设备…"));
    m_devices.clear();
    refreshDeviceList();

    auto *watcher = new QFutureWatcher<DeviceScanResult>(this);
    connect(watcher, &QFutureWatcher<DeviceScanResult>::finished,
            this, [this, watcher]() {
        const DeviceScanResult result = watcher->result();
        m_scanRunning = false;
        watcher->deleteLater();
        m_devices = result.devices;
        m_deviceStatus->setText(result.status);
        if (result.success) {
            m_hasScanned = true;
            refreshDeviceList();
        } else {
            // 保留具体失败原因，不让空列表占位文案覆盖它。
            m_deviceList->clear();
        }
        m_btnRefreshDevices->setEnabled(true);
    });

    // 两个数据源，自动择优，用户无需理解区别：
    //  1) ipp-usb check —— 上游权威判定（真 libusb 枚举），但枚举 USB 需要
    //     对 /dev/bus/usb/* 的读写权；udev 规则已把节点设为 root:lp 0664，
    //     故仅 lp 组成员可免提权。此处**不主动提权**，避免无谓弹窗。
    //  2) lsusb -v —— 我们的复刻判定（≥2 个 7/1/4 接口 + IN/OUT），免密码。
    // 优先用 1，失败或无权限时静默回退到 2。
    watcher->setFuture(QtConcurrent::run([]() {
        DeviceScanResult result;
        int code = 0;
        QString out;

        // 1) 优先权威路径：仅在不需提权时尝试，成功即用
        if (Privileges::inGroup(QStringLiteral("lp"))) {
            out = runCommand(QStringLiteral("ipp-usb"),
                             {QStringLiteral("check")}, 60000, &code);
            if (code == 0 && !out.isEmpty()
                && !out.contains(QStringLiteral("requires root privileges"), Qt::CaseInsensitive)) {
                for (const QString &line : out.split('\n')) {
                    UsbDevice dev;
                    if (parseIppUsbDevice(line, dev))
                        result.devices.append(dev);
                }
                result.status = AdvancedSettings::tr("状态：检测到 %1 个 IPP-over-USB 设备")
                                    .arg(result.devices.size());
                result.success = true;
                return result;
            }
        }

        // 2) 回退：lsusb -v 严格判定。仅列真正的 IPP-over-USB 候选设备，
        //    Hub、键鼠等无关设备不会出现——放行它们毫无意义。
        for (const QString &line : EnvChecker::lsusbImagingDevices()) {
            UsbDevice dev;
            if (parseEnvDeviceLine(line, dev))
                result.devices.append(dev);
        }
        result.status = AdvancedSettings::tr("状态：检测到 %1 个 IPP-over-USB 候选设备")
                            .arg(result.devices.size());
        result.success = true;
        return result;
    }));
}

void AdvancedSettings::refreshDeviceList()
{
    m_deviceList->clear();
    for (int i = 0; i < m_devices.size(); ++i) {
        const UsbDevice &dev = m_devices[i];
        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, i);

        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        // 单行内容无需太高的留白，紧凑显示更协调
        rowLayout->setContentsMargins(12, 4, 12, 4);
        rowLayout->setSpacing(12);

        QString text = QStringLiteral("<b>%1</b>&nbsp;&nbsp;<span style='color:#666;'>%2:%3</span>")
            .arg(dev.model.toHtmlEscaped())
            .arg(QString::number(dev.vid, 16).rightJustified(4, '0'))
            .arg(QString::number(dev.pid, 16).rightJustified(4, '0'));
        auto *label = new QLabel(text);
        label->setTextFormat(Qt::RichText);
        rowLayout->addWidget(label, 1);

        bool allowed = isDeviceAllowed(dev.model);
        auto *btn = new QPushButton(allowed ? tr("已放行") : tr("放行"));
        btn->setEnabled(!allowed);
        if (!allowed) {
            connect(btn, &QPushButton::clicked, this, [this, i]() { addDeviceRule(i); });
        }
        rowLayout->addWidget(btn);

        item->setSizeHint(row->sizeHint());
        m_deviceList->addItem(item);
        m_deviceList->setItemWidget(item, row);
    }

    if (m_devices.isEmpty()) {
        m_deviceStatus->setText(tr("状态：未检测到 USB 打印/扫描设备"));
    }
}

void AdvancedSettings::load()
{
    m_list->clear();
    m_rules.clear();

    const QString text = readFile(QLatin1String(kQuirkPath));
    if (text.isEmpty()) {
        m_status->setText(tr("状态：未加载到放行规则，保存后将自动创建 quirks 文件。"));
    } else {
        // 解析 quirks 文件中的 [Section] 段名
        static const QRegularExpression re(R"(^\[([^\]]+)\]\s*$)");
        for (const QString &raw : text.split('\n')) {
            QString line = raw.trimmed();
            auto m = re.match(line);
            if (!m.hasMatch())
                continue;
            Rule r;
            r.deviceName = m.captured(1).trimmed();
            if (!r.deviceName.isEmpty()) {
                m_rules.append(r);
                m_list->addItem(r.deviceName);
            }
        }
        m_status->setText(tr("状态：已加载 %1 条放行规则").arg(m_rules.size()));
    }

    refreshDeviceList();
    loadScanExcludes();
}

void AdvancedSettings::save()
{
    QStringList out;
    out << tr("# IPP-USB 免驱助手 - 放行规则");
    out << tr("# 被列出的设备不会被 ipp-usb 接管，原厂 SANE/扫描软件可直接通过 USB 访问。");
    out << QString();

    for (const Rule &r : m_rules)
        out << formatQuirkSection(r.deviceName);

    const QByteArray data = out.join('\n').toUtf8();

    // 确保 quirks 目录存在（pkexec tee 不能自动创建父目录）
    QProcess mkdirProc;
    mkdirProc.setProgram("pkexec");
    mkdirProc.setArguments({"mkdir", "-p", QLatin1String("/etc/ipp-usb/quirks")});
    mkdirProc.start();
    if (!mkdirProc.waitForStarted(5000) || !mkdirProc.waitForFinished(10000) || mkdirProc.exitCode() != 0) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                             tr("无法创建 /etc/ipp-usb/quirks 目录：%1")
                                 .arg(QString::fromLocal8Bit(mkdirProc.readAllStandardError())));
        return;
    }

    QProcess pkexec;
    pkexec.setProgram("pkexec");
    pkexec.setArguments({"tee", QLatin1String(kQuirkPath)});
    pkexec.start();
    if (!pkexec.waitForStarted(5000)) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                             tr("无法启动权限提升工具：%1").arg(pkexec.errorString()));
        return;
    }
    pkexec.write(data);
    pkexec.closeWriteChannel();
    if (!pkexec.waitForFinished(30000) || pkexec.exitCode() != 0) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                             tr("保存失败：%1").arg(QString::fromLocal8Bit(pkexec.readAllStandardError())));
        return;
    }

    m_status->setText(tr("状态：已保存 %1 条放行规则").arg(m_rules.size()));
    DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                              tr("配置已保存到 /etc/ipp-usb/quirks/。请重新插拔设备或重启 ipp-usb 服务使规则生效。"));
}

void AdvancedSettings::addRule()
{
    bool ok = false;
    QString text = QInputDialog::getText(this, tr("手动添加放行设备"),
                                         tr("粘贴 lsusb 显示的“VID:PID”（例如 232b:5f20），程序将自动查询设备名称并生成放行规则；\n"
                                            "也可直接输入设备名称（如 Pantum M6600NW series）。"),
                                         QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;

    QString input = text.trimmed();
    int vid = -1, pid = -1;

    Rule r;
    // 优先按 VID:PID 解析，自动查 lsusb -v 生成正确规则名
    if (parseVidPid(input, vid, pid) && vid >= 0) {
        UsbDevice dev;
        if (queryDeviceName(vid, pid, dev) && !dev.model.isEmpty()) {
            r.vid = vid;
            r.pid = pid;
            r.deviceName = dev.model;
        } else {
            DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                                 tr("未能通过 VID:PID 查询到设备名称（设备是否已连接？）。\n"
                                    "请改用设备名称手动添加。"));
            return;
        }
    } else {
        // 非 VID:PID 输入，按纯设备名称处理
        r.deviceName = input;
    }

    if (isDeviceAllowed(r.deviceName)) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                                  tr("该设备已存在于放行列表中。"));
        return;
    }

    m_rules.append(r);
    m_list->addItem(r.deviceName);
    m_status->setText(tr("状态：已添加 %1 条放行规则（尚未保存）").arg(m_rules.size()));
}

void AdvancedSettings::removeRule()
{
    int row = m_list->currentRow();
    if (row < 0 || row >= m_rules.size()) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"), tr("请先选择要删除的放行规则。"));
        return;
    }
    m_rules.removeAt(row);
    delete m_list->takeItem(row);
    m_status->setText(tr("状态：已移除 1 条规则，当前共 %1 条（尚未保存）").arg(m_rules.size()));
    refreshDeviceList();
}

void AdvancedSettings::addDeviceRule(int index)
{
    if (index < 0 || index >= m_devices.size())
        return;
    const UsbDevice &dev = m_devices[index];
    if (isDeviceAllowed(dev.model)) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"), tr("该设备已存在于放行列表中。"));
        return;
    }

    Rule r;
    r.deviceName = dev.model;
    r.vid = dev.vid;
    r.pid = dev.pid;
    m_rules.append(r);
    m_list->addItem(r.deviceName);
    m_status->setText(tr("状态：已添加 %1 条放行规则（尚未保存）").arg(m_rules.size()));

    refreshDeviceList();
}

// ---------------------------------------------------------------------------
// 扫描排除（sane-airscan）
// ---------------------------------------------------------------------------

// airscan-discover 输出形如：
//   [devices]
//     Pantum M6600NW series (USB) = http://127.0.0.1:60000/eSCL/, eSCL
// 其中左侧就是 sane-airscan 用于 [blacklist] name 匹配的 DNS-SD network name
// （zeroconf_devinfo_lookup(dev->name)->name），可直接写入配置文件。
bool AdvancedSettings::parseAirscanDevice(const QString &line, AirscanDevice &dev)
{
    const QString s = line.trimmed();
    if (s.isEmpty() || s.startsWith('[') || s.startsWith('#'))
        return false;

    // 名称可含空格与逗号（如 "... series (USB)"），故左侧用非贪婪；
    // URI 与协议名均不含空白，用 \S+ 精确界定。
    static const QRegularExpression re(R"(^(.+?)\s+=\s+(\S+?)\s*,\s*(\S+)\s*$)");
    auto m = re.match(s);
    if (!m.hasMatch())
        return false;

    dev.name = dequote(m.captured(1).trimmed());
    dev.uri = m.captured(2).trimmed();
    dev.proto = m.captured(3).trimmed();
    // airscan-discover 对本地回环可能输出 IPv6 字面量 [::1]，
    // 与扫描管理页保持一致，显示为 localhost 更易读。
    if (dev.uri.contains(QLatin1String("[::1]")))
        dev.uri.replace(QLatin1String("[::1]"), QLatin1String("localhost"));
    return !dev.name.isEmpty();
}

QString AdvancedSettings::dequoteIniValue(const QString &raw)
{
    return dequote(raw);
}

// 排除 airscan 后必须有别的 SANE 后端接管，否则该设备扫描功能直接消失。
// dll.conf 中未注释的后端（airscan 本身除外）即视为可用。
bool AdvancedSettings::hasAlternativeSaneBackend()
{
    const QString text = readFile(QStringLiteral("/etc/sane.d/dll.conf"));
    if (text.isEmpty())
        return false;

    for (const QString &raw : text.split('\n')) {
        QString line = raw.trimmed();
        const int hash = line.indexOf('#');
        if (hash >= 0)
            line = line.left(hash).trimmed();
        if (line.isEmpty())
            continue;
        if (line.compare(QStringLiteral("airscan"), Qt::CaseInsensitive) == 0)
            continue;
        return true;
    }
    return false;
}

bool AdvancedSettings::isScanExcluded(const QString &pattern, ExcludeField field) const
{
    for (const ScanExcludeRule &r : m_scanExcludes) {
        if (r.field == field && r.pattern.compare(pattern, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void AdvancedSettings::addExcludeRule(const QString &pattern, ExcludeField field)
{
    // sane-airscan 的 INI 解析器把 " 当作字符串起止（PRS_STRING），
    // 把 \ 当作转义引导（PRS_STRING_BSLASH，支持 \n \t \xNN 等），
    // 二者出现在设备名里都会破坏配置语义，直接拒绝。
    if (pattern.contains('"') || pattern.contains('\\')) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                             tr("设备名称不能包含双引号或反斜杠，否则会破坏配置文件格式。"));
        return;
    }
    if (isScanExcluded(pattern, field)) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                                 tr("该规则已存在于扫描排除列表中。"));
        return;
    }

    m_scanExcludes.append({pattern, field});
    refreshScanExcludeList();
    m_scanExcludeStatus->setText(tr("状态：已添加规则，当前共 %1 条（尚未保存）")
                                     .arg(m_scanExcludes.size()));
}

void AdvancedSettings::loadScanExcludes()
{
    m_scanExcludes.clear();

    const QString text = readFile(QLatin1String(kAirscanPath));
    if (text.isEmpty()) {
        m_scanExcludeStatus->setText(tr("状态：暂无扫描排除规则，保存后自动创建配置文件。"));
    } else {
        bool inBlacklist = false;
        for (const QString &raw : text.split('\n')) {
            QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
                continue;

            if (line.startsWith('[')) {
                inBlacklist = line.compare(QStringLiteral("[blacklist]"), Qt::CaseInsensitive) == 0;
                continue;
            }
            if (!inBlacklist)
                continue;

            const int eq = line.indexOf('=');
            if (eq < 0)
                continue;
            const QString key = line.left(eq).trimmed().toLower();
            const QString value = dequote(line.mid(eq + 1).trimmed());
            if (value.isEmpty())
                continue;

            if (key == QLatin1String("model"))
                m_scanExcludes.append({value, ExcludeField::Model});
            else if (key == QLatin1String("name"))
                m_scanExcludes.append({value, ExcludeField::Name});
        }
        m_scanExcludeStatus->setText(tr("状态：已加载 %1 条扫描排除规则").arg(m_scanExcludes.size()));
    }

    refreshScanExcludeList();
}

void AdvancedSettings::refreshScanExcludeList()
{
    m_scanExcludeList->clear();
    for (const ScanExcludeRule &r : m_scanExcludes) {
        const QString tag = (r.field == ExcludeField::Model) ? tr("型号") : tr("网络名");
        m_scanExcludeList->addItem(QStringLiteral("%1    [%2]").arg(r.pattern, tag));
    }
}

void AdvancedSettings::refreshAirscanList()
{
    m_airscanList->clear();
    for (int i = 0; i < m_airscanDevices.size(); ++i) {
        const AirscanDevice &dev = m_airscanDevices[i];
        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, i);

        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        // 扫描设备也是单行内容，行高压紧更协调
        rowLayout->setContentsMargins(12, 4, 12, 4);
        rowLayout->setSpacing(12);

        const QString text = QStringLiteral("<b>%1</b>&nbsp;&nbsp;"
                                            "<span style='color:#666;'>%2 · %3</span>")
                                 .arg(dev.name.toHtmlEscaped())
                                 .arg(dev.proto.toHtmlEscaped())
                                 .arg(dev.uri.toHtmlEscaped());
        auto *label = new QLabel(text);
        label->setTextFormat(Qt::RichText);
        rowLayout->addWidget(label, 1);

        // 已按网络名排除时置灰；型号维度单独判断，两者互不覆盖。
        const bool excluded = isScanExcluded(dev.name, ExcludeField::Name);
        auto *btn = new QPushButton(excluded ? tr("已排除") : tr("排除扫描"));
        btn->setEnabled(!excluded);
        if (!excluded) {
            connect(btn, &QPushButton::clicked, this, [this, i]() { excludeDeviceScan(i); });
        }
        rowLayout->addWidget(btn);

        item->setSizeHint(row->sizeHint());
        m_airscanList->addItem(item);
        m_airscanList->setItemWidget(item, row);
    }
}

void AdvancedSettings::refreshAirscanDevices()
{
    if (m_airscanScanRunning)
        return;

    m_airscanScanRunning = true;
    m_btnRefreshAirscan->setEnabled(false);
    m_airscanStatus->setText(tr("正在通过 airscan-discover 检测扫描设备…"));
    m_airscanDevices.clear();
    refreshAirscanList();

    auto *watcher = new QFutureWatcher<QPair<QList<AirscanDevice>, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<QList<AirscanDevice>, QString>>::finished,
            this, [this, watcher]() {
        const auto result = watcher->result();
        m_airscanScanRunning = false;
        watcher->deleteLater();
        m_airscanDevices = result.first;
        m_airscanStatus->setText(result.second);
        refreshAirscanList();
        m_btnRefreshAirscan->setEnabled(true);
    });
    watcher->setFuture(QtConcurrent::run([]() {
        QList<AirscanDevice> devs;
        QString status;
        int code = 0;

        // airscan-discover 不需要 root，输出即权威的 DNS-SD network name。
        const QString out = runCommand(QStringLiteral("airscan-discover"), {}, 30000, &code);
        if (code != 0 || out.isEmpty()) {
            status = AdvancedSettings::tr(
                "未检测到 airscan 设备。请确认已安装 sane-airscan，且 avahi-daemon 正在运行。");
            return qMakePair(devs, status);
        }

        QString lastName;
        for (const QString &line : out.split('\n')) {
            AirscanDevice dev;
            if (!parseAirscanDevice(line, dev))
                continue;
            // 同一设备的多个端点（eSCL/WSD）会输出多行同名记录，只保留第一条。
            if (dev.name == lastName)
                continue;
            lastName = dev.name;
            devs.append(dev);
        }
        status = AdvancedSettings::tr("检测到 %1 个 airscan 扫描设备").arg(devs.size());
        return qMakePair(devs, status);
    }));
}

void AdvancedSettings::excludeDeviceScan(int index)
{
    if (index < 0 || index >= m_airscanDevices.size())
        return;

    const AirscanDevice &dev = m_airscanDevices[index];

    // 排除后若无其他后端接管，用户将彻底失去扫描能力，先确认再落地。
    if (!hasAlternativeSaneBackend()) {
        const int rc = DMessageBox::warning(
            this, tr("IPP-USB 免驱助手"),
            tr("未检测到其他已启用的 SANE 后端（/etc/sane.d/dll.conf 中仅有 airscan）。\n\n"
               "排除该设备后，若厂商驱动未正确安装，扫描功能将完全不可用。\n"
               "是否仍要继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (rc != QMessageBox::Yes)
            return;
    }

    addExcludeRule(dev.name, ExcludeField::Name);
    refreshAirscanList();
}

void AdvancedSettings::addScanExclude()
{
    bool ok = false;
    QString text = QInputDialog::getText(
        this, tr("手动添加扫描排除规则"),
        tr("输入设备型号或 DNS-SD 网络名，支持通配符（例如 Pantum*）。\n"
           "型号形如“Pantum M6600NW series”，网络名形如“Pantum M6600NW series (USB)”。"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;

    const QString pattern = text.trimmed();
    // ipp-usb 接管的设备网络名都带 " (USB)" 后缀，据此自动选择匹配字段。
    const ExcludeField field = pattern.contains(QStringLiteral(" (USB)"))
                                   ? ExcludeField::Name
                                   : ExcludeField::Model;
    addExcludeRule(pattern, field);
}

void AdvancedSettings::removeScanExclude()
{
    const int row = m_scanExcludeList->currentRow();
    if (row < 0 || row >= m_scanExcludes.size()) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"), tr("请先选择要删除的排除规则。"));
        return;
    }
    m_scanExcludes.removeAt(row);
    refreshScanExcludeList();
    refreshAirscanList();
    m_scanExcludeStatus->setText(tr("状态：已移除 1 条规则，当前共 %1 条（尚未保存）")
                                     .arg(m_scanExcludes.size()));
}

QString AdvancedSettings::renderScanExcludeFile() const
{
    QStringList out;
    out << tr("# IPP-USB 免驱助手 - 扫描排除规则");
    out << tr("# 本文件由 ipp-usb 免驱助手自动生成，手工修改可能被覆盖。");
    out << QStringLiteral("#");
    out << tr("# 被列出的设备不再通过 sane-airscan（eSCL）提供扫描，");
    out << tr("# 但 IPP 免驱打印不受影响：CUPS 打印链路不经过 sane-airscan。");
    out << tr("# 适用于厂商提供原生 SANE 扫描驱动、打印仍走免驱的一体机。");
    out << QStringLiteral("#");
    out << tr("# model = 硬件型号（DNS-SD TXT 记录）；name = DNS-SD 网络名，");
    out << tr("#         ipp-usb 接管的设备网络名带 \" (USB)\" 后缀。");
    out << tr("# 两个字段均由 sane-airscan 用 fnmatch 做 glob 匹配。");
    out << QString();
    out << QStringLiteral("[blacklist]");

    for (const ScanExcludeRule &r : m_scanExcludes) {
        const QString key = (r.field == ExcludeField::Model)
                                ? QStringLiteral("model")
                                : QStringLiteral("name");
        out << QStringLiteral("%1 = %2").arg(key, quoteIniValue(r.pattern));
    }
    out << QString();
    return out.join('\n');
}

bool AdvancedSettings::writeScanExcludeFile(QString *errorText) const
{
    QProcess mkdirProc;
    mkdirProc.setProgram(QStringLiteral("pkexec"));
    mkdirProc.setArguments({QStringLiteral("mkdir"), QStringLiteral("-p"),
                            QLatin1String(kAirscanDir)});
    mkdirProc.start();
    if (!mkdirProc.waitForStarted(5000) || !mkdirProc.waitForFinished(10000)
        || mkdirProc.exitCode() != 0) {
        if (errorText)
            *errorText = tr("无法创建 %1 目录：%2")
                             .arg(QLatin1String(kAirscanDir),
                                  QString::fromLocal8Bit(mkdirProc.readAllStandardError()));
        return false;
    }

    QProcess pkexec;
    pkexec.setProgram(QStringLiteral("pkexec"));
    pkexec.setArguments({QStringLiteral("tee"), QLatin1String(kAirscanPath)});
    pkexec.start();
    if (!pkexec.waitForStarted(5000)) {
        if (errorText)
            *errorText = tr("无法启动权限提升工具：%1").arg(pkexec.errorString());
        return false;
    }
    pkexec.write(renderScanExcludeFile().toUtf8());
    pkexec.closeWriteChannel();
    if (!pkexec.waitForFinished(30000) || pkexec.exitCode() != 0) {
        if (errorText)
            *errorText = tr("保存失败：%1")
                             .arg(QString::fromLocal8Bit(pkexec.readAllStandardError()));
        return false;
    }
    return true;
}

void AdvancedSettings::saveScanExcludes()
{
    // 规则为空时删除配置文件，避免残留一个空的 [blacklist] 段。
    if (m_scanExcludes.isEmpty()) {
        const QString text = readFile(QLatin1String(kAirscanPath));
        if (text.isEmpty()) {
            m_scanExcludeStatus->setText(tr("状态：没有需要保存的规则。"));
            return;
        }
        QProcess rm;
        rm.setProgram(QStringLiteral("pkexec"));
        rm.setArguments({QStringLiteral("rm"), QStringLiteral("-f"),
                         QLatin1String(kAirscanPath)});
        rm.start();
        if (!rm.waitForStarted(5000) || !rm.waitForFinished(10000) || rm.exitCode() != 0) {
            DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                                 tr("删除配置文件失败：%1")
                                     .arg(QString::fromLocal8Bit(rm.readAllStandardError())));
            return;
        }
        m_scanExcludeStatus->setText(tr("状态：已清空扫描排除规则"));
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                                 tr("已删除扫描排除配置，所有设备恢复通过 airscan 扫描。"));
        refreshAirscanList();
        return;
    }

    QString err;
    if (!writeScanExcludeFile(&err)) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"), err);
        return;
    }

    m_scanExcludeStatus->setText(tr("状态：已保存 %1 条扫描排除规则").arg(m_scanExcludes.size()));
    DMessageBox::information(
        this, tr("IPP-USB 免驱助手"),
        tr("配置已保存到 %1。\n\n规则立即生效，无需重新插拔设备或重启服务；"
           "打印功能不受影响。")
            .arg(QLatin1String(kAirscanPath)));
    refreshAirscanList();
}
