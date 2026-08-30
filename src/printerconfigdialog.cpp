#include "printerconfigdialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <DMessageBox>

PrinterConfigDialog::PrinterConfigDialog(const QString &queue, QWidget *parent)
    : DDialog(parent), m_queue(queue)
{
    setWindowTitle(tr("驱动微调 - ") + queue);
    setFixedSize(560, 340);

    PpdConfig cfg;
    const QString ppd = PpdConfig::ppdPathForQueue(queue);
    if (!cfg.read(queue, m_current)) {
        DMessageBox::critical(this, tr("IPP-USB 免驱助手"), tr("无法读取 PPD：") + ppd);
        return;
    }

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(16);

    // 标题说明
    auto *tip = new QLabel(tr("针对免驱驱动生成的关键参数进行修正。\n"
                              "长边翻页是免驱驱动最常见的缺陷，开启后可修正双面打印方向。"));
    tip->setWordWrap(true);
    tip->setStyleSheet("color: #6b7280; font-size: 12px;");
    layout->addWidget(tip);

    // 页面大小
    auto *psRow = new QHBoxLayout;
    psRow->setSpacing(12);
    auto *psLabel = new QLabel(tr("默认页面大小："));
    psLabel->setFixedWidth(150);
    m_pageSize = new QComboBox;
    m_pageSize->setMinimumWidth(220);
    m_pageSize->setFixedHeight(32);
    m_pageSize->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pageSize->addItems(PpdConfig::supportedPageSizes(ppd));
    if (m_pageSize->findText(m_current.pageSize) >= 0)
        m_pageSize->setCurrentText(m_current.pageSize);
    else if (!m_current.pageSize.isEmpty())
        m_pageSize->insertItem(0, m_current.pageSize);
    // 优先用队列实际生效的默认纸张（lpadmin -o PageSize=A4 设置的值，
    // PPD 文件里的 *DefaultPageSize 不会同步更新）
    QProcess lpopt;
    lpopt.start("lpoptions", {"-p", queue, "-l"});
    if (lpopt.waitForFinished(5000)) {
        const QString out = QString::fromLocal8Bit(lpopt.readAllStandardOutput());
        for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
            if (!line.startsWith("PageSize/", Qt::CaseInsensitive))
                continue;
            for (const QString &tok : line.section(':', 1).split(' ', Qt::SkipEmptyParts)) {
                if (tok.startsWith('*')) {
                    const QString def = tok.mid(1);
                    if (m_pageSize->findText(def) >= 0)
                        m_pageSize->setCurrentText(def);
                    break;
                }
            }
            break;
        }
    }
    psRow->addWidget(psLabel);
    psRow->addWidget(m_pageSize, 1);
    layout->addLayout(psRow);

    // 长边翻页：开关行独立，避免挤压换行
    auto *dupRow = new QHBoxLayout;
    dupRow->setSpacing(12);
    auto *dupLabelFixed = new QLabel(tr("双面长边翻页"));
    dupLabelFixed->setFixedWidth(150);
    m_longEdge = new DSwitchButton;
    m_longEdge->setChecked(m_current.longEdgeDuplex);
    dupRow->addWidget(dupLabelFixed);
    dupRow->addWidget(m_longEdge);
    dupRow->addStretch();
    layout->addLayout(dupRow);

    auto *status = new QLabel(
        QString(tr("当前 cupsBackSide: %1  →  %2"))
            .arg(m_current.backSide,
                 m_current.longEdgeDuplex ? tr("Rotated(长边)") : tr("Normal(短边/默认)")));
    status->setObjectName("statusLabel");
    status->setStyleSheet("color: #9ca3af; font-size: 12px;");
    layout->addWidget(status);
    layout->addStretch();

    addContent(content);
    addButton(tr("取消"), false);
    addButton(tr("应用并重启队列"), true, DDialog::ButtonRecommend);

    connect(this, &DDialog::buttonClicked, this, [this](int idx) {
        if (idx == 1) // 第二个按钮（应用）
            onAccepted();
    });
}

void PrinterConfigDialog::onAccepted()
{
    PpdConfig::Options opt;
    opt.pageSize = m_pageSize->currentText();
    opt.longEdgeDuplex = m_longEdge->isChecked();
    opt.backSide = opt.longEdgeDuplex ? "Rotated" : "Normal";

    PpdConfig cfg;
    QString err;
    if (cfg.apply(m_queue, opt, err)) {
        DMessageBox::information(this, tr("IPP-USB 免驱助手"),
            QString(tr("已更新驱动配置：\n页面大小 = %1\n长边翻页 = %2\n队列已重启生效。"))
                .arg(opt.pageSize, opt.longEdgeDuplex ? tr("开启(Rotated)") : tr("关闭(Normal)")));
        close();
    } else {
        DMessageBox::critical(this, tr("IPP-USB 免驱助手"), tr("配置失败：") + err);
    }
}
