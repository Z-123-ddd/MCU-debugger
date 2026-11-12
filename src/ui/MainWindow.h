#pragma once

#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "core/FaultStatus.h"
#include "core/GpioCodec.h"
#include "core/IProbe.h"
#include "core/SvdTypes.h"

#include <QMainWindow>

#include <QHash>
#include <QSet>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QDockWidget;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QToolBar;
class QTimer;

namespace mrd {

class BitFieldStrip;
class InspectorPanel;
class PackagePage;
class PeripheralTree;
class ScopePage;
class PortGridView;
class ProbeController;
class RegisterBlockView;
class RegisterSession;

/// Main window: probe toolbar, peripheral tree, package/port/register views and
/// the register inspector.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Load an SVD file (`--svd` or File/Open).
    bool openSvdFile(const QString &path);

    /// Load a package description directory (defaults to `<data>/packages`).
    void loadPackages(const QString &directory);

    /// Connect to the target right after the window is shown.
    void scheduleAutoConnect();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    /// Park every target-dependent view (also reachable via invokeMethod for tests).
    void parkDeviceViews(const QString &reason);
    void onLogMessage(const QString &text, int level);

    void onOpenSvd();
    void onConnectClicked();
    void onDisconnectClicked();
    void onHaltClicked();
    void onRunClicked();
    void onResetClicked();
    void onUndoClicked();
    void onSearchSvd();

    void onPeripheralSelected(const QString &name);
    void onPinnedChanged(const QStringList &names);
    void onRegisterSelected(int index);
    void onFieldSelected(int index);
    void onFieldWrite(const SvdField &field, quint32 value);
    void onRegisterWrite(quint32 value);
    void onRegisterRead();
    void onPinConfigure(const QString &portName, int index, const GpioPinConfig &config);
    void onPinLevel(const QString &portName, int index, bool high);
    void onDeviceTreeScanned(const QVector<PeripheralStatus> &statuses, const QString &error);

    void onProbeOpened(bool ok, const QString &error, const ProbeInfo &info);
    void onProbeConnectFinished(bool ok, const QString &error, const ProbeInfo &info);
    void onIdentifyFinished(bool ok, const QString &error, const IdentificationResult &result);

    /// Identification the probe already did while attaching; reused instead of
    /// running the whole thing a second time.
    void onEarlyIdentified(bool ok, const QString &error, const IdentificationResult &result);
    void onValuesChanged(quint32 firstAddress, quint32 lastAddress);
    void onWriteFinished(quint32 address, bool ok, const QString &error);
    void onReadFailed(quint32 address, const QString &error);
    void resetReadFailureState();
    void onCpuStateChanged(bool halted, bool ok, const QString &error);
    void onPollTimeout();
    void pollTimerCounters();
    /// Read the SCB fault registers and refresh the status indicator.
    void pollCpuHealth();
    void updateCpuStateLabel();
    void showFaultDetails();

private:
    void createUi();
    void createToolBar();
    void createDocks();
    void createCentralPages();
    void loadUiState();
    void saveUiState();

    void applyDevice();
    void updateMemoryLabel();
    /// Park every target-dependent view (inspector, bit strip, register map, port
    /// grid, package, tree) and forget the current selection.
    ///
    /// Push live values (timer counters) into the peripheral tree.
    void refreshLiveValues();
    /// Fill the tree / port grid / register view. Only called once the target's
    /// device tree has been read back, so nothing is shown for an unconnected SVD.
    void populateDeviceUi();
    void setConnectedUi(bool connected);
    void refreshValues(bool force = false);
    void refreshInspector();
    void refreshPortGrid();
    void startPolling();
    void stopPolling();
    const SvdPeripheral *currentPeripheral() const;
    const SvdRegister *currentRegister() const;
    QString effectiveChip() const;

    std::unique_ptr<ProbeController> m_probe;
    std::unique_ptr<RegisterSession> m_session;

    SvdDevice m_device;
    QString m_currentPeripheral;
    int m_currentRegister = -1;
    QString m_svdPath;
    bool m_autoLoadAttempted = false;

    // widgets
    QToolBar *m_toolBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    PeripheralTree *m_tree = nullptr;
    InspectorPanel *m_inspector = nullptr;
    RegisterBlockView *m_registerView = nullptr;
    BitFieldStrip *m_strip = nullptr;
    PackagePage *m_packagePage = nullptr;
    PortGridView *m_portGrid = nullptr;
    ScopePage *m_scopePage = nullptr;
    QDockWidget *m_variableDock = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_connLabel = nullptr;
    QLabel *m_pollLabel = nullptr;
    QLabel *m_deviceLabel = nullptr;
    /// "Flash 512 KB / RAM 192 KB" of the attached part (empty until identified).
    QLabel *m_memoryLabel = nullptr;
    int m_flashBytes = 0;
    int m_ramBytes = 0;
    QString m_sramText;
    QString m_ccmText;
    QString m_partNumber;
    QComboBox *m_backendBox = nullptr;
    QComboBox *m_interfaceBox = nullptr;
    QSpinBox *m_speedBox = nullptr;
    QComboBox *m_chipBox = nullptr;
    QTimer *m_pollTimer = nullptr;
    /// Coalesces value updates into one UI refresh per burst.
    ///
    /// Every completed read emits valuesChanged, and a poll issues a dozen of
    /// them (pinned peripherals + timer counters). Repainting the block view,
    /// inspector, port grid and package per completion made the window crawl,
    /// so refreshes are merged into one per ~30 ms.
    QTimer *m_refreshTimer = nullptr;
    bool m_refreshPending = false;
    /// Timer counters move slowly; poll them less often than the registers.
    QTimer *m_counterTimer = nullptr;
    /// Polls the SCB fault registers so a crashed MCU is visible in the status bar.
    QTimer *m_faultTimer = nullptr;
    QLabel *m_cpuStateLabel = nullptr;
    FaultSnapshot m_fault;
    CpuHealth m_cpuHealth = CpuHealth::Unknown;
    bool m_cpuHalted = false;
    /// Consecutive read failures that look like a lost session.
    ///
    /// A dead GDB server makes every polled read fail; logging each one flooded the
    /// log with "GDB Server 未连接" every 200 ms and kept polling a corpse.
    int m_deadSessionReads = 0;
    QString m_lastReadError;
    bool m_readErrorLogged = false;
    /// Guards the field-selection mirror between the two bit-field strips.
    bool m_syncingFieldSelection = false;
    int m_pollIntervalMs = 200;
    bool m_firstPollLogged = false;
    /// Pins this session configured itself; keeps them from being reported as
    /// "uninitialised" once their bits happen to equal the reset value again.
    QSet<QString> m_configuredPins;
    /// Port ("PA") -> the bit numbers this package bonds out. Empty when no
    /// package description is available.
    QHash<QString, QVector<int>> m_bondedBits;
    /// Vendor per-model peripheral counts (CMSIS-Pack .pdsc) for the identified
    /// part; empty when the pack has no entry, which disables static trimming.
    QHash<QString, int> m_capabilities;
    /// Identification already done by the probe while attaching (auto mode).
    bool m_identifyPending = false;
    IdentificationResult m_pendingIdentification;
    /// GPIO ports the die does not implement (see DeviceIdentifier::probeGpioPorts).
    PortProbe m_portProbe;
    /// Last counter reading per timer, so the tree can show whether it advances.
    QHash<QString, quint32> m_lastCounter;
    QVector<PeripheralStatus> m_statuses;
    QStringList m_pinned;
};

} // namespace mrd
