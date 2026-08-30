#include "ppdconfig.h"
#include "privileges.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QProcess>
#include <QTemporaryDir>

PpdConfig::PpdConfig(QObject *parent)
    : QObject(parent)
{
}

QString PpdConfig::ppdPathForQueue(const QString &queue)
{
    return "/etc/cups/ppd/" + queue + ".ppd";
}

QStringList PpdConfig::supportedPageSizes(const QString &ppdPath)
{
    QStringList sizes;
    QFile f(ppdPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return sizes;

    bool inPageSize = false;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QString line;
    while (ts.readLineInto(&line)) {
        if (line.startsWith("*OpenUI *PageSize"))
            inPageSize = true;
        else if (line.startsWith("*CloseUI: *PageSize"))
            inPageSize = false;
        else if (inPageSize && line.startsWith("*PageSize ")) {
            // *PageSize A4/A4 (210 x 297mm): -> 取 A4
            QRegularExpression re("^\\*PageSize\\s+([A-Za-z0-9_]+)/");
            auto m = re.match(line);
            if (m.hasMatch())
                sizes.append(m.captured(1));
        }
    }
    return sizes;
}

bool PpdConfig::read(const QString &queue, Options &out)
{
    const QString path = ppdPathForQueue(queue);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QString line;
    bool inPageSize = false;
    while (ts.readLineInto(&line)) {
        if (line.startsWith("*DefaultPageSize:")) {
            out.pageSize = line.section(':', 1).trimmed();
        } else if (line.startsWith("*cupsBackSide:")) {
            const QString v = line.section(':', 1).trimmed();
            out.backSide = v;
            out.longEdgeDuplex = (v.compare("Rotated", Qt::CaseInsensitive) == 0);
        } else if (line.startsWith("*OpenUI *PageSize")) {
            inPageSize = true;
        } else if (line.startsWith("*CloseUI: *PageSize")) {
            inPageSize = false;
        } else if (inPageSize && line.startsWith("*Default")) {
            // 兜底：若上面没拿到（极少数 PPD 写法不同）
            if (out.pageSize.isEmpty())
                out.pageSize = line.section(':', 1).trimmed();
        }
    }
    // 若 PPD 完全无 cupsBackSide 字段，视为尚未应用本项目的实机旋转修正。
    if (out.backSide.isEmpty()) {
        out.backSide = "Normal";
        out.longEdgeDuplex = false;
    }
    return true;
}

bool PpdConfig::apply(const QString &queue, const Options &opt, QString &errMsg)
{
    const QString path = ppdPathForQueue(queue);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errMsg = tr("无法读取 PPD：") + path + tr("（可能需要 root 权限）");
        return false;
    }

    QStringList lines;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    QString line;
    bool hasBackSide = false;
    while (in.readLineInto(&line)) {
        if (line.startsWith("*DefaultPageSize:")) {
            lines.append("*DefaultPageSize: " + opt.pageSize);
        } else if (line.startsWith("*cupsBackSide:")) {
            lines.append(QString("*cupsBackSide: ") + (opt.longEdgeDuplex ? "Rotated" : "Normal"));
            hasBackSide = true;
        } else {
            lines.append(line);
        }
    }
    // 若原 PPD 无 cupsBackSide 字段（极少），追加到 Duplex 段之后更稳妥，
    // 这里简单追加到文件末尾（CUPS 解析按关键字覆盖，末尾生效）。
    if (!hasBackSide) {
        lines.append("");
        lines.append(QString("*cupsBackSide: ") + (opt.longEdgeDuplex ? "Rotated" : "Normal"));
    }
    f.close();

    // 写回。/etc/cups/ppd 为 root:lp 且权限 755（组 lp 仅有 r-x），
    // 因此即便在 lpadmin 组也写不进去，必须 root —— 这里用 Always 而非常规提权。
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // 尝试通过 pkexec 提权写回
        QTemporaryDir tmp;
        QString tmpPath = tmp.path() + "/ppd.tmp";
        QFile tf(tmpPath);
        if (!tf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            errMsg = tr("无法写入临时文件");
            return false;
        }
        QTextStream ts(&tf);
        ts.setEncoding(QStringConverter::Utf8);
        ts << lines.join('\n');
        tf.close();

        QString cpErr;
        const int code = Privileges::run(QStringLiteral("cp"),
                                        {tmpPath, path},
                                        Privileges::Elevation::Always,
                                        nullptr, &cpErr, 15000);
        if (code != 0) {
            errMsg = tr("写入 PPD 需要管理员授权，请重试并允许提权。");
            if (!cpErr.trimmed().isEmpty())
                errMsg += QStringLiteral("\n") + cpErr.trimmed();
            return false;
        }
    } else {
        QTextStream ts(&out);
        ts.setEncoding(QStringConverter::Utf8);
        ts << lines.join('\n');
        out.close();
    }

    // 重启队列使 CUPS 重新加载 PPD。这三个都是队列管理操作，
    // 与写文件不同：在 lpadmin 组时无需提权，交给 Privileges 判定。
    Privileges::run(QStringLiteral("cupsaccept"), {queue},
                    Privileges::Elevation::Auto, nullptr, nullptr, 5000);
    Privileges::run(QStringLiteral("cupsenable"), {queue},
                    Privileges::Elevation::Auto, nullptr, nullptr, 5000);
    // 通过 lpadmin -p <q> -o 触发 PPD 重载（部分版本需要）
    Privileges::run(QStringLiteral("lpadmin"),
                    {QStringLiteral("-p"), queue,
                     QStringLiteral("-o"), QStringLiteral("printer-error-policy=retry-job")},
                    Privileges::Elevation::Auto, nullptr, nullptr, 5000);

    return true;
}
