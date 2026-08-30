#pragma once

#include <QObject>
#include <QStringList>
#include <QStringListModel>

/**
 * 设备诊断
 *
 * 汇总排查外设问题所需的全部现场信息，生成一份可导出的纯文本报告，
 * 便于用户把问题现象连同环境信息一起发给支持人员，而不必来回追问
 * "你的 ipp-usb 起来了吗""设备被识别成什么"。
 *
 * 报告内容：
 *   1. 底层服务状态（ipp-usb / cups / avahi-daemon / saned）
 *   2. IPP-over-USB 候选设备（复用 EnvChecker 的严格判定）
 *   3. DNS-SD 广播（_ipp._tcp 打印 / _uscan._tcp 扫描）
 *   4. CUPS 打印队列
 *   5. SANE 扫描设备
 *
 * 全部是只读操作，不提权。
 */
class Diagnostics : public QObject
{
    Q_OBJECT
public:
    explicit Diagnostics(QObject *parent = nullptr);

    /// 采集全部诊断信息（耗时数秒，建议在后台线程调用）
    void scan();

    /// 导出报告到用户文档目录；返回写入的文件路径，失败返回空串
    QString exportReport();

    /// 最近一次 scan() 生成的报告行
    QStringList reportLines() const { return m_report; }

    QStringListModel *model() { return &m_model; }

private:
    /// 查询 systemd 单元状态，失败或未知返回 "unknown"
    static QString serviceStatus(const QString &unit);

    /// 执行只读命令并返回标准输出；超时或失败返回空串
    static QString runReadOnly(const QString &program,
                               const QStringList &args,
                               int timeoutMs = 5000);

    QStringListModel m_model;
    QStringList m_report;
};
