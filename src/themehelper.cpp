#include "themehelper.h"

#include <QApplication>
#include <QStyle>

ThemeHelper::ThemeHelper(QObject *parent)
    : QObject(parent)
{
    // 跟随系统主题变化，自动重新下发样式表
    connect(Dtk::Gui::DGuiApplicationHelper::instance(),
            &Dtk::Gui::DGuiApplicationHelper::themeTypeChanged,
            this, [this](Dtk::Gui::DGuiApplicationHelper::ColorType type) {
                applyTheme(type);
                emit themeChanged(type);
            });
}

ThemeHelper &ThemeHelper::instance()
{
    static ThemeHelper s_instance;
    return s_instance;
}

void ThemeHelper::applyTheme(Dtk::Gui::DGuiApplicationHelper::ColorType type)
{
    m_type = type;
    qApp->setStyleSheet(type == Dtk::Gui::DGuiApplicationHelper::DarkType
                        ? darkStyleSheet()
                        : lightStyleSheet());
}

void ThemeHelper::reapply()
{
    applyTheme(m_type);
}

namespace {
/* 基础布局样式（不随主题变化，颜色由主题段单独设置） */
const char *kBaseStyle = R"(
    QWidget { font-family: 'Noto Sans CJK SC', 'Source Han Sans SC', sans-serif; }

    QWidget#pageHeader { border-bottom: 1px solid [BORDER]; }
    DLabel#pageTitle { font-size: 22px; font-weight: 700; }
    DLabel#pageSubtitle { font-size: 13px; }

    /* 高级设置页的子标签：去掉默认边框，用下划线指示选中项，
       与 DTK 内容区风格一致 */
    QTabWidget#advancedTabs::pane {
      border: none;
      border-top: 1px solid [BORDER];
      background: transparent;
    }
    QTabWidget#advancedTabs > QTabBar {
      background: transparent;
      border: none;
    }
    QTabWidget#advancedTabs > QTabBar::tab {
      background: transparent;
      color: [TEXT_SUB];
      border: none;
      border-bottom: 2px solid transparent;
      padding: 8px 4px;
      margin-right: 28px;
      font-size: 14px;
      font-weight: 500;
    }
    QTabWidget#advancedTabs > QTabBar::tab:hover { color: [TEXT]; }
    QTabWidget#advancedTabs > QTabBar::tab:selected {
      color: [ACCENT];
      font-weight: 600;
      border-bottom: 2px solid [ACCENT];
    }
    QTabWidget#advancedTabs > QTabBar::tab:focus { outline: none; }

    /* 权限提示条：不在 lpadmin 组时显示 */
    QFrame#privilegeNotice {
      border: 1px solid [BTN_BORDER];
      border-left: 3px solid [ACCENT];
      border-radius: 8px;
      background: [CARD];
    }
    DLabel#privilegeNoticeIcon {
      background: [ACCENT]; color: #ffffff;
      border-radius: 12px; font-size: 13px; font-weight: 800;
    }
    DLabel#privilegeNoticeLabel {
      font-size: 12px; line-height: 1.55; color: [TEXT_SUB];
      background: transparent;
    }

    QPushButton#navButton {
      background: transparent;
      border: none; border-radius: 10px;
      font-size: 14px; font-weight: 500;
      padding-left: 14px; text-align: left;
    }

    QPushButton#toolButton {
      background: [CARD]; color: [TEXT];
      border: 1px solid [BTN_BORDER]; border-radius: 8px;
      padding: 7px 18px; font-size: 13px; outline: none;
    }
    QPushButton#toolButton:hover { background: [HOVER]; border: 1px solid [BTN_BORDER_HOVER]; }
    QPushButton#toolButton:pressed { background: [HOVER]; }
    QPushButton#toolButton:disabled { color: [TEXT_WEAK]; background: [BTN_DISABLED]; }
    QPushButton:focus { outline: none; }
    DSuggestButton {
      color: #ffffff;
      background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #0081ff, stop:1 #1f6feb);
      border: none; border-radius: 8px;
      padding: 7px 18px; font-size: 13px; outline: none;
    }
    DSuggestButton:hover {
      background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #0066cc, stop:1 #1d5dd8);
    }
    DSuggestButton:pressed {
      background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #005bb5, stop:1 #1854c7);
    }
    DSuggestButton:disabled { background: [PRIMARY_DISABLED]; color: #ffffff; }

    QFrame#serviceCard {
      border: 1px solid [BORDER]; border-radius: 12px;
    }
    DLabel#serviceName { font-size: 15px; font-weight: 700; }
    DLabel#statusLine { font-size: 12px; font-weight: 600; background: transparent; }
    DLabel#serviceDesc { font-size: 11px; line-height: 1.45; }
    DLabel#sectionLabel { font-size: 12px; font-weight: 600; }
    QPushButton#serviceBtn {
      border: 1px solid [BTN_BORDER]; border-radius: 7px;
      font-size: 12px;
    }
    QPushButton#serviceBtn:hover { border-color: [BTN_BORDER_HOVER]; }
    QPushButton#serviceBtn:disabled {
      border-color: [BTN_BORDER]; background: [BTN_DISABLED];
    }

    QFrame#quickCheckCard { border: 1px solid [BORDER]; border-radius: 14px; }
    DLabel#quickCheckTitle { font-size: 16px; font-weight: 700; }
    DLabel#quickCheckSubtitle { font-size: 12px; }
    DLabel#quickCheckDetail { font-size: 12px; line-height: 1.55; border-radius: 8px; padding: 8px 12px; }
    QPushButton#primaryAction {
      color: #ffffff; border: none; border-radius: 8px;
      padding: 0 18px; font-size: 13px; font-weight: 600;
    }
    QPushButton#primaryAction:disabled { background: [PRIMARY_DISABLED]; color: #ffffff; }
    QPushButton#secondaryAction {
      border: 1px solid [BTN_BORDER]; border-radius: 8px;
      padding: 0 16px; font-size: 12px;
    }
    QPushButton#secondaryAction:hover { border: 1px solid [BTN_BORDER_HOVER]; }

    QFrame#offlinePanel { border: 1px solid [WARN_BORDER]; border-radius: 12px; }
    DLabel#offlineTitle { font-size: 13px; font-weight: 700; }
    DLabel#offlineRepo {
      font-family: 'monospace'; font-size: 12px;
      border: 1px solid [WARN_BORDER]; border-radius: 6px; padding: 6px 10px;
    }
    DLabel#offlineTip { font-size: 11px; line-height: 1.5; }

    AddPrinterDialog { background: [BG]; }
    QFrame#addLeftBar { border-right: 1px solid [BORDER]; }
    QFrame#addRight, QFrame#addBottom { background: [CARD]; }
    QFrame#addBottom { border-top: 1px solid [BORDER]; }
    QPushButton#navBtn {
      border: 1px solid transparent; border-radius: 8px;
      text-align: left; padding-left: 14px; font-size: 13px;
    }
    DLabel#addTitle { font-size: 16px; font-weight: 700; }
    DLabel#addHint { font-size: 11px; }
    QPushButton#addRefresh, QPushButton#addScan {
      border: 1px solid [BTN_BORDER]; border-radius: 6px;
      padding: 0 12px; font-size: 12px;
    }
    QPushButton#addRefresh:hover, QPushButton#addScan:hover { background: [HOVER]; }
    QLineEdit#addNameEdit, QLineEdit#addUriEdit, QComboBox#addDriverCombo {
      border: 1px solid [BTN_BORDER]; border-radius: 6px; padding: 0 10px;
    }
    QLineEdit#addNameEdit:focus, QLineEdit#addUriEdit:focus, QComboBox#addDriverCombo:focus {
      border-color: [ACCENT];
    }
    DLabel#addUriHelp {
      font-family: monospace; font-size: 11px; line-height: 1.6;
      border: 1px solid [BORDER]; border-radius: 6px; padding: 8px 10px;
    }
    QPushButton#addCancel {
      border: 1px solid [BTN_BORDER]; border-radius: 7px; font-size: 13px;
    }
    QPushButton#addCancel:hover { background: [HOVER]; }

    QFrame#addSuccessToast { border: 1px solid [SUCCESS_BORDER]; border-radius: 16px; }
    DLabel#addSuccessTitle { font-size: 16px; font-weight: 700; color: [SUCCESS]; }
    DLabel#addSuccessSubtitle { font-size: 11px; line-height: 1.5; }
    QPushButton#addSuccessView {
      color: [SUCCESS]; border: 1px solid [SUCCESS_BORDER]; border-radius: 7px; font-size: 12px;
    }
    QPushButton#addSuccessView:hover { background: [SUCCESS_HOVER]; }

    DListView { background: [CARD]; border-radius: 12px; border: 1px solid [BORDER]; outline: none; }
    DListView::item { border-bottom: 1px solid [LIST_DIVIDER]; }
    DListView::item:last { border-bottom: none; }
)";

QString fill(const QString &base,
             const QString &bg, const QString &card, const QString &border,
             const QString &text, const QString &textSub, const QString &textWeak,
             const QString &hover, const QString &btnBorder, const QString &btnBorderHover,
             const QString &btnDisabled, const QString &accent,
             const QString &primaryDisabled, const QString &warnBg, const QString &warnBorder,
             const QString &warnText, const QString &success, const QString &successBorder,
             const QString &successHover, const QString &listDivider,
             const QString &navActiveBg, const QString &navActiveText)
{
    QString s = base;
    s.replace("[BG]", bg)
     .replace("[CARD]", card)
     .replace("[BORDER]", border)
     .replace("[TEXT]", text)
     .replace("[TEXT_SUB]", textSub)
     .replace("[TEXT_WEAK]", textWeak)
     .replace("[HOVER]", hover)
     .replace("[BTN_BORDER]", btnBorder)
     .replace("[BTN_BORDER_HOVER]", btnBorderHover)
     .replace("[BTN_DISABLED]", btnDisabled)
     .replace("[ACCENT]", accent)
     .replace("[PRIMARY_DISABLED]", primaryDisabled)
     .replace("[WARN_BG]", warnBg)
     .replace("[WARN_BORDER]", warnBorder)
     .replace("[WARN_TEXT]", warnText)
     .replace("[SUCCESS]", success)
     .replace("[SUCCESS_BORDER]", successBorder)
     .replace("[SUCCESS_HOVER]", successHover)
     .replace("[LIST_DIVIDER]", listDivider)
     .replace("[NAV_ACTIVE_BG]", navActiveBg)
     .replace("[NAV_ACTIVE_TEXT]", navActiveText);
    return s;
}
}

QString ThemeHelper::lightStyleSheet() const
{
    const QString themeColors = R"(
        QStackedWidget#contentArea { background: #f5f6f8; }
        QWidget#sidebar { background: #ffffff; border-right: 1px solid #e5e8ef; }
        DLabel#appTitle { color: #161922; font-size: 22px; font-weight: 700; padding: 6px 2px; }
        DLabel#aboutLabel { color: #7c869a; font-size: 11px; padding: 6px 2px; }
        QWidget#pageHeader { background: #ffffff; }
        DLabel#pageTitle { color: #161922; }
        DLabel#pageSubtitle { color: #6b7280; }
        QPushButton#navButton { color: #4b5563; }
        QPushButton#navButton:hover { background: #f3f4f6; color: #161922; }
        QPushButton#navButton:checked { background: #e6f2ff; color: #0066cc; font-weight: 700; }
        QPushButton#toolButton { background: #ffffff; color: #374151; }
        QPushButton#toolButton:hover { background: #f9fafb; }
        DLabel#serviceName { color: #161922; }
        DLabel#statusLine { color: #4b5563; }
        DLabel#serviceDesc { color: #6b7280; }
        DLabel#sectionLabel { color: #6b7280; }
        QPushButton#serviceBtn { background: #ffffff; color: #374151; }
        QPushButton#serviceBtn:disabled { color: #c3c7cf; background: #fbfbfc; }
        DLabel#quickCheckTitle { color: #161922; }
        DLabel#quickCheckSubtitle { color: #6b7280; }
        DLabel#quickCheckDetail { color: #4b5563; background: #f7f9fc; }
        QPushButton#primaryAction { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #0081ff, stop:1 #1f6feb); }
        QPushButton#primaryAction:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #0066cc, stop:1 #1d5dd8); }
        QPushButton#secondaryAction { background: #ffffff; color: #374151; }
        QPushButton#secondaryAction:hover { background: #f9fafb; }
        QFrame#offlinePanel { background: #fff8ec; }
        DLabel#offlineTitle { color: #92400e; }
        DLabel#offlineRepo { color: #1f6feb; background: #ffffff; }
        DLabel#offlineTip { color: #6b7280; }
        QFrame#addSuccessToast { background: #ffffff; }
        DLabel#addSuccessSubtitle { color: #6b7280; }
        QPushButton#addSuccessView { background: #ffffff; }
        QPushButton#addSuccessView:hover { background: #f0fdf4; }
        DLabel#addHint { color: #6b7280; }
        QPushButton#addRefresh, QPushButton#addScan { background: #ffffff; color: #374151; }
        QPushButton#addCancel { background: #ffffff; color: #374151; }
        QLineEdit#addNameEdit, QLineEdit#addUriEdit, QComboBox#addDriverCombo { background: #f7f9fc; color: #161922; }
        DLabel#addUriHelp { color: #6b7280; background: #f7f9fc; }
        QPushButton#navBtn { color: #374151; }
        QPushButton#navBtn:hover { background: #f3f4f6; }
        QPushButton#navBtn:checked { background: #e6f2ff; color: #0066cc; border: 1px solid #c7e0ff; }
    )";
    QString s = themeColors + QString(kBaseStyle);
    return fill(s,
        /*bg*/ "#f5f6f8", /*card*/ "#ffffff", /*border*/ "#e5e8ef",
        /*text*/ "#161922", /*textSub*/ "#6b7280", /*textWeak*/ "#7c869a",
        /*hover*/ "#f9fafb", /*btnBorder*/ "#d1d5db", /*btnBorderHover*/ "#9ca3af",
        /*btnDisabled*/ "#eceef2", /*accent*/ "#1f6feb",
        /*primaryDisabled*/ "#c3c7cf",
        /*warnBg*/ "#fff8ec", /*warnBorder*/ "#f5d491", /*warnText*/ "#92400e",
        /*success*/ "#15803d", /*successBorder*/ "#c8e6c9", /*successHover*/ "#f0fdf4",
        /*listDivider*/ "#f1f3f6",
        /*navActiveBg*/ "#e6f2ff", /*navActiveText*/ "#0066cc");
}

QString ThemeHelper::darkStyleSheet() const
{
    const QString themeColors = R"(
        QStackedWidget#contentArea { background: #1a1a1a; }
        QWidget#sidebar { background: #202020; border-right: 1px solid rgba(255,255,255,0.08); }
        DLabel#appTitle { color: #e8e8e8; font-size: 22px; font-weight: 700; padding: 6px 2px; }
        DLabel#aboutLabel { color: #8a8a8a; font-size: 11px; padding: 6px 2px; }
        QWidget#pageHeader { background: #242424; }
        DLabel#pageTitle { color: #e8e8e8; }
        DLabel#pageSubtitle { color: #a0a0a0; }
        QPushButton#navButton { color: #c0c0c0; }
        QPushButton#navButton:hover { background: #2c2c2c; color: #ffffff; }
        QPushButton#navButton:checked { background: #1f3a5f; color: #4ea1ff; font-weight: 700; }
        QPushButton#toolButton { background: #2c2c2c; color: #e0e0e0; }
        QPushButton#toolButton:hover { background: #383838; }
        DLabel#serviceName { color: #e8e8e8; }
        DLabel#statusLine { color: #c0c0c0; }
        DLabel#serviceDesc { color: #a0a0a0; }
        DLabel#sectionLabel { color: #a0a0a0; }
        QPushButton#serviceBtn { background: #2c2c2c; color: #e0e0e0; }
        QPushButton#serviceBtn:disabled { color: #6a6a6a; background: #262626; }
        DLabel#quickCheckTitle { color: #e8e8e8; }
        DLabel#quickCheckSubtitle { color: #a0a0a0; }
        DLabel#quickCheckDetail { color: #c0c0c0; background: #2a2a2a; }
        QPushButton#primaryAction { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #2b8fff, stop:1 #1f6feb); }
        QPushButton#primaryAction:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #0081ff, stop:1 #1d5dd8); }
        QPushButton#secondaryAction { background: #2c2c2c; color: #e0e0e0; }
        QPushButton#secondaryAction:hover { background: #383838; }
        QFrame#offlinePanel { background: #3a2f1a; }
        DLabel#offlineTitle { color: #f0b860; }
        DLabel#offlineRepo { color: #4ea1ff; background: #2a2a2a; }
        DLabel#offlineTip { color: #a0a0a0; }
        QFrame#addSuccessToast { background: #2a2a2a; }
        DLabel#addSuccessSubtitle { color: #a0a0a0; }
        QPushButton#addSuccessView { background: #2a2a2a; }
        QPushButton#addSuccessView:hover { background: #24362a; }
        DLabel#addHint { color: #a0a0a0; }
        QPushButton#addRefresh, QPushButton#addScan { background: #2c2c2c; color: #e0e0e0; }
        QPushButton#addCancel { background: #2c2c2c; color: #e0e0e0; }
        QLineEdit#addNameEdit, QLineEdit#addUriEdit, QComboBox#addDriverCombo { background: #2a2a2a; color: #e8e8e8; }
        DLabel#addUriHelp { color: #a0a0a0; background: #2a2a2a; }
        QPushButton#navBtn { color: #e0e0e0; }
        QPushButton#navBtn:hover { background: #2c2c2c; }
        QPushButton#navBtn:checked { background: #1f3a5f; color: #4ea1ff; border: 1px solid #2f5a8f; }
    )";
    QString s = themeColors + QString(kBaseStyle);
    return fill(s,
        /*bg*/ "#1a1a1a", /*card*/ "#2a2a2a", /*border*/ "rgba(255,255,255,0.10)",
        /*text*/ "#e8e8e8", /*textSub*/ "#a0a0a0", /*textWeak*/ "#8a8a8a",
        /*hover*/ "#383838", /*btnBorder*/ "rgba(255,255,255,0.18)", /*btnBorderHover*/ "rgba(255,255,255,0.35)",
        /*btnDisabled*/ "rgba(255,255,255,0.06)", /*accent*/ "#4ea1ff",
        /*primaryDisabled*/ "#4a4a4a",
        /*warnBg*/ "#3a2f1a", /*warnBorder*/ "#6a5320", /*warnText*/ "#f0b860",
        /*success*/ "#5fd08a", /*successBorder*/ "#2f5538", /*successHover*/ "#24362a",
        /*listDivider*/ "rgba(255,255,255,0.08)",
        /*navActiveBg*/ "#1f3a5f", /*navActiveText*/ "#4ea1ff");
}
