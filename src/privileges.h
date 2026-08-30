#pragma once

#include <QString>
#include <QStringList>

/**
 * 统一权限管理层
 *
 * 本项目此前权限策略不一致：PrintManager / PrintPropertiesDialog 直接调用
 * lpadmin/cupsaccept/cupsenable 且不做提权（依赖当前用户在 lpadmin 组），
 * 而 PpdConfig / EnvChecker / AdvancedSettings 一律走 pkexec。
 * 结果是：在 lpadmin 组的用户会被无谓地反复弹授权框，而不在组里的用户
 * 点"添加/删除打印机"则必然失败，只能靠错误文案兜底。
 *
 * 本模块按"操作需要什么权限"而不是"哪个模块调用"来决策：
 *   - Never  ：只读探测（lpinfo / driverless / lpoptions / scanimage 等）
 *   - Auto   ：CUPS 队列管理（lpadmin / cupsaccept / cupsenable / lpadmin -p -o）
 *              在 lpadmin 组时直跑，否则自动提权
 *   - Always : 必须有 root（写 /etc/cups/ppd、systemctl、apt-get、
 *              写 /etc/ipp-usb/quirks 与 /etc/sane.d/airscan.d）
 *
 * Auto 模式还带一次失败回退：直跑若报权限不足，自动改用 pkexec 重试，
 * 以覆盖"用户在 lpadmin 组但 CUPS SystemGroup 被改过"等边界情况。
 */
namespace Privileges
{
    enum class Elevation {
        Never,   // 只读，绝不提权
        Auto,    // CUPS 队列管理：依 lpadmin 组决定
        Always,  // 必须 root
    };

    // ---- 环境探测（结果带缓存，进程内只探测一次）----

    // 当前进程用户是否在指定组中（含附加组）。root（uid 0）恒为 true。
    bool inGroup(const QString &group);

    // 是否能直接管理 CUPS 队列（lpadmin 组或 root）。
    // CUPS 的 SystemGroup 默认含 lpadmin，组成员可免密管理队列。
    bool canManageQueues();

    // 是否具备本地 USB 扫描权限（scanner 组或 root）。
    bool canScan();

    // polkit 提权是否可用（pkexec 存在且可启动）。
    bool polkitAvailable();

    // 推荐用于提示文案的加组命令，例如 "sudo usermod -aG lpadmin $USER"。
    QString joinGroupHint(const QString &group);

    // ---- 命令执行 ----

    // 按 mode 决定提权策略执行外部命令。
    // 返回进程退出码；启动失败返回 -1。out/err 可为 nullptr。
    int run(const QString &program,
            const QStringList &args,
            Elevation mode,
            QString *out = nullptr,
            QString *err = nullptr,
            int timeoutMs = 30000);

    // 便捷重载：把 program 与 args 合成一个列表传入
    // （printmanager 中现有代码就是这种 QStringList 形式）
    int run(const QStringList &argv,
            Elevation mode,
            QString *out = nullptr,
            QString *err = nullptr,
            int timeoutMs = 30000);

    // 实际执行时是否会走 pkexec（用于界面上提前告知用户将弹出授权框）
    bool willElevate(Elevation mode);
}
