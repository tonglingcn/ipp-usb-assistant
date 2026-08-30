#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

/**
 * PPD 驱动微调：针对免驱(driverless)生成的 PPD 做关键修正。
 *
 * 核心场景——实机兼容的长边翻页修正：
 *   在已验证的一批设备上，DuplexNoTumble / DuplexTumble 都只能得到短边
 *   双面效果；将 *cupsBackSide 改为 "Rotated" 后，背面图像旋转方向符合
 *   长边翻页预期。因此这里保留该实机兼容策略，而不是把 cupsBackSide
 *   当作所有打印机都通用的长边/短边语义开关。
 */
class PpdConfig : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString pageSize;       // 如 A4 / Letter
        bool longEdgeDuplex;    // true=长边翻页(Rotated)  false=默认(Normal)
        QString backSide;       // 派生：Normal 或 Rotated
    };

    explicit PpdConfig(QObject *parent = nullptr);

    /// 给定 CUPS 队列名，定位其 PPD 文件路径（/etc/cups/ppd/<name>.ppd）
    static QString ppdPathForQueue(const QString &queue);

    /// 读取队列当前可配置项
    bool read(const QString &queue, Options &out);

    /// 写入修改：页面大小 + 长边翻页，并重启队列使 CUPS 重载 PPD
    bool apply(const QString &queue, const Options &opt, QString &errMsg);

    /// 枚举 PPD 支持的页面大小（*OpenUI *PageSize 下的候选）
    static QStringList supportedPageSizes(const QString &ppdPath);
};
