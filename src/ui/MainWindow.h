#pragma once

#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "core/FaultStatus.h"
#include "core/GpioCodec.h"
#include "core/HssSampler.h"
#include "core/IProbe.h"
#include "core/SvdTypes.h"

#include <QMainWindow>

#include <QHash>
#include <QSet>

#include <memory>

class QComboBox;
class QPushButton;
class QLabel;
class QLineEdit;
class QDockWidget;
class QMenu;
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
class SimulationPage;
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

    /// Period of the selected register's own poll.
    ///
    /// ~50 Hz: fast enough that a counter, a status bit or a duty register reads as
    /// continuous motion, while one word read per tick is still trivial bus traffic. It is
    /// a *ceiling*, not a promise -- onFastPollTimeout() skips any tick whose predecessor
    /// has not returned yet, so on a slow clock the effective rate follows the probe.
    static constexpr int kFastPollIntervalMs = 20;

    /// Whether the selected register should be sampled by its own fast timer.
    ///
    /// This is the *fallback* path: it runs when HSS is unavailable (GDB backend, a DLL
    /// without the interface, a halted core) or when another page owns the HSS session.
    /// Kept as a pure function because the conditions are the whole behaviour.
    static bool shouldFastPoll(bool connected, bool hasSelection, bool halted);

    /// Whether the selected register should be sampled by HSS instead of by polling.
    ///
    /// HSS is a single global session on the probe, and the variable-curve page needs the
    /// same one. Rather than let them clobber each other (whichever calls start() last
    /// silently takes the block table, and the loser keeps waiting for data that never
    /// comes), ownership follows the visible page: the register view only takes the
    /// session while its own tab is in front, and hands it back on the way out.
    static bool shouldUseRegisterHss(bool connected, bool hasSelection, bool halted,
                                     bool hssSupported, bool registerPageVisible);

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
    /// The selected register was released (right-click in the register grid).
    void onRegisterDeselected();
    void onFieldSelected(int index);
    void onFieldWrite(const SvdField &field, quint32 value);
    void onRegisterWrite(quint32 value);
    void onRegisterRead();
    void onPinConfigure(const QString &portName, int index, const GpioPinConfig &config);
    void onPinLevel(const QString &portName, int index, bool high);
    void onDeviceTreeScanned(const QVector<PeripheralStatus> &statuses, const QString &error);

    void onProbeOpened(bool ok, const QString &error, const ProbeInfo &info);
    void onProbeConnectFinished(bool ok, const QString &error, const ProbeInfo &info);

    void onValuesChanged(quint32 firstAddress, quint32 lastAddress);
    void onWriteFinished(quint32 address, bool ok, const QString &error);
    void onReadFailed(quint32 address, const QString &error);
    void resetReadFailureState();
    void onCpuStateChanged(bool halted, bool ok, const QString &error);
    void onPollTimeout();
    /// Fast tick: reads only the selected register (see updateFastPoll()).
    void onFastPollTimeout();
    /// HSS delivered samples for the selected register.
    void onRegisterSampled(const mrd::HssSampler::Result &result);
    /// Drain tick for the register's own HSS session.
    void onRegisterDrainTick();
    /// Page activity changed: arbitrate the one HSS session.
    void onMainTabChanged(int index);
    void pollTimerCounters();
    /// Read the SCB fault registers and refresh the status indicator.
    void pollCpuHealth();
    void updateCpuStateLabel();
    void showFaultDetails();

private:
    void createUi();
    /// Analyse a firmware image and offer to burn it (menu 固件 → 烧录固件).
    void onBurnFirmware();
    /// Open the series/order-code picker and apply the choice.
    void onPickChipClicked();
    /// Update the toolbar button to show the picked order code.
    void updateChipLabel();
    /// Everything derived from the part selection goes through here.
    ///
    /// One entry point because five things depend on the same decision and must never
    /// disagree: the SVD device model, the peripheral capabilities (which filter the
    /// tree), flash/RAM figures, the pinout (which also drives m_bondedBits and therefore
    /// the port grid), and the J-Link device name. Refreshing them from different paths is
    /// how the register map and the pinout could end up describing different chips.
    ///
    /// Callers: the picker (switching part/package while connected), the connect path, and
    /// nothing else.
    ///
    /// Single entry point on purpose: the SVD, the memory figures, the pinout and the
    /// device tree all derive from the same selection, and letting them be refreshed by
    /// different paths is how the register map and the pinout could end up describing
    /// different chips.
    bool applyChipSelection();
    /// Ask the worker to probe every peripheral of the loaded SVD.
    void scanAndValidateDeviceTree();
    /// Start or stop the selected-register poll to match the current state.
    void updateFastPoll();
    /// Start/stop HSS for the selected register, and fall back to polling when it cannot
    /// run. This is the only place that decides which of the two mechanisms is in charge.
    void updateRegisterSampling();
    /// Is HSS currently sampling the selected register?
    bool registerHssInCharge() const;
    /// Rewrite the "轮询: ..." indicator from the current rates.
    void updatePollLabel();
    /// Undo the session and explain why (wrong part, unusable SVD).
    void refuseConnection(const QString &reason);
    /// After an async burn + reconnect: say whether the target came back up running.
    void reportPostBurnState();
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
    SimulationPage *m_simulationPage = nullptr;
    QDockWidget *m_leftDock = nullptr;
    QTabWidget *m_leftTabs = nullptr;
    QMenu *m_viewMenu = nullptr;
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
    QComboBox *m_interfaceBox = nullptr;
    QSpinBox *m_speedBox = nullptr;
    QPushButton *m_partButton = nullptr;
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
    /// Polls *only* the register the user has selected, much faster than the rest.
    ///
    /// Selecting a register is how the operator says "this one, I am watching it change",
    /// so it gets its own cheap single-word read instead of waiting for the 200 ms sweep
    /// of the whole peripheral. The other registers keep the slow rate -- a whole-peripheral
    /// read at this frequency would be pure bus traffic for values nobody is looking at.
    QTimer *m_fastPollTimer = nullptr;
    /// HSS session for the selected register only.
    ///
    /// A separate instance from the curve page's on purpose: each one manages its own
    /// start/stop/drain state, and the page arbitration above decides which of them is
    /// allowed to hold the (single, global) probe session at any moment.
    HssSampler *m_registerSampler = nullptr;
    /// Drains the register's HSS buffer. The probe samples at its own period; this only
    /// picks up whatever has accumulated.
    QTimer *m_registerDrainTimer = nullptr;
    /// Address currently being sampled, so a selection change is detected and the session
    /// is rebuilt (the block table is fixed at start()).
    quint32 m_registerHssAddress = 0;
    /// The 寄存器全景 tab page, so the arbitration can ask "is this page in front?" by
    /// widget identity instead of a hardcoded tab index.
    QWidget *m_registerPage = nullptr;
    /// Set when HSS refused this register *after* start() had already returned success.
    ///
    /// The refusal is asynchronous (`started(false, ...)`), and the handler would
    /// otherwise re-run the arbitration, ask to start again, and be refused again -- a
    /// retry loop at timer speed. Cleared whenever the selection or the link changes,
    /// i.e. the next time a human action makes a fresh attempt meaningful.
    bool m_registerHssRefused = false;
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
    /// Last counter reading per timer, so the tree can show whether it advances.
    QHash<QString, quint32> m_lastCounter;
    QVector<PeripheralStatus> m_statuses;
    QStringList m_pinned;
    /// Set while a burn is waiting for its reconnect, so the post-burn check runs once.
    bool m_pendingPostBurnCheck = false;
    QString m_postBurnRange;
};

} // namespace mrd
