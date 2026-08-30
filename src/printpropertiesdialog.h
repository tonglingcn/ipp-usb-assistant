#pragma once

#include <DDialog>
#include <DLabel>
#include <QString>
#include <QStringList>
#include <QMap>

class QComboBox;
class QListWidget;
class QStackedWidget;

DWIDGET_USE_NAMESPACE

/**
 * 打印属性对话框：读取 CUPS 队列当前 PPD 选项并允许用户修改。
 * 参考系统打印属性布局：左侧选项分类导航，右侧对应表单项。
 */
class PrintPropertiesDialog : public DDialog
{
    Q_OBJECT
public:
    explicit PrintPropertiesDialog(const QString &queue, QWidget *parent = nullptr);

private slots:
    void onAccepted();

private:
    struct OptionItem {
        QString name;        // 选项关键字，如 PageSize
        QString text;        // 人类可读名称，如 纸张大小
        QString current;     // 当前选中值（显示文本）
        QStringList choices; // 可选值列表（显示文本）
        QStringList raw;     // 可选值列表（提交给 lpadmin 的原始值）
        QString initialRaw;  // 加载时的初始原始值，用于判断用户是否修改
        QComboBox *combo = nullptr;
    };

    void loadBasicInfo();
    void loadOptions();
    void loadGenericOptions();
    void buildUi();
    QWidget *buildBasicPage();
    QWidget *buildOptionPage(const QString &title, const QList<int> &optionIndexes);

    QString m_queue;
    QString m_driver;
    QString m_uri;
    QString m_location;
    QString m_description;
    QList<OptionItem> m_options;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
};