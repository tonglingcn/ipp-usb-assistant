#pragma once

#include <QWidget>
#include <DLabel>

class QListWidget;
class QPushButton;

DWIDGET_USE_NAMESPACE

/**
 * 高级设置页
 *
 * 提供两个互相独立的过滤维度：
 *
 * 1) 整机放行（ipp-usb quirks）
 *    写入 /etc/ipp-usb/quirks/ipp-usb-driverless-assistant.conf，按设备名称
 *    （iManufacturer + iProduct）匹配，使特定设备完全不被 ipp-usb 接管。
 *    粒度是整机的：打印与扫描一起交给原厂驱动。
 *
 * 2) 扫描排除（sane-airscan blacklist）
 *    写入 /etc/sane.d/airscan.d/ipp-usb-assistant.conf 的 [blacklist] 段，
 *    只让该设备不再通过 airscan（eSCL）提供扫描，IPP 打印完全不受影响。
 *    适用于厂商提供原生 SANE 驱动、但打印仍希望走 ipp-usb 免驱的一体机。
 *
 * 之所以需要 (2)：ipp-usb 上游的 quirks 只有 blacklist 一个过滤键，且命中后
 * 整台设备被放弃（ErrBlackListed），无法做到"只禁扫描、保留打印"。
 * 而 sane-airscan 提供按设备的 name/model 过滤，且只作用于自动发现，
 * 与 CUPS 打印链路无关，正好可以实现这个粒度。
 */
class AdvancedSettings : public QWidget
{
    Q_OBJECT
public:
    explicit AdvancedSettings(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void load();
    void save();
    void addRule();
    void removeRule();
    void scanDevices();
    void addDeviceRule(int index);

    void refreshAirscanDevices();
    void addScanExclude();
    void removeScanExclude();
    void saveScanExcludes();
    void excludeDeviceScan(int index);

private:
    void buildUi();
    QWidget *buildIppUsbTab();      // 子标签一：ipp-usb 接管过滤（整机放行）
    QWidget *buildScanExcludeTab(); // 子标签二：扫描通道排除（airscan blacklist）

    struct Rule {
        QString deviceName;   // iManufacturer + iProduct，例如 "Pantum BM4240ADW series"
        int vid{0};           // 仅用于展示
        int pid{0};
    };

    // sane-airscan [blacklist] 支持的匹配字段。
    // Model = 硬件型号（TXT 记录的 model）；Name = DNS-SD 网络名（含 " (USB)" 后缀）。
    // 两者在 airscan-zeroconf.c 中分别由 zeroconf_device_model() /
    // zeroconf_device_name() 提供，均经 fnmatch 做 glob 匹配。
    enum class ExcludeField {
        Model,
        Name,
    };

    struct ScanExcludeRule {
        QString pattern;      // 支持 glob（由 sane-airscan 的 fnmatch 执行）
        ExcludeField field{ExcludeField::Model};
    };

    // airscan-discover 输出的一条设备记录。
    struct AirscanDevice {
        QString name;   // DNS-SD network name，含 ipp-usb 追加的 " (USB)" 后缀
        QString uri;    // 端点 URI，如 http://127.0.0.1:60000/eSCL/
        QString proto;  // eSCL 或 WSD
    };

    struct UsbDevice {
        int vid{0};
        int pid{0};
        QString manufacturer; // iManufacturer
        QString product;      // iProduct
        QString model;        // manufacturer + " " + product
    };

    struct DeviceScanResult {
        QList<UsbDevice> devices;
        QString status;
        bool success{false};
    };

    static bool parseIppUsbDevice(const QString &line, UsbDevice &dev);
    // 解析 EnvChecker::lsusbImagingDevices() 的行（已按 IPP-over-USB 规则过滤）
    static bool parseEnvDeviceLine(const QString &line, UsbDevice &dev);
    static bool parseLsusbDevice(const QString &line, UsbDevice &dev);
    static bool queryDeviceName(int vid, int pid, UsbDevice &dev);
    QString formatRuleDisplay(const Rule &rule) const;
    QString formatQuirkSection(const QString &deviceName) const;
    bool isDeviceAllowed(const QString &deviceName) const;
    void refreshDeviceList();

    // ---- 扫描排除（sane-airscan）----
    static bool parseAirscanDevice(const QString &line, AirscanDevice &dev);
    static QString dequoteIniValue(const QString &raw);
    static bool hasAlternativeSaneBackend();
    bool isScanExcluded(const QString &pattern, ExcludeField field) const;
    bool writeScanExcludeFile(QString *errorText) const;
    QString renderScanExcludeFile() const;
    void refreshAirscanList();
    void refreshScanExcludeList();
    void loadScanExcludes();
    void addExcludeRule(const QString &pattern, ExcludeField field);

    QList<Rule> m_rules;
    QList<UsbDevice> m_devices;
    bool m_hasScanned{false};
    bool m_scanRunning{false};

    QListWidget *m_deviceList{};
    DLabel *m_deviceStatus{};
    QPushButton *m_btnRefreshDevices{};

    QListWidget *m_list{};
    DLabel *m_status{};
    QPushButton *m_btnAdd{};
    QPushButton *m_btnRemove{};
    QPushButton *m_btnSave{};
    QPushButton *m_btnReload{};

    QList<ScanExcludeRule> m_scanExcludes;
    QList<AirscanDevice> m_airscanDevices;
    bool m_airscanScanRunning{false};

    QListWidget *m_airscanList{};
    DLabel *m_airscanStatus{};
    QPushButton *m_btnRefreshAirscan{};

    QListWidget *m_scanExcludeList{};
    DLabel *m_scanExcludeStatus{};
    QPushButton *m_btnAddExclude{};
    QPushButton *m_btnRemoveExclude{};
    QPushButton *m_btnSaveExclude{};
};
