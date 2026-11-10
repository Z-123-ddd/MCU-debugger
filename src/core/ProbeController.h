#pragma once

#include "core/DeviceIdentifier.h"
#include "core/IProbe.h"
#include "core/DeviceTreeScanner.h"
#include "core/JLinkGdbProbe.h"
#include "core/ProbeRsProbe.h"
#include "core/SvdLibrary.h"

#include <QByteArray>
#include <QObject>
#include <QThread>

#include <memory>

namespace mrd {

/// Lives in the probe thread: every `probe-rs` process is spawned and awaited
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
    void doValidateChip(const QString &chipName);
    void doScanDeviceTree(const QString &svdPath);

signals:
    void opened(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void closed();
    void connectFinished(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void identifyFinished(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void readFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok, const QString &error);
    void writeFinished(quint64 requestId, quint32 address, bool ok, const QString &error);
    void cpuStateChanged(bool halted, bool ok, const QString &error);
    void probesListed(const QStringList &serials, const QString &error);
    void chipValidated(const QString &chipName, bool ok, const QString &message);
    void deviceTreeScanned(const QVector<mrd::PeripheralStatus> &statuses, const QString &error);

private:
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

signals:
    void opened(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void closed();
    void connectFinished(bool ok, const QString &error, const mrd::ProbeInfo &info);
    void identifyFinished(bool ok, const QString &error, const mrd::IdentificationResult &result);
    void readFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok, const QString &error);
    void writeFinished(quint64 requestId, quint32 address, bool ok, const QString &error);
    void cpuStateChanged(bool halted, bool ok, const QString &error);
    void probesListed(const QStringList &serials, const QString &error);
    void chipValidated(const QString &chipName, bool ok, const QString &message);
    void deviceTreeScanned(const QVector<mrd::PeripheralStatus> &statuses, const QString &error);

private:
    template <typename Fn>
    void dispatch(Fn &&fn);

    QThread m_thread;
    ProbeWorker *m_worker = nullptr;
    ProbeInfo m_info;
    QString m_backend;
    quint64 m_requestId = 0;
};

} // namespace mrd
