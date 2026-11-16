#pragma once

#include "core/DeviceIdentifier.h"
#include "core/IProbe.h"
#include "core/DeviceTreeScanner.h"
#include "core/JLinkGdbProbe.h"
#include "core/SvdLibrary.h"

#include <QByteArray>
#include <QObject>
#include <QThread>

#include <memory>

namespace mrd {

/// Lives in the probe thread: the DLL session and every helper process are owned
/// here so the UI never blocks on a multi-second attach.
class ProbeWorker : public QObject
{
    Q_OBJECT

public:
    explicit ProbeWorker(QObject *parent = nullptr);
    ~ProbeWorker() override;

    void doOpen(const QString &backend, const QString &binaryPath, const QString &interfaceName,
                int speedKHz);
    void doClose();
    void doConnect(const QString &chipName);
    void doDisconnect();
    void doIdentify(const QString &overrideChip);
    void doRead(quint64 requestId, quint32 address, int length);
    void doWrite(quint64 requestId, quint32 address, const QByteArray &data);
    void doHalt();
    void doRun();
    void doReset();
    void doSelectChip(const QString &chipName);
    void doExec(const QString &command);
    void doListProbes();
    /// Insert or clear a software breakpoint on the worker thread.
    void doValidateChip(const QString &chipName);
    void doScanDeviceTree(const QString &svdPath);
    /// Start/stop/drain HSS. These must run on the worker thread: the DLL session
    /// lives there, and HSS is stateful (start once, then read repeatedly), so the UI
    /// cannot drive it by pretending each call is an independent request.
    void doHssStart(const QVector<QPair<quint32, int>> &blocks, int periodUs);
    void doHssRead(quint64 requestId, int maxSamples, int sampleBytes);
    void doHssStop();

signals:
    void opened(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void closed();
    void connectFinished(bool ok, const QString &error, const mrd::ProbeInfo &info);

    /// Identification already performed while attaching (auto mode).
    ///
    /// doConnect() has to identify the part anyway, in order to re-attach with the
    /// right device name. Emitting that result lets the UI skip a second, identical
    /// identify, which otherwise doubles the connect log and the library scans.
    void earlyIdentified(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void identifyFinished(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void readFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok, const QString &error);
    void writeFinished(quint64 requestId, quint32 address, bool ok, const QString &error);
    void cpuStateChanged(bool halted, bool ok, const QString &error);
    void probesListed(const QStringList &serials, const QString &error);
    void chipValidated(const QString &chipName, bool ok, const QString &message);
    void deviceTreeScanned(const QVector<mrd::PeripheralStatus> &statuses, const QString &error);
    /// HSS outcome from the worker thread (the UI cannot reach the probe object).
    void hssStarted(bool ok, const QString &error);
    void hssReadFinished(quint64 requestId, const QByteArray &records, int samples,
                         int sampleBytes, bool ok, const QString &error);
    void hssFailed(const QString &error);
    /// Whether the current session can do HSS; published when the probe opens.
    void hssAvailability(bool supported);

private:
    /// Set while opening: the UI asks this instead of the probe, which lives here.
    bool m_supportsHss = false;

    SvdLibrary &library();
    QString probeSerial() const;
    QString m_backend = QStringLiteral("jlink");

    std::unique_ptr<IProbe> m_probe;
    SvdLibrary m_library;
    bool m_libraryScanned = false;
};

/// UI-facing facade: marshals calls onto the probe thread and re-emits results.
class ProbeController : public QObject
{
    Q_OBJECT

public:
    explicit ProbeController(QObject *parent = nullptr);
    ~ProbeController() override;

    bool isOpen() const { return m_info.open; }
    bool isConnected() const { return m_info.connected; }
    ProbeInfo info() const { return m_info; }
    QString backend() const { return m_backend; }
    quint64 nextRequestId() { return ++m_requestId; }

    /// True when the active backend can do HSS background sampling.
    ///
    /// The probe object lives on the worker thread and cannot be queried from the UI,
    /// so the worker publishes this flag when it opens the session.
    bool supportsHss() const { return m_supportsHss; }

public slots:
    void requestOpen(const QString &backend, const QString &binaryPath, const QString &interfaceName,
                     int speedKHz);
    void requestClose();
    void requestConnect(const QString &chipName);
    void requestDisconnect();
    void requestIdentify(const QString &overrideChip);
    void requestRead(quint64 requestId, quint32 address, int length);
    void requestWrite(quint64 requestId, quint32 address, const QByteArray &data);
    void requestHalt();
    void requestRun();
    void requestReset();
    void requestSelectChip(const QString &chipName);
    void requestExec(const QString &command);
    void requestListProbes();
    void requestValidateChip(const QString &chipName);
    /// Walk every SVD peripheral through the probe to build the real device tree.
    void requestScanDeviceTree(const QString &svdPath);

    void requestHssStart(const QVector<QPair<quint32, int>> &blocks, int periodUs);
    void requestHssRead(quint64 requestId, int maxSamples, int sampleBytes);
    void requestHssStop();
    /// Insert ( insert true) or clear a breakpoint; result via breakpointChanged.

signals:
    void opened(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void closed();
    void connectFinished(bool ok, const QString &error, const mrd::ProbeInfo &info);

    /// Identification already done while attaching (see ProbeWorker::earlyIdentified).
    void earlyIdentified(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void identifyFinished(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void readFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok, const QString &error);
    void writeFinished(quint64 requestId, quint32 address, bool ok, const QString &error);
    void cpuStateChanged(bool halted, bool ok, const QString &error);
    void probesListed(const QStringList &serials, const QString &error);
    void chipValidated(const QString &chipName, bool ok, const QString &message);
    void deviceTreeScanned(const QVector<mrd::PeripheralStatus> &statuses, const QString &error);
    /// Re-emitted from the worker so the UI can follow the HSS lifecycle.
    void hssStarted(bool ok, const QString &error);
    void hssReadFinished(quint64 requestId, const QByteArray &records, int samples,
                         int sampleBytes, bool ok, const QString &error);
    void hssFailed(const QString &error);

private:
    template <typename Fn>
    void dispatch(Fn &&fn);

    QThread m_thread;
    ProbeWorker *m_worker = nullptr;
    ProbeInfo m_info;
    QString m_backend;
    quint64 m_requestId = 0;
    bool m_supportsHss = false;
};

} // namespace mrd
