#include "core/ProbeController.h"

#include "core/AppSettings.h"
#include "core/Logger.h"
#include "core/JLinkRunner.h"
#include "core/ProbeRsRunner.h"

#include <QMetaObject>

namespace mrd {

// ---------------------------------------------------------------------------
// ProbeWorker
// ---------------------------------------------------------------------------

ProbeWorker::ProbeWorker(QObject *parent)
    : QObject(parent)
{
}

ProbeWorker::~ProbeWorker() = default;

SvdLibrary &ProbeWorker::library()
{
    if (!m_libraryScanned) {
        m_library.setRoots(AppSettings::svdRoots());
        m_library.scan(false);
        m_libraryScanned = true;
    }
    return m_library;
}

QString ProbeWorker::probeSerial() const
{
    if (!m_probe)
        return QString();
    const ProbeInfo info = m_probe->info();
    return info.probeSerials.isEmpty() ? QString() : info.probeSerials.first();
}

void ProbeWorker::doOpen(const QString &backend, const QString &binaryPath, const QString &interfaceName,
                         int speedKHz)
{
    const QString wanted = (backend == QLatin1String("probe-rs")) ? QStringLiteral("probe-rs")
                                                                  : QStringLiteral("jlink");

    // Rebuild the probe when the backend changes.
    if (m_probe && m_backend != wanted) {
        m_probe->close();
        m_probe.reset();
    }
    m_backend = wanted;

    if (!m_probe) {
        if (wanted == QLatin1String("probe-rs"))
            m_probe = std::make_unique<ProbeRsProbe>();
        else
            m_probe = std::make_unique<JLinkGdbProbe>();
    }

    if (auto *jlink = dynamic_cast<JLinkGdbProbe *>(m_probe.get())) {
        jlink->setExecutable(binaryPath.isEmpty() ? JLinkRunner::locate(AppSettings::jlinkPath()) : binaryPath);
    } else if (auto *probeRs = dynamic_cast<ProbeRsProbe *>(m_probe.get())) {
        if (probeRs->runner())
            probeRs->runner()->setBinary(binaryPath);
    }

    m_probe->setInterface(interfaceName);
    m_probe->setSpeed(speedKHz);

    const Status st = m_probe->open();
    emit opened(static_cast<bool>(st), st.error, m_probe->info());
}

void ProbeWorker::doClose()
{
    if (m_probe) {
        m_probe->close();
        m_probe.reset();
    }
    emit closed();
}

void ProbeWorker::doSelectChip(const QString &chipName)
{
    if (!m_probe) {
        emit connectFinished(false, QStringLiteral("探针未打开"), ProbeInfo());
        return;
    }
    const Status st = m_probe->setDevice(chipName);
    emit connectFinished(static_cast<bool>(st), st.error, m_probe->info());
}

void ProbeWorker::doConnect(const QString &chipName)
{
    if (!m_probe) {
        emit connectFinished(false, QStringLiteral("探针未打开"), ProbeInfo());
        return;
    }
    if (!m_probe->isOpen()) {
        const Status opened = m_probe->open();
        if (!opened) {
            emit connectFinished(false, opened.error, m_probe->info());
            return;
        }
    }

    // Without an explicit chip we attach with a generic core, then identify the
    // part from DBGMCU_IDCODE and re-attach with the resolved name.
    const bool probeGenerically = chipName.trimmed().isEmpty();
    m_probe->setDevice(probeGenerically ? QStringLiteral("Cortex-M4") : chipName);

    Status st = m_probe->connectTarget();
    if (st && probeGenerically) {
        QString dbError;
        if (DeviceIdentifier::database().isEmpty())
            DeviceIdentifier::loadDatabase(&dbError);
        const auto identified = DeviceIdentifier::identify(*m_probe, library(), QString(), probeSerial());
        if (identified && !identified.value.probeRsChip.isEmpty()
            && identified.value.probeRsChip.compare(QLatin1String("Cortex-M4"), Qt::CaseInsensitive) != 0) {
            Logger::instance()->info(QStringLiteral("按 DEV_ID 识别为 %1，重新连接")
                                         .arg(identified.value.probeRsChip));
            m_probe->setDevice(identified.value.probeRsChip);
            const Status reconnect = m_probe->connectTarget();
            if (!reconnect) {
                Logger::instance()->warn(QStringLiteral("以 %1 重连失败: %2")
                                             .arg(identified.value.probeRsChip, reconnect.error));
            } else {
                st = reconnect;
            }
        }
    }

    emit connectFinished(static_cast<bool>(st), st.error, m_probe->info());
}

void ProbeWorker::doDisconnect()
{
    if (m_probe)
        m_probe->disconnectTarget();
    emit closed();
}

void ProbeWorker::doIdentify(const QString &overrideChip)
{
    if (!m_probe || !m_probe->isConnected()) {
        emit identifyFinished(false, QStringLiteral("目标未连接，无法识别 MCU"), IdentificationResult());
        return;
    }

    QString dbError;
    if (DeviceIdentifier::database().isEmpty() && !DeviceIdentifier::loadDatabase(&dbError))
        Logger::instance()->warn(QStringLiteral("器件识别库加载失败: %1").arg(dbError));

    const auto result = DeviceIdentifier::identify(*m_probe, library(), overrideChip, probeSerial());
    if (!result) {
        emit identifyFinished(false, result.error, IdentificationResult());
        return;
    }
    emit identifyFinished(true, QString(), result.value);
}

void ProbeWorker::doRead(quint64 requestId, quint32 address, int length)
{
    if (!m_probe) {
        emit readFinished(requestId, address, QByteArray(), false, QStringLiteral("探针未打开"));
        return;
    }
    QByteArray data;
    const Status st = m_probe->readMemory(address, length, &data);
    emit readFinished(requestId, address, data, static_cast<bool>(st), st.error);
}

void ProbeWorker::doWrite(quint64 requestId, quint32 address, const QByteArray &data)
{
    if (!m_probe) {
        emit writeFinished(requestId, address, false, QStringLiteral("探针未打开"));
        return;
    }
    const Status st = m_probe->writeMemory(address, data);
    emit writeFinished(requestId, address, static_cast<bool>(st), st.error);
}

void ProbeWorker::doHalt()
{
    if (!m_probe) {
        emit cpuStateChanged(false, false, QStringLiteral("探针未打开"));
        return;
    }
    const Status st = m_probe->halt();
    emit cpuStateChanged(true, static_cast<bool>(st), st.error);
}

void ProbeWorker::doRun()
{
    if (!m_probe) {
        emit cpuStateChanged(false, false, QStringLiteral("探针未打开"));
        return;
    }
    const Status st = m_probe->resume();
    emit cpuStateChanged(false, static_cast<bool>(st), st.error);
}

void ProbeWorker::doReset()
{
    if (!m_probe) {
        emit cpuStateChanged(false, false, QStringLiteral("探针未打开"));
        return;
    }
    const Status st = m_probe->resetTarget();
    emit cpuStateChanged(false, static_cast<bool>(st), st.error);
}

void ProbeWorker::doExec(const QString &command)
{
    if (!m_probe)
        return;
    const Status st = m_probe->exec(command);
    if (!st)
        Logger::instance()->warn(QStringLiteral("probe-rs 命令失败: %1").arg(st.error));
}

void ProbeWorker::doListProbes()
{
    // J-Link: report the emulator list from the vendor CLI.
    if (m_backend != QLatin1String("probe-rs")) {
        const QString exe = JLinkRunner::locate(AppSettings::jlinkPath());
        if (exe.isEmpty()) {
            emit probesListed({}, QStringLiteral("未找到 JLink.exe"));
            return;
        }
        const auto result = JLinkRunner::runScript(exe, {QStringLiteral("ShowEmuList")}, 30000);
        if (!result) {
            emit probesListed({}, result.error);
            return;
        }
        QStringList lines;
        for (const QString &line : result.value.split(QLatin1Char(10), Qt::SkipEmptyParts)) {
            if (line.contains(QLatin1String("J-Link[")))
                lines << line.trimmed();
        }
        emit probesListed(lines, lines.isEmpty() ? QStringLiteral("未发现 J-Link") : QString());
        return;
    }

    ProbeRsRunner runner;
    if (auto *probeRs = dynamic_cast<ProbeRsProbe *>(m_probe.get())) {
        if (probeRs->runner())
            runner.setBinary(probeRs->runner()->binary());
    }
    const QString error = runner.ensureBinary();
    if (!error.isEmpty()) {
        emit probesListed({}, error);
        return;
    }
    const ProbeRsRun run = runner.run({QStringLiteral("list")}, 30000);
    if (!run.ok()) {
        emit probesListed({}, run.errorText().trimmed());
        return;
    }
    QStringList serials;
    for (const QString &line : run.stdOut.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')))
            serials << trimmed;
    }
    emit probesListed(serials, QString());
}

void ProbeWorker::doValidateChip(const QString &chipName)
{
    if (!m_probe) {
        emit chipValidated(chipName, false, QStringLiteral("探针未打开"));
        return;
    }
    QString message;
    const Status st = m_probe->validateChipName(chipName, &message);
    emit chipValidated(chipName, static_cast<bool>(st), message);
}

// ---------------------------------------------------------------------------
// ProbeController
// ---------------------------------------------------------------------------

ProbeController::ProbeController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<mrd::ProbeInfo>("mrd::ProbeInfo");
    qRegisterMetaType<mrd::IdentificationResult>("mrd::IdentificationResult");

    m_worker = new ProbeWorker();
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &ProbeWorker::opened, this, [this](bool ok, const QString &error, const ProbeInfo &info) {
        m_info = info;
        emit opened(ok, error, info);
    });
    connect(m_worker, &ProbeWorker::closed, this, [this] {
        m_info = ProbeInfo();
        emit closed();
    });
    connect(m_worker, &ProbeWorker::connectFinished, this,
            [this](bool ok, const QString &error, const ProbeInfo &info) {
                m_info = info;
                emit connectFinished(ok, error, info);
            });
    connect(m_worker, &ProbeWorker::identifyFinished, this, &ProbeController::identifyFinished);
    connect(m_worker, &ProbeWorker::readFinished, this, &ProbeController::readFinished);
    connect(m_worker, &ProbeWorker::writeFinished, this, &ProbeController::writeFinished);
    connect(m_worker, &ProbeWorker::probesListed, this, &ProbeController::probesListed);
    connect(m_worker, &ProbeWorker::chipValidated, this, &ProbeController::chipValidated);
    connect(m_worker, &ProbeWorker::cpuStateChanged, this, [this](bool halted, bool ok, const QString &error) {
        if (ok)
            m_info.halted = halted;
        emit cpuStateChanged(halted, ok, error);
    });

    m_thread.setObjectName(QStringLiteral("ProbeThread"));
    m_thread.start();
}

ProbeController::~ProbeController()
{
    m_thread.quit();
    m_thread.wait(5000);
}

template <typename Fn>
void ProbeController::dispatch(Fn &&fn)
{
    QMetaObject::invokeMethod(m_worker, std::forward<Fn>(fn), Qt::QueuedConnection);
}

void ProbeController::requestOpen(const QString &backend, const QString &binaryPath,
                                 const QString &interfaceName, int speedKHz)
{
    m_backend = backend;
    dispatch([worker = m_worker, backend, binaryPath, interfaceName, speedKHz] {
        worker->doOpen(backend, binaryPath, interfaceName, speedKHz);
    });
}

void ProbeController::requestClose()
{
    dispatch([worker = m_worker] { worker->doClose(); });
}

void ProbeController::requestConnect(const QString &chipName)
{
    dispatch([worker = m_worker, chipName] { worker->doConnect(chipName); });
}

void ProbeController::requestDisconnect()
{
    dispatch([worker = m_worker] { worker->doDisconnect(); });
}

void ProbeController::requestIdentify(const QString &overrideChip)
{
    dispatch([worker = m_worker, overrideChip] { worker->doIdentify(overrideChip); });
}

void ProbeController::requestRead(quint64 requestId, quint32 address, int length)
{
    dispatch([worker = m_worker, requestId, address, length] { worker->doRead(requestId, address, length); });
}

void ProbeController::requestWrite(quint64 requestId, quint32 address, const QByteArray &data)
{
    dispatch([worker = m_worker, requestId, address, data] { worker->doWrite(requestId, address, data); });
}

void ProbeController::requestHalt()
{
    dispatch([worker = m_worker] { worker->doHalt(); });
}

void ProbeController::requestRun()
{
    dispatch([worker = m_worker] { worker->doRun(); });
}

void ProbeController::requestReset()
{
    dispatch([worker = m_worker] { worker->doReset(); });
}

void ProbeController::requestSelectChip(const QString &chipName)
{
    dispatch([worker = m_worker, chipName] { worker->doSelectChip(chipName); });
}

void ProbeController::requestExec(const QString &command)
{
    dispatch([worker = m_worker, command] { worker->doExec(command); });
}

void ProbeController::requestListProbes()
{
    dispatch([worker = m_worker] { worker->doListProbes(); });
}

void ProbeController::requestValidateChip(const QString &chipName)
{
    dispatch([worker = m_worker, chipName] { worker->doValidateChip(chipName); });
}

} // namespace mrd
