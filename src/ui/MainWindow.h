#pragma once

#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/IProbe.h"
#include "core/SvdTypes.h"

#include <QMainWindow>

#include <QSet>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
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
    void closeEvent(QCloseEvent *event) override;

private slots:
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

    void onProbeOpened(bool ok, const QString &error, const ProbeInfo &info);
    void onProbeConnectFinished(bool ok, const QString &error, const ProbeInfo &info);
    void onIdentifyFinished(bool ok, const QString &error, const IdentificationResult &result);
    void onValuesChanged(quint32 firstAddress, quint32 lastAddress);
    void onWriteFinished(quint32 address, bool ok, const QString &error);
    void onReadFailed(quint32 address, const QString &error);
    void onCpuStateChanged(bool halted, bool ok, const QString &error);
    void onPollTimeout();

private:
    void createUi();
    void createToolBar();
    void createDocks();
    void createCentralPages();
    void loadUiState();
    void saveUiState();

    void applyDevice();
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
    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_connLabel = nullptr;
    QLabel *m_pollLabel = nullptr;
    QLabel *m_deviceLabel = nullptr;
    QComboBox *m_backendBox = nullptr;
    QComboBox *m_interfaceBox = nullptr;
    QSpinBox *m_speedBox = nullptr;
    QComboBox *m_chipBox = nullptr;
    QTimer *m_pollTimer = nullptr;
    int m_pollIntervalMs = 200;
    bool m_firstPollLogged = false;
    /// Pins this session configured itself; keeps them from being reported as
    /// "uninitialised" once their bits happen to equal the reset value again.
    QSet<QString> m_configuredPins;
    QStringList m_pinned;
};

} // namespace mrd
