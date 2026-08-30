#include "diagnostics.h"
#include "envchecker.h"

#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

Diagnostics::Diagnostics(QObject *parent)
    : QObject(parent)
{
}

QString Diagnostics::runReadOnly(const QString &program,
                                 const QStringList &args,
                                 int timeoutMs)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    if (proc.exitCode() != 0)
        return {};
    return QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
}

QString Diagnostics::serviceStatus(const QString &unit)
{
    const QString out = runReadOnly(QStringLiteral("systemctl"),
                                    {QStringLiteral("is-active"), unit}, 2000);
    return out.isEmpty() ? QStringLiteral("unknown") : out;
}

void Diagnostics::scan()
{
    m_report.clear();
    const QString hr = QStringLiteral("----------------------------------------");

    // 1) 底层服务状态
    m_report << hr
             << tr("1. 底层服务状态")
             << hr
             << QStringLiteral("ipp-usb       : %1").arg(serviceStatus(QStringLiteral("ipp-usb")))
             << QStringLiteral("cups          : %1").arg(serviceStatus(QStringLiteral("cups")))
             << QStringLiteral("avahi-daemon  : %1").arg(serviceStatus(QStringLiteral("avahi-daemon")))
             << QStringLiteral("saned         : %1").arg(serviceStatus(QStringLiteral("saned")))
             << QString();

    // 2) IPP-over-USB 候选设备
    // 复用 EnvChecker 的严格判定（≥2 个 7/1/4 接口 + IN/OUT 端点），
    // 与 ipp-usb 上游规则一致，避免把 Hub、键鼠列进来干扰判断。
    m_report << hr
             << tr("2. IPP-over-USB 候选设备")
             << hr;
    const QStringList candidates = EnvChecker::lsusbImagingDevices();
    if (candidates.isEmpty()) {
        m_report << tr("未检测到 IPP-over-USB 设备（设备可能不支持免驱，"
                       "或未被 ipp-usb 识别）");
    } else {
        for (const QString &dev : candidates)
            m_report << dev;
    }
    m_report << QString();

    // 3) ipp-usb 已接管的设备
    // 注意：不能用 "ipp-usb status"！上游 status.go 在多设备时
    // 解引用 nil 会 panic，直接把守护进程打死。check 是独立进程，安全。
    m_report << hr
             << tr("3. ipp-usb 已接管设备")
             << hr;
    const QString ippCheck = runReadOnly(QStringLiteral("ipp-usb"),
                                         {QStringLiteral("check")}, 15000);
    if (ippCheck.isEmpty()) {
        m_report << tr("ipp-usb check 无输出（服务未运行，或 ipp-usb 未安装）");
    } else {
        for (const QString &line : ippCheck.split('\n', Qt::SkipEmptyParts))
            m_report << line.trimmed();
    }
    m_report << QString();

    // 4) DNS-SD 广播：决定 CUPS 与 sane-airscan 能否发现设备
    m_report << hr
             << tr("4. DNS-SD 广播（打印 / 扫描）")
             << hr;
    const QString dnssd = runReadOnly(
        QStringLiteral("avahi-browse"),
        {QStringLiteral("-rt"), QStringLiteral("_ipp._tcp"),
         QStringLiteral("_uscan._tcp")}, 15000);
    if (dnssd.isEmpty()) {
        m_report << tr("未获取到 DNS-SD 广播（avahi-daemon 可能未运行）");
    } else {
        for (const QString &line : dnssd.split('\n', Qt::SkipEmptyParts)) {
            const QString t = line.trimmed();
            // 只保留服务名、地址与端口，丢掉 avahi 的冗长前缀
            if (t.startsWith(QLatin1String("hostname")) || t.startsWith(QLatin1String("address"))
                || t.startsWith(QLatin1String("port")) || t.contains(QLatin1String("IPv4")))
                m_report << t;
        }
    }
    m_report << QString();

    // 5) CUPS 打印队列
    m_report << hr
             << tr("5. CUPS 打印队列")
             << hr;
    const QString queues = runReadOnly(QStringLiteral("lpstat"),
                                       {QStringLiteral("-v")}, 5000);
    if (queues.isEmpty()) {
        m_report << tr("无已配置打印队列");
    } else {
        for (const QString &line : queues.split('\n', Qt::SkipEmptyParts))
            m_report << line.trimmed();
    }
    m_report << QString();

    // 6) SANE 扫描设备
    m_report << hr
             << tr("6. SANE 扫描设备")
             << hr;
    const QString sane = runReadOnly(QStringLiteral("scanimage"),
                                     {QStringLiteral("-L")}, 60000);
    if (sane.isEmpty()) {
        m_report << tr("未发现扫描仪（设备可能不支持 eSCL，"
                       "或 sane-airscan / avahi-daemon 未就位）");
    } else {
        for (const QString &line : sane.split('\n', Qt::SkipEmptyParts))
            m_report << line.trimmed();
    }
    m_report << QString();

    m_model.setStringList(m_report);
}

QString Diagnostics::exportReport()
{
    if (m_report.isEmpty())
        scan();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QStringLiteral("%1/ipp-usb-diag_%2.txt")
                             .arg(dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << tr("IPP-USB 免驱助手 诊断报告") << "\n";
    ts << tr("生成时间：") << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
    for (const QString &line : m_report)
        ts << line << "\n";
    f.close();

    return path;
}
