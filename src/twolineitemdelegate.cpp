#include "twolineitemdelegate.h"
#include "themehelper.h"

#include <QPainter>
#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>

TwoLineItemDelegate::TwoLineItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

namespace {
bool isDarkTheme()
{
    return ThemeHelper::instance().currentType() == Dtk::Gui::DGuiApplicationHelper::DarkType;
}
}

void TwoLineItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString title = index.data(Qt::DisplayRole).toString();
    const QString subtitle = index.data(Qt::UserRole).toString();
    const QColor accent = index.data(Qt::DecorationRole).value<QColor>();
    const QString iconChar = index.data(Qt::UserRole + 1).toString();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    // 背景
    QRect r = opt.rect;
    const bool dark = isDarkTheme();
    if (opt.state & QStyle::State_Selected) {
        painter->fillRect(r, dark ? QColor(31, 58, 95) : QColor(228, 238, 255));
        painter->setPen(dark ? QColor(78, 161, 255) : QColor(0, 129, 255));
        painter->drawLine(r.left(), r.top(), r.right(), r.top());
        painter->drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    } else if (opt.state & QStyle::State_MouseOver) {
        painter->fillRect(r, dark ? QColor(42, 42, 42) : QColor(245, 247, 250));
    } else {
        painter->fillRect(r, dark ? QColor(42, 42, 42) : Qt::white);
    }

    // 分组标题（以 == 开头结尾）
    if (title.startsWith("==") && title.endsWith("==")) {
        QFont f = opt.font;
        f.setPointSize(10);
        f.setWeight(QFont::Bold);
        painter->setFont(f);
        painter->setPen(dark ? QColor(158, 158, 158) : QColor(108, 115, 131));
        QRect rr(r.left() + 16, r.top(), r.width() - 32, 40);
        painter->drawText(rr, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(f).elidedText(title, Qt::ElideRight, rr.width()));
        painter->restore();
        return;
    }

    // 图标色块
    const int iconSize = 38;
    const int left = r.left() + 16;
    const int top = r.top() + (r.height() - iconSize) / 2;
    QRect iconRect(left, top, iconSize, iconSize);
    painter->setPen(Qt::NoPen);
    painter->setBrush(accent.isValid() ? accent : QColor(0, 129, 255));
    painter->drawRoundedRect(iconRect, 10, 10);

    // 色块内白色图标字符（取首字母或自定义）
    QString ch = iconChar;
    if (ch.isEmpty() && !title.isEmpty())
        ch = title.left(1).toUpper();
    if (!ch.isEmpty()) {
        QFont iconFont = opt.font;
        iconFont.setPointSize(13);
        iconFont.setWeight(QFont::Bold);
        painter->setFont(iconFont);
        painter->setPen(Qt::white);
        painter->drawText(iconRect, Qt::AlignCenter, ch);
    }

    // 标题
    QFont titleFont = opt.font;
    titleFont.setPointSize(11);
    titleFont.setWeight(QFont::Medium);
    painter->setFont(titleFont);
    painter->setPen(dark ? QColor(232, 232, 232) : QColor(32, 35, 43));
    const int textX = iconRect.right() + 14;
    const int textRight = r.right() - 16;
    QRect titleRect(textX, r.top() + 10, textRight - textX, 20);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, titleRect.width()));

    // 副标题
    if (!subtitle.isEmpty()) {
        QFont subFont = opt.font;
        subFont.setPointSize(9);
        painter->setFont(subFont);
        painter->setPen(dark ? QColor(160, 160, 160) : QColor(118, 126, 140));
        QRect subRect(textX, r.top() + 34, textRight - textX, 18);
        painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(subFont).elidedText(subtitle, Qt::ElideRight, subRect.width()));
    }

    painter->restore();
}

QSize TwoLineItemDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    const QString title = index.data(Qt::DisplayRole).toString();
    if (title.startsWith("==") && title.endsWith("=="))
        return QSize(0, 40);
    return QSize(0, 68);
}
