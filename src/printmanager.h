#pragma once

#include <QObject>
#include <QRegularExpression>
#include <QStringListModel>
#include <QUrl>
#include <QDateTime>

/**
 * 一台打印机的统一抽象：
 *  - 已配置 CUPS 队列：name 是队列名，uri 是 CUPS device-uri，protocols 从 uri 推断
 *  - 待添加免驱设备：name=队列建议名，uri=driverless 输出的 URI
 */
struct PrinterEntry {
    // 基础标识
    QString name;          // CUPS 队列名（已配置）或默认建议名（待添加）
    QString uri;           // driverless URI / CUPS device-uri
    QString makeAndModel;  // 厂商型号（如 "Pantum BM4240ADW Series A3024A"）
    QString location;      // 设备位置（来自 dnssd txt record 或 driverless 反查）
    QStringList protocols; // 支持的协议：[ipp, ipps, dnssd, socket, lpd, usb]
    QString ppdPath;        // 队列对应的 PPD 路径
    QString ppdMakeModel;  // PPD 内部的 *PCFileName / *Manufacturer-*Model
    QDateTime lastUsed;    // 队列最后使用时间（lpstat -t 解析）
    bool everywhere = false;    // 是否支持 IPP Everywhere（driverless 免驱）

    // 状态
    bool isQueue = false;          // true = 已配置 CUPS 队列
    bool isDefault = false;        // 是否为默认队列
    bool accepting = true;         // 是否接受新任务

    // 显示
    QString title;       // 用于主列表展示的主标题
    QString subtitle;    // 用于主列表展示的副标题

    /// 推断协议（只从 uri 头部读）
    static QString detectProtocol(const QString &uri);
    /// 是否已存在 uri 对应的队列（uri 等价比较）
    static bool sameUri(const QString &a, const QString &b);
    /// 判断 URI 是否真正可走 IPP 免驱：ipp / ipps，或 dnssd 中的 _ipp._tcp / _ipps._tcp。
    /// _pdl-datastream._tcp / _printer._tcp 等原始套接字/LPD 服务返回 false。
    static bool isIppUri(const QString &uri);
    /// 成员便捷方法
    bool isIpp() const { return isIppUri(uri); }
};

struct PrinterAddResult {
    bool ok = false;
    QString name;
    QString uri;
    QString makeModel;
    QString error;
};

/**
 * 打印管理：基于 CUPS + driverless (IPP-USB) 实现免驱打印发现、队列添加与测试页打印。
 */
class PrintManager : public QObject
{
    Q_OBJECT
public:
    explicit PrintManager(QObject *parent = nullptr);

    /// 发现打印机：先尝试 driverless，再合并 CUPS 已配置队列（异步）
    void discover();

    /// 轻量刷新：只重读 CUPS 已配置队列（不跑 driverless 扫描，秒级完成），
    /// 用于添加打印机后让新队列立即显示。异步，完成后发 discoveryFinished。
    void refreshQueuesOnly();

    /// 打印测试页到指定打印机
    // 返回是否发送成功；失败原因写入 errorText（可为空指针）。
    // 不负责弹对话框，提示由调用方（UI 层）完成。
    bool printTestPage(const QString &queue, QString *errorText = nullptr);

    /// 通过 driverless URI 添加打印机队列（pkexec lpadmin）
    /// driver 可为空；为空时按 everywhere / driverless:uri 顺序兜底
    /// prettyName 真实品牌型号；用于设置 lpadmin -D（CUPS printer-info），
    /// 让"打印属性 → 基础信息 → 描述"以及 lpstat -l -p 显示真实型号，
    /// 而不是 PPD 默认的 "Printer - IPP Everywhere"。
    bool addPrinter(const QString &name, const QString &uri,
                    const QString &driver, const QString &prettyName, QString &errMsg);
    bool addPrinter(const QString &name, const QString &uri, QString &errMsg) {
        return addPrinter(name, uri, QString(), QString(), errMsg);
    }

    /// 后台添加打印机，避免 lpadmin 重试期间阻塞界面。
    void addPrinterAsync(const QString &name, const QString &uri,
                         const QString &driver = QString(),
                         const QString &prettyName = QString());
    bool addPrinterInProgress() const { return m_addPrinterRunning; }

    /// 删除打印机队列（pkexec lpadmin -x）
    bool removePrinter(const QString &name, QString &errMsg);

    /// 设置默认队列（lpoptions -d）
    bool setDefault(const QString &name, QString &errMsg);

    /// 启用 / 拒绝队列接受新任务（cupsaccept / cupsreject）
    bool setAccepting(const QString &name, bool accept, QString &errMsg);

    /// 查询队列的 PPD 文件第一行属性（同步，可能抛错）
    static QString ppdManufacturer(const QString &queue);
    static QString ppdModel(const QString &queue);
    static QString ppdDriverPath(const QString &queue);

    /// 根据 URI 生成可读显示名
    static QString prettyNameFromUri(const QString &uri);

    /// 根据 URI 生成不重复的 CUPS 队列名
    ///
    /// 当能拿到真实品牌型号（如 driverless 反查得到的 "Pantum BM4240ADW Series"）时，
    /// 应优先用 realModel 派生队列名（如 "Pantum-BM4240ADW-Series"），这会让"已配置
    /// 队列"列表的主标题直接显示品牌，比 URI 派生的 "localhost-60000" 直观得多。
    /// 仍然走去重逻辑，重名追加 -2/-3。
    static QString makeDefaultName(const QString &uri, const QString &realModel = QString());

    /// 序列化为可作 CUPS 队列名的安全 ASCII：字母数字加连字符/下划线，
    /// 截断到 127 字符；连续空白合并为单空格再转连字符。
    /// 仅在 makeDefaultName 内部使用，也供 addPrinter 给 lpadmin 验名前清洗。
    static QString sanitizeQueueName(const QString &s);

    /// 最近一次添加成功后，回读的 PPD 厂商型号（用于成功提示更精确）
    QString lastAddedMakeModel() const;

    /// 当前发现的打印机列表
    QList<PrinterEntry> printers() const { return m_printers; }

    QStringListModel *model() { return &m_model; }

signals:
    void discoveryFinished();
    void addPrinterFinished(const PrinterAddResult &result);

private:
    /// 把 driverless 列表按 make+model 合并（去重 ipp / ipps）
    static QList<PrinterEntry> mergeByMakeModel(QList<PrinterEntry> raw);
    /// 反查 driverless URI 的 make+model（解析 driverless list 输出）
    static void enrichFromDriverless(QList<PrinterEntry> &list, const QString &dlList);
    /// 读取 CUPS 已配置队列（同步，供 discover / refreshQueuesOnly 复用）
    static QList<PrinterEntry> readCupsQueues();
    void publishDiscovery(const QList<PrinterEntry> &discovered,
                          const QList<PrinterEntry> &queues);

    QStringListModel m_model;
    QList<PrinterEntry> m_printers;
    QString m_lastAddedMakeModel;
    bool m_addPrinterRunning = false;
    bool m_discoveryRunning = false;
    bool m_queueRefreshRunning = false;

    static QString queuePpdPath(const QString &queue);
};
