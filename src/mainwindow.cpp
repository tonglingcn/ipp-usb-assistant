#include "mainwindow.h"
#include "envchecker.h"
#include "privileges.h"
#include "printerconfigdialog.h"
#include "printpropertiesdialog.h"
#include "twolineitemdelegate.h"
#include "addprinterdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QAbstractItemView>
#include <QInputDialog>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStandardItemModel>
#include <QButtonGroup>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QGuiApplication>
#include <QIcon>
#include <QTimer>
#include <QScrollArea>
#include <QPdfWriter>
#include <QPainter>
#include <QImage>
#include <QStandardPaths>
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <functional>
#include <DListView>
#include <DMessageBox>
#include <DSuggestButton>
#include <DIconTheme>
#include <DApplication>
#include "qtcompat.h"

namespace {

/// 状态徽章内联样式（运行中/已停止/未安装）
QString badgeStyle(const QString &bg, const QString &fg)
{
    return QString("color: %1; background: %2; border-radius: 10px;"
                   "padding: 2px 10px; font-size: 11px; font-weight: 600;")
        .arg(fg, bg);
}

QStandardItem *makeItem(const QString &title, const QString &subtitle,
                        const QColor &accent, const QString &iconChar = QString())
{
    auto *item = new QStandardItem(title);
    item->setData(subtitle, Qt::UserRole);
    item->setData(accent, Qt::DecorationRole);
    item->setData(iconChar, Qt::UserRole + 1);
    item->setEditable(false);
    return item;
}

QWidget *makePageHeader(QWidget *parent, const QString &title, const QString &subtitle)
{
    auto *header = new QWidget(parent);
    header->setObjectName("pageHeader");
    auto *layout = new QVBoxLayout(header);
    layout->setContentsMargins(24, 18, 24, 18);
    layout->setSpacing(4);
    auto *t = new DLabel(title);
    t->setObjectName("pageTitle");
    auto *s = new DLabel(subtitle);
    s->setObjectName("pageSubtitle");
    s->setWordWrap(true);
    s->setTextFormat(Qt::PlainText);  // 显式声明，避免 DTK 样式对换行的歧义判断
    layout->addWidget(t);
    layout->addWidget(s);
    return header;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    m_env = new EnvChecker(this);
    m_print = new PrintManager(this);
    m_scan = new ScannerManager(this);
    m_delegate = new TwoLineItemDelegate(this);

    m_envModel = new QStandardItemModel(this);
    m_printModel = new QStandardItemModel(this);
    m_scanModel = new QStandardItemModel(this);

    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // 左侧导航栏
    auto *side = new QWidget(this);
    side->setObjectName("sidebar");
    side->setFixedWidth(230);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(16, 20, 16, 20);
    sideLayout->setSpacing(14);

    m_envStatus = new DLabel(tr("环境检测中…"));
    m_envStatus->setObjectName("envStatus");
    m_envStatus->setWordWrap(true);
    m_envStatus->setMinimumHeight(88);
    // updateEnvStatus() 传入的是 "<b>● …</b><br/>" 富文本，
    // 不声明 RichText 会把标签原样显示出来。
    m_envStatus->setTextFormat(Qt::RichText);
    sideLayout->addWidget(m_envStatus);

    sideLayout->addSpacing(8);
    buildSidebar(sideLayout);
    sideLayout->addStretch();

    auto *about = new QPushButton;
    about->setObjectName("aboutLabel");
    about->setFlat(true);
    about->setCursor(Qt::PointingHandCursor);
    about->setToolTip(tr("关于 IPP-USB 免驱助手"));
    about->setFixedHeight(32);
    connect(about, &QPushButton::clicked, this, &MainWindow::showAbout);
    sideLayout->addWidget(about);

    // 右侧内容区
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName("contentArea");

    // 四个功能页
    setupEnvPage();
    setupPrintPage();
    setupScanPage();
    setupAdvancedPage();

    mainLayout->addWidget(side);
    mainLayout->addWidget(m_pages, 1);

    // 发现完成后回填列表（discover 是异步的）
    connect(m_print, &PrintManager::discoveryFinished,
            this, &MainWindow::fillPrintList);
    connect(m_print, &PrintManager::addPrinterFinished,
            this, &MainWindow::onAddPrinterFinished);
    connect(m_env, &EnvChecker::basicCheckFinished,
            this, &MainWindow::onBasicEnvCheckFinished);

    // 若当前用户不在 lpadmin 组，立刻显示提示（不必等 main.cpp 的延后调用）
    if (!Privileges::canManageQueues())
        showPrivilegeNotice();

    refreshEnv();
    lightRefreshPrint();   // 只读已配置队列，不自动做全量发现（由「发现设备」按钮触发）
    refreshScan();

    // 避免按钮获得焦点后绘制焦点椭圆
    disableButtonFocus();
}

void MainWindow::buildSidebar(QVBoxLayout *sideLayout)
{
    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    auto *navBox = new QVBoxLayout;
    navBox->setSpacing(8);
    navBox->setContentsMargins(0, 0, 0, 0);

    const QStringList navNames = {tr("环境检测"), tr("打印管理"), tr("扫描管理"), tr("高级设置")};
    for (int i = 0; i < navNames.size(); ++i) {
        auto *btn = new QPushButton(navNames[i]);
        btn->setObjectName("navButton");
        btn->setCheckable(true);
        btn->setFixedHeight(44);
        btn->setCursor(Qt::PointingHandCursor);
        m_navGroup->addButton(btn, i);

        QString icon;
        switch (i) {
        case 0: icon = "◉"; break;
        case 1: icon = "⎙"; break;
        case 2: icon = "⎗"; break;
        case 3: icon = "⚙"; break;
        }
        btn->setText("  " + icon + "  " + navNames[i]);
        navBox->addWidget(btn);
    }
    sideLayout->addLayout(navBox);

    // Qt 6 用 idClicked(int)，Qt 5.15 之前叫 buttonClicked(int)，由 qtcompat.h 统一
    connect(m_navGroup, QOverload<int>::of(IPP_USB_BUTTON_GROUP_ID_SIGNAL),
            this, &MainWindow::switchPage);

    if (auto *first = m_navGroup->button(0))
        first->setChecked(true);
}

void MainWindow::switchPage(int index)
{
    m_pages->setCurrentIndex(index);
    if (index == 0) refreshEnv();
    else if (index == 1) lightRefreshPrint();   // 只读 CUPS 队列，不触发 driverless 全量扫描
    else if (index == 2) refreshScan();
    // index == 3 为高级设置页，页面 showEvent 中会自动进行首次无密码扫描
}

void MainWindow::lightRefreshPrint()
{
    if (m_print->printers().isEmpty()) {
        // 缓存为空：显示占位并做轻量队列刷新（秒级），已配置队列自动显示
        m_printModel->clear();
        m_printModel->appendRow(makeItem(tr("正在读取已配置打印队列…"),
                                         QString(), QColor(150, 155, 165), "⟳"));
        m_print->refreshQueuesOnly();
        return;
    }
    fillPrintList();
}

void MainWindow::onQuickCheck()
{
    if (m_env->isCheckRunning())
        return;

    m_btnQuickCheck->setEnabled(false);
    m_btnQuickCheck->setText(tr("检测中…"));
    m_quickCheckBadge->setText(tr("● 检测中"));
    m_quickCheckBadge->setStyleSheet(
        badgeStyle("rgba(0,129,255,0.12)", "#0081ff"));
    m_quickCheckDetail->setText(
        QString(tr("正在检查 ipp-usb / 系统仓库 / 网络连接，请稍候…")));
    m_btnInstall->setVisible(false);
    m_btnReinstall->setVisible(false);
    m_btnOffline->setVisible(false);
    m_offlinePanel->setVisible(false);
    m_env->checkWithInstallInfo();
}

void MainWindow::onQuickCheckFinished(const InstallInfo &info)
{
    // 完整自检也刷新了基础服务/设备结果，同步更新侧栏和服务卡片。
    onBasicEnvCheckFinished();
    m_btnQuickCheck->setEnabled(true);
    m_btnQuickCheck->setText(tr("一键环境自检"));

    // 本地检测结论
    // 按 unit 名查找，不依赖 services() 的数组顺序。此前用 value(0)，
    // 一旦 EnvChecker 里调整服务顺序或增删服务，这里就会静默取到别的服务。
    const ServiceInfo *ippSvc = m_env->findService(QStringLiteral("ipp-usb"));
    const QString ippStatus = ippSvc ? ippSvc->status : QString();
    const bool ippInstalled = ippSvc && ippSvc->installed;
    const bool ippActive = ippStatus == "active";
    const bool ippFailed = ippStatus == "failed";
    const QString preset = EnvChecker::ippUsbPreset();
    const bool hasDevice = !m_env->supportedDevices().isEmpty();

    QStringList details;
    details << QString(tr("系统：%1  ·  架构：%2"))
                   .arg(info.distroLabel, info.arch.isEmpty() ? tr("未知") : info.arch);
    details << QString(tr("ipp-usb 包：%1 %2"))
                   .arg(ippInstalled ? tr("已安装")
                                     : (info.aptSourceAvailable
                                            ? tr("未安装，但仓库中存在")
                                            : tr("未安装，仓库中也未发现")),
                        info.packageVersion.isEmpty()
                            ? QString()
                            : QString(tr("(最新 %1)")).arg(info.packageVersion));
    details << QString(tr("网络状态：%1%2"))
                   .arg(info.online ? tr("已联网") : tr("离线"),
                        info.aptSourceAvailable
                            ? QString(tr("  ·  仓库可达"))
                            : QString());
    if (ippInstalled) {
        details << QString(tr("启动策略：%1%2"))
                       .arg(preset.isEmpty() ? tr("未知") : preset,
                            preset == "static"
                                ? tr("  （由 udev 规则在插入 IPP-USB 设备时触发）")
                                : (preset == "enabled" ? tr("  （已开机自启）") : QString()));
        details << QString(tr("候选设备：%1")).arg(
            hasDevice ? tr("已识别到 IPP-USB 候选 USB 设备")
                      : tr("未识别到 IPP-USB 候选 USB 设备（插上后会自动触发）"));
    }

    m_quickCheckDetail->setText(details.join("\n"));

    // 决定徽章与下一步按钮
    // 保存百度网盘地址到按钮属性，供点击打开
    m_btnOffline->setProperty("baiduPanUrl", info.baiduPanUrl);

    if (ippActive) {
        m_quickCheckBadge->setText(tr("● 就绪"));
        m_quickCheckBadge->setStyleSheet(
            badgeStyle("rgba(37,184,100,0.14)", "#15803d"));
        m_btnInstall->setVisible(false);
        m_btnReinstall->setVisible(false);
        m_btnOffline->setVisible(false);
        m_offlinePanel->setVisible(false);
    } else if (!ippInstalled) {
        // 只要未安装就显示「离线下载地址」，与是否联网无关
        m_btnOffline->setVisible(true);
        m_btnReinstall->setVisible(false);
        if (info.online && info.aptSourceAvailable) {
            m_quickCheckBadge->setText(tr("● 可在线安装"));
            m_quickCheckBadge->setStyleSheet(
                badgeStyle("rgba(245,158,11,0.14)", "#b45309"));
            m_btnInstall->setVisible(true);
            m_offlinePanel->setVisible(false);
        } else {
            m_quickCheckBadge->setText(tr("● 离线 / 仓库不可达"));
            m_quickCheckBadge->setStyleSheet(
                badgeStyle("rgba(239,68,68,0.12)", "#b91c1c"));
            m_btnInstall->setVisible(false);
            showOfflineHelp(info);
        }
    } else {
        // 已安装但未运行 / 启动失败：根因是 ipp-usb.service 是 static，由 udev 触发。
        // 若有候选设备，提示用户重新插拔以触发 udev；否则提供启用自启作为兜底。
        if (ippFailed) {
            m_quickCheckBadge->setText(tr("● 启动失败"));
            m_quickCheckBadge->setStyleSheet(
                badgeStyle("rgba(239,68,68,0.12)", "#b91c1c"));
        } else if (hasDevice) {
            m_quickCheckBadge->setText(tr("● 已插入但未自动启动"));
            m_quickCheckBadge->setStyleSheet(
                badgeStyle("rgba(245,158,11,0.14)", "#b45309"));
        } else {
            m_quickCheckBadge->setText(tr("● 已安装但未运行"));
            m_quickCheckBadge->setStyleSheet(
                badgeStyle("rgba(245,158,11,0.14)", "#b45309"));
        }
        // 把"从系统源安装"按钮临时复用为"立即启动 ipp-usb"
        m_btnInstall->setVisible(true);
        m_btnInstall->setText(hasDevice
                              ? tr("立即启动 ipp-usb 服务")
                              : tr("立即启动并启用自启"));
        m_btnReinstall->setVisible(true);  // 已安装异常时允许一键重装
        m_btnOffline->setVisible(false);   // 已安装时不显示离线下载
        m_offlinePanel->setVisible(false);
    }
}

void MainWindow::onInstallIppUsb()
{
    // 这个按钮在不同状态下承担不同动作：
    //   - 未安装 + 仓库可达：执行 apt-get install ipp-usb
    //   - 已安装但未运行：根据候选设备决定"立即启动"或"启动并启用自启"
    // 与 onQuickCheckFinished() 一致：按 unit 名取，不用下标
    const ServiceInfo *ippSvc = m_env->findService(QStringLiteral("ipp-usb"));
    const bool installed = ippSvc && ippSvc->installed;
    const QString btnText = m_btnInstall->text();

    auto runAsync = [this](std::function<QString(QString&)> op, const QString &okTitle,
                           const QString &okMsg, const QString &verb) {
        m_btnInstall->setEnabled(false);
        const QString originalText = m_btnInstall->text();
        m_btnInstall->setText(verb + tr("中…"));
        using OperationResult = QPair<QString, QString>; // result code, error text
        auto *watcher = new QFutureWatcher<OperationResult>(this);
        connect(watcher, &QFutureWatcher<OperationResult>::finished,
                this, [this, watcher, okTitle, okMsg, verb, originalText]() {
            const OperationResult operationResult = watcher->result();
            watcher->deleteLater();
            const QString &result = operationResult.first;
            const QString &err = operationResult.second;
            m_btnInstall->setEnabled(true);
            m_btnInstall->setText(originalText);
            if (result.isEmpty()) {
                DMessageBox::information(this, okTitle, okMsg);
                onQuickCheck();
            } else {
                DMessageBox::critical(this, verb + tr("失败"),
                    QString("%1（%2）").arg(err, result));
            }
        });
        watcher->setFuture(QtConcurrent::run([op]() {
            QString err;
            const QString result = op(err); // 空串表示成功
            return qMakePair(result, err);
        }));
    };

    if (!installed) {
        auto *ask = new DMessageBox(this);
        ask->setIcon(DMessageBox::Information);
        ask->setText(tr("将通过 pkexec 调用 apt-get install ipp-usb "
                        "安装最新版本（含依赖）。\n\n是否继续？"));
        QPushButton *ok = ask->addButton(tr("安装"), QMessageBox::AcceptRole);
        ask->addButton(tr("取消"), QMessageBox::RejectRole);
        ok->setDefault(true);
        ask->setWindowTitle(tr("安装 ipp-usb"));
        ask->exec();
        if (ask->clickedButton() != ok) return;

        runAsync([](QString &err) {
            const int code = EnvChecker::installIppUsb(err);
            return code == 0 ? QString() : QString::number(code);
        }, tr("安装成功"), tr("ipp-usb 已安装，正在刷新检测…"), tr("安装"));
    } else {
        // 已安装未运行：根据按钮文案决定动作
        const bool enableAuto = btnText.contains("自启");
        const QString confirm = enableAuto
            ? QString(tr("将通过 pkexec 立即启动 ipp-usb 服务，"
                         "并设为开机自启（systemctl enable --now）。\n\n是否继续？"))
            : QString(tr("将通过 pkexec 立即启动 ipp-usb 服务（systemctl start）。\n\n"
                         "提示：ipp-usb 默认由 udev 在插入设备时触发，"
                         "如果设备已插上但未自动启动，可先重新插拔一次。\n\n是否继续？"));
        auto *ask = new DMessageBox(this);
        ask->setIcon(DMessageBox::Information);
        ask->setText(confirm);
        QPushButton *ok = ask->addButton(tr("启动"), QMessageBox::AcceptRole);
        ask->addButton(tr("取消"), QMessageBox::RejectRole);
        ok->setDefault(true);
        ask->setWindowTitle(tr("启动 ipp-usb"));
        ask->exec();
        if (ask->clickedButton() != ok) return;

        runAsync([enableAuto](QString &err) {
            if (!EnvChecker::controlService("ipp-usb", "start", err))
                return QString("start failed");
            if (enableAuto) {
                if (!EnvChecker::enableIppUsb(err))
                    return QString("enable failed");
            }
            return QString();
        }, tr("启动成功"),
        enableAuto
            ? tr("ipp-usb 已启动并设为开机自启。")
            : tr("ipp-usb 已启动，正在刷新检测…"),
        tr("启动"));
    }
}

void MainWindow::onReinstallIppUsb()
{
    // 一键重装：先 apt-get remove --purge -y，再 apt-get install -y --no-install-recommends。
    // 用于修复配置文件损坏、systemd 单元残留等导致反复 start failed 的顽固问题。
    auto *ask = new DMessageBox(this);
    ask->setIcon(DMessageBox::Warning);
    ask->setText(tr("将先卸载 ipp-usb（保留设备状态文件），然后重新安装最新版本。\n"
                    "此操作会短暂中断打印/扫描服务。\n\n是否继续？"));
    QPushButton *ok = ask->addButton(tr("重装"), QMessageBox::AcceptRole);
    ask->addButton(tr("取消"), QMessageBox::RejectRole);
    ok->setDefault(true);
    ask->setWindowTitle(tr("重装 ipp-usb"));
    ask->exec();
    if (ask->clickedButton() != ok)
        return;

    m_btnReinstall->setEnabled(false);
    const QString originalText = m_btnReinstall->text();
    m_btnReinstall->setText(tr("重装中…"));

    using OperationResult = QPair<QString, QString>;
    auto *watcher = new QFutureWatcher<OperationResult>(this);
    connect(watcher, &QFutureWatcher<OperationResult>::finished,
            this, [this, watcher, originalText]() {
        const OperationResult r = watcher->result();
        watcher->deleteLater();
        m_btnReinstall->setEnabled(true);
        m_btnReinstall->setText(originalText);
        if (r.first.isEmpty()) {
            DMessageBox::information(this, tr("重装成功"),
                                     tr("ipp-usb 已重装，正在刷新检测…"));
            onQuickCheck();
        } else {
            DMessageBox::critical(this, tr("重装失败"),
                                  QString("%1（%2）").arg(r.second, r.first));
        }
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        QString err;
        QString step;
        // 1) 卸载
        step = tr("卸载");
        int code = EnvChecker::uninstallIppUsb(err);
        if (code != 0)
            return qMakePair(QString::number(code), QString("%1: %2").arg(step, err));

        // 2) 安装
        step = tr("安装");
        code = EnvChecker::installIppUsb(err);
        if (code != 0)
            return qMakePair(QString::number(code), QString("%1: %2").arg(step, err));

        return qMakePair(QString(), QString());
    }));
}

void MainWindow::showOfflineHelp(const InstallInfo &info)
{
    auto *offRepo = m_offlinePanel->findChild<DLabel *>("offlineRepo");
    auto *offTip = m_offlinePanel->findChild<DLabel *>("offlineTip");
    if (offRepo) offRepo->setText(info.repoUrl);
    if (offTip) offTip->setText(info.downloadTip);
    m_offlinePanel->setProperty("repoUrl", info.repoUrl);
    m_offlinePanel->setVisible(true);
}

void MainWindow::showPrivilegeNotice()
{
    if (!m_privilegeNotice || !m_privilegeNoticeLabel)
        return;

    // CUPS 的 SystemGroup 默认含 lpadmin，组成员可免密管理打印队列。
    // 不在该组时，添加/删除打印机、修改打印属性都会弹 pkexec 授权框，
    // 用户往往不清楚原因，这里给一次明确说明与加组命令。
    m_privilegeNoticeLabel->setText(
        tr("当前用户不在 lpadmin 组，管理打印队列时每次都需要输入管理员密码。\n"
           "加入该组后可免密操作（需注销重新登录生效）：%1")
            .arg(Privileges::joinGroupHint(QStringLiteral("lpadmin"))));
    m_privilegeNotice->setVisible(true);
}

void MainWindow::setupEnvPage()
{
    auto *envPage = new QWidget;
    auto *envLayout = new QVBoxLayout(envPage);
    envLayout->setContentsMargins(0, 0, 0, 0);
    envLayout->setSpacing(0);
    envLayout->addWidget(makePageHeader(envPage, tr("环境检测"),
        tr("管理免驱打印/扫描依赖的系统服务，检测已连接的 IPP-USB 设备。")));

    auto *envBody = new QWidget;
    auto *envBodyLayout = new QVBoxLayout(envBody);
    envBodyLayout->setContentsMargins(24, 16, 24, 24);
    envBodyLayout->setSpacing(14);

    // 权限提示条：默认隐藏，由 showPrivilegeNotice() 在检测到
    // 当前用户不在 lpadmin 组时显示（此时队列管理操作每次都要过授权框）。
    m_privilegeNotice = new QFrame;
    m_privilegeNotice->setObjectName("privilegeNotice");
    m_privilegeNotice->setVisible(false);
    auto *pnLayout = new QHBoxLayout(m_privilegeNotice);
    pnLayout->setContentsMargins(16, 12, 16, 12);
    pnLayout->setSpacing(12);

    auto *pnIcon = new DLabel("!");
    pnIcon->setObjectName("privilegeNoticeIcon");
    pnIcon->setFixedSize(24, 24);
    pnIcon->setAlignment(Qt::AlignCenter);

    m_privilegeNoticeLabel = new DLabel;
    m_privilegeNoticeLabel->setObjectName("privilegeNoticeLabel");
    m_privilegeNoticeLabel->setWordWrap(true);

    pnLayout->addWidget(pnIcon);
    pnLayout->addWidget(m_privilegeNoticeLabel, 1);
    envBodyLayout->addWidget(m_privilegeNotice);

    // 一键环境自检 + 在线安装/离线帮助 大区
    m_quickCheckCard = new QFrame;
    m_quickCheckCard->setObjectName("quickCheckCard");
    auto *qcLayout = new QVBoxLayout(m_quickCheckCard);
    qcLayout->setContentsMargins(20, 16, 20, 16);
    qcLayout->setSpacing(10);

    auto *qcTitleRow = new QHBoxLayout;
    auto *qcIcon = new DLabel("Q");
    qcIcon->setObjectName("quickCheckIcon");
    qcIcon->setFixedSize(36, 36);
    qcIcon->setAlignment(Qt::AlignCenter);
    qcIcon->setStyleSheet(
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        " stop:0 #0081ff, stop:1 #1f6feb);"
        " color: #ffffff; border-radius: 18px;"
        " font-size: 16px; font-weight: 800;");
    auto *qcTitle = new DLabel(tr("一键环境自检"));
    qcTitle->setObjectName("quickCheckTitle");
    auto *qcSubtitle = new DLabel(tr("点击下方按钮一键排查 ipp-usb 安装、仓库可达性、网络状态"));
    qcSubtitle->setObjectName("quickCheckSubtitle");
    m_quickCheckBadge = new DLabel(tr("未检测"));
    m_quickCheckBadge->setObjectName("quickCheckBadge");
    m_quickCheckBadge->setStyleSheet(
        badgeStyle("rgba(107,114,128,0.12)", "#6b7280"));

    auto *qcTextWrap = new QVBoxLayout;
    qcTextWrap->setSpacing(2);
    qcTextWrap->addWidget(qcTitle);
    qcTextWrap->addWidget(qcSubtitle);

    qcTitleRow->addWidget(qcIcon);
    qcTitleRow->addLayout(qcTextWrap, 1);
    qcTitleRow->addWidget(m_quickCheckBadge, 0, Qt::AlignTop);
    qcLayout->addLayout(qcTitleRow);

    m_quickCheckDetail = new DLabel;
    m_quickCheckDetail->setObjectName("quickCheckDetail");
    m_quickCheckDetail->setWordWrap(true);
    m_quickCheckDetail->setMinimumHeight(20);
    qcLayout->addWidget(m_quickCheckDetail);

    auto *qcBtnRow = new QHBoxLayout;
    qcBtnRow->setSpacing(8);
    m_btnQuickCheck = new DSuggestButton(tr("一键环境自检"));
    m_btnQuickCheck->setIcon(DIconTheme::findQIcon("refresh"));
    m_btnQuickCheck->setFixedHeight(34);
    m_btnQuickCheck->setObjectName("primaryAction");
    auto *btnRecheck = new QPushButton(tr("只刷新本地检测"));
    btnRecheck->setObjectName("secondaryAction");
    btnRecheck->setFixedHeight(34);
    m_btnInstall = new QPushButton(tr("从系统源安装 ipp-usb"));
    m_btnInstall->setObjectName("secondaryAction");
    m_btnInstall->setFixedHeight(34);
    m_btnInstall->setVisible(false);

    m_btnReinstall = new QPushButton(tr("一键重装 ipp-usb"));
    m_btnReinstall->setObjectName("secondaryAction");
    m_btnReinstall->setFixedHeight(34);
    m_btnReinstall->setVisible(false);
    m_btnReinstall->setToolTip(tr("先卸载再重新安装 ipp-usb，用于修复启动失败等顽固问题"));

    m_btnOffline = new QPushButton(tr("离线下载地址"));
    m_btnOffline->setObjectName("secondaryAction");
    m_btnOffline->setFixedHeight(34);
    m_btnOffline->setVisible(false);
    m_btnOffline->setToolTip(tr("跳转到百度网盘离线下载页面"));

    qcBtnRow->addWidget(m_btnQuickCheck);
    qcBtnRow->addWidget(m_btnInstall);
    qcBtnRow->addWidget(m_btnReinstall);
    qcBtnRow->addWidget(m_btnOffline);
    qcBtnRow->addStretch();
    qcBtnRow->addWidget(btnRecheck);
    qcLayout->addLayout(qcBtnRow);

    // 离线帮助面板（首次自检后按需显示）
    m_offlinePanel = new QFrame;
    m_offlinePanel->setObjectName("offlinePanel");
    m_offlinePanel->setVisible(false);
    auto *offLay = new QVBoxLayout(m_offlinePanel);
    offLay->setContentsMargins(20, 14, 20, 14);
    offLay->setSpacing(6);
    auto *offTitle = new DLabel(tr("离线环境？使用下面的链接下载 .deb 离线安装包"));
    offTitle->setObjectName("offlineTitle");
    auto *offRepo = new DLabel;
    offRepo->setObjectName("offlineRepo");
    offRepo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *offTip = new DLabel;
    offTip->setObjectName("offlineTip");
    offTip->setWordWrap(true);
    auto *offBtnRow = new QHBoxLayout;
    auto *offBtnOpen = new QPushButton(tr("打开下载页面"));
    offBtnOpen->setObjectName("secondaryAction");
    offBtnOpen->setFixedHeight(32);
    auto *offBtnCopy = new QPushButton(tr("复制下载链接"));
    offBtnCopy->setObjectName("secondaryAction");
    offBtnCopy->setFixedHeight(32);
    offBtnRow->addWidget(offBtnOpen);
    offBtnRow->addWidget(offBtnCopy);
    offBtnRow->addStretch();
    offLay->addWidget(offTitle);
    offLay->addWidget(offRepo);
    offLay->addWidget(offTip);
    offLay->addLayout(offBtnRow);

    envBodyLayout->addWidget(m_quickCheckCard);
    envBodyLayout->addWidget(m_offlinePanel);

    // 服务控制卡片：ipp-usb / avahi(mDNS) / CUPS
    auto *cardsRow = new QHBoxLayout;
    cardsRow->setSpacing(10);
    const QStringList units = { "ipp-usb", "avahi-daemon", "cups" };
    static const QStringList placeholderNames = {
        "IPP-USB", "Avahi (mDNS)", "CUPS"
    };
    for (int i = 0; i < units.size(); ++i) {
        ServiceInfo info;
        info.unit = units[i];
        info.name = placeholderNames[i];
        info.desc = tr("正在获取服务状态…");
        info.status = QStringLiteral("checking");
        info.installed = true;
        auto *c = buildServiceCard(info);
        c->setMinimumWidth(260);
        cardsRow->addWidget(c, 1);
    }
    envBodyLayout->addLayout(cardsRow);

    // USB 设备列表
    auto *usbTitle = new DLabel(tr("已连接 USB 设备（IPP-USB 候选）"));
    usbTitle->setObjectName("sectionLabel");
    envBodyLayout->addWidget(usbTitle);

    m_deviceView = new DListView;
    m_deviceView->setItemDelegate(m_delegate);
    m_deviceView->setModel(m_envModel);
    m_deviceView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceView->setSelectionMode(QAbstractItemView::NoSelection);
    envBodyLayout->addWidget(m_deviceView, 1);

    envLayout->addWidget(envBody, 1);
    m_pages->addWidget(envPage);

    connect(m_btnQuickCheck, &QPushButton::clicked, this, &MainWindow::onQuickCheck);
    connect(btnRecheck, &QPushButton::clicked, this, &MainWindow::refreshEnv);
    connect(m_btnInstall, &QPushButton::clicked, this, &MainWindow::onInstallIppUsb);
    connect(m_btnReinstall, &QPushButton::clicked, this, &MainWindow::onReinstallIppUsb);
    connect(m_btnOffline, &QPushButton::clicked, this, [this]() {
        const QString url = m_btnOffline->property("baiduPanUrl").toString();
        if (url.isEmpty())
            return;

        // 内网/离线场景下浏览器可能无法直接打开网盘，改用对话框展示地址，
        // 用户可手动复制后在联网机器上访问下载，再拷贝回本机安装。
        auto *dlg = new DDialog(this);
        dlg->setWindowTitle(tr("离线下载地址"));
        dlg->setIcon(QIcon(":/icons/ipp-usb-assistant.svg"));
        dlg->setMessage(tr("若是内网或离线环境，请复制下方网盘地址下载。"));

        auto *edit = new DLineEdit(dlg);
        edit->setText(url);
        edit->lineEdit()->setReadOnly(true);
        edit->lineEdit()->setFrame(false);
        edit->setClearButtonEnabled(false);

        auto *layout = new QVBoxLayout;
        layout->setSpacing(12);
        layout->addWidget(edit);

        // 提供一键复制按钮，复制成功后自动关闭对话框
        auto *btnCopy = new QPushButton(tr("复制地址"), dlg);
        connect(btnCopy, &QPushButton::clicked, this, [dlg, edit]() {
            QGuiApplication::clipboard()->setText(edit->text());
            DMessageBox::information(dlg, tr("IPP-USB 免驱助手"), tr("地址已复制到剪贴板。"));
            dlg->close();
        });
        layout->addWidget(btnCopy, 0, Qt::AlignRight);

        auto *content = new QWidget(dlg);
        content->setLayout(layout);
        dlg->addContent(content);
        dlg->addButton(tr("关闭"), false, DDialog::ButtonNormal);
        dlg->exec();
    });
    connect(offBtnOpen, &QPushButton::clicked, this, [this]() {
        const QString url = m_offlinePanel->property("repoUrl").toString();
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    });
    connect(offBtnCopy, &QPushButton::clicked, this, [this, offRepo]() {
        QGuiApplication::clipboard()->setText(offRepo->text());
        DMessageBox::information(this, tr("已复制"),
            tr("下载链接已复制到剪贴板，可在浏览器粘贴访问。"));
    });

    connect(m_env, &EnvChecker::checkFinished,
            this, &MainWindow::onQuickCheckFinished);
}

QWidget *MainWindow::buildServiceCard(const ServiceInfo &info)
{
    // 品牌色 + 图标字形：每个服务一个稳定的视觉标识
    QString iconChar = "S";
    QString iconColor = "#0081ff";
    if (info.unit == "ipp-usb") {
        iconChar = "U"; iconColor = "#1f6feb";
    } else if (info.unit == "avahi-daemon") {
        iconChar = "M"; iconColor = "#0d9488";
    } else if (info.unit == "cups") {
        iconChar = "P"; iconColor = "#7c3aed";
    }

    auto *card = new QFrame;
    card->setObjectName("serviceCard");
    card->setMinimumHeight(180);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *outer = new QHBoxLayout(card);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 左侧彩色色带：根据状态动态变色
    auto *stripe = new QFrame;
    stripe->setObjectName("serviceStripe");
    stripe->setFixedWidth(4);
    stripe->setStyleSheet(
        "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 #c3c7cf, stop:1 #9ca3af); border: none;");
    outer->addWidget(stripe);

    auto *lay = new QVBoxLayout;
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);
    outer->addLayout(lay, 1);

    // —— 标题行：彩色图标 + 服务名（完整不省略）
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);

    auto *icon = new DLabel(iconChar);
    icon->setObjectName("serviceIcon");
    icon->setFixedSize(32, 32);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QString(
        "background: %1; color: #ffffff; border-radius: 16px;"
        "font-size: 14px; font-weight: 700;").arg(iconColor));

    auto *name = new DLabel(info.name);
    name->setObjectName("serviceName");
    name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    name->setMinimumWidth(0);

    titleRow->addWidget(icon);
    titleRow->addWidget(name, 1);

    // 状态徽章（右侧，固定尺寸）
    auto *badge = new DLabel(tr("检测中…"));
    badge->setObjectName("statusBadge");
    badge->setMinimumWidth(72);
    badge->setAlignment(Qt::AlignCenter);
    titleRow->addWidget(badge, 0, Qt::AlignTop);

    lay->addLayout(titleRow);

    // —— 状态文案行：展示当前运行状态 / 实时提示（如"未运行 · static 策略，udev 触发"）
    auto *statusLine = new DLabel(tr("正在获取服务状态…"));
    statusLine->setObjectName("statusLine");
    statusLine->setWordWrap(true);
    // 服务功能描述以 tooltip 形式呈现，避免和状态行视觉重复
    statusLine->setToolTip(info.desc);
    lay->addWidget(statusLine);

    // —— 按钮行：启动 / 停止 / 重启
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    auto *btnStart = new QPushButton(tr("启动"));
    auto *btnStop = new QPushButton(tr("停止"));
    auto *btnRestart = new QPushButton(tr("重启"));
    for (auto *btn : { btnStart, btnStop, btnRestart }) {
        btn->setObjectName("serviceBtn");
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(30);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btnRow->addWidget(btn, 1);
    }
    lay->addLayout(btnRow);

    const QString unit = info.unit;
    connect(btnStart, &QPushButton::clicked, this, [this, unit]() {
        onServiceAction(unit, "start");
    });
    connect(btnStop, &QPushButton::clicked, this, [this, unit]() {
        onServiceAction(unit, "stop");
    });
    connect(btnRestart, &QPushButton::clicked, this, [this, unit]() {
        onServiceAction(unit, "restart");
    });

    ServiceCard sc;
    sc.unit = unit;
    sc.card = card;
    sc.stripe = stripe;
    sc.icon = icon;
    sc.name = name;
    sc.badge = badge;
    sc.statusLine = statusLine;
    sc.btnStart = btnStart;
    sc.btnStop = btnStop;
    sc.btnRestart = btnRestart;
    m_serviceCards.append(sc);
    return card;
}

void MainWindow::updateServiceCards()
{
    const auto &services = m_env->services();
    for (auto &sc : m_serviceCards) {
        const ServiceInfo *info = nullptr;
        for (const ServiceInfo &s : services) {
            if (s.unit == sc.unit) {
                info = &s;
                break;
            }
        }
        if (!info)
            continue;

        // 名称（EnvChecker::check() 后填充真值）。描述作为 statusLine 的 tooltip。
        sc.name->setText(info->name);
        sc.statusLine->setToolTip(info->desc);

        QString stripeTop, stripeBottom;
        QString badgeText, badgeBg, badgeFg;
        QString statusHint;
        bool enableStart = true, enableStop = true, enableRestart = true;

        if (!info->installed) {
            badgeText = tr("● 未安装");
            badgeBg = "rgba(107,114,128,0.14)";
            badgeFg = "#6b7280";
            stripeTop = "#e5e7eb"; stripeBottom = "#d1d5db";
            statusHint = QString(tr("systemd 单元未找到，请先安装 %1 包")).arg(info->unit);
            enableStart = enableStop = enableRestart = false;
        } else if (info->status == "active") {
            badgeText = tr("● 运行中");
            badgeBg = "rgba(22,163,74,0.14)";
            badgeFg = "#15803d";
            stripeTop = "#22c55e"; stripeBottom = "#16a34a";
            statusHint = tr("服务已激活，正在监听请求");
            enableStart = false;
        } else if (info->status == "failed") {
            badgeText = tr("● 启动失败");
            badgeBg = "rgba(239,68,68,0.12)";
            badgeFg = "#b91c1c";
            stripeTop = "#ef4444"; stripeBottom = "#dc2626";
            statusHint = tr("上次启动失败，请查看 /var/log/syslog 后点「重启」");
            // 启动失败状态下，服务单元仍然“存在”，用户点「启动」等同于
            // systemd 重新 start，语义与「重启」一致，因此两者都可用；
            // 只有「停止」无意义（本来就没在运行）。
            enableStart = true;
            enableStop = false;
        } else {
            badgeText = tr("● 已停止");
            badgeBg = "rgba(234,88,12,0.12)";
            badgeFg = "#9a3412";
            stripeTop = "#fb923c"; stripeBottom = "#ea580c";
            // ipp-usb 给特殊提示：因为它是 static 策略
            if (info->unit == "ipp-usb") {
                statusHint = tr("未运行 · static 策略，由 udev 在插入 IPP-USB 设备时自动触发");
            } else {
                statusHint = tr("未运行，点「启动」激活服务");
            }
            enableStop = false;
        }

        sc.badge->setText(badgeText);
        sc.badge->setStyleSheet(badgeStyle(badgeBg, badgeFg));
        sc.stripe->setStyleSheet(
            QString("background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                    " stop:0 %1, stop:1 %2); border: none;").arg(stripeTop, stripeBottom));
        sc.statusLine->setText(statusHint);
        sc.btnStart->setEnabled(enableStart);
        sc.btnStop->setEnabled(enableStop);
        sc.btnRestart->setEnabled(enableRestart);
    }
}

void MainWindow::onServiceAction(const QString &unit, const QString &action)
{
    if (m_serviceBusy)
        return;
    m_serviceBusy = true;

    const QString verb = action == "start" ? tr("启动")
                       : action == "stop"  ? tr("停止")
                                           : tr("重启");

    // 操作期间禁用所有服务按钮，避免重复弹出授权窗口
    for (auto &sc : m_serviceCards) {
        sc.btnStart->setEnabled(false);
        sc.btnStop->setEnabled(false);
        sc.btnRestart->setEnabled(false);
        if (sc.unit == unit)
            sc.badge->setText(tr("● 操作中…"));
    }

    using ServiceResult = QPair<bool, QString>;
    auto *watcher = new QFutureWatcher<ServiceResult>(this);
    connect(watcher, &QFutureWatcher<ServiceResult>::finished,
            this, [this, watcher, unit, verb]() {
        const ServiceResult result = watcher->result();
        watcher->deleteLater();
        m_serviceBusy = false;
        if (!result.first)
            DMessageBox::critical(this, tr("服务操作"),
                QString(tr("「%1」%2失败：\n%3")).arg(unit, verb, result.second));
        refreshEnv();
    });
    watcher->setFuture(QtConcurrent::run([unit, action]() {
        QString err;
        const bool ok = EnvChecker::controlService(unit, action, err);
        return qMakePair(ok, err);
    }));
}

void MainWindow::setupPrintPage()
{
    m_printPage = new QWidget;
    auto *layout = new QVBoxLayout(m_printPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makePageHeader(m_printPage, tr("打印管理"),
        tr("发现 IPP-USB 设备，添加免驱队列，并对 PPD 做专业化微调。")));

    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(24, 16, 24, 24);
    bodyLayout->setSpacing(16);

    auto *btnRow = new QHBoxLayout;
    auto *btnDiscover = new QPushButton(tr("发现设备"));
    m_btnAddPrinter = new DSuggestButton(tr("添加打印机"));
    m_btnDeletePrinter = new QPushButton(tr("删除打印机"));
    m_btnPrinterProps = new QPushButton(tr("打印属性"));
    m_btnDriverTweak = new QPushButton(tr("驱动微调"));
    auto *btnTest = new QPushButton(tr("打印测试页"));
    btnDiscover->setObjectName("toolButton");
    m_btnDeletePrinter->setObjectName("toolButton");
    m_btnPrinterProps->setObjectName("toolButton");
    m_btnDriverTweak->setObjectName("toolButton");
    btnTest->setObjectName("toolButton");
    // 删除/打印属性/驱动微调仅在选中已配置队列时可点击
    m_btnDeletePrinter->setEnabled(false);
    m_btnPrinterProps->setEnabled(false);
    m_btnDriverTweak->setEnabled(false);
    m_btnDeletePrinter->setToolTip(tr("请先在下方列表中选择一台已配置的打印机"));
    m_btnPrinterProps->setToolTip(tr("请先在下方列表中选择一台已配置的打印机"));
    m_btnDriverTweak->setToolTip(tr("请先在下方列表中选择一台已配置的打印机"));
    btnRow->addWidget(btnDiscover);
    btnRow->addWidget(m_btnAddPrinter);
    btnRow->addWidget(m_btnDeletePrinter);
    btnRow->addWidget(m_btnPrinterProps);
    btnRow->addWidget(m_btnDriverTweak);
    btnRow->addWidget(btnTest);
    btnRow->addStretch();
    bodyLayout->addLayout(btnRow);

    m_printerView = new DListView;
    m_printerView->setItemDelegate(m_delegate);
    m_printerView->setModel(m_printModel);
    m_printerView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bodyLayout->addWidget(m_printerView);
    layout->addWidget(body, 1);
    m_pages->addWidget(m_printPage);

    connect(btnDiscover, &QPushButton::clicked, this, &MainWindow::refreshPrint);
    connect(m_btnAddPrinter, &QPushButton::clicked, this, &MainWindow::onAddPrinter);
    connect(m_btnDeletePrinter, &QPushButton::clicked, this, &MainWindow::onDeletePrinter);
    connect(m_btnPrinterProps, &QPushButton::clicked, this, &MainWindow::onShowPrinterProperties);
    connect(m_btnDriverTweak, &QPushButton::clicked, this, &MainWindow::onDriverTweak);
    connect(btnTest, &QPushButton::clicked, this, &MainWindow::onTestPage);
    // 列表选中变化驱动删除/属性按钮启用
    if (m_printerView) {
        connect(m_printerView, &DListView::clicked,
                this, &MainWindow::onPrinterSelectionChanged);
        if (m_printerView->selectionModel())
            connect(m_printerView->selectionModel(),
                    &QItemSelectionModel::currentRowChanged,
                    this, &MainWindow::onPrinterSelectionChanged);
    }
}

void MainWindow::setupScanPage()
{
    m_scanPage = new QWidget;
    auto *layout = new QVBoxLayout(m_scanPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(makePageHeader(m_scanPage, tr("扫描管理"),
        tr("免驱扫描基于 SANE + sane-airscan (eSCL)，USB 扫描依赖 ipp-usb 与 avahi-daemon。\n"
           "若厂商已提供 SANE 驱动，本机也会一并列出并支持基础扫描。")));

    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(24, 16, 24, 24);
    bodyLayout->setSpacing(16);

    auto *btnRow = new QHBoxLayout;
    auto *btnDiscover = new QPushButton(tr("发现扫描仪"));
    btnDiscover->setObjectName("toolButton");

    m_btnOpenScan = new QPushButton(tr("打开图片"));
    m_btnOpenScan->setObjectName("toolButton");
    m_btnOpenScan->setEnabled(false);

    m_btnSaveScan = new QPushButton(tr("保存文件"));
    m_btnSaveScan->setObjectName("toolButton");
    m_btnSaveScan->setEnabled(false);

    // 分辨率选择
    m_scanResCombo = new QComboBox;
    m_scanResCombo->addItems({tr("75 DPI"), tr("150 DPI"), tr("300 DPI"), tr("600 DPI")});
    m_scanResCombo->setCurrentIndex(2);
    m_scanResCombo->setMinimumContentsLength(8);
    m_scanResCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    // 色彩模式选择
    m_scanModeCombo = new QComboBox;
    m_scanModeCombo->addItem(tr("彩色"), "Color");
    m_scanModeCombo->addItem(tr("灰度"), "Gray");
    m_scanModeCombo->addItem(tr("黑白"), "Lineart");
    m_scanModeCombo->setMinimumContentsLength(6);
    m_scanModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    auto *btnScan = new DSuggestButton(tr("预览并扫描"));

    btnRow->addWidget(btnDiscover);
    btnRow->addSpacing(8);
    btnRow->addWidget(new QLabel(tr("分辨率")));
    btnRow->addWidget(m_scanResCombo);
    btnRow->addSpacing(8);
    btnRow->addWidget(new QLabel(tr("色彩")));
    btnRow->addWidget(m_scanModeCombo);
    btnRow->addSpacing(8);
    btnRow->addWidget(btnScan);
    btnRow->addWidget(m_btnOpenScan);
    btnRow->addWidget(m_btnSaveScan);
    btnRow->addStretch();
    bodyLayout->addLayout(btnRow);

    m_scannerView = new DListView;
    m_scannerView->setItemDelegate(m_delegate);
    m_scannerView->setModel(m_scanModel);
    m_scannerView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_scannerView->setMaximumHeight(160);
    bodyLayout->addWidget(m_scannerView);

    // 内嵌扫描预览区
    auto *previewScroll = new QScrollArea;
    previewScroll->setWidgetResizable(true);
    previewScroll->setAlignment(Qt::AlignCenter);
    previewScroll->setFrameShape(QFrame::StyledPanel);
    m_scanPreview = new DLabel;
    m_scanPreview->setAlignment(Qt::AlignCenter);
    m_scanPreview->setText(tr("扫描结果将显示在这里\n（选中列表中的设备后点击“预览并扫描”）"));
    previewScroll->setWidget(m_scanPreview);
    bodyLayout->addWidget(previewScroll, 1);

    layout->addWidget(body, 1);
    m_pages->addWidget(m_scanPage);

    connect(btnDiscover, &QPushButton::clicked, this, &MainWindow::refreshScan);
    connect(btnScan, &DSuggestButton::clicked, this, [this]() {
        if (m_scanBusy) return;
        if (!m_scan || !m_scan->hasDevices()) {
            DMessageBox::warning(this, tr("IPP-USB 免驱助手"),
                tr("没有可用的扫描仪，请先点击\"发现扫描仪\"。"));
            return;
        }
        QModelIndex idx = m_scannerView->currentIndex();
        int devIdx = idx.isValid() ? idx.row() : 0;

        ScanOptions opts;
        static const int resTable[] = {75, 150, 300, 600};
        opts.resolution = resTable[m_scanResCombo->currentIndex()];
        opts.colorMode = m_scanModeCombo->currentData().toString();

        m_scanBusy = true;
        m_lastScanPath.clear();
        m_btnOpenScan->setEnabled(false);
        m_btnSaveScan->setEnabled(false);
        m_scanPreview->setText(tr("正在扫描，请稍候…（首次扫描设备预热可能较慢）"));
        m_scan->scan(devIdx, opts);
    });
    connect(m_btnOpenScan, &QPushButton::clicked, this, &MainWindow::onOpenScanImage);
    connect(m_btnSaveScan, &QPushButton::clicked, this, &MainWindow::onSaveScanImage);
    connect(m_scan, &ScannerManager::discoveryFinished,
            this, &MainWindow::onScanDiscovery);
    connect(m_scan, &ScannerManager::scanFinished,
            this, &MainWindow::onScanFinished);
}

void MainWindow::setupAdvancedPage()
{
    m_advanced = new AdvancedSettings(this);
    m_pages->addWidget(m_advanced);
}

void MainWindow::updateEnvStatus()
{
    const QString base = "border-radius: 12px; padding: 10px; font-size: 11px; line-height: 1.4; ";
    const QString border = "border: 1px solid ";
    switch (m_env->overall()) {
    case EnvChecker::Status::Ok:
        m_envStatus->setText(tr("<b>● 环境就绪</b><br/>") + m_env->summary());
        m_envStatus->setStyleSheet(base + border + "rgba(37,184,100,0.35); "
            "background: rgba(37,184,100,0.08); color: #15803d;");
        break;
    case EnvChecker::Status::Warn:
        m_envStatus->setText(tr("<b>● 部分就绪</b><br/>") + m_env->summary());
        m_envStatus->setStyleSheet(base + border + "rgba(245,158,11,0.40); "
            "background: rgba(245,158,11,0.10); color: #b45309;");
        break;
    default:
        m_envStatus->setText(tr("<b>● 不可用</b><br/>") + m_env->summary());
        m_envStatus->setStyleSheet(base + border + "rgba(239,68,68,0.35); "
            "background: rgba(239,68,68,0.08); color: #b91c1c;");
        break;
    }
}

void MainWindow::refreshEnv()
{
    if (m_env->isCheckRunning())
        return;

    m_envModel->clear();
    m_envModel->appendRow(makeItem(tr("正在检测环境…"), tr("请稍候"), QColor(150, 155, 165), "⟳"));
    if (m_btnQuickCheck)
        m_btnQuickCheck->setEnabled(false);
    m_env->checkAsync();
}

void MainWindow::onBasicEnvCheckFinished()
{
    if (m_btnQuickCheck)
        m_btnQuickCheck->setEnabled(true);
    updateServiceCards();
    updateEnvStatus();

    m_envModel->clear();
    static const QRegularExpression usbRe(
        R"(Bus\s+(\d+)\s+Device\s+(\d+)\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s+(.+))");
    const QStringList devices = m_env->supportedDevices();
    if (devices.isEmpty()) {
        m_envModel->appendRow(makeItem(tr("未检测到打印/扫描类 USB 设备"),
                                       tr("插上设备后点「重新检测」"), QColor(245, 158, 11), "!"));
    } else {
        for (const QString &dev : devices) {
            const auto m = usbRe.match(dev);
            if (m.hasMatch()) {
                m_envModel->appendRow(makeItem(m.captured(5).trimmed(),
                    QString(tr("USB 总线 %1 · 设备 %2 · ID %3:%4 · IPP-USB 候选"))
                        .arg(m.captured(1), m.captured(2),
                             m.captured(3), m.captured(4)),
                    QColor(0, 129, 255), "B"));
            } else {
                m_envModel->appendRow(makeItem(dev, tr("IPP-USB 候选设备"),
                                               QColor(0, 129, 255), "B"));
            }
        }
    }
}

void MainWindow::refreshPrint()
{
    m_printModel->clear();
    m_printModel->appendRow(makeItem(tr("正在发现打印机…"),
                                     tr("driverless 扫描 + CUPS 队列合并，请稍候"),
                                     QColor(150, 155, 165), "⟳"));
    m_print->discover();
    // discover 是异步的，完成后由 fillPrintList() 填充真实列表
}

void MainWindow::fillPrintList()
{
    m_printModel->clear();

    const auto &list = m_print->printers();
    bool hasQueue = false, hasPending = false;
    for (const auto &p : list) {
        if (p.isQueue) hasQueue = true;
        else hasPending = true;
    }

    if (hasQueue) {
        m_printModel->appendRow(makeItem(tr("== 已配置打印队列 =="), QString(),
                                         QColor(150, 155, 165)));
        for (const auto &p : list) {
            if (!p.isQueue) continue;
            QColor accent = p.isDefault ? QColor(37, 184, 100) : QColor(0, 129, 255);
            QString subtitle = p.subtitle;
            if (!p.ppdMakeModel.isEmpty())
                subtitle += "  ·  " + p.ppdMakeModel;
            auto *item = makeItem(p.title, subtitle, accent,
                                  p.isDefault ? "★" : "P");
            item->setData(p.name, Qt::UserRole + 2);
            item->setData(p.isQueue, Qt::UserRole + 3);
            m_printModel->appendRow(item);
        }
    }
    if (hasPending) {
        m_printModel->appendRow(makeItem(tr("== 发现的免驱设备（待添加） =="), QString(),
                                         QColor(150, 155, 165)));
        for (const auto &p : list) {
            if (p.isQueue) continue;
            // 只显示真正支持 IPP 的免驱设备，过滤 socket/lpd/smb/_pdl-datastream 等
            if (!p.isIpp())
                continue;
            auto *item = makeItem(p.title, p.subtitle,
                                  QColor(245, 158, 11), "+");
            item->setData(p.name, Qt::UserRole + 2);
            item->setData(p.isQueue, Qt::UserRole + 3);
            m_printModel->appendRow(item);
        }
    }
    if (!hasQueue && !hasPending) {
        m_printModel->appendRow(makeItem(tr("尚未发现打印机"),
            tr("点击上方「发现设备」搜索免驱打印机；已配置的 CUPS 队列会自动显示"),
            QColor(150, 155, 165), "!"));
    }
}

void MainWindow::onAddPrinter()
{
    if (m_print->addPrinterInProgress())
        return;

    // 收集所有"待添加"免驱设备（仅真正支持 IPP 的地址）
    const auto &list = m_print->printers();
    QList<PrinterEntry> pending;
    for (const auto &p : list) {
        if (p.isQueue) continue;
        if (!p.isIpp())
            continue;
        pending.append(p);
    }
    if (pending.isEmpty()) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
            tr("当前没有待添加的免驱设备。\n请先点「发现设备」，并确保 ipp-usb 已运行、设备已连接。"));
        return;
    }

    // 直接弹出 dde-printer 风格的对话框（参考图）。
    // 添加成功后关闭对话框并轻量刷新列表；如需再次添加重新点按钮即可。
    AddPrinterDialog dlg(pending, m_print, this);
    const int rc = dlg.exec();
    if (rc != QDialog::Accepted) return;

    const QString name = dlg.addedQueueName();
    const QString uri  = dlg.property("uri").toString();
    const QString driver = dlg.property("driver").toString();
    const QString prettyName = dlg.property("prettyName").toString();
    if (name.isEmpty() || uri.isEmpty()) return;

    m_btnAddPrinter->setEnabled(false);
    m_btnAddPrinter->setText(tr("安装") + QStringLiteral("…"));
    m_print->addPrinterAsync(name, uri, driver, prettyName);
}

void MainWindow::onAddPrinterFinished(const PrinterAddResult &result)
{
    m_btnAddPrinter->setEnabled(true);
    m_btnAddPrinter->setText(tr("添加打印机"));

    if (result.ok) {
        PrinterEntry matched;
        for (const auto &p : m_print->printers()) {
            if (p.uri == result.uri) {
                matched = p;
                break;
            }
        }
        if (!result.makeModel.isEmpty())
            matched.makeAndModel = result.makeModel;
        showAddSuccessToast(result.name, matched);
        // 轻量刷新：只重读 CUPS 队列（秒级），让新添加的打印机立即显示。
        m_print->refreshQueuesOnly();
    } else {
        DMessageBox::critical(this, tr("添加失败"),
            QString(tr("无法添加打印机 %1：\n\n%2\n\n"
                       "可能原因：\n"
                       "· ipp-usb 服务未运行（请到「环境检测」启动）\n"
                       "· 设备已被其他应用独占\n"
                       "· 当前用户在 lpadmin 组外（无权限管理队列）\n"
                       "· 驱动模型不匹配（可手动选择别的驱动重试）"))
                .arg(result.name, result.error));
    }
}

void MainWindow::showAddSuccessToast(const QString &queue, const PrinterEntry &e)
{
    // 紧凑型顶部提示条：单行布局（小图标 + 文案 + 操作按钮），不遮挡内容区
    auto *toast = new QFrame(this);
    toast->setObjectName("addSuccessToast");
    toast->setStyleSheet(
        "QFrame#addSuccessToast {"
        "  background-color: #ffffff;"
        "  border: 1px solid rgba(0, 0, 0, 0.08);"
        "  border-radius: 10px;"
        "}");

    auto *lay = new QHBoxLayout(toast);
    lay->setContentsMargins(16, 10, 16, 10);
    lay->setSpacing(12);

    // 小绿勾
    auto *check = new DLabel("✓");
    check->setObjectName("addSuccessCheck");
    check->setFixedSize(28, 28);
    check->setAlignment(Qt::AlignCenter);
    check->setStyleSheet(
        "background: #16a34a;"
        "color: #ffffff; border-radius: 14px;"
        "font-size: 16px; font-weight: 900;");

    // 文案区（标题 + 驱动一行小字）
    auto *textCol = new QVBoxLayout;
    textCol->setSpacing(2);
    auto *title = new DLabel(QString(tr("安装成功：%1")).arg(queue));
    title->setObjectName("addSuccessTitle");
    title->setStyleSheet("color: #1f2937; font-size: 14px; font-weight: 600;");
    auto *subtitle = new DLabel(QString(tr("驱动：%1")).arg(e.makeAndModel));
    subtitle->setObjectName("addSuccessSubtitle");
    subtitle->setStyleSheet("color: #6b7280; font-size: 12px;");
    textCol->addWidget(title);
    textCol->addWidget(subtitle);

    auto *btnTest = new DSuggestButton(tr("打印测试页"));
    btnTest->setObjectName("addSuccessTest");
    btnTest->setFixedHeight(30);
    btnTest->setFixedWidth(96);
    auto *btnClose = new QPushButton(tr("取消"));
    btnClose->setObjectName("addSuccessClose");
    btnClose->setFixedHeight(30);
    btnClose->setFixedWidth(64);
    connect(btnClose, &QPushButton::clicked, toast, &QFrame::deleteLater);

    lay->addWidget(check);
    lay->addLayout(textCol, 1);
    lay->addWidget(btnTest);
    lay->addWidget(btnClose);

    // 底部居中（类似系统通知位置，不遮挡顶部按钮与列表）
    toast->adjustSize();
    const int margin = 24;
    const int x = (width() - toast->width()) / 2;
    const int y = height() - toast->height() - margin;
    toast->move(qMax(margin, x), qMax(margin, y));
    toast->raise();
    toast->show();

    connect(btnTest, &QPushButton::clicked, this, [this, queue, toast]() {
        QString err;
        // 先关提示条，再弹结果框，避免两个浮层叠在一起
        toast->deleteLater();
        if (m_print->printTestPage(queue, &err))
            DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                                     tr("已发送测试页到：") + queue);
        else
            DMessageBox::warning(this, tr("IPP-USB 免驱助手"), err);
    });

    // 6 秒自动消失
    QTimer::singleShot(6000, toast, &QFrame::deleteLater);
}

QString MainWindow::selectedPrinterName() const
{
    const QModelIndex idx = m_printerView ? m_printerView->currentIndex() : QModelIndex();
    if (!idx.isValid()) return {};
    return idx.data(Qt::UserRole + 2).toString();
}

void MainWindow::onPrinterSelectionChanged()
{
    const QModelIndex idx = m_printerView ? m_printerView->currentIndex() : QModelIndex();
    bool enabled = false;
    if (idx.isValid())
        enabled = idx.data(Qt::UserRole + 3).toBool();
    if (m_btnDeletePrinter) m_btnDeletePrinter->setEnabled(enabled);
    if (m_btnPrinterProps)  m_btnPrinterProps->setEnabled(enabled);
    if (m_btnDriverTweak)   m_btnDriverTweak->setEnabled(enabled);
}

void MainWindow::onDeletePrinter()
{
    const QString name = selectedPrinterName();
    if (name.isEmpty()) return;
    auto *ask = new DMessageBox(this);
    ask->setIcon(DMessageBox::Warning);
    ask->setText(QString(tr("确定要删除打印队列「%1」吗？\n\n"
                            "这将一并移除队列的 PPD 配置与历史任务，已打印文件不会受影响。"))
                      .arg(name));
    QPushButton *ok = ask->addButton(tr("删除"), QMessageBox::DestructiveRole);
    ask->addButton(tr("取消"), QMessageBox::RejectRole);
    ok->setDefault(false);
    ask->setWindowTitle(tr("删除打印机"));
    ask->exec();
    if (ask->clickedButton() != ok) return;

    QString err;
    if (!m_print->removePrinter(name, err)) {
        DMessageBox::critical(this, tr("删除失败"),
            QString(tr("无法删除队列「%1」：\n%2")).arg(name, err));
        return;
    }
    DMessageBox::information(this, tr("IPP-USB 免驱助手"),
        QString(tr("队列「%1」已删除。")).arg(name));
    refreshPrint();
}

// 新增“打印属性”：读取 PPD 当前选项并允许用户调整
void MainWindow::onShowPrinterProperties()
{
    const QString name = selectedPrinterName();
    if (name.isEmpty()) return;
    PrintPropertiesDialog dlg(name, this);
    dlg.exec();
}

// 原“打印属性”按钮改名为“驱动微调”，功能保持 PrinterConfigDialog
void MainWindow::onDriverTweak()
{
    const QString name = selectedPrinterName();
    if (name.isEmpty()) return;
    PrinterConfigDialog dlg(name, this);
    dlg.exec();
}

void MainWindow::onTestPage()
{
    const QString name = selectedPrinterName();
    if (name.isEmpty()) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"), tr("请先选择一个打印机。"));
        return;
    }
    QString err;
    if (m_print->printTestPage(name, &err))
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
                                 tr("已发送测试页到：") + name);
    else
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"), err);
}

void MainWindow::refreshScan()
{
    if (m_scanBusy)
        return;

    m_scanModel->clear();
    m_scanModel->appendRow(makeItem(tr("正在发现扫描仪（约需 10~20 秒，请稍候）…"),
        QString(), QColor(118, 126, 140), "S"));
    m_scan->discover();
}

void MainWindow::onScanDiscovery(bool found)
{
    m_scanModel->clear();
    const auto devs = m_scan->devices();
    if (!found) {
        m_scanModel->appendRow(makeItem(
            tr("未发现扫描仪：请确认设备支持 eSCL、ipp-usb 与 avahi-daemon 已运行"),
            QString(), QColor(240, 65, 66), "S"));
        return;
    }
    for (int i = 0; i < devs.size(); ++i) {
        // 双保险：部分 SANE 后端返回的设备 URI 含 IPv6 字面量 [::1]，
        // scanimage 无法打开。即使 ScannerManager 已做替换，这里再处理一次，
        // 确保 UI 显示与 scanimage 实际可用形式一致（localhost）。
        QString display = devs.at(i);
        if (display.contains(QLatin1String("[::1]")))
            display.replace(QLatin1String("[::1]"), QLatin1String("localhost"));

        // 按后端如实标注：免驱 eSCL 与厂商原生 SANE 驱动是两类东西，
        // 混为一谈会让用户（和支持人员）误判设备的免驱能力。
        const ScanBackend be = m_scan->backend(i);
        QString sub;
        QColor color(0, 166, 156);
        if (be == ScanBackend::Escl) {
            sub = tr("免驱 eSCL 扫描设备（IPP-USB）");
        } else if (be == ScanBackend::Vendor) {
            sub = tr("厂商自带 SANE 驱动（非免驱）");
            color = QColor(0, 129, 200);
        } else {
            sub = tr("SANE 扫描设备");
        }
        m_scanModel->appendRow(makeItem(display, sub, color, "S"));
    }
    // 默认选中第一台设备，方便直接扫描
    if (m_scannerView && m_scanModel->rowCount() > 0)
        m_scannerView->setCurrentIndex(m_scanModel->index(0, 0));
}

void MainWindow::onScanFinished(bool ok, const QString &outPath, const QString &errMsg)
{
    m_scanBusy = false;
    if (ok) {
        QPixmap img(outPath);
        if (!img.isNull()) {
            // 按预览区宽度等比缩放显示
            m_scanPreview->setPixmap(img.scaled(m_scanPreview->width(),
                m_scanPreview->height(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            m_scanPreview->setText(tr("扫描完成，但图片无法加载：") + outPath);
        }
        m_lastScanPath = outPath;
        m_btnOpenScan->setEnabled(true);
        m_btnSaveScan->setEnabled(true);
    } else {
        m_scanPreview->setText(tr("扫描失败：") + errMsg);
        DMessageBox::critical(this, tr("IPP-USB 免驱助手"), tr("扫描失败：") + errMsg);
    }
}

void MainWindow::onOpenScanImage()
{
    if (m_lastScanPath.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastScanPath));
}

void MainWindow::onSaveScanImage()
{
    if (m_lastScanPath.isEmpty())
        return;
    QImage img(m_lastScanPath);
    if (img.isNull()) {
        DMessageBox::warning(this, tr("IPP-USB 免驱助手"), tr("扫描完成，但图片无法加载：") + m_lastScanPath);
        return;
    }

    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                         + "/scan_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    const QString path = QFileDialog::getSaveFileName(this, tr("保存扫描文件"), base + ".pdf",
        tr("PDF 文档 (*.pdf);;PNG 图片 (*.png);;JPEG 图片 (*.jpg)"));
    if (path.isEmpty())
        return;

    bool ok = false;
    QString err;
    if (path.endsWith(".pdf", Qt::CaseInsensitive)) {
        // 图片嵌入 PDF（A4 页面，等比居中）
        QPdfWriter pdf(path);
        pdf.setPageSize(QPageSize(QPageSize::A4));
        pdf.setPageMargins(QMarginsF(10, 10, 10, 10));
        QPainter painter(&pdf);
        if (painter.isActive()) {
            const QRectF area = painter.viewport();
            QImage scaled = img.scaled(area.size().toSize(), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
            const qreal x = area.x() + (area.width() - scaled.width()) / 2.0;
            const qreal y = area.y() + (area.height() - scaled.height()) / 2.0;
            painter.drawImage(QPointF(x, y), scaled);
            painter.end();
            ok = true;
        } else {
            err = tr("无法写入文件：") + path;
        }
    } else if (path.endsWith(".jpg", Qt::CaseInsensitive)
               || path.endsWith(".jpeg", Qt::CaseInsensitive)) {
        ok = img.save(path, "JPEG", 92);
    } else {
        ok = img.save(path, "PNG");
    }

    if (ok)
        DMessageBox::information(this, tr("IPP-USB 免驱助手"), tr("已保存到：") + path);
    else if (err.isEmpty())
        err = tr("保存失败，请检查文件权限。");
    if (!err.isEmpty())
        DMessageBox::critical(this, tr("IPP-USB 免驱助手"), err);
}

namespace
{
    // 作者名单。不是 URL，因此不作为链接目标使用。
    const QString &authorsText()
    {
        static const QString s = QStringLiteral("克亮、deepin-skills、hy3(free)、ox(free)");
        return s;
    }

    // DAboutDialog 里 "Homepage"（中文环境显示“主页”）这个左侧标签由 DTK 库
    // 内部创建，没有任何公开 API 能改它的文本。
    //
    // 实测控件树（由 DAboutDialogPrivate::updateWebsiteLabel 生成）：
    //     QLabel  (objectName 为空)  text="Homepage"        ← 我们要改名的标签
    //     QLabel  objectName="WebsiteLabel"
    //             text="<a href='websiteLink'>websiteName</a>"
    // 注意：setWebsiteName() 改的是右侧链接的**显示文本**，不是左侧标签。
    // 两者在对象树中相邻，故用“WebsiteLabel 的前一个兄弟”定位，与语言无关。
    class AboutAuthorRelabeler : public QObject
    {
    public:
        explicit AboutAuthorRelabeler(DAboutDialog *dlg)
            : QObject(dlg)
            , m_dlg(dlg)
        {
            dlg->installEventFilter(this);
        }

        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (watched == m_dlg && !m_done
                && (event->type() == QEvent::Show
                    || event->type() == QEvent::ShowToParent)) {
                m_done = true;
                // 内容区位于 QScrollArea 内，Show 时才完成布局，延后一拍改名
                QTimer::singleShot(0, this, [this]() { apply(); });
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        void apply()
        {
            const QList<QLabel *> labels = m_dlg->findChildren<QLabel *>();

            int idx = -1;
            for (int i = 0; i < labels.size(); ++i) {
                if (labels.at(i)->objectName() == QLatin1String("WebsiteLabel")) {
                    idx = i;
                    break;
                }
            }
            if (idx <= 0)
                return;

            QLabel *caption = labels.at(idx - 1);
            // 结构校验：该标签 objectName 为空，确认没有误伤其他标签
            if (!caption->objectName().isEmpty())
                return;

            // 把左侧"主页"标签改名为"作者"，并用富文本加粗 + 主题色，
            // 使其与版本/描述等字段的 label 相比更突出。
            caption->setTextFormat(Qt::RichText);
            caption->setText(QObject::tr("<span style='color:#0066cc; font-weight:700;'>作者</span>"));

            // 作者名单不是网址，避免被当作可点击链接打开
            QLabel *value = labels.at(idx);
            value->setText(authorsText());
            value->setOpenExternalLinks(false);
            value->setTextInteractionFlags(Qt::NoTextInteraction);
        }

        DAboutDialog *m_dlg;
        bool m_done{false};
    };
}

void MainWindow::setupAboutDialog(DAboutDialog *dlg)
{
    dlg->setProductName(tr("IPP-USB 免驱助手"));
    dlg->setVersion(IPP_USB_ASSISTANT_VERSION);
    dlg->setProductIcon(QIcon(":/icons/ipp-usb-assistant.svg"));
    dlg->setDescription(tr(
        "面向 deepin 25 的打印机与扫描仪管理工具，"
        "基于 IPP-USB 协议实现免驱动打印与扫描。"));
    dlg->setLicense(tr("本程序基于 GNU 通用公共许可证第 3 版（GPLv3）发布。"));

    // websiteName 是右侧显示文本 → 放作者名单
    dlg->setWebsiteName(authorsText());
    // 名单不是网址，链接留空，避免点击后尝试打开无效地址
    dlg->setWebsiteLink(QString());

    // “主页”标签由库内建，改名需直接操作控件树
    new AboutAuthorRelabeler(dlg);
}

void MainWindow::showAbout()
{
    if (!m_aboutDialog) {
        m_aboutDialog = new DAboutDialog(this);
        setupAboutDialog(m_aboutDialog);
    }
    m_aboutDialog->exec();
}

void MainWindow::disableButtonFocus()
{
    for (QAbstractButton *btn : findChildren<QAbstractButton *>()) {
        if (btn)
            btn->setFocusPolicy(Qt::NoFocus);
    }
}
