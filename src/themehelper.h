#pragma once

#include <QObject>
#include <QString>

#include <DGuiApplicationHelper>

DGUI_USE_NAMESPACE

/**
 * 主题助手：为应用提供亮/暗两套样式表，并跟随系统或用户手动切换。
 *
 * 用法：
 *   ThemeHelper::instance().applyTheme(DGuiApplicationHelper::themeType());
 * 切换主题后会自动重新下发样式表，并触发主窗口重新着色。
 */
class ThemeHelper : public QObject
{
    Q_OBJECT
public:
    static ThemeHelper &instance();

    /// 应用指定主题（LightType / DarkType），并下发对应样式表
    void applyTheme(Dtk::Gui::DGuiApplicationHelper::ColorType type);

    /// 仅重新下发当前主题的样式表（例如窗口重建后调用）
    void reapply();

    Dtk::Gui::DGuiApplicationHelper::ColorType currentType() const { return m_type; }

signals:
    /// 主题变化（供主窗口刷新图标等）
    void themeChanged(Dtk::Gui::DGuiApplicationHelper::ColorType type);

private:
    explicit ThemeHelper(QObject *parent = nullptr);

    QString lightStyleSheet() const;
    QString darkStyleSheet() const;

    Dtk::Gui::DGuiApplicationHelper::ColorType m_type = Dtk::Gui::DGuiApplicationHelper::LightType;
};
