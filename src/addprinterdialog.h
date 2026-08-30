#pragma once

#include <QDialog>
#include <DLabel>
#include <DListView>
#include <DWidget>

DWIDGET_USE_NAMESPACE

class QStandardItemModel;
class QPushButton;
class QComboBox;

struct PrinterEntry;
class PrintManager;

/// lpinfo -m 解析出的一条驱动候选
struct LpDriver {
    QString id;     // lpadmin 用的 driver 标识（driverless:ipp://... 或 PPD 路径）
    QString label;  // 给用户看的描述
};

/**
 * "添加打印机"对话框（简化版）
 *
 * 布局（参考 UOS dde-printer）：
 *   ┌─────────────────────────────────────┐
 *   │ 选择打印机                       刷新 │
 *   │ 已识别的免驱打印机（driverless /    │
 *   │  IPP Everywhere）                  │
 *   │ ┌─────────────────────────────┐    │
 *   │ │ [打印机列表]                 │    │
 *   │ └─────────────────────────────┘    │
 *   ├─────────────────────────────────────┤
 *   │         驱动：[______]…            │
 *   │              [取消] [安装驱动]      │
 *   └─────────────────────────────────────┘
 *
 * 驱动优先级（只显示免驱相关选项）：
 *   1. 设备专属 driverless（来自 lpinfo -m，最匹配）
 *   2. 通用 driverless（driverless:<uri>）
 *   3. IPP Everywhere（默认选中，最稳）
 *   4. Generic 厂商通用 PPD（lpinfo -m 异步加载）
 *   5. 本地 PPD（…手动选）
 */
class AddPrinterDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddPrinterDialog(const QList<PrinterEntry> &candidates,
                              PrintManager *pm,
                              QWidget *parent = nullptr);

    QString addedQueueName() const { return m_addedName; }

protected:
    void showEvent(QShowEvent *e) override;

private slots:
    void onAutoRescan();
    void onAddClicked();
    void onRescan();
    void onDiscoveryFinished();

private:
    void buildUi();
    void buildAutoPage();
    void selectCandidateFromList();
    void fillDriverCombo();
    void fetchLpinfoAsync();

    QString selectedDriver() const;
    QString defaultNameFor(const QString &uri) const;

    QList<PrinterEntry> m_candidates;
    QString m_addedName;
    QString m_currentUri;   // 当前选中的候选 URI，供 fillDriverCombo 匹配 make/model

    // lpinfo -m 异步缓存（避免阻塞弹窗打开）
    QList<LpDriver> m_allDrivers;
    bool m_lpinfoFetched = false;
    bool m_lpinfoFetching = false;

    DListView *m_autoView{};
    QStandardItemModel *m_autoModel{};

    QComboBox *m_driverCombo{};
    QPushButton *m_btnAdd{};
    QPushButton *m_btnCancel{};
    QPushButton *m_btnRefresh{};

    PrintManager *m_printManager{};
};