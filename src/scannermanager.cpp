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
    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
        const QString t = line.trimmed();
        if (!t.startsWith("--" + opt + ' '))
            continue;
        // 取 "Color|Gray|Lineart [Color]" 中括号前的值段
        QString vals = t.mid(opt.size() + 3);
        const int bracket = vals.indexOf('[');
        if (bracket >= 0)
            vals = vals.left(bracket);
        QStringList list;
        for (const QString &v : vals.split('|', Qt::SkipEmptyParts))
            list.append(v.trimmed());
        return list;
    }
    return {};
}

void ScannerManager::discover()
{
    if (m_discoveryRunning || m_scanRunning)
        return;

    // SANE airscan 发现依赖 zeroconf 就绪，可能耗时 5~20 秒，
    // 必须在后台线程执行，避免冻结 UI
    m_model.setStringList({QObject::tr("正在发现扫描仪（需 10~20 秒，请稍候）…")});

    m_discoveryRunning = true;
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher]() {
        const QStringList found = watcher->result();
        m_discoveryRunning = false;
        watcher->deleteLater();

        m_devices = found;

        // 为每个设备探测扫描源（Flatbed/ADF），生成带标注的显示名。
        // 这是必要的：同一个物理设备的多个 eSCL 端点会列成多个设备，
        // 其中 ADF-only 的端点在无纸时会报 "Document feeder out of documents"，
        // 用户无法从原始名称区分，所以必须标注来源。
        m_deviceLabels.clear();
        for (const QString &dev : m_devices)
            m_deviceLabels << formatDeviceLabel(dev, optionValues(dev, "source"));

        if (m_devices.isEmpty()) {
            m_model.setStringList({QObject::tr(
                "未发现扫描仪：请确认设备支持 eSCL，"
                "且 ipp-usb 与 avahi-daemon 均已启动")});
        } else {
            m_model.setStringList(m_deviceLabels);
        }
        emit discoveryFinished(!m_devices.isEmpty());
    });
    watcher->setFuture(QtConcurrent::run([]() {
        QStringList found;
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
                found.append(name);
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
    watcher->setFuture(QtConcurrent::run([dev, opts, out]() {
        // 参数能力探测同样可能阻塞数秒，必须和正式扫描一起放到工作线程。
        const QStringList modes = optionValues(dev, "mode");
        const QStringList sources = optionValues(dev, "source");

        QStringList args;
        args << "--format=png"
             << "--resolution" << QString::number(opts.resolution);

        // 色彩模式：设备支持才传；Lineart 不被支持时降级为 Gray
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

        // 扫描来源：优先平板（预览场景），避免 ADF 缺纸报错
        if (sources.contains("Flatbed"))
            args << "--source" << "Flatbed";

        args << "-d" << dev << "-o" << out;

        QProcess proc;
        proc.start("scanimage", args);
        ScanTaskResult result;
        result.outPath = out;
        result.ok = proc.waitForFinished(120000)
                    && proc.exitStatus() == QProcess::NormalExit
                    && proc.exitCode() == 0;

        result.error = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
        if (result.ok && !QFileInfo::exists(out)) {
            result.ok = false;
            result.error = QObject::tr("scanimage 已正常退出，但未生成输出文件");
        }

        // 对常见 ADF 缺纸错误做中文友好提示，避免用户看到一长串英文不知所措
        if (!result.ok && result.error.contains(QLatin1String("Document feeder out of documents"), Qt::CaseInsensitive)) {
            result.error = QObject::tr("扫描失败：所选设备为自动进纸器（ADF），请确认已放置纸张，\n"
                                       "或选择带 [Flatbed] 标注的平板扫描源。");
        }
        return result;
    }));
}
