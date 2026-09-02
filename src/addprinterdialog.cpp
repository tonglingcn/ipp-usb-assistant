#include "addprinterdialog.h"
#include "printmanager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QComboBox>
#include <QStandardItemModel>
#include <QListView>
#include <QStandardItem>
#include <QAbstractItemView>
#include <QProcess>
#include <QTimer>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QRegularExpression>
#include <QTimer>

#include <DLabel>
#include <DListView>
#include <DSuggestButton>
#include <DMessageBox>
#include <DFrame>
#include <DFontSizeManager>

#include <cups/cups.h>
#include "qtcompat.h"

DWIDGET_USE_NAMESPACE

AddPrinterDialog::AddPrinterDialog(const QList<PrinterEntry> &candidates,
                                   PrintManager *pm,
                                   QWidget *parent)
    : QDialog(parent)
    , m_candidates(candidates)
    , m_printManager(pm)
{
    setWindowTitle(tr("添加打印机"));
    setFixedSize(580, 460);
    buildUi();
    // 居中到父窗口
    if (parent)
        move(parent->geometry().center() - geometry().center());
    if (m_printManager) {
        connect(m_printManager, &PrintManager::discoveryFinished,
                this, &AddPrinterDialog::onDiscoveryFinished);
    }
    QTimer::singleShot(0, this, &AddPrinterDialog::onAutoRescan);

    for (QAbstractButton *btn : findChildren<QAbstractButton *>()) {
        if (btn)
            btn->setFocusPolicy(Qt::NoFocus);
    }
}

void AddPrinterDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // —— 主体区域（去掉左侧来源 tab，只保留右侧选择区）
    auto *body = new QFrame;
    body->setObjectName("addBody");
    auto *bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(24, 16, 24, 16);
    bodyLay->setSpacing(10);

    auto *title = new DLabel(tr("选择打印机"));
    title->setObjectName("addTitle");
    m_btnRefresh = new QPushButton(tr("刷新"));
    m_btnRefresh->setObjectName("addRefresh");
    m_btnRefresh->setFixedHeight(28);
    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(title, 1);
    titleRow->addWidget(m_btnRefresh);
    bodyLay->addLayout(titleRow);

    // 简化的"已识别的免驱打印机"提示
    auto *hint = new DLabel(tr("已识别的免驱打印机（driverless / IPP Everywhere）"));
    hint->setObjectName("addHint");
    bodyLay->addWidget(hint);

    // 单一列表视图（自动发现）
    m_autoModel = new QStandardItemModel(this);
    m_autoView = new DListView;
    m_autoView->setModel(m_autoModel);
    m_autoView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_autoView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_autoView->setMinimumHeight(160);
    bodyLay->addWidget(m_autoView, 1);

    root->addWidget(body, 1);

    // —— 底部驱动 + 操作（队列名由系统自动生成，无需用户输入）
    auto *bottom = new QFrame;
    bottom->setObjectName("addBottom");
    auto *bLay = new QVBoxLayout(bottom);
    bLay->setContentsMargins(24, 14, 24, 16);
    bLay->setSpacing(10);

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(10);
    auto *drvLabel = new DLabel(tr("驱动："));
    m_driverCombo = new QComboBox;
    m_driverCombo->setObjectName("addDriverCombo");
    m_driverCombo->setFixedHeight(32);
    m_driverCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_driverCombo->setMinimumWidth(220);
    m_driverCombo->setToolTip(
        tr("驱动优先级：\n"
           "1. 设备专属 driverless（来自 lpinfo -m，最匹配）\n"
           "2. 通用 driverless（driverless:<uri> 兜底）\n"
           "3. IPP Everywhere 通用驱动\n"
           "点击右侧 \"…\" 手动选择本地 PPD"));
    fillDriverCombo();

    auto *drvRow = new QHBoxLayout;
    drvRow->setSpacing(4);
    drvRow->addWidget(m_driverCombo, 1);
    auto *btnPick = new QPushButton("…");
    btnPick->setObjectName("addPickPp");
    btnPick->setFixedSize(32, 32);
    btnPick->setToolTip(tr("手动选择本地 PPD 文件"));
    drvRow->addWidget(btnPick);

    row1->addWidget(drvLabel);
    row1->addLayout(drvRow, 1);

    auto *row2 = new QHBoxLayout;
    row2->addStretch();
    m_btnCancel = new QPushButton(tr("取消"));
    m_btnCancel->setObjectName("addCancel");
    m_btnCancel->setFixedHeight(34);
    m_btnCancel->setFixedWidth(96);
    m_btnAdd = new DSuggestButton(tr("安装驱动"));
    m_btnAdd->setObjectName("addInstall");
    m_btnAdd->setFixedHeight(34);
    m_btnAdd->setFixedWidth(140);
    row2->addWidget(m_btnCancel);
    row2->addWidget(m_btnAdd);

    bLay->addLayout(row1);
    bLay->addLayout(row2);
    root->addWidget(bottom);

    connect(m_btnRefresh, &QPushButton::clicked, this, &AddPrinterDialog::onRescan);
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_btnAdd, &QPushButton::clicked, this, &AddPrinterDialog::onAddClicked);
    connect(btnPick, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this,
            tr("选择 PPD 文件"), "/usr/share/cups/model", tr("PPD (*.ppd);;所有文件 (*)"));
        if (path.isEmpty()) return;
        m_driverCombo->insertItem(0,
            QString(tr("本地 PPD：%1")).arg(QFileInfo(path).fileName()),
            path);
        m_driverCombo->setCurrentIndex(0);
    });
    connect(m_autoView, &QListView::clicked, this,
            &AddPrinterDialog::selectCandidateFromList);

}

void AddPrinterDialog::buildAutoPage()
{
    // （已合并到 buildUi 中以简化布局）
}

void AddPrinterDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
}

QString AddPrinterDialog::selectedDriver() const
{
    return m_driverCombo->currentData().toString();
}

QString AddPrinterDialog::defaultNameFor(const QString &uri) const
{
    return PrintManager::makeDefaultName(uri);
}

void AddPrinterDialog::selectCandidateFromList()
{
    const auto idx = m_autoView->currentIndex();
    if (!idx.isValid()) return;
    const QString uri = idx.data(Qt::UserRole).toString();
    if (uri.isEmpty()) return;
    m_currentUri = uri;
    // 设备变了，重新匹配驱动
    fillDriverCombo();
}

void AddPrinterDialog::fetchLpinfoAsync()
{
    if (m_lpinfoFetched || m_lpinfoFetching)
        return;
    m_lpinfoFetching = true;
    auto *proc = new QProcess(this);

    // 超时保护：lpinfo -m 需要扫描全部 CUPS 驱动，在慢速环境（网络驱动、
    // 大量 PPD）下可能长时间无响应。此前没有超时，进程会一直挂着，
    // 驱动下拉框永远停在"获取中"。
    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);
    timer->setInterval(15000);
    connect(timer, &QTimer::timeout, this, [proc]() {
        if (proc->state() == QProcess::Running) {
            proc->kill();
            proc->waitForFinished(2000);
        }
    });

    proc->start("lpinfo", {"-m"});
    timer->start();
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError) {
        proc->deleteLater();
        m_lpinfoFetching = false;
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int exitCode, QProcess::ExitStatus st) {
        proc->deleteLater();
        m_lpinfoFetching = false;
        if (st != QProcess::NormalExit || exitCode != 0)
            return;
        const QString out = QString::fromLocal8Bit(proc->readAllStandardOutput());
        for (const QString &line : out.split('\n', kSkipEmptyParts)) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            // 第一个空格分两段：id + 描述
            const int first = trimmed.indexOf(' ');
            if (first <= 0) continue;
            LpDriver d;
            d.id = trimmed.left(first);
            d.label = trimmed.mid(first + 1).trimmed();
            if (!d.label.isEmpty())
                m_allDrivers.append(d);
        }
        m_lpinfoFetched = true;
        // 缓存就绪后重刷下拉框（此时不阻塞，立即完成）
        fillDriverCombo();
    });
}

void AddPrinterDialog::fillDriverCombo()
{
    // 记住当前选择，重刷后尽量恢复
    const QString prevDriver = selectedDriver();

    m_driverCombo->clear();

    // 当前选中设备的 make/model + URI
    const QString currentUri = m_currentUri;
    QString deviceMake;
    for (const PrinterEntry &e : m_candidates) {
        if (e.uri == currentUri) {
            deviceMake = e.makeAndModel;
            break;
        }
    }
    if (deviceMake.isEmpty()) {
        for (const PrinterEntry &e : m_candidates) {
            deviceMake = e.makeAndModel;
            if (!deviceMake.isEmpty()) break;
        }
    }
    // 取 make 主段（去 Series / 一串数字）
    QString makeShort;
    if (!deviceMake.isEmpty()) {
        QStringList tokens = deviceMake.split(' ', kSkipEmptyParts);
        // 第一段往往是厂商，第二段是型号前缀
        if (tokens.size() >= 2) makeShort = tokens[0] + " " + tokens[1];
        else makeShort = tokens.value(0);
    }

    // 只显示免驱相关驱动（参考 dde-printer 优先级）：
    //   1) 设备专属 driverless（lpinfo -m 命中当前 make/model，最匹配）
    //   2) 通用 driverless:<uri>（自动适配当前设备）
    //   3) IPP Everywhere 通用兜底（everywhere）
    //   4) Generic 厂商通用 PPD（lpinfo -m 异步缓存，就绪后自动补上）
    int selIdx = -1;

    // 1) 设备专属 driverless 放最前并默认选中
    if (!makeShort.isEmpty()) {
        for (const LpDriver &d : std::as_const(m_allDrivers)) {
            if (!d.id.startsWith("driverless:", Qt::CaseInsensitive))
                continue;
            if (!d.label.contains(makeShort, Qt::CaseInsensitive))
                continue;
            m_driverCombo->addItem(QString(tr("● 设备专属驱动：%1")).arg(d.label), d.id);
            if (selIdx < 0) selIdx = 0;
            break;   // 只取最匹配的一条
        }
    }

    // 2) 通用 driverless:<uri>
    if (!currentUri.isEmpty()) {
        const QString drvLess = QString("driverless:%1").arg(currentUri);
        m_driverCombo->addItem(tr("● 通用 driverless（自动适配）"), drvLess);
        if (selIdx < 0) selIdx = m_driverCombo->count() - 1;
    }

    // 3) IPP Everywhere 通用兜底
    m_driverCombo->addItem(tr("● IPP Everywhere（通用兜底）"), "everywhere");
    if (selIdx < 0) selIdx = m_driverCombo->count() - 1;

    // 4) Generic 厂商通用 PPD（免驱兜底，其余厂商驱动不显示）
    for (const LpDriver &d : std::as_const(m_allDrivers)) {
        if (m_driverCombo->findData(d.id) >= 0) continue;
        if (!d.label.startsWith("Generic", Qt::CaseInsensitive)) continue;
        m_driverCombo->addItem(d.label, d.id);
    }

    if (m_driverCombo->count() == 0)
        m_driverCombo->addItem("everywhere", "everywhere");

    // 恢复之前的选择；无记录时默认选最匹配项
    int restore = m_driverCombo->findData(prevDriver);
    if (restore >= 0)
        m_driverCombo->setCurrentIndex(restore);
    else if (selIdx >= 0)
        m_driverCombo->setCurrentIndex(selIdx);
    else
        m_driverCombo->setCurrentIndex(0);

    // lpinfo 尚未加载：后台异步获取，就绪后自动补上 Generic 驱动（不阻塞 UI）
    fetchLpinfoAsync();
}

void AddPrinterDialog::onAutoRescan()
{
    m_autoModel->clear();
    for (const PrinterEntry &e : m_candidates) {
        auto *item = new QStandardItem;
        item->setData(e.uri, Qt::UserRole);
        const QString title = e.makeAndModel.isEmpty()
                                  ? PrintManager::prettyNameFromUri(e.uri)
                                  : e.makeAndModel;
        item->setData(title, Qt::DisplayRole);
        QString sub = QString(tr("协议：%1")).arg(e.protocols.join("/"));
        if (e.everywhere)
            sub += tr("  ·  免驱（IPP Everywhere）");
        item->setData(sub, Qt::UserRole + 1);
        // UserRole+2 单独保存 makeAndModel 原始值，便于添加队列时取出来作为
        // lpadmin -D 的 printer-info，以及生成更具品牌感的队列名。
        item->setData(e.makeAndModel, Qt::UserRole + 2);
        m_autoModel->appendRow(item);
    }
    if (m_autoModel->rowCount() == 0) {
        auto *item = new QStandardItem(tr("未发现可添加的设备，请确认 ipp-usb 已运行"));
        item->setEnabled(false);
        m_autoModel->appendRow(item);
    }
    // 默认选中第一项
    if (m_autoModel->rowCount() > 0) {
        auto idx = m_autoModel->index(0, 0);
        m_autoView->setCurrentIndex(idx);
        // 自动填好当前 URI
        const QString uri = idx.data(Qt::UserRole).toString();
        m_currentUri = uri;
        fillDriverCombo();
    }
}

void AddPrinterDialog::onAddClicked()
{
    if (m_candidates.isEmpty()) {
        DMessageBox::warning(this, tr("添加打印机"), tr("没有可用的免驱打印机。"));
        return;
    }
    const auto idx = m_autoView->currentIndex();
    QString uri = idx.isValid()
                  ? idx.data(Qt::UserRole).toString()
                  : m_candidates.first().uri;
    if (uri.isEmpty()) {
        DMessageBox::warning(this, tr("添加打印机"), tr("请先在列表中选择一台打印机。"));
        return;
    }
    // 取候选的 makeAndModel（驱动反查拿到的真实型号，如 "Pantum BM4240ADW Series"），
    // 用于 lpadmin -D（让"描述"显示真实型号）以及生成更具品牌感的队列名。
    const auto chosen = idx.isValid() ? idx.data(Qt::UserRole + 2).toString()
                                       : m_candidates.first().makeAndModel;

    // 队列名由系统自动生成；优先用真实品牌型号生成（如 "Pantum-BM4240ADW-Series"），
    // 重名时追加 -2/-3，支持同一设备用不同免驱驱动反复添加做对比测试
    QString name = PrintManager::makeDefaultName(uri, chosen);

    cups_dest_t *dests = nullptr;
    int n = cupsGetDests(&dests);
    QSet<QString> existing;
    for (int i = 0; i < n; ++i)
        existing.insert(QString::fromLocal8Bit(dests[i].name));
    cupsFreeDests(n, dests);
    if (existing.contains(name)) {
        for (int i = 2; ; ++i) {
            const QString cand = name + "-" + QString::number(i);
            if (!existing.contains(cand)) {
                name = cand;
                break;
            }
        }
    }

    m_addedName = name;
    setProperty("uri", uri);
    setProperty("driver", selectedDriver());
    setProperty("prettyName", chosen);
    accept();
}

void AddPrinterDialog::onRescan()
{
    if (!m_printManager)
        return;
    m_btnRefresh->setEnabled(false);
    m_btnRefresh->setText(tr("发现中…"));
    m_btnAdd->setEnabled(false);
    m_printManager->discover();
}

void AddPrinterDialog::onDiscoveryFinished()
{
    if (!m_printManager)
        return;

    // 只取尚未配置为 CUPS 队列的待添加设备，且仅保留真正支持 IPP 免驱的地址。
    // dnssd 中的 _pdl-datastream._tcp / _printer._tcp 等原始服务会被过滤，
    // 避免用户选中后 driverless 无法生成 PPD 而 30 秒超时。
    QList<PrinterEntry> pending;
    const QList<PrinterEntry> printers = m_printManager->printers();
    for (const PrinterEntry &e : printers) {
        if (e.isQueue) continue;
        if (!e.isIpp())
            continue;
        pending.append(e);
    }
    m_candidates = pending;

    onAutoRescan();

    m_btnRefresh->setEnabled(true);
    m_btnRefresh->setText(tr("刷新"));
    m_btnAdd->setEnabled(!m_candidates.isEmpty());
}
