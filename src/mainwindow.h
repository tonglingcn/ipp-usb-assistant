#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <DLabel>
#include <DDialog>
#include <DLineEdit>
#include <DListView>
#include <DAboutDialog>

#include "printmanager.h"
#include "scannermanager.h"
#include "envchecker.h"
#include "advancedsettings.h"

class TwoLineItemDelegate;
class QStandardItemModel;
class QButtonGroup;
class QVBoxLayout;
class QFrame;
class QPushButton;

DWIDGET_USE_NAMESPACE

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // 启动时若当前用户不在 lpadmin 组，由 main.cpp 调用，
    // 在环境检测页顶部提示"部分操作需要授权"并给出加组命令。
    void showPrivilegeNotice();

    // 统一配置关于对话框，main.cpp 与 showAbout() 共用，避免两处文案各写一份。
    static void setupAboutDialog(DAboutDialog *dlg);

private slots:
    void switchPage(int index);
    void refreshEnv();
    void onBasicEnvCheckFinished();
    void refreshPrint();
    void lightRefreshPrint();
    void fillPrintList();
    void onAddPrinter();
    void onAddPrinterFinished(const PrinterAddResult &result);
    void onDriverTweak();
    void onDeletePrinter();
    void onShowPrinterProperties();
    void onTestPage();
    void onPrinterSelectionChanged();
    QString selectedPrinterName() const;
    void showAddSuccessToast(const QString &queue, const PrinterEntry &e);
    void refreshScan();
    void onScanDiscovery(bool found);
    void onScanFinished(bool ok, const QString &outPath, const QString &errMsg);
    void onOpenScanImage();
    void onSaveScanImage();
    void disableButtonFocus();   // 禁止按钮焦点，避免随机出现椭圆圈
    void showAbout();   // 打开关于对话框

private:
    void buildSidebar(QVBoxLayout *sideLayout);
    void setupEnvPage();
    void setupPrintPage();
    void setupScanPage();
    void setupAdvancedPage();
    void updateEnvStatus();

    /// 环境页服务控制卡片
    QWidget *buildServiceCard(const ServiceInfo &info);
    void updateServiceCards();
    void onServiceAction(const QString &unit, const QString &action);
    void onQuickCheck();
    void onQuickCheckFinished(const InstallInfo &info);
    void onInstallIppUsb();
    void onReinstallIppUsb();   // 一键重装：remove --purge + install
    void showOfflineHelp(const InstallInfo &info);

    struct ServiceCard {
        QString unit;
        QFrame *card{};
        QFrame *stripe{};
        DLabel *icon{};
        DLabel *name{};
        DLabel *badge{};
        DLabel *statusLine{};
        QPushButton *btnStart{};
        QPushButton *btnStop{};
        QPushButton *btnRestart{};
    };
    QList<ServiceCard> m_serviceCards;
    bool m_serviceBusy = false;

    // 一键检测 + 安装区
    QFrame *m_quickCheckCard{};
    QFrame *m_privilegeNotice{};
    DLabel *m_privilegeNoticeLabel{};
    DLabel *m_quickCheckBadge{};
    DLabel *m_quickCheckDetail{};
    QPushButton *m_btnQuickCheck{};
    QPushButton *m_btnInstall{};
    QPushButton *m_btnReinstall{};     // 已安装但启动失败/异常时的一键重装
    QPushButton *m_btnOffline{};       // 百度网盘离线下载入口
    QFrame *m_offlinePanel{};
    bool m_offlineShown = false;

    // 打印管理页按钮
    QPushButton *m_btnAddPrinter{};
    QPushButton *m_btnDeletePrinter{};
    QPushButton *m_btnPrinterProps{};   // 新增：打印属性（PPD 选项）
    QPushButton *m_btnDriverTweak{};    // 原“打印属性”按钮，现改为“驱动微调"

    // 扫描页控件
    QComboBox *m_scanResCombo{};        // 分辨率
    QComboBox *m_scanModeCombo{};       // 色彩模式
    DLabel *m_scanPreview{};            // 内嵌扫描预览
    QPushButton *m_btnOpenScan{};       // 打开图片
    QPushButton *m_btnSaveScan{};       // 保存文件（PNG/JPEG/PDF）
    QString m_lastScanPath;
    bool m_scanBusy = false;

    EnvChecker *m_env{};
    PrintManager *m_print{};
    ScannerManager *m_scan{};
    AdvancedSettings *m_advanced{};

    // 视觉：统一的两行列表代理与模型
    TwoLineItemDelegate *m_delegate{};
    QStandardItemModel *m_envModel{};
    QStandardItemModel *m_printModel{};
    QStandardItemModel *m_scanModel{};

    DListView *m_printerView{};
    DListView *m_scannerView{};
    DListView *m_deviceView{};

    DLabel *m_envStatus{};
    QStackedWidget *m_pages{};
    QButtonGroup *m_navGroup{};
    QWidget *m_printPage{};
    QWidget *m_scanPage{};

    DAboutDialog *m_aboutDialog{};   // 关于对话框（复用应用级）
};
