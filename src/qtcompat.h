// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Qt5 / Qt6 兼容层。
//
// 本项目的目标平台是 deepin/UOS 25（Qt 6 + DTK 6），但同样需要在
// UOS 20（Qt 5.11 + DTK 5.6）上编译。两者之间有少数 API 发生了
// 不兼容的迁移，全部集中在本文件里用宏抹平，业务代码只使用统一的名字，
// 不再直接写版本相关的调用。
//
// 迁移对应关系：
//   1) QString::split() 的 SplitBehavior 枚举
//      Qt < 5.14 : QString::SkipEmptyParts （QString 作用域）
//      Qt >= 5.14: kSkipEmptyParts      （Qt 作用域，QString:: 版为废弃别名）
//      Qt 6      : 仅有 kSkipEmptyParts，QString:: 版已删除
//   2) QButtonGroup 的「按 id 点击」信号
//      Qt < 5.15 : buttonClicked(int)
//      Qt >= 5.15: idClicked(int)
//      Qt 6      : 仅有 idClicked(int)
//   3) QTextStream 的编码设置
//      Qt 5 : setCodec("UTF-8")
//      Qt 6 : setEncoding(QStringConverter::Utf8)（setCodec 已删除）

#pragma once

#include <QtGlobal>

#include <QButtonGroup>
#include <QString>
#include <QTextStream>

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
// Qt 5.14+ 将枚举从 QString 作用域迁到 Qt 作用域。
// 注意：右侧必须是 Qt:: 限定的真实枚举，不能写成 kSkipEmptyParts 自身，
// 否则会变成 "use of 'kSkipEmptyParts' before deduction of 'auto'" 编译错误。
constexpr auto kSkipEmptyParts = Qt::SkipEmptyParts;
#else
constexpr auto kSkipEmptyParts = QString::SkipEmptyParts;
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#  define IPP_USB_BUTTON_GROUP_ID_SIGNAL &QButtonGroup::idClicked
#else
#  define IPP_USB_BUTTON_GROUP_ID_SIGNAL &QButtonGroup::buttonClicked
#endif

// 把 QTextStream 统一设为 UTF-8。Qt 5 用 setCodec，Qt 6 用 setEncoding。
inline void setUtf8Encoding(QTextStream &stream)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // 注意：这里必须调用流对象的 setEncoding，写成 setUtf8Encoding(stream)
    // 会递归调用本函数导致栈溢出。
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
}
