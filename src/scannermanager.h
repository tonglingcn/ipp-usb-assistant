#pragma once

#include <QObject>
#include <QStringListModel>

/**
 * 扫描参数：分辨率与色彩模式
 */
struct ScanOptions {
    int resolution = 300;        // DPI：75 / 150 / 300 / 600
    QString colorMode = "Color"; // SANE mode 选项值：Color / Gray / Lineart
};

/**
 * 扫描设备所属后端类别。
 *
 * SANE 一次枚举会同时返回免驱设备与厂商原生驱动设备，两者能力差异很大：
 *  - Escl   ：airscan / escl 后端，即本软件主打的 IPP-USB 免驱扫描
 *             （设备名前缀 airscan: 或 escl:）
 *  - Vendor ：厂商自带 SANE 驱动（如 HP 的 hpaio:、爱普生的 epson2:、
 *             佳能的 pixma:）。这类设备不走 eSCL，参数集与免驱设备不同。
 *
 * 区分二者是为了：1) 界面上如实标注，不让用户误以为是免驱设备；
 * 2) 扫描时按类别选用不同的参数策略——厂商驱动对 --source 等选项
 * 支持度不一，强行传递会直接报 "invalid option"。
 */
enum class ScanBackend {
    Escl,     ///< 免驱 eSCL（airscan / escl）
    Vendor,   ///< 厂商原生 SANE 驱动
    Unknown   ///< 未能判定，按最保守参数处理
};

/**
 * 扫描管理：基于 SANE 实现 USB eSCL 扫描发现与成像。
 *
 * 设计要点：
 *  - 通过 sane_get_devices(SANE_FALSE) 列举全部设备，**不做后端过滤**。
 *    实测同一台设备可能同时被多个后端发现（如 sane-escl 与 sane-airscan），
 *    且环境中还存在网络扫描仪，按后端名过滤会误杀可用设备，
 *    因此保留全部，由 UI 通过 [Flatbed]/[ADF] 标注帮助用户区分。
 *  - 注意：必须传 SANE_FALSE。传 SANE_TRUE 会只返回本地设备，
 *    导致 sane-airscan 设备全部为空（见 docs/principles.md）。
 *  - 扫描参数（分辨率/色彩）通过 scanimage 命令行传递。
 *  - 结果通过 scanFinished 信号回传，由界面负责展示预览。
 */
class ScannerManager : public QObject
{
    Q_OBJECT
public:
    explicit ScannerManager(QObject *parent = nullptr);

    /// 发现扫描仪（USB eSCL / airscan）
    void discover();

    /// 使用指定设备执行扫描，结果保存为 PNG
    /// @param deviceIndex 设备在 devices() 中的下标；-1 表示第一台可用设备
    void scan(int deviceIndex, const ScanOptions &opts);

    QStringListModel *model() { return &m_model; }
    QStringList devices() const { return m_devices; }

    /// 指定设备的后端类别；下标越界时返回 Unknown
    ScanBackend backend(int index) const;

    /// 当前设备列表是否包含真实扫描仪（过滤提示文案）
    bool hasDevices() const;

    /// 判断设备属于免驱 eSCL 还是厂商原生驱动。
    /// @param name SANE 设备名（如 "airscan:e0:..."、"hpaio:/usb/HP..."）
    /// @param type SANE_Device::type（如 "virtual device"、"flatbed scanner"）
    static ScanBackend classifyBackend(const QString &name, const QString &type);

signals:
    /// 单次发现完成（成功或失败均会发出）
    void discoveryFinished(bool found);

    /// 单次扫描完成；ok=false 时 errMsg 携带失败原因
    void scanFinished(bool ok, const QString &outPath, const QString &errMsg);

private:
    /// 为设备生成显示标签：原始名称 + [扫描源]，例如
    /// airscan:e0:... [Flatbed] / airscan:e1:... [ADF]
    static QString formatDeviceLabel(const QString &dev, const QStringList &sources);

    QStringListModel m_model;
    QStringList m_devices;       // 完整 SANE 设备名（scanimage -d 使用）
    QStringList m_deviceLabels;  // 界面显示名（含扫描源标注）
    QList<ScanBackend> m_backends;  // 与 m_devices 一一对应的后端类别
    bool m_discoveryRunning = false;
    bool m_scanRunning = false;
};
