#include <DApplication>
#include <DAboutDialog>
#include <DGuiApplicationHelper>
#include <DMainWindow>
#include <DTitlebar>
#include <DIconTheme>

#include <QIcon>

#include "mainwindow.h"
#include "themehelper.h"
#include "focusstyle.h"

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

int main(int argc, char *argv[])
{
    DApplication app(argc, argv);
    // applicationName 必须用英文 ID：DApplication::loadTranslator 以它匹配
    // translations/<name>_<locale>.qm 文件，中文会导致翻译加载失败
    app.setApplicationName("ipp-usb-assistant");
    app.setApplicationDisplayName(QObject::tr("IPP-USB 免驱助手"));
    app.setApplicationVersion(IPP_USB_ASSISTANT_VERSION);
    app.loadTranslator();

    // 主题：默认跟随系统（亮/暗），用户可在标题栏菜单手动切换
    ThemeHelper::instance().applyTheme(Dtk::Gui::DGuiApplicationHelper::instance()->themeType());

    // 全局隐藏按钮焦点椭圆，避免界面随机出现虚线框
    app.setStyle(new FocusStyle(app.style()));

    // 关于对话框（标题栏"关于"菜单项使用）。
    // 与 MainWindow::showAbout() 共用同一份配置，避免两处文案不一致。
    DAboutDialog *about = new DAboutDialog;
    MainWindow::setupAboutDialog(about);
    app.setAboutDialog(about);

    // 专业化商业观感：样式表由 ThemeHelper 按主题下发（见 themehelper.cpp）
    Q_UNUSED(app)

    DMainWindow w;
    w.setMinimumSize(900, 640);
    w.titlebar()->setTitle(QObject::tr("IPP-USB 免驱助手"));
    w.titlebar()->setIcon(DIconTheme::findQIcon("ipp-usb-assistant"));
    // 标题栏菜单：启用主题切换（跟随系统/浅色/深色）与"关于"
    w.titlebar()->setSwitchThemeMenuVisible(true);
    w.titlebar()->setMenuVisible(true);

    auto *mw = new MainWindow(&w);
    w.setCentralWidget(mw);
    w.show();

    // 权限自检由 MainWindow 构造函数完成（不在 lpadmin 组时在环境检测页
    // 顶部显示提示条），此处无需重复处理。

    return app.exec();
}
