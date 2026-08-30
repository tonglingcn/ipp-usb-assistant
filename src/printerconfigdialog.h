#pragma once

#include <DDialog>
#include <QComboBox>
#include <DSwitchButton>

#include "ppdconfig.h"

DWIDGET_USE_NAMESPACE

/**
 * 驱动微调对话框：针对已添加打印机的 PPD 进行专业化配置。
 *  - 页面大小（*DefaultPageSize）
 *  - 双面长边翻页实机修正（*cupsBackSide: Rotated / Normal）
 */
class PrinterConfigDialog : public DDialog
{
    Q_OBJECT
public:
    explicit PrinterConfigDialog(const QString &queue, QWidget *parent = nullptr);

private slots:
    void onAccepted();

private:
    QString m_queue;
    QComboBox *m_pageSize{};
    DSwitchButton *m_longEdge{};
    PpdConfig::Options m_current;
};
