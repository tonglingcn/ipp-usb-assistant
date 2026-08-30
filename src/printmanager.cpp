#include "printmanager.h"
#include "privileges.h"

#include <QProcess>
#include <QThread>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <cups/cups.h>

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

QString PrintManager::makeDefaultName(const QString &uri)
{
    QString base = prettyNameFromUri(uri);
    base.replace(QRegularExpression("[^\\w-]"), " ");
    base = base.split(' ', Qt::SkipEmptyParts).join(" ");
    if (base.isEmpty())
        base = "printer";
    base.replace(' ', '-');

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

// 解析 driverless 输出，driverless 在 List 模式下输出形如：
//   " ipp://192.168.1.10:631/ipp/print  Pantum BM4240ADW Series A3024A "
// 但实际不同版本输出差异较大，所以我们优先依赖 make+model 反查
static QString reverseLookupMakeModel(const QString &uri, const QString &dlList)
{
    // 使用预取的 `driverless list` 输出，避免每台设备重复起进程（8 秒超时会叠加）
    const QString &out = dlList;

    auto strip = [](QString s) {
        if (s.startsWith("ipps://")) s = s.mid(7);
        else if (s.startsWith("ipp://")) s = s.mid(6);
        else if (s.startsWith("dnssd://")) s = s.mid(8);
        return s.section('?', 0, 0).section('#', 0, 0).toLower();
    };
    const QString target = strip(uri);

    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        // 简单启发：每行第一个 URI 后面的字符串作为描述
        QString cand;
        QString rest;
        if (trimmed.startsWith("ipp://") || trimmed.startsWith("ipps://")
            || trimmed.startsWith("dnssd://")) {
            const int sp = trimmed.indexOf(' ');
            if (sp > 0) {
                cand = trimmed.left(sp);
                rest = trimmed.mid(sp + 1).trimmed();
            } else {
                cand = trimmed;
            }
        } else {
            continue;
        }
        if (strip(cand) == target)
            return rest;
    }
    return {};
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
    for (const QString &line : dlList.split('\n', Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (!(trimmed.startsWith("ipp://") || trimmed.startsWith("ipps://")
              || trimmed.startsWith("dnssd://")))
            continue;
        const int sp = trimmed.indexOf(' ');
        const QString cand = sp > 0 ? trimmed.left(sp) : trimmed;
        if (strip(cand) == target)
            return true;
    }
    return false;
}

void PrintManager::enrichFromDriverless(QList<PrinterEntry> &list, const QString &dlList)
{
    for (PrinterEntry &e : list) {
        if (!e.makeAndModel.isEmpty()) continue;
        QString m = reverseLookupMakeModel(e.uri, dlList);
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
                for (const QString line : out.split('\n', Qt::SkipEmptyParts)) {
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
                    e.name = makeDefaultName(uri);
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
            for (const QString line : out.split('\n', Qt::SkipEmptyParts)) {
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

bool PrintManager::addPrinter(const QString &name, const QString &uri,
                              const QString &driver, QString &errMsg)
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
    // 通过 lpadmin -o 设置队列默认 PageSize（用户在 lpadmin 组即可，无需改 PPD 文件）；
    // PPD 不支持 A4 时该命令失败，静默忽略保持原默认。
    run({"lpadmin", "-p", name, "-o", "PageSize=A4"}, out, err);

    // 启用队列并接受任务
    run({"cupsaccept", name}, out, err);
    run({"cupsenable", name}, out, err);

    // 回读 PPD 真实 make/model，写入队列信息（PPD 关联）
    m_lastAddedMakeModel = ppdManufacturer(name);
    return true;
}

void PrintManager::addPrinterAsync(const QString &name, const QString &uri,
                                   const QString &driver)
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
    watcher->setFuture(QtConcurrent::run([name, uri, driver]() {
        // 临时管理器完全生活在工作线程中，后台任务不捕获界面对象。
        PrintManager worker;
        PrinterAddResult result;
        result.name = name;
        result.uri = uri;
        result.ok = worker.addPrinter(name, uri, driver, result.error);
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
    QProcess proc;
    proc.start("lpadmin", {"-x", name});
    const bool ok = proc.waitForFinished(15000)
                    && proc.exitStatus() == QProcess::NormalExit
                    && proc.exitCode() == 0;
    if (!ok) {
        errMsg = QString::fromLocal8Bit(proc.readAllStandardError());
        if (errMsg.isEmpty()) errMsg = tr("删除失败（请确认当前用户在 lpadmin 组）");
        return false;
    }
    return true;
}

bool PrintManager::setDefault(const QString &name, QString &errMsg)
{
    QProcess proc;
    proc.start("lpoptions", {"-d", name});
    const bool ok = proc.waitForFinished(8000)
                    && proc.exitStatus() == QProcess::NormalExit
                    && proc.exitCode() == 0;
    if (!ok) {
        errMsg = QString::fromLocal8Bit(proc.readAllStandardError());
        if (errMsg.isEmpty()) errMsg = tr("设为默认失败");
        return false;
    }
    return true;
}

bool PrintManager::setAccepting(const QString &name, bool accept, QString &errMsg)
{
    QProcess proc;
    proc.start(accept ? "cupsaccept" : "cupsreject", {name});
    const bool ok = proc.waitForFinished(8000)
                    && proc.exitStatus() == QProcess::NormalExit
                    && proc.exitCode() == 0;
    if (!ok) {
        errMsg = QString::fromLocal8Bit(proc.readAllStandardError());
        if (errMsg.isEmpty())
            errMsg = accept ? tr("启用接受任务失败") : tr("暂停接受任务失败");
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
