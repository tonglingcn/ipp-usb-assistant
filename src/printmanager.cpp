#include "printmanager.h"
#include "privileges.h"

#include <QProcess>
#include <QThread>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <cups/cups.h>
#include "qtcompat.h"

PrintManager::PrintManager(QObject *parent)
    : QObject(parent)
{
}

QString PrintManager::queuePpdPath(const QString &queue)
{
    return "/etc/cups/ppd/" + queue + ".ppd";
}

QString PrinterEntry::detectProtocol(const QString &uri)
{
    if (uri.startsWith("ipps://")) return QStringLiteral("ipps");
    if (uri.startsWith("ipp://"))  return QStringLiteral("ipp");
    if (uri.startsWith("dnssd://")) return QStringLiteral("dnssd");
    if (uri.startsWith("socket://")) return QStringLiteral("socket");
    if (uri.startsWith("lpd://"))   return QStringLiteral("lpd");
    if (uri.startsWith("usb://"))   return QStringLiteral("usb");
    return QStringLiteral("ipp");
}

bool PrinterEntry::sameUri(const QString &a, const QString &b)
{
    auto stripProto = [](QString s) {
        if (s.startsWith("ipps://")) s = s.mid(7);
        else if (s.startsWith("ipp://")) s = s.mid(6);
        else if (s.startsWith("dnssd://")) s = s.mid(8);
        // 去掉尾部端口与可能的端口差异，保留 host+path+query
        QString base = s.section('?', 0, 0);
        base = base.section('#', 0, 0);
        return base.toLower();
    };
    return stripProto(a) == stripProto(b);
}

bool PrinterEntry::isIppUri(const QString &uri)
{
    if (uri.startsWith("ipp://") || uri.startsWith("ipps://"))
        return true;
    if (uri.startsWith("dnssd://")) {
        // dnssd 可能广告多种服务：只有 _ipp._tcp / _ipps._tcp 才是 IPP 免驱；
        // _pdl-datastream._tcp / _printer._tcp / _fax._tcp 等是原始套接字/LPD，
        // driverless 无法为其生成 PPD，会导致 lpadmin 30 秒超时。
        const QString u = uri.toLower();
        return u.contains("_ipp._tcp") || u.contains("_ipps._tcp");
    }
    return false;
}

static int ippUriRank(const QString &uri)
{
    // 排名越高越优先被保留为 device-uri。
    // 优先选择 ipp://（明文），原因：
    //   1) 很多国产/新款打印机的 TLS 证书配置不规范（EKU/serverAuth 缺失、
    //      自签名或主机名不匹配），使用 ipps:// 会导致 CUPS/OpenSSL 报
    //      "Key usage violation in certificate" 而添加失败。
    //   2) 内网打印通常无需 TLS；需要加密时用户仍可手动选择 ipps 条目。
    //   3) dnssd _ipp._tcp / _ipps._tcp 作为兜底保留。
    if (uri.startsWith("ipp://"))  return 4;
    if (uri.startsWith("ipps://")) return 3;
    if (uri.startsWith("dnssd://")) {
        const QString u = uri.toLower();
        if (u.contains("_ipp._tcp"))  return 2;
        if (u.contains("_ipps._tcp")) return 1;
    }
    return 0;
}

static bool looksLikeProtocolStub(const QString &uri)
{
    // lpinfo -v 会输出两类占位：
    //   1) 纯协议名："network ipp" / "network socket" / "network lpd" 等，无冒号。
    //   2) 无实际设备的后端："cups-brf:/" / "serial:" / "usb:" / "parallel:" / "socket:" 等。
    // 本应用只面向 IPP-USB / IPP Everywhere 免驱设备，因此只保留带 "://" 真实地址的 URI。
    const QString s = uri.trimmed();
    if (s.isEmpty() || !s.contains("://"))
        return true;

    // scheme 仍做白名单校验，防止 cups-brf:/file:// 等异常后端混入
    static const char *ACCEPTED[] = {
        "ipp", "ipps", "http", "https", "socket", "lpd", "smb",
        "dnssd", "usb", "file"
    };
    const QString scheme = s.section(':', 0, 0).trimmed().toLower();
    for (const char *a : ACCEPTED) {
        if (scheme == QLatin1String(a))
            return false;
    }
    return true;
}

QString PrintManager::prettyNameFromUri(const QString &uri)
{
    QString s = uri;
    if (s.startsWith("dnssd://"))
        s = s.mid(8);
    else if (s.startsWith("ipps://"))
        s = s.mid(7);
    else if (s.startsWith("ipp://"))
        s = s.mid(6);

    s = QUrl::fromPercentEncoding(s.toUtf8());
    s.remove("._ipp._tcp.local");
    s.remove("._ipps._tcp.local");
    s.remove(" (USB)");
    s = s.section('/', 0, 0);
    s = s.section('?', 0, 0);
    s.replace('.', ' ');
    s.replace('-', ' ');
    s = s.simplified();
    return s.isEmpty() ? uri : s;
}

QString PrintManager::sanitizeQueueName(const QString &s)
{
    QString base = s;
    // CUPS 队列名允许字母、数字、连字符、下划线、点。其他字符替换为空格，
    // 再合并多空格，最后转为连字符，得到如 "Pantum-BM4240ADW-Series"。
    base.replace(QRegularExpression("[^\\w-]"), " ");
    base = base.split(' ', kSkipEmptyParts).join(" ");
    if (base.isEmpty())
        base = "printer";
    base.replace(' ', '-');
    return base.left(127);
}

QString PrintManager::makeDefaultName(const QString &uri, const QString &realModel)
{
    // 优先用真实品牌型号派生队列名（更直观，与"已配置队列"列表主标题一致）。
    // 缺失或清洗后为空时，回退到从 URI 派生。
    QString base = sanitizeQueueName(realModel.isEmpty() ? prettyNameFromUri(uri) : realModel);
    if (base == QStringLiteral("printer") && !realModel.isEmpty()) {
        // 清洗后恰好等于默认名，说明 realModel 全是 URI 风格字符（如 URI 后半段），
        // 此时 URI 派生更靠谱，回退。
        base = sanitizeQueueName(prettyNameFromUri(uri));
    }

    cups_dest_t *dests = nullptr;
    const int numDests = cupsGetDests(&dests);
    QSet<QString> existing;
    for (int i = 0; i < numDests; ++i)
        existing.insert(QString::fromLocal8Bit(dests[i].name));
    cupsFreeDests(numDests, dests);

    QString candidate = base;
    for (int i = 1; existing.contains(candidate); ++i)
        candidate = base + "-" + QString::number(i);
    return candidate.left(127);
}

// 解析 driverless 输出。需要兼容至少两种输出格式：
//   新（>= cups-filters 1.28）：双引号包围的 5 字段：
//     "driverless:ipp://host/ipp/print"  lang  "Pantum"  "Pantum BM4240ADW Series, driverless, cups-filters 1.21.6"  "MFG:...;MDL:..."
//   旧：空格分隔单 URI + 描述：
//     ipp://host/ipp/print  Pantum BM4240ADW Series
// 本函数只负责匹配 URI，并尽量从描述里剥离出干净的型号名。
static QString reverseLookupMakeModel(const QString &uri, const QString &dlList)
{
    // 使用预取的 `driverless list` 输出，避免每台设备重复起进程（8 秒超时会叠加）
    if (dlList.isEmpty())
        return {};

    auto strip = [](QString s) {
        if (s.startsWith("ipps://")) s = s.mid(7);
        else if (s.startsWith("ipp://")) s = s.mid(6);
        else if (s.startsWith("dnssd://")) s = s.mid(8);
        return s.section('?', 0, 0).section('#', 0, 0).toLower();
    };
    const QString target = strip(uri);

    // 先按新格式匹配。实际样本（cups-filters 1.21.6）：
//   "driverless:ipp://localhost:60000/ipp/print" en "Pantum" "Pantum BM4240ADW Series, driverless, cups-filters 1.21.6" "MFG:Pantum;MDL:..."
// 注意：lang 字段（"en"）是**裸文本不带引号**。其他 4 段是带引号。
// 字段顺序：URI · lang · make · description · attributes。URI 可能有 "driverless:" 前缀。
// 注意：QStringLiteral 不能与 R"(...)" 嵌套（宏内的引号会被 raw string 提前闭合），
// 因此这里用普通字符串字面量 + 双反斜杠表达正则反斜杠。
    static const QRegularExpression kQuoted(
        QStringLiteral("\"([^\"]+)\"[ \\t]+(?:\"([^\"]*)\"|(\\S+))[ \\t]+\"([^\"]*)\"[ \\t]+\"([^\"]*)\""));
    // 旧格式作为兜底。
    static const QRegularExpression kPlain(
        QStringLiteral("^(?:driverless:)?([a-z]+://\\S+)\\s+(.+)$"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString &raw : dlList.split('\n', kSkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("DEBUG")))
            continue;

        QString cand, model;
        auto m = kQuoted.match(line);
        if (m.hasMatch()) {
            // 新格式（5 字段：URI · lang · make · description · attributes）
            // 捕获组 1=URI  2=lang(quoted) 3=lang(unquoted) 4=make 5=description
            QString u = m.captured(1);
            if (u.startsWith(QStringLiteral("driverless:")))
                u = u.mid(10);
            cand = u;
            QString desc = m.captured(5);
            // 描述形如 "Pantum BM4240ADW Series, driverless, cups-filters 1.21.6"
            // 去掉 ", driverless, cups-filters ..." 后缀
            const int idx = desc.indexOf(QStringLiteral(", driverless,"));
            if (idx >= 0)
                desc = desc.left(idx).trimmed();
            model = desc;
        } else {
            // 旧格式
            m = kPlain.match(line);
            if (!m.hasMatch())
                continue;
            cand = m.captured(1);
            QString desc = m.captured(2).trimmed();
            const int idx = desc.indexOf(QStringLiteral(", driverless,"));
            if (idx >= 0)
                desc = desc.left(idx).trimmed();
            model = desc;
        }

        if (strip(cand) == target)
            return model;
    }
    return {};
}

// 兜底：通过 IPP 协议直接问设备本身的 printer-make-and-model 属性。
// 这是最权威的来源，但每台设备都需要起一个 ipptool 进程（~200ms）。
// 只在 driverless 反查失败时调用，避免给已经走得通的设备增加延迟。
//
// 注意：ipptool 必须从文件读测试定义（不支持 stdin），且默认输出格式是测试报告
// （"    xxx (xxx) = value"）。这里走 -tv 模式产生单行 key=value。
static QString fetchMakeModelViaIpp(const QString &uri)
{
    static const QRegularExpression kModelLine(
        QStringLiteral("printer-make-and-model\\s+\\(textWithoutLanguage\\)\\s*=\\s*(.+)$"),
        QRegularExpression::MultilineOption);

    // 写测试定义到临时文件
    QTemporaryFile tmp(QStringLiteral("/tmp/ippusb-assistant-XXXXXX.printer"));
    tmp.setAutoRemove(true);
    if (!tmp.open())
        return {};
    tmp.write(QStringLiteral(
        "{\n"
        "  OPERATION Get-Printer-Attributes\n"
        "  GROUP operation-attributes-tag\n"
        "  ATTR charset attributes-charset utf-8\n"
        "  ATTR naturalLanguage attributes-natural-language en\n"
        "  ATTR uri printer-uri $uri\n"
        "  ATTR keyword requested-attributes printer-make-and-model\n"
        "}\n").toUtf8());
    tmp.close();

    QProcess p;
    // -tv 给出测试报告，printer-make-and-model 会作为单行 key=value 出现
    p.start(QStringLiteral("ipptool"),
            {QStringLiteral("-tv"), uri, tmp.fileName()});
    if (!p.waitForFinished(6000))
        return {};
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    auto m = kModelLine.match(out);
    if (!m.hasMatch())
        return {};
    QString name = m.captured(1).trimmed();
    // 防御：噪声行匹配
    if (name.isEmpty() || name.contains(QLatin1Char(' ')) == false)
        return {};
    return name;
}

// 取一次 `driverless list` 输出并缓存（反查型号 + 免驱判定共用），
// 替代逐设备的 `driverless -d` 探测（每台最长阻塞 4 秒，叠加严重）。
static QString fetchDriverlessList()
{
    QProcess p;
    p.start("driverless", {"list"});
    if (!p.waitForFinished(8000))
        return {};
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

// 通过 `driverless list` 成员关系判定免驱能力（无额外进程开销）。
// 同 reverseLookupMakeModel：必须兼容 driverless list 的带引号新格式
// （"driverless:ipp://..."），否则整个列表都会被跳过、everyone 全 false。
static bool inDriverlessList(const QString &uri, const QString &dlList)
{
    if (dlList.isEmpty())
        return false;
    auto strip = [](QString s) {
        if (s.startsWith("ipps://")) s = s.mid(7);
        else if (s.startsWith("ipp://")) s = s.mid(6);
        else if (s.startsWith("dnssd://")) s = s.mid(8);
        return s.section('?', 0, 0).section('#', 0, 0).toLower();
    };
    const QString target = strip(uri);

    // 既要兼容新格式（"driverless:ipp://..."），也要兼容旧格式（ipp://...<空格>...）
    // 不同 cups-filters 版本在 URI 后可能多一个 lang 字段（"en"），因此后续字段都放宽。
    static const QRegularExpression kQuoted(
        QStringLiteral("\"driverless:([^\"]+)\"\\s+(?:\"[^\"]*\"\\s+)?\""));
    static const QRegularExpression kPlain(
        QStringLiteral("^(?:driverless:)?([a-z]+://\\S+)"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString &raw : dlList.split('\n', kSkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("DEBUG")))
            continue;
        QString cand;
        auto m = kQuoted.match(line);
        if (m.hasMatch()) {
            cand = m.captured(1);
        } else {
            m = kPlain.match(line);
            if (!m.hasMatch())
                continue;
            cand = m.captured(1);
        }
        if (strip(cand) == target)
            return true;
    }
    return false;
}

void PrintManager::enrichFromDriverless(QList<PrinterEntry> &list, const QString &dlList)
{
    for (PrinterEntry &e : list) {
        if (!e.makeAndModel.isEmpty()) continue;
        // 先用 driverless 反查；只有 ipptool 适用的 IPP 设备才能继续兜底。
        QString m = reverseLookupMakeModel(e.uri, dlList);
        if (m.isEmpty() && PrinterEntry::isIppUri(e.uri))
            m = fetchMakeModelViaIpp(e.uri);
        if (!m.isEmpty())
            e.makeAndModel = m;
        if (e.makeAndModel.isEmpty())
            e.makeAndModel = prettyNameFromUri(e.uri);
        // 标记是否支持 IPP Everywhere（免驱即打即驱）
        if (!e.everywhere)
            e.everywhere = inDriverlessList(e.uri, dlList);
    }
}

QList<PrinterEntry> PrintManager::mergeByMakeModel(QList<PrinterEntry> raw)
{
    // 同 make+model 但协议不同的条目合并为一条
    // 注意：保留首个 URI 作 device-uri，其余 URI 协议累加进 protocols，但不拼接。
    QHash<QString, int> indexByKey;
    QList<PrinterEntry> out;
    for (const PrinterEntry &e : raw) {
        const QString key = e.makeAndModel.toLower();
        if (!indexByKey.contains(key)) {
            indexByKey.insert(key, out.size());
            out.append(e);
        }
        PrinterEntry &back = out[indexByKey[key]];
        // 仅累加协议，不拼接 URI
        const QString p = PrinterEntry::detectProtocol(e.uri);
        if (!back.protocols.contains(p))
            back.protocols.append(p);
        if (back.location.isEmpty())
            back.location = e.location;
        back.everywhere = back.everywhere || e.everywhere;
        // 优先保留真实 IPP URI：ipps > ipp > dnssd(_ipp._tcp)，避免 socket/lpd
        // 等非 IPP 地址被保留为 device-uri 导致 driverless 超时。
        if (back.uri.isEmpty() || ippUriRank(e.uri) > ippUriRank(back.uri))
            back.uri = e.uri;
    }
    return out;
}

static QString normalizeUriForLpadmin(const QString &uri)
{
    // driverless / lpinfo -v 输出的 mDNS URI 形如：
    //   ipp://Pantum%20BM4240ADW%20Series%20A3024A%20(USB)._ipp._tcp.local/
    // 注意："(USB)" 是 mDNS 服务实例名的一部分，删掉会导致
    //   lpadmin 报 "Couldn't resolve mDNS URI"。
    // 因此这里只做空白裁剪，保留 URI 原样（含 percent-encoding 与尾部斜杠）。
    return uri.trimmed();
}

void PrintManager::discover()
{
    if (m_discoveryRunning) {
        emit discoveryFinished();
        return;
    }

    m_discoveryRunning = true;
    m_model.setStringList({tr("正在发现打印机…")});

    // 两段式发现（对齐系统打印管理器的秒级体验）：
    //   阶段 1（1~3 秒）：CUPS 队列 + lpinfo -v，立即出列表
    //   阶段 2（10~20 秒）：driverless mDNS 扫描，完成后合并免驱设备再刷新
    using StageOneResult = QPair<QList<PrinterEntry>, QList<PrinterEntry>>;
    auto *stageOneWatcher = new QFutureWatcher<StageOneResult>(this);
    connect(stageOneWatcher, &QFutureWatcher<StageOneResult>::finished,
            this, [this, stageOneWatcher]() {
        const StageOneResult result = stageOneWatcher->result();
        stageOneWatcher->deleteLater();

        QList<PrinterEntry> stage1Raw = result.first;
        // 先生成稳定的临时型号再合并，避免多个空 makeAndModel 被合成一台设备。
        for (PrinterEntry &e : stage1Raw) {
            if (e.makeAndModel.isEmpty())
                e.makeAndModel = prettyNameFromUri(e.uri);
        }
        QList<PrinterEntry> stage1 = mergeByMakeModel(stage1Raw);
        for (PrinterEntry &e : stage1) {
            e.title = e.makeAndModel;
            e.subtitle = QString(tr("免驱待添加  ·  %1")).arg(e.protocols.join("/"));
        }
        publishDiscovery(stage1, result.second);

        // 阶段 2 继续在独立后台任务中做较慢的 driverless/mDNS 扫描。
        auto *stageTwoWatcher = new QFutureWatcher<StageOneResult>(this);
        connect(stageTwoWatcher, &QFutureWatcher<StageOneResult>::finished,
                this, [this, stageTwoWatcher]() {
            const StageOneResult finalResult = stageTwoWatcher->result();
            stageTwoWatcher->deleteLater();
            m_discoveryRunning = false;
            publishDiscovery(finalResult.first, finalResult.second);
        });
        stageTwoWatcher->setFuture(QtConcurrent::run([raw = result.first]() mutable {
            QList<PrinterEntry> raw2;
            QProcess drv;
            drv.start("driverless", QStringList());
            if (drv.waitForFinished(15000)) {
                const QString out = QString::fromLocal8Bit(drv.readAllStandardOutput());
                for (const QString line : out.split('\n', kSkipEmptyParts)) {
                    const QString s = line.trimmed();
                    if (!s.startsWith("ipp://") && !s.startsWith("ipps://")
                        && !s.startsWith("dnssd://"))
                        continue;
                    QString uri = s;
                    QString desc;
                    const int sp = s.indexOf(' ');
                    if (sp > 0) {
                        uri = s.left(sp);
                        desc = s.mid(sp + 1).trimmed();
                    }
                    uri = normalizeUriForLpadmin(uri);
                    PrinterEntry e;
                    e.uri = uri;
                    // 优先用 driverless 给出的真实型号生成队列名（如
                    // "Pantum-BM4240ADW-Series"），"已配置队列"列表主标题就能直接显示品牌。
                    e.name = makeDefaultName(uri, desc);
                    e.makeAndModel = desc;
                    e.protocols = QStringList{ PrinterEntry::detectProtocol(uri) };
                    e.everywhere = true;
                    raw2.append(e);
                }
            }

            raw.append(raw2);
            const QString dlList = fetchDriverlessList();
            enrichFromDriverless(raw, dlList);
            QList<PrinterEntry> discovered = mergeByMakeModel(raw);
            for (PrinterEntry &e : discovered) {
                e.title = e.makeAndModel;
                e.subtitle = QString(PrintManager::tr("免驱待添加  ·  %1"))
                                 .arg(e.protocols.join("/"));
            }
            return qMakePair(discovered, readCupsQueues());
        }));
    });
    stageOneWatcher->setFuture(QtConcurrent::run([]() -> StageOneResult {
        // ---------- 阶段 1：快速发现 ----------
        QList<PrinterEntry> raw;

        // lpinfo -v 枚举 network/usb 后端（秒级）
        QProcess lpinfoV;
        lpinfoV.start("lpinfo", {"-v"});
        if (lpinfoV.waitForFinished(10000)) {
            const QString out = QString::fromLocal8Bit(lpinfoV.readAllStandardOutput());
            for (const QString line : out.split('\n', kSkipEmptyParts)) {
                QString s = line.trimmed();
                // 形如 "network socket://192.168.1.10:9100" 或 "direct usb://..."
                const int sp = s.indexOf(' ');
                if (sp <= 0) continue;
                s = s.mid(sp + 1).trimmed();   // device uri
                if (s.isEmpty() || looksLikeProtocolStub(s)) continue;
                // 只保留真正支持 IPP 的地址；_pdl-datastream._tcp 等原始服务
                // driverless 无法生成 PPD，会导致添加时 30 秒超时。
                if (!PrinterEntry::isIppUri(s)) continue;
                PrinterEntry e;
                e.uri = normalizeUriForLpadmin(s);
                e.name = makeDefaultName(e.uri);
                e.makeAndModel.clear();
                e.protocols = QStringList{ PrinterEntry::detectProtocol(e.uri) };
                raw.append(e);
            }
        }
        return qMakePair(raw, readCupsQueues());
    }));
}

void PrintManager::publishDiscovery(const QList<PrinterEntry> &discovered,
                                    const QList<PrinterEntry> &queues)
{
    QList<PrinterEntry> pending;
    for (const PrinterEntry &e : discovered) {
        bool matched = false;
        for (const PrinterEntry &q : queues) {
            if (PrinterEntry::sameUri(e.uri, q.uri)
                || (e.makeAndModel == q.makeAndModel && !e.makeAndModel.isEmpty())) {
                matched = true;
                break;
            }
        }
        if (!matched)
            pending.append(e);
    }

    m_printers = queues;
    m_printers.append(pending);
    QStringList display;
    for (const auto &p : std::as_const(m_printers))
        display.append(p.title + "  |  " + p.subtitle);
    m_model.setStringList(display);
    emit discoveryFinished();
}

QList<PrinterEntry> PrintManager::readCupsQueues()
{
    QList<PrinterEntry> queues;
    cups_dest_t *dests = nullptr;
    int numDests = cupsGetDests(&dests);
    const QString defaultQueue = QString::fromLocal8Bit(cupsGetDefault());
    for (int i = 0; i < numDests; ++i) {
        PrinterEntry e;
        e.name = QString::fromLocal8Bit(dests[i].name);
        // 取 device-uri（option "device-uri"）
        QString devUri;
        for (int j = 0; j < dests[i].num_options; ++j) {
            if (qstrcmp(dests[i].options[j].name, "device-uri") == 0) {
                devUri = QString::fromLocal8Bit(dests[i].options[j].value);
                break;
            }
        }
        e.uri = devUri;
        e.protocols = QStringList{ PrinterEntry::detectProtocol(devUri) };
        e.isQueue = true;
        e.ppdPath = queuePpdPath(e.name);
        e.ppdMakeModel = ppdManufacturer(e.name);
        if (e.makeAndModel.isEmpty()) e.makeAndModel = e.ppdMakeModel;
        if (e.makeAndModel.isEmpty()) e.makeAndModel = e.name;
        e.isDefault = (e.name == defaultQueue);
        e.title = e.name;
        const QString mark = e.isDefault ? tr("  ·  默认") : QString();
        e.subtitle = QString(tr("已配置队列%1  ·  %2")).arg(mark,
            e.protocols.join("/"));
        queues.append(e);
    }
    cupsFreeDests(numDests, dests);

    // 一次性跑 lpstat -l -p 把每台队列的 printer-info（Description）拿到，
    // 覆盖 PPD 默认的 "Printer - IPP Everywhere"——这样"已配置队列"的副标题
    // 与 driverless 待添加项就能显示同一个品牌型号。
    //
    // 中文 lpstat -l 段落形如：
    //   打印机 Pantum-BM4240ADW-Series ... 描述: Pantum BM4240ADW Series ...
    // 这里以队列名为锚点切分段落，再在该段落里匹配 "描述：".
    QProcess lpstat;
    lpstat.start("lpstat", {"-l", "-p"});
    if (lpstat.waitForFinished(8000)) {
        const QString out = QString::fromLocal8Bit(lpstat.readAllStandardOutput());
        static const QRegularExpression rxDes(
            QStringLiteral("描述[\\s:]+\"([^\"]+)\""),
            QRegularExpression::CaseInsensitiveOption);
        for (PrinterEntry &e : queues) {
            const int block = out.indexOf(e.name);
            if (block < 0)
                continue;
            const int from = block + e.name.length();
            // 段落边界：下一个队列的 "打印机 " 起始；找不到则取全文末尾
            const int next = out.indexOf(QStringLiteral("打印机 "), from);
            const int end = next < 0 ? out.length() : next;
            const auto m = rxDes.match(out.mid(from, end - from));
            if (m.hasMatch()) {
                const QString d = m.captured(1).trimmed();
                if (!d.isEmpty() && d != e.name) {
                    e.ppdMakeModel = d;
                    if (e.makeAndModel.isEmpty())
                        e.makeAndModel = d;
                }
            }
        }
    }

    return queues;
}

void PrintManager::refreshQueuesOnly()
{
    if (m_queueRefreshRunning) {
        emit discoveryFinished();
        return;
    }

    m_queueRefreshRunning = true;
    const QList<PrinterEntry> previous = m_printers;
    auto *watcher = new QFutureWatcher<QList<PrinterEntry>>(this);
    connect(watcher, &QFutureWatcher<QList<PrinterEntry>>::finished,
            this, [this, watcher]() {
        m_printers = watcher->result();
        m_queueRefreshRunning = false;
        watcher->deleteLater();
        QStringList display;
        for (const auto &p : std::as_const(m_printers))
            display.append(p.title + "  |  " + p.subtitle);
        m_model.setStringList(display);
        emit discoveryFinished();
    });
    watcher->setFuture(QtConcurrent::run([previous]() {
        const QList<PrinterEntry> queues = readCupsQueues();

        // 保留尚未配置的待添加设备（同 URI/同型号的移除，已被队列覆盖）
        QList<PrinterEntry> pending;
        for (const PrinterEntry &e : previous) {
            if (e.isQueue) continue;
            bool matched = false;
            for (const PrinterEntry &q : std::as_const(queues)) {
                if (PrinterEntry::sameUri(e.uri, q.uri)
                    || (e.makeAndModel == q.makeAndModel
                        && !e.makeAndModel.isEmpty())) {
                    matched = true;
                    break;
                }
            }
            if (!matched) pending.append(e);
        }

        QList<PrinterEntry> all;
        all.append(queues);
        all.append(pending);

        return all;
    }));
}

QString PrintManager::ppdManufacturer(const QString &queue)
{
    QFile f(queuePpdPath(queue));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    QString manu, model;
    static const QRegularExpression quoted(R"re("([^"]+)")re");
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        // PPD 行形如：*Manufacturer: "Pantum"  /  *NickName: "Pantum BM4240ADW"
        if (line.startsWith("*Manufacturer:")) {
            const auto m = quoted.match(line);
            if (m.hasMatch()) manu = m.captured(1);
        } else if (line.startsWith("*NickName:")) {
            const auto m = quoted.match(line);
            if (m.hasMatch()) model = m.captured(1);
        }
        if (!manu.isEmpty() && !model.isEmpty()) break;
    }
    if (!model.isEmpty()) return model;
    if (!manu.isEmpty()) return manu;
    return {};
}

QString PrintManager::ppdModel(const QString &queue)
{
    return ppdManufacturer(queue);
}

QString PrintManager::ppdDriverPath(const QString &queue)
{
    return queuePpdPath(queue);
}

// 等待 CUPS 为队列生成好 PPD 文件。
//
// 关键陷阱：lpadmin 创建队列之后，PPD 是由 cupsd 异步生成的。在 PPD 落盘之前
// 调用 "lpadmin -p NAME -o PageSize=A4" 会返回退出码 0（看起来成功），但随后
// 生成的 PPD 会用它自己的默认值把这次设置覆盖掉——默认纸张退回 Letter。
// 系统自带的打印管理器读的正是 PPD 里的 *DefaultPageSize，于是那边仍显示 Letter，
// 而本应用若从别处读值，两边就此永久分叉。
//
// 探测方式用 "lpoptions -p <queue> -l"：PPD 未就绪时 CUPS 会返回
// "无法为 <queue> 获取 PPD 文件"，就绪后才会列出各选项（含 PageSize）。
// 这样既不依赖已废弃的 cupsGetPPD()（CUPS 1.6 起标记 deprecated），
// 也不需要硬编码具体的 ppd 目录配置。
static bool waitForPpdReady(const QString &name, int timeoutMs = 8000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QProcess proc;
        proc.start(QStringLiteral("lpoptions"),
                   {QStringLiteral("-p"), name, QStringLiteral("-l")});
        if (proc.waitForFinished(3000)) {
            const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
            if (out.contains(QStringLiteral("PageSize")))
                return true;
        }
        QThread::msleep(150);
    }
    return false;
}

bool PrintManager::addPrinter(const QString &name, const QString &uri,
                              const QString &driver, const QString &prettyName,
                              QString &errMsg)
{
    if (name.isEmpty() || uri.isEmpty()) {
        errMsg = tr("队列名与 driverless URI 均不能为空");
        return false;
    }
    if (!PrinterEntry::isIppUri(uri)) {
        errMsg = tr("该设备不支持 IPP 免驱协议（可能为原始打印端口），请选择 _ipp._tcp / _ipps._tcp 服务");
        return false;
    }

    // 很多新款网络打印机只广播 ipps://，但 TLS 证书配置不规范会导致添加失败。
    // 这里优先尝试构造对应的 ipp:// 明文地址（同一主机+路径，端口改为 631），
    // 失败再回退到原始 ipps://，让用户在无证书问题的路径下添加成功。
    QStringList uriCandidates;
    if (uri.startsWith("ipps://")) {
        QString ippUri = uri;
        ippUri.replace(0, 5, "ipp");   // ipps:// -> ipp://
        QUrl u(ippUri);
        if (u.port() == 443 || u.port() == -1)
            u.setPort(631);
        uriCandidates << u.toString();
    }
    uriCandidates << uri;

    // lpadmin / cupsaccept / cupsenable 属 CUPS 队列管理操作，权限由
    // Privileges 统一判定：在 lpadmin 组时直跑不弹授权框，不在组里则自动
    // 走 pkexec。此前这里固定不提权，导致非 lpadmin 用户添加打印机必然失败。
    auto run = [&](const QStringList &args, QString &out, QString &err) -> int {
        const int code = Privileges::run(args, Privileges::Elevation::Auto, &out, &err, 30000);
        if (code == -1 && err.isEmpty())
            err = tr("操作超时（30 秒）");
        return code;
    };

    QString out, err;

    // 驱动匹配优先级（对齐 dde-printer 的 getDriverSolutions 决策）：
    //  1) 用户手动指定的本地 PPD / 专属 driverless
    //  2) 设备专属 driverless（driverless:<uri>，最匹配当前设备）
    //  3) IPP Everywhere 通用驱动（everywhere，最稳）
    //
    // 注意：dnssd:// URI（mDNS 服务名）使用 driverless:<uri> 时，
    // cups-browsed/driverless 常需二次解析 mDNS 并校验 TLS，容易因
    // 证书问题失败。system-config-printer 对 dnssd 设备默认用 everywhere
    // 驱动，经验证更稳。因此 dnssd:// 优先 everywhere，再兜底 driverless。
    auto buildPrimary = [&](const QString &u) {
        QStringList list;
        if (!driver.isEmpty())
            list << driver;
        if (u.startsWith("dnssd://")) {
            list << "everywhere";
            list << (QString("driverless:%1").arg(u));
        } else {
            list << (QString("driverless:%1").arg(u));
            list << "everywhere";
        }
        return list;
    };

    // 设备忙（如正在扫描/预热）时 PPD 生成会返回空文件而失败，
    // 每条 URI 自动重试一次，间隔 2 秒等待设备空闲
    QString lastErr;
    int code = -1;
    QString lastUri;
    for (const QString &u : uriCandidates) {
        lastUri = u;
        const QStringList primary = buildPrimary(u);
        int round = 0;
        for (; round < 2 && code != 0; ++round) {
            if (round > 0)
                QThread::sleep(2);
            for (const QString &m : primary) {
                code = run({"lpadmin", "-p", name, "-v", u, "-m", m, "-E"}, out, err);
                if (code == 0) break;
                lastErr = err;
                if (lastErr.isEmpty()) lastErr = out;
            }
        }
        if (code == 0) break;
    }
    if (code != 0) {
        errMsg = lastErr;
        if (errMsg.isEmpty()) {
            errMsg = tr("lpadmin 失败，请确认当前用户在 lpadmin 组");
        } else if (lastUri.startsWith("ipps://") &&
                   (errMsg.contains("Key usage violation", Qt::CaseInsensitive)
                    || errMsg.contains("certificate", Qt::CaseInsensitive)
                    || errMsg.contains("TLS", Qt::CaseInsensitive)
                    || errMsg.contains("SSL", Qt::CaseInsensitive))) {
            errMsg += QStringLiteral("\n") + tr("打印机 TLS/SSL 证书存在问题，已尝试 ipp:// 和 ipps:// 两种协议均失败。请检查打印机网络设置、关闭 IPPS，或手动输入 IP 地址添加。");
        } else if (errMsg.contains("Key usage violation", Qt::CaseInsensitive)
                   || errMsg.contains("certificate", Qt::CaseInsensitive)
                   || errMsg.contains("TLS", Qt::CaseInsensitive)
                   || errMsg.contains("SSL", Qt::CaseInsensitive)) {
            errMsg += QStringLiteral("\n") + tr("打印机 TLS/SSL 证书存在问题，已优先尝试使用非加密的 ipp:// 协议；如仍失败，请检查打印机网络设置或关闭 IPPS。");
        } else {
            errMsg += QStringLiteral("\n") + tr("设备可能正忙（扫描/预热中），请稍后重试");
        }
        return false;
    }

    // 免驱 PPD 默认纸张多为 Letter（美制），国内默认应为 A4。
    //
    // 必须先等 PPD 落盘再设置：lpadmin 建队列后 PPD 是异步生成的，在此之前调用
    // lpadmin -o 会返回 0 却静默失效（PPD 随后被默认内容覆盖，纸张退回 Letter）。
    //
    // 绝不要改用 "lpoptions -p NAME -o PageSize=A4"：那写的是用户级
    // ~/.cups/lpoptions，会形成一层覆盖把 PPD 的真实值遮蔽掉——本应用读到 A4，
    // 而系统打印管理器读 PPD 原值，两边就此永久不同步（用户改 A5 应用也看不到）。
    // 统一以 PPD 为唯一真相源，两个程序才会一致。
    //
    // lpadmin -o 由 cupsd 代写 PPD，在 lpadmin 组即可，不需要额外提权。
    // PPD 不支持 A4 时该命令失败，静默忽略保持原默认。
    waitForPpdReady(name);

    // 顺带清理历史遗留的用户级 PageSize 覆盖：早期版本曾误用 lpoptions 写入
    // ~/.cups/lpoptions，那层覆盖会遮蔽 PPD 的真实值，导致在系统打印管理器里
    // 改过的纸张在本应用中读不到。传空值可单独解除该项，其余选项不受影响。
    run({"lpoptions", "-p", name, "-o", "PageSize="}, out, err);

    run({"lpadmin", "-p", name, "-o", "PageSize=A4"}, out, err);

    // 启用队列并接受任务
    run({"cupsaccept", name}, out, err);
    run({"cupsenable", name}, out, err);

    // 设置 CUPS 队列的 description（printer-info）。在 lpstat -l -p 的输出里，
    // 这就是 "描述：" 字段；而 "打印属性 → 基础信息 → 描述" 也是从这里读的。
    // PPD 里只有 "Printer - IPP Everywhere"，缺乏品牌型号——只有显式设置 printer-info
    // 才能让用户看到 "Pantum BM4240ADW Series" 等真实型号。
    // 空字符串或与 PPD 自身重复时跳过，避免覆盖已经合理的值。
    if (!prettyName.isEmpty() && prettyName != QLatin1String("Printer - IPP Everywhere")) {
        run({"lpadmin", "-p", name, "-D", prettyName}, out, err);
    }

    // 回读 PPD 真实 make/model，写入队列信息（PPD 关联）
    m_lastAddedMakeModel = ppdManufacturer(name);
    // 优先以 caller 给出的 prettyName 为准——这是从 driverless 拿到的最准确型号，
    // PPD 里只有通用模板字符串。回读失败时仍能给 UI 提供有效文案。
    if (m_lastAddedMakeModel.isEmpty() && !prettyName.isEmpty())
        m_lastAddedMakeModel = prettyName;
    return true;
}

void PrintManager::addPrinterAsync(const QString &name, const QString &uri,
                                   const QString &driver,
                                   const QString &prettyName)
{
    if (m_addPrinterRunning)
        return;

    m_addPrinterRunning = true;
    auto *watcher = new QFutureWatcher<PrinterAddResult>(this);
    connect(watcher, &QFutureWatcher<PrinterAddResult>::finished,
            this, [this, watcher]() {
        const PrinterAddResult result = watcher->result();
        m_lastAddedMakeModel = result.makeModel;
        m_addPrinterRunning = false;
        watcher->deleteLater();
        emit addPrinterFinished(result);
    });
    watcher->setFuture(QtConcurrent::run([name, uri, driver, prettyName]() {
        // 临时管理器完全生活在工作线程中，后台任务不捕获界面对象。
        PrintManager worker;
        PrinterAddResult result;
        result.name = name;
        result.uri = uri;
        result.ok = worker.addPrinter(name, uri, driver, prettyName, result.error);
        result.makeModel = worker.lastAddedMakeModel();
        return result;
    }));
}

QString PrintManager::lastAddedMakeModel() const
{
    return m_lastAddedMakeModel;
}

bool PrintManager::removePrinter(const QString &name, QString &errMsg)
{
    // 删除队列是 CUPS 队列管理的写操作，必须经 Privileges 统一判定：
    // lpadmin 组直跑，否则自动 pkexec。此前这里裸调 lpadmin 且不提权，
    // 非 lpadmin 用户点"删除打印机"必然失败，只能靠错误文案兜底。
    QString err;
    const int code = Privileges::run(QStringLiteral("lpadmin"),
                                     {QStringLiteral("-x"), name},
                                     Privileges::Elevation::Auto,
                                     nullptr, &err, 15000);
    if (code != 0) {
        errMsg = err.trimmed();
        if (errMsg.isEmpty())
            errMsg = tr("删除失败（请确认当前用户在 lpadmin 组，或授权后重试）");
        return false;
    }
    return true;
}

bool PrintManager::setDefault(const QString &name, QString &errMsg)
{
    // 这里刻意用 Never 而不是 Auto，原因有三：
    //   1) lpoptions -d 写的是当前用户的 ~/.cups/lpoptions，任何用户都能写，
    //      根本不需要 root，提权没有收益；
    //   2) 一旦走 pkexec，CUPS 会改写成系统级 /etc/cups/lpoptions，
    //      语义从"改我自己的默认打印机"变成"改全系统默认打印机"；
    //   3) 用 Auto 会让行为随调用者是否在 lpadmin 组而漂移——组内改用户级、
    //      组外改系统级，同一个按钮两种结果。
    // 注意 privileges.h 把 lpoptions 列在 Never 类，指的是 -p/-l 只读探测；
    // 本函数是 -d 写操作，虽同样不需要 root，但不属于只读。
    QString err;
    const int code = Privileges::run(QStringLiteral("lpoptions"),
                                     {QStringLiteral("-d"), name},
                                     Privileges::Elevation::Never,
                                     nullptr, &err, 8000);
    if (code != 0) {
        errMsg = err.trimmed();
        if (errMsg.isEmpty()) errMsg = tr("设为默认失败");
        return false;
    }
    return true;
}

bool PrintManager::setAccepting(const QString &name, bool accept, QString &errMsg)
{
    // cupsaccept / cupsreject 修改系统队列的接单状态，是写操作，
    // 与 removePrinter 同样走 Auto（privileges.h 已把 cupsaccept 归为队列管理）。
    QString err;
    const int code = Privileges::run(accept ? QStringLiteral("cupsaccept")
                                            : QStringLiteral("cupsreject"),
                                     {name},
                                     Privileges::Elevation::Auto,
                                     nullptr, &err, 8000);
    if (code != 0) {
        errMsg = err.trimmed();
        if (errMsg.isEmpty())
            errMsg = accept ? tr("启用接受任务失败（请确认当前用户在 lpadmin 组）")
                            : tr("暂停接受任务失败（请确认当前用户在 lpadmin 组）");
        return false;
    }
    return true;
}

// 只负责发命令并返回结果，不弹对话框：
// 业务层持有 UI 会让 PrintManager 无法在工作线程中安全复用，
// 且此前用 nullptr 作 parent，对话框不会居中于主窗口。
// errorText 为空表示成功，否则为失败原因。
bool PrintManager::printTestPage(const QString &queue, QString *errorText)
{
    if (queue.isEmpty()) {
        if (errorText)
            *errorText = tr("请先选择一个已配置的打印机。");
        return false;
    }

    // lp 只是把作业交给 CUPS，属队列操作：lpadmin 组直跑，否则由 Privileges 提权
    QString err;
    const int code = Privileges::run(QStringLiteral("lp"),
                                     {QStringLiteral("-d"), queue,
                                      QStringLiteral("-o"), QStringLiteral("media=a4"),
                                      QStringLiteral("/usr/share/cups/data/testprint")},
                                     Privileges::Elevation::Auto,
                                     nullptr, &err, 8000);
    if (code != 0) {
        if (errorText) {
            *errorText = tr("测试页发送失败：")
                         + (err.trimmed().isEmpty()
                                ? tr("退出码 %1").arg(code) : err.trimmed());
        }
        return false;
    }
    return true;
}
