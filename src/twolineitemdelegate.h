#pragma once

#include <QStyledItemDelegate>

/**
 * 两行列表项绘制代理：
 *  - 左侧 40x40 圆角色块（可设置背景色 / 图标）
 *  - 主标题：粗体
 *  - 副标题：灰色小字
 *  - 右侧状态点（可选）
 */
class TwoLineItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit TwoLineItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};
