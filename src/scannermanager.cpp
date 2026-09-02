#include "scannermanager.h"

#include <QProcess>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>

extern "C" {
#include <sane/sane.h>
#include "qtcompat.h"
}

ScannerManager::ScannerManager(QObject *parent)
    : QObject(parent)
{
}

// 查询设备某选项的可用值列表（scanimage -d dev -A，秒级）。
// 输出形如 "    --mode Color|Gray|Lineart [Color]"
static QStringList optionValues(const QString &dev, const QString &opt)
{
    QProcess p;
    p.start("scanimage", {"-d", dev, "-A"});
    if (!p.waitForFinished(10000))
        return {};
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    for (const QString &line : out.split('\n', kSkipEmptyParts)) {
        const QString t = line.trimmed();
        if (!t.startsWith("--" + opt + ' '))
            continue;
        // 取 "Color|Gray|Lineart [Color]" 中括号前的值段
        QString vals = t.mid(opt.size() + 3);
        const int bracket = vals.indexOf('[');
        if (bracket >= 0)
            vals = vals.left(bracket);
        QStringList list;
        for (const QString &v : vals.split('|', kSkipEmptyParts))
            list.append(v.trimmed());
        return list;
    }
    return {};
}

// SANE 枚举结果：设备名与其后端类别一一对应。
struct ScanDeviceList {
    QStringList names;
    QList<ScanBackend> backends;
};

ScanBackend ScannerManager::classifyBackend(const QString &name, const QString &type)
{
    const QString n = name.toLower();

    // 1) 设备名前缀判定（最可靠）：免驱 eSCL 固定由这两个后端提供
    if (n.startsWith(QLatin1String("airscan:")) || n.startsWith(QLatin1String("escl:")))
        return ScanBackend::Escl;

    // 2) 已知厂商原生 SANE 后端。这些不走 eSCL，由厂商驱动负责。
    static const char *vendorPrefixes[] = {
        "hpaio:",    // HP（hplip）
        "epson2:", "epson:",  // 爱普生（iscan / imagescan）
        "pixma:",    // 佳能
        "brother",   // 兄弟（brother4: / brother:）
        "genesys:", "gt68xx:", "plustek:", "u12:", "umax:", "niash:",
        "snapscan:", "mustek:", "artec:", "avision:", "epjitsu:", "fujitsu:",
        "kodak:", "panasonic:", "ricoh:", "samsung:", "xerox:", "lexmark:",
        "canon",    // canon_dr: / canon_pp:
    };
    for (const char *p : vendorPrefixes) {
        if (n.startsWith(QLatin1String(p)))
            return ScanBackend::Vendor;
    }

    // 3) 回退：看 SANE 上报的设备类型。
    //    注意不要仅凭 type 含 "virtual" 就判为 Escl——sane-test 之类的
    //    虚拟后端同样自称 virtual device，而它们的选项集与 eSCL 无关。
    //    真正的 escl 后端设备名是 URL（escl:http://...），据此区分。
    const QString t = type.toLower();
    if (t.contains(QLatin1String("virtual")) && n.contains(QLatin1String("http")))
        return ScanBackend::Escl;
    if (t.contains(QLatin1String("scanner")) || t.contains(QLatin1String("camera"))
        || t.contains(QLatin1String("film")))
        return ScanBackend::Vendor;

    // 判定不了就按 Unknown，扫描时采用最保守参数（不附加 --source），
    // 宁可少传选项，也不要触发 invalid option。
    return ScanBackend::Unknown;
}

ScanBackend ScannerManager::backend(int index) const
{
    if (index < 0 || index >= m_backends.size())
        return ScanBackend::Unknown;
    return m_backends.at(index);
}

void ScannerManager::discover()
{
    if (m_discoveryRunning || m_scanRunning)
        return;

    // SANE airscan 发现依赖 zeroconf 就绪，可能耗时 5~20 秒，
    // 必须在后台线程执行，避免冻结 UI
    m_model.setStringList({QObject::tr("正在发现扫描仪（需 10~20 秒，请稍候）…")});

    m_discoveryRunning = true;
    auto *watcher = new QFutureWatcher<ScanDeviceList>(this);
    connect(watcher, &QFutureWatcher<ScanDeviceList>::finished, this, [this, watcher]() {
        const ScanDeviceList found = watcher->result();
        m_discoveryRunning = false;
        watcher->deleteLater();

        m_devices = found.names;
        m_backends = found.backends;

        // 为每个设备探测扫描源（Flatbed/ADF），生成带标注的显示名。
        // 这是必要的：同一个物理设备的多个 eSCL 端点会列成多个设备，
        // 其中 ADF-only 的端点在无纸时会报 "Document feeder out of documents"，
        // 用户无法从原始名称区分，所以必须标注来源。
        m_deviceLabels.clear();
        for (const QString &dev : m_devices)
            m_deviceLabels << formatDeviceLabel(dev, optionValues(dev, "source"));

        if (m_devices.isEmpty()) {
            m_model.setStringList({QObject::tr(
                "未发现扫描仪：免驱扫描需设备支持 eSCL 且 ipp-usb 与 avahi-daemon 已启动；"
                "若使用厂商自带 SANE 驱动，请确认驱动已正确安装")});
        } else {
            m_model.setStringList(m_deviceLabels);
        }
        emit discoveryFinished(!m_devices.isEmpty());
    });
    watcher->setFuture(QtConcurrent::run([]() {
        ScanDeviceList found;
        sane_init(nullptr, nullptr);
        const SANE_Device **deviceList = nullptr;
        if (sane_get_devices(&deviceList, SANE_FALSE) == SANE_STATUS_GOOD) {
            for (int i = 0; deviceList[i]; ++i) {
                // 注意：SANE 设备名可含空格（如 "airscan:e0:Pantum BM4240ADW …"），
                // 必须完整保存，供 sane_open / scanimage -d 使用。
                //
                // 修正：部分 SANE 后端（如 escl）会把本地回环地址写成 IPv6 字面量
                // [::1]，例如 "escl:http://[::1]:60002/"；该 URI 无法被 sane_open
                // 解析，scanimage 报 "Invalid argument"。scanimage -L 实际显示为
                // "escl:http://localhost:60002/"，故此处统一规范化为 localhost。
                QString name = QString::fromLatin1(deviceList[i]->name);
                if (name.contains(QLatin1String("[::1]"))) {
                    name.replace(QLatin1String("[::1]"), QLatin1String("localhost"));
                }

                // SANE_Device::type 形如 "virtual device"（airscan/escl）或
                // "flatbed scanner"（厂商原生）。部分后端填得随意，
                // 故以设备名前缀为主、type 为辅判定。
                const QString type = QString::fromLatin1(
                    deviceList[i]->type ? deviceList[i]->type : "");

                found.names.append(name);
                found.backends.append(ScannerManager::classifyBackend(name, type));
            }
        }
        sane_exit();
        return found;
    }));
}

QString ScannerManager::formatDeviceLabel(const QString &dev, const QStringList &sources)
{
    if (sources.isEmpty())
        return dev;

    QStringList tags;
    for (const QString &raw : sources) {
        const QString s = raw.toLower();
        if (s == QLatin1String("flatbed"))
            tags << QObject::tr("Flatbed");
        else if (s == QLatin1String("adf") || s.contains(QLatin1String("document feeder")))
            tags << QObject::tr("ADF");
        else
            tags << raw;  // 未知源，保留原文
    }
    return dev + QStringLiteral(" [") + tags.join(QLatin1String(", ")) + QStringLiteral("]");
}

bool ScannerManager::hasDevices() const
{
    return !m_devices.isEmpty();
}

namespace {
struct ScanTaskResult {
    bool ok = false;
    QString outPath;
    QString error;
};
}

void ScannerManager::scan(int deviceIndex, const ScanOptions &opts)
{
    if (m_scanRunning || m_discoveryRunning) {
        emit scanFinished(false, QString(),
                          QObject::tr("已有扫描任务正在进行，请稍候。"));
        return;
    }

    const bool valid = deviceIndex >= 0 && deviceIndex < m_devices.size();
    if (!valid) {
        emit scanFinished(false, QString(),
                          QObject::tr("No available scanner. Click \"Discover scanners\" first."));
        return;
    }

    // 使用完整 SANE 设备名（含空格），不做切分
    const QString dev = m_devices.at(deviceIndex);
    const ScanBackend backend = this->backend(deviceIndex);

    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    const QString out = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                        + "/scan_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png";

    m_scanRunning = true;
    auto *watcher = new QFutureWatcher<ScanTaskResult>(this);
    connect(watcher, &QFutureWatcher<ScanTaskResult>::finished,
            this, [this, watcher]() {
        const ScanTaskResult result = watcher->result();
        m_scanRunning = false;
        watcher->deleteLater();
        emit scanFinished(result.ok, result.outPath, result.error);
    });
    watcher->setFuture(QtConcurrent::run([dev, backend, opts, out]() {
        // 参数能力探测同样可能阻塞数秒，必须和正式扫描一起放到工作线程。
        const QStringList modes = optionValues(dev, "mode");
        const QStringList sources = optionValues(dev, "source");

        QStringList args;
        args << "--format=png"
             << "--resolution" << QString::number(opts.resolution);

        // 色彩模式：设备支持才传；Lineart 不被支持时降级为 Gray。
        // 厂商原生驱动常见的取值是 "Color"/"Gray"，探测失败时干脆不传，
        // 交给设备自身的默认值，避免触发 invalid option。
        if (!modes.isEmpty()) {
            QString mode = opts.colorMode;
            if (!modes.contains(mode)) {
                if (mode == "Lineart" && modes.contains("Gray"))
                    mode = "Gray";
                else if (modes.contains("Color"))
                    mode = "Color";
                else
                    mode.clear();   // 无匹配，用设备默认
            }
            if (!mode.isEmpty())
                args << "--mode" << mode;
        }

        // 扫描来源：优先平板（预览场景），避免 ADF 缺纸报错。
        //
        // 仅对免驱 eSCL 设备传递：airscan/escl 的 source 选项名与取值固定
        // （Flatbed / ADF）。而厂商原生 SANE 驱动差异极大——部分机型没有
        // source 选项，或取值不同，强行传递会直接报 invalid option。
        if (backend == ScanBackend::Escl && sources.contains("Flatbed"))
            args << "--source" << "Flatbed";

        args << "-d" << dev;

        // 关键修复：不再使用 "-o <file>"。
        // scanimage 的 "-o" 并非所有后端都接受（HP hpaio 等厂商原生驱动
        // 会直接报 "invalid option -- 'o'"）。不指定输出文件时，scanimage
        // 会把图像写到标准输出，这是由 scanimage 前端统一处理的，
        // 对 eSCL 与厂商原生驱动一视同仁，因此改从 stdout 取回图像再落盘。
        QProcess proc;
        proc.start("scanimage", args);
        ScanTaskResult result;
        result.outPath = out;

        // 厂商原生驱动扫描较慢（部分机型预热就要几十秒），放宽到 3 分钟
        const bool finished = proc.waitForFinished(180000);
        const QByteArray png = proc.readAllStandardOutput();

        result.ok = finished
                    && proc.exitStatus() == QProcess::NormalExit
                    && proc.exitCode() == 0
                    && !png.isEmpty();

        result.error = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
        if (!finished && result.error.isEmpty())
            result.error = QObject::tr("扫描超时（3 分钟），设备可能未就绪或正在预热");

        if (result.ok) {
            QFile f(out);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(png);
                f.close();
            } else {
                result.ok = false;
                result.error = QObject::tr("无法写入扫描结果文件：") + out;
            }
        }

        // 对常见 ADF 缺纸错误做中文友好提示，避免用户看到一长串英文不知所措
        if (!result.ok && result.error.contains(QLatin1String("Document feeder out of documents"), Qt::CaseInsensitive)) {
            result.error = QObject::tr("扫描失败：所选设备为自动进纸器（ADF），请确认已放置纸张，\n"
                                       "或选择带 [Flatbed] 标注的平板扫描源。");
        }

        // 选项类错误的措辞在不同 sane-backends 版本里不一致，这里一并覆盖：
        //   "invalid option -- 'o'"          getopt 拒绝短选项（1.1+ 已移除 -o）
        //   "unrecognized option '--xxx'"    scanimage 不认识的长选项
        //   "setting of option --xxx failed" 选项存在但取值不被设备接受
        // 厂商原生 SANE 驱动的能力差异最大，此时给出明确指引，
        // 比让用户对着一段英文报错干瞪眼有用。
        if (!result.ok) {
            const bool optErr =
                result.error.contains(QLatin1String("invalid option"), Qt::CaseInsensitive)
                || result.error.contains(QLatin1String("unrecognized option"), Qt::CaseInsensitive)
                || result.error.contains(QLatin1String("unknown option"), Qt::CaseInsensitive)
                || result.error.contains(QLatin1String("setting of option"), Qt::CaseInsensitive);
            if (optErr) {
                result.error = QObject::tr("扫描失败：该设备不支持所选的扫描参数\n"
                                           "（设备使用厂商自带 SANE 驱动，可用选项与免驱设备不同）。\n"
                                           "请尝试更换分辨率或色彩模式，\n"
                                           "或改用厂商提供的扫描软件。");
            }
        }
        return result;
    }));
}
