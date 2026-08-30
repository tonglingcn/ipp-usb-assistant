#pragma once

#include <QProxyStyle>

/**
 * 焦点样式代理：屏蔽所有控件的焦点虚线框（PE_FrameFocusRect），
 * 避免 QPushButton / DSuggestButton 获得焦点时随机出现椭圆/虚线圈。
 */
class FocusStyle : public QProxyStyle
{
    using QProxyStyle::QProxyStyle;

public:
    void drawPrimitive(QStyle::PrimitiveElement element,
                       const QStyleOption *option,
                       QPainter *painter,
                       const QWidget *widget = nullptr) const override;
};