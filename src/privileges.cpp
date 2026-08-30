#include "privileges.h"

#include <QProcess>
#include <QRegularExpression>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

namespace
{
    // ---- 缓存：组信息与 polkit 可用性在进程内只需探测一次 ----

    struct Cache {
        bool inited{false};
        bool isRoot{false};
        bool lpadmin{false};
        bool scanner{false};
        bool polkit{false};
    };

    Cache &cache()
    {
        static Cache c;
        return c;
    }

    void initCache()
    {
        Cache &c = cache();
        if (c.inited)
            return;
        c.inited = true;

        c.isRoot = (::geteuid() == 0);

        // 用 getgroups() 读取附加组，比解析 `id` 输出可靠（不受 locale 影响）。
        const int ngroups = ::getgroups(0, nullptr);
        if (ngroups > 0) {
            QVector<gid_t> gids(ngroups);
            if (::getgroups(ngroups, gids.data()) == ngroups) {
                for (auto probe : {std::make_pair("lpadmin", &c.lpadmin),
                                   std::make_pair("scanner", &c.scanner)}) {
                    const struct group *g = ::getgrnam(probe.first);
                    if (!g)
                        continue;
                    for (gid_t gid : gids) {
                        if (gid == g->gr_gid) {
                            *probe.second = true;
                            break;
                        }
                    }
                }
            }
        }

        // root 不受组限制
        if (c.isRoot)
            c.lpadmin = c.scanner = true;

        // polkit：pkexec 存在即可。能否真正授权由会话决定，运行时才可知。
        QProcess which;
        which.start(QStringLiteral("sh"),
                    {QStringLiteral("-c"), QStringLiteral("command -v pkexec")});
        c.polkit = which.waitForFinished(3000) && which.exitCode() == 0;
    }

    // 判断一次失败的直跑是否"看起来像权限不足"。
    // CUPS 工具在这类情况下通常返回 1，信息走 stderr。
    bool looksLikePermissionError(const QString &stderrText)
    {
        static const QStringList needles = {
            QStringLiteral("not authorized"),
            QStringLiteral("Forbidden"),
            QStringLiteral("permission denied"),
            QStringLiteral("Operation not permitted"),
            QStringLiteral("Insufficient"),
            QStringLiteral("authentication required"),
            QStringLiteral("认证失败"),
        };
        const QString low = stderrText.toLower();
        for (const QString &n : needles) {
            if (low.contains(n.toLower()))
                return true;
        }
        return false;
    }

    struct ProcResult {
        int code{-1};
        QString out;
        QString err;
    };

    ProcResult execSync(const QString &program, const QStringList &args, int timeoutMs)
    {
        ProcResult r;
        QProcess proc;
        proc.start(program, args);
        if (!proc.waitForStarted(5000)) {
            r.err = proc.errorString();
            return r;
        }
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(2000);
            r.code = -1;
            r.err = QStringLiteral("timeout");
            return r;
        }
        r.code = proc.exitCode();
        r.out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        r.err = QString::fromLocal8Bit(proc.readAllStandardError());
        return r;
    }
}

namespace Privileges
{
    bool inGroup(const QString &group)
    {
        initCache();
        const Cache &c = cache();
        if (c.isRoot)
            return true;
        if (group.compare(QStringLiteral("lpadmin"), Qt::CaseInsensitive) == 0)
            return c.lpadmin;
        if (group.compare(QStringLiteral("scanner"), Qt::CaseInsensitive) == 0)
            return c.scanner;
        return false;
    }

    bool canManageQueues()
    {
        initCache();
        return cache().lpadmin;
    }

    bool canScan()
    {
        initCache();
        return cache().scanner;
    }

    bool polkitAvailable()
    {
        initCache();
        return cache().polkit;
    }

    QString joinGroupHint(const QString &group)
    {
        return QStringLiteral("sudo usermod -aG %1 $USER").arg(group);
    }

    bool willElevate(Elevation mode)
    {
        switch (mode) {
        case Elevation::Never:
            return false;
        case Elevation::Always:
            return true;
        case Elevation::Auto:
            return !canManageQueues();
        }
        return false;
    }

    int run(const QString &program,
            const QStringList &args,
            Elevation mode,
            QString *out,
            QString *err,
            int timeoutMs)
    {
        // 预判：Always 无条件提权；Auto 在不在 lpadmin 组时直接提权，
        // 免得先跑一次注定失败的命令（慢，且会在 CUPS 日志里留无用记录）。
        const bool elevateFirst = (mode == Elevation::Always)
                                  || (mode == Elevation::Auto && !canManageQueues());
        if (elevateFirst) {
            QStringList pk;
            pk << program << args;
            const ProcResult r = execSync(QStringLiteral("pkexec"), pk, timeoutMs);
            if (out) *out = r.out;
            if (err) *err = r.err;
            return r.code;
        }

        // 直跑（Never，或 Auto 且已在 lpadmin 组）
        ProcResult r = execSync(program, args, timeoutMs);

        // 兜底：Auto 模式下明明在 lpadmin 组却仍报权限不足
        // （例如管理员改过 CUPS 的 SystemGroup），退回提权重试一次。
        if (mode == Elevation::Auto && r.code != 0 && looksLikePermissionError(r.err)) {
            QStringList pk;
            pk << program << args;
            const ProcResult r2 = execSync(QStringLiteral("pkexec"), pk, timeoutMs);
            if (out) *out = r2.out;
            if (err) *err = r2.err;
            return r2.code;
        }

        if (out) *out = r.out;
        if (err) *err = r.err;
        return r.code;
    }

    int run(const QStringList &argv,
            Elevation mode,
            QString *out,
            QString *err,
            int timeoutMs)
    {
        if (argv.isEmpty())
            return -1;
        return run(argv.first(), argv.mid(1), mode, out, err, timeoutMs);
    }
}
