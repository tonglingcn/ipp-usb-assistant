#include "printpropertiesdialog.h"
#include "printmanager.h"
#include "privileges.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <DLabel>
#include <QCoreApplication>
#include <DMessageBox>
#include "qtcompat.h"

DWIDGET_USE_NAMESPACE

struct OptionMeta {
    const char *name;
    QString label;
    QString group;
};

// name -> 中文标签 + 所属分组
static const OptionMeta OPTION_METAS[] = {
    { "PageSize", "纸张大小", "纸张" },
    { "MediaType", "纸张类型", "纸张" },
    { "InputSlot", "纸张来源", "纸张" },
    { "ptInputSlot", "纸张来源", "纸张" },
    { "FeedEdge", "进纸方向", "纸张" },

    { "ColorModel", "颜色", "质量" },
    { "OutputMode", "打印模式", "质量" },
    { "cupsPrintQuality", "打印质量", "质量" },
    { "PrintQuality", "打印质量", "质量" },
    { "Resolution", "分辨率", "质量" },
    { "Density", "浓度", "质量" },
    { "TonerMode", "省墨", "质量" },
    { "TonerSave", "省墨", "质量" },
    { "EconoMode", "省墨", "质量" },
    { "Economode", "省墨", "质量" },
    { "FineMode", "精细模式", "质量" },

    { "Duplex", "双面打印", "双面" },
    { "JCLDuplex", "双面打印", "双面" },
    { "cupsBackSide", "双面打印后端", "双面" },

    { "OrientationRequested", "方向", "布局" },
    { "LandscapeOrientation", "方向", "布局" },
    { "OutputOrder", "打印顺序", "布局" },
    { "number-up", "每页版数", "布局" },
    { "Collate", "逐份打印", "布局" },
    { "print-scaling", "打印缩放", "布局" },

    { "OutputBin", "输出托盘", "输出" },
    { "OutputTray", "出纸托盘", "输出" },

    { "SkipBlank", "跳过空白页", "高级" },
    { "SkipBlankPage", "跳过空白页", "高级" },
};

static const OptionMeta *findMeta(const QString &name)
{
    for (const auto &e : OPTION_METAS) {
        if (name.compare(QLatin1String(e.name), Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

static QString optionLabel(const QString &name)
{
    const auto *m = findMeta(name);
    return m ? QCoreApplication::translate("PrintPropertiesDialog", m->label.toUtf8().constData()) : name;
}

static QString optionGroup(const QString &name)
{
    const auto *m = findMeta(name);
    return m ? QCoreApplication::translate("PrintPropertiesDialog", m->group.toUtf8().constData()) : QCoreApplication::translate("PrintPropertiesDialog", "高级");
}

// 兼容中英文冒号 ':' / '：'，取第一个冒号后的内容
static QString valueAfterColon(const QString &line)
{
    for (int i = 0; i < line.length(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char(':') || c == QChar(0xFF1A))
            return line.mid(i + 1).trimmed();
    }
    return QString();
}

PrintPropertiesDialog::PrintPropertiesDialog(const QString &queue, QWidget *parent)
    : DDialog(parent), m_queue(queue)
{
    setWindowTitle(tr("打印属性 - ") + queue);
    setFixedSize(820, 580);

    loadBasicInfo();
    loadOptions();
    buildUi();

    addButton(tr("取消"), false);
    addButton(tr("应用"), true, DDialog::ButtonRecommend);

    connect(this, &DDialog::buttonClicked, this, [this](int idx) {
        if (idx == 1)
            onAccepted();
    });
}

void PrintPropertiesDialog::loadBasicInfo()
{
    QProcess proc;
    proc.start("lpstat", {"-l", "-p", m_queue});
    proc.waitForFinished(8000);
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    const QStringList lines = out.split('\n');

    for (const QString &line : lines) {
        const QString t = line.trimmed();
        // 兼容中英文 lpstat -l -p 输出
        if (t.startsWith(QLatin1String("Description:"), Qt::CaseInsensitive) ||
            t.startsWith(QStringLiteral("描述：")) ||
            t.startsWith(QStringLiteral("描述:"))) {
            m_description = valueAfterColon(t);
        } else if (t.startsWith(QLatin1String("Location:"), Qt::CaseInsensitive) ||
                   t.startsWith(QStringLiteral("位置：")) ||
                   t.startsWith(QStringLiteral("位置:"))) {
            m_location = valueAfterColon(t);
        } else if (t.startsWith(QLatin1String("URI:"), Qt::CaseInsensitive) ||
                   t.startsWith(QLatin1String("Device URI:"), Qt::CaseInsensitive)) {
            m_uri = valueAfterColon(t);
        }
    }
    if (m_uri.isEmpty()) {
        // 兜底：尝试 lpstat -v（中英文格式均兼容）。
        // 输出形如：
        //   用于 queue 的设备：ipps://Pantum%20...
        //   device for queue: ipps://Pantum%20...
        QProcess p2;
        p2.start("lpstat", {"-v", m_queue});
        p2.waitForFinished(5000);
        const QString v = QString::fromLocal8Bit(p2.readAllStandardOutput()).trimmed();
        int pos = v.indexOf(m_queue);
        if (pos >= 0) {
            // 取队列名之后的第一个冒号（: 或 ：）后的内容
            const QString after = v.mid(pos + m_queue.length());
            m_uri = valueAfterColon(after);
        }
    }
    // 对 URL 编码的 URI（如 %20）做解码，显示成人类可读形式
    if (!m_uri.isEmpty())
        m_uri = QUrl::fromPercentEncoding(m_uri.toUtf8());

    m_driver = PrintManager::ppdManufacturer(m_queue);
    if (m_driver.isEmpty())
        m_driver = PrintManager::ppdModel(m_queue);
    if (m_driver.isEmpty())
        m_driver = PrintManager::ppdDriverPath(m_queue);
}

void PrintPropertiesDialog::loadOptions()
{
    QProcess proc;
    proc.start("lpoptions", {"-p", m_queue, "-l"});
    proc.waitForFinished(8000);
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    const QStringList lines = out.split('\n', kSkipEmptyParts);

    for (QString line : lines) {
        line = line.trimmed();
        if (!line.contains(' ') || !line.contains('/'))
            continue;

        // 格式：Name/Display: *Choice choice1 choice2 ...
        const int slash = line.indexOf('/');
        const int colon = line.indexOf(':', slash);
        if (slash < 0 || colon < 0 || colon <= slash)
            continue;

        const QString name = line.left(slash).trimmed();
        const QString text = line.mid(slash + 1, colon - slash - 1).trimmed();
        const QString rest = line.mid(colon + 1).trimmed();
        const QStringList tokens = rest.split(QRegularExpression("\\s+"), kSkipEmptyParts);
        if (tokens.isEmpty())
            continue;

        OptionItem item;
        item.name = name;
        item.text = text.isEmpty() ? optionLabel(name) : text;
        for (const QString &tok : tokens) {
            if (tok.startsWith('*')) {
                item.current = tok.mid(1);
                item.choices.append(item.current);
                item.raw.append(item.current);
            } else {
                item.choices.append(tok);
                item.raw.append(tok);
            }
        }
        if (item.current.isEmpty() && !item.choices.isEmpty())
            item.current = item.choices.first();
        item.initialRaw = item.current;
        m_options.append(item);
    }

    // 加载 CUPS 通用任务选项（方向、打印顺序、每页版数等），它们不在 PPD 安装选项中。
    // dde-printer 的属性页也把这些通用选项与 PPD 选项合并展示。
    loadGenericOptions();
}

static QString genericOptionRawDefault(const QString &name, const QString &currentValue)
{
    // 若 lpoptions 已设置过该选项，保持原值；否则给 CUPS 默认值。
    if (!currentValue.isEmpty()) return currentValue;
    if (name.compare("OrientationRequested", Qt::CaseInsensitive) == 0) return "3";
    if (name.compare("OutputOrder", Qt::CaseInsensitive) == 0)    return "normal";
    if (name.compare("number-up", Qt::CaseInsensitive) == 0)      return "1";
    return QString();
}

void PrintPropertiesDialog::loadGenericOptions()
{
    struct GenericDef {
        const char *name;
        const char *label;
        const char *rawDefaults;  // 空格分隔的原始值
        const char *dispDefaults; // 对应中文显示文本
    };
    static const GenericDef GENERIC[] = {
        { "OrientationRequested", QT_TR_NOOP("方向"),
          "3 4 5 6", QT_TR_NOOP("纵向 横向 反向纵向 反向横向") },
        { "OutputOrder", QT_TR_NOOP("打印顺序"),
          "normal reverse", QT_TR_NOOP("从前到后 从后到前") },
        { "number-up", QT_TR_NOOP("每页版数"),
          "1 2 4 6 9 16", "1 2 4 6 9 16" }
    };

    // 读取当前 CUPS 默认选项
    QProcess proc;
    proc.start("lpoptions", {"-p", m_queue});
    proc.waitForFinished(8000);
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
    QMap<QString, QString> current;
    for (const QString &tok : out.split(QRegularExpression("\\s+"), kSkipEmptyParts)) {
        const int eq = tok.indexOf('=');
        if (eq > 0)
            current[tok.left(eq)] = tok.mid(eq + 1);
    }

    for (const auto &g : GENERIC) {
        // 若 PPD 中已存在同名安装选项则跳过（例如双面/Duplex 已覆盖）
        bool exists = false;
        for (const OptionItem &existing : m_options)
            if (existing.name.compare(QLatin1String(g.name), Qt::CaseInsensitive) == 0) {
                exists = true; break;
            }
        if (exists) continue;

        const QStringList rawList = QString(QLatin1String(g.rawDefaults)).split(' ', kSkipEmptyParts);
        const QStringList dispList = tr(g.dispDefaults).split(' ', kSkipEmptyParts);
        if (rawList.isEmpty()) continue;

        const QString currentRaw = genericOptionRawDefault(QLatin1String(g.name),
                                                           current.value(QLatin1String(g.name)));
        int currentIdx = rawList.indexOf(currentRaw);
        if (currentIdx < 0) currentIdx = 0;

        OptionItem item;
        item.name = QLatin1String(g.name);
        item.text = optionLabel(item.name);
        item.raw = rawList;
        item.choices = dispList.isEmpty() ? rawList : dispList;
        item.current = item.choices.value(currentIdx);
        item.initialRaw = currentRaw;
        m_options.append(item);
    }
}

void PrintPropertiesDialog::buildUi()
{
    auto *content = new QWidget(this);
    auto *root = new QHBoxLayout(content);
    root->setSpacing(0);
    root->setContentsMargins(0, 0, 0, 0);

    // 左侧导航
    m_nav = new QListWidget;
    m_nav->setFixedWidth(140);
    m_nav->setObjectName("propNavList");
    m_nav->setSpacing(4);

    // 右侧堆叠页
    m_stack = new QStackedWidget;

    // 基础信息页
    m_nav->addItem(tr("基础信息"));
    m_stack->addWidget(buildBasicPage());

    // 选项分类
    QMap<QString, QList<int>> groups;
    for (int i = 0; i < m_options.size(); ++i)
        groups[optionGroup(m_options[i].name)].append(i);

    const QStringList groupOrder = {tr("纸张"), tr("质量"), tr("双面"), tr("布局"), tr("输出"), tr("高级")};
    for (const QString &g : groupOrder) {
        if (!groups.contains(g)) continue;
        m_nav->addItem(g);
        m_stack->addWidget(buildOptionPage(g, groups[g]));
    }

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    root->addWidget(m_nav);
    root->addWidget(m_stack, 1);
    addContent(content);
}

QWidget *PrintPropertiesDialog::buildBasicPage()
{
    auto *page = new QWidget;
    auto *lay = new QFormLayout(page);
    lay->setLabelAlignment(Qt::AlignLeft);
    lay->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    lay->setSpacing(14);

    auto makeField = [](const QString &text) -> DLabel * {
        auto *l = new DLabel(text.isEmpty() ? "—" : text);
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setMinimumHeight(36);
        l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        l->setStyleSheet("color: #374151; font-size: 13px; padding: 4px 0;");
        return l;
    };

    lay->addRow(tr("驱动："), makeField(m_driver));
    lay->addRow("URI：", makeField(m_uri));
    lay->addRow(tr("位置："), makeField(m_location));
    lay->addRow(tr("描述："), makeField(m_description));
    lay->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return page;
}

QWidget *PrintPropertiesDialog::buildOptionPage(const QString &title, const QList<int> &optionIndexes)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget;
    auto *lay = new QFormLayout(page);
    lay->setLabelAlignment(Qt::AlignLeft);
    lay->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    lay->setSpacing(16);

    auto *header = new DLabel(title);
    header->setStyleSheet("color: #111827; font-size: 15px; font-weight: 600;");
    lay->addRow(header);

    for (int idx : optionIndexes) {
        OptionItem &item = m_options[idx];
        auto *combo = new QComboBox;
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        combo->setMinimumWidth(220);
        for (int i = 0; i < item.choices.size(); ++i)
            combo->addItem(item.choices[i], item.raw.value(i));
        int sel = combo->findText(item.current);
        if (sel >= 0) combo->setCurrentIndex(sel);
        item.combo = combo;

        auto *label = new DLabel(optionLabel(item.name));
        label->setStyleSheet("color: #4b5563; font-size: 13px;");
        label->setMinimumWidth(110);
        lay->addRow(label, combo);
    }

    lay->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
    scroll->setWidget(page);

    // scroll 必须被其他 widget 持有，这里用一个 QFrame 包装以便加入 stacked
    auto *wrap = new QWidget;
    auto *wlay = new QVBoxLayout(wrap);
    wlay->setContentsMargins(0, 0, 0, 0);
    wlay->addWidget(scroll);
    return wrap;
}

void PrintPropertiesDialog::onAccepted()
{
    QStringList opts;
    for (const OptionItem &item : m_options) {
        if (!item.combo) continue;
        const QString raw = item.combo->currentData().toString();
        if (raw != item.initialRaw)
            opts << QString("%1=%2").arg(item.name, raw);
    }

    if (opts.isEmpty()) {
        close();
        return;
    }

    // 使用 lpadmin -p <queue> -o Name=Value ... 直接修改队列 PPD 的默认选项，
    // 对所有用户生效。属队列管理操作：lpadmin 组直跑，否则由 Privileges 提权。
    QStringList args = {"-p", m_queue};
    for (const QString &o : opts)
        args << "-o" << o;

    QString err;
    const int code = Privileges::run(QStringLiteral("lpadmin"), args,
                                     Privileges::Elevation::Auto,
                                     nullptr, &err, 15000);
    if (code != 0) {
        DMessageBox::critical(this, tr("IPP-USB 免驱助手"),
            tr("应用打印属性失败：") + (err.trimmed().isEmpty()
                ? tr("命令退出码 %1").arg(code) : err.trimmed()));
        return;
    }

    DMessageBox::information(this, tr("IPP-USB 免驱助手"),
        QString(tr("已更新 %1 项打印属性并同步到 PPD：\n%2")).arg(opts.size()).arg(opts.join("\n")));
    close();
}
