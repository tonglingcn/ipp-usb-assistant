#pragma once

#include <QObject>
#include <QStringListModel>
#include <QList>

/**
 * 单个依赖服务的状态
 */
struct ServiceInfo {
    QString unit;       // systemd unit 名，如 "ipp-usb"
    QString name;       // 显示名，如 "IPP-USB"
    QString desc;       // 一句话描述
    QString status;     // active / inactive / failed / not-found
    bool installed = false;
};

/**
 * 一键自检 + 在线/离线安装 ipp-usb 的辅助信息
 */
struct InstallInfo {
    bool online = false;                 // 当前是否联网
    bool aptSourceAvailable = false;     // apt 仓库是否可达（含 ipp-usb 包）
    QString osId;                        // /etc/os-release 中 ID（deepin / uos / debian ...）
    QString osVersion;                   // VERSION_ID 或 VERSION
    QString arch;                        // amd64 / arm64 / ...
    QString distroLabel;                 // "Deepin 25" / "UOS 1050a" / "Debian 12" 之类
    QString packageName = "ipp-usb";     // 仓库里的包名
    QString packageVersion;              // apt-cache 显示的最新版本（如果有）
    QString repoUrl;                     // 离线时给用户下载 .deb 的页面（仓库 packages 站）
    QString downloadTip;                 // 给用户的离线下载提示
    QString baiduPanUrl;                 // 百度网盘离线下载地址（不依赖发行版）
};

/**
 * 环境检测：判断当前系统是否具备 IPP-USB 免驱能力，
 * 并提供依赖服务的 启动/停止/重启 控制（经 pkexec 提权）。
 */
class EnvChecker : public QObject
{
    Q_OBJECT
public:
    enum class Status { Ok, Warn, Error, Unknown };

    struct CheckResult {
        QStringList report;
        QStringList supportedDevices;
        QList<ServiceInfo> services;
        QString summary;
        Status overall = Status::Unknown;
    };

    explicit EnvChecker(QObject *parent = nullptr);

    /// 执行一次完整环境检测（同步，调用方应已确认耗时可控）
    void check();

    /// 在后台线程执行基础环境检测，完成后在对象所属线程更新结果。
    void checkAsync();

    /// 一键自检：除了基础 check()，还附带联网/仓库/系统识别（异步，建议在工作线程中调用）
    /// 通过 checkFinished(InstallInfo) 回调结果
    void checkWithInstallInfo();

    bool isCheckRunning() const { return m_checkRunning; }

    /// 结论摘要，供界面状态条使用
    QString summary() const { return m_summary; }
    Status overall() const { return m_overall; }

    /// 文本报告，供诊断导出
    QStringListModel *model() { return &m_model; }

    /// 已连接的 IPP-USB 候选 USB 设备
    QStringList supportedDevices() const { return m_supportedDevices; }

    /// 三个关键依赖服务：ipp-usb / avahi-daemon / cups
    QList<ServiceInfo> services() const { return m_services; }

    /// 查询单个 unit 的 active 状态
    static QString unitStatus(const QString &unit);

    /// unit 文件是否存在（即服务是否已安装）
    static bool unitInstalled(const QString &unit);

    /// 列出已连接的 IPP-over-USB 候选设备（lsusb -v 解析）。
    /// 按 ipp-usb 上游规则严格过滤：需 ≥2 个 Class=7/SubClass=1/Proto=4
    /// 且同时具备 IN/OUT 端点的接口。USB Hub、键鼠等无关设备不会出现。
    /// 返回行格式："Bus %1 Device %2  %3:%4  %5"
    static QStringList lsusbImagingDevices();

    /// 控制服务：action = start / stop / restart（经 pkexec 提权）
    static bool controlService(const QString &unit, const QString &action, QString &errMsg);

    /// 当前是否在线（HTTP HEAD 探测 + apt-cache 兜底，超时 5 秒）
    static bool checkOnline(int timeoutMs = 5000);

    /// 收集系统识别信息（OS / 架构 / 仓库可达性 / 包名+版本 / 离线下载页面）
    static InstallInfo collectInstallInfo();

    /// 通过 apt 安装 ipp-usb（pkexec 提权）。返回 exitCode；errMsg 写入失败原因。
    /// 调起前应已经过 checkWithInstallInfo() 确认在线。
    static int installIppUsb(QString &errMsg);

    /// 卸载 ipp-usb 包（apt-get remove --purge -y，pkexec 提权）。返回 exitCode。
    static int uninstallIppUsb(QString &errMsg);

    /// 启用 ipp-usb 开机自启（systemctl enable ipp-usb.service）
    static bool enableIppUsb(QString &errMsg);

    /// 读取 ipp-usb.service 的 systemd 预设（static/enabled/disabled）
    static QString ippUsbPreset();

signals:
    void basicCheckFinished();
    void checkFinished(const InstallInfo &info);

private:
    static bool commandExists(const QString &cmd);
    static CheckResult collectCheckResult();
    void applyCheckResult(const CheckResult &result);

    QStringListModel m_model;
    QStringList m_report;
    QStringList m_supportedDevices;
    QList<ServiceInfo> m_services;
    QString m_summary;
    Status m_overall = Status::Unknown;
    bool m_checkRunning = false;
};
