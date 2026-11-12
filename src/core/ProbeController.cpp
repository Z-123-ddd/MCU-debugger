#include "core/ProbeController.h"

#include "core/RttClient.h"

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
        if (identified)
            emit earlyIdentified(true, QString(), identified.value);
        else
            emit earlyIdentified(false, identified.error, IdentificationResult());
        // Re-attach with the name J-Link actually accepts. SEGGER ships these parts
        // inside JLinkARM.dll but spelled without the order-code suffix, so
        // "N32G455CC" works while "N32G455CCL7"/"N32G455" fail with
        // "Failed to get index for device name".
        QString jlinkName;
        if (identified)
            jlinkName = identified.value.jlinkDeviceName;
        if (jlinkName.isEmpty() && identified)
            jlinkName = identified.value.probeRsChip;

        if (!jlinkName.isEmpty()
            && jlinkName.compare(QLatin1String("Cortex-M4"), Qt::CaseInsensitive) != 0) {
            Logger::instance()->debug(QStringLiteral("按 DEV_ID 识别为 %1，以 %2 重新连接")
                                         .arg(identified.value.probeRsChip, jlinkName));
            m_probe->setDevice(jlinkName);
            Status reconnect = m_probe->connectTarget();
            if (!reconnect && identified) {
                for (const QString &candidate : identified.value.jlinkDeviceCandidates) {
                    if (candidate.compare(jlinkName, Qt::CaseInsensitive) == 0)
                        continue;
                    m_probe->setDevice(candidate);
                    reconnect = m_probe->connectTarget();
                    if (reconnect) {
                        jlinkName = candidate;
                        break;
                    }
                }
            }
            if (!reconnect) {
                // setDevice() already tore the server down, so leaving the session
                // "connected" here produced a stream of "GDB Server 未连接" reads.
                Logger::instance()->warn(
                    QStringLiteral("以 %1 重连失败: %2；退回通用 Cortex-M4 保持会话可用")
                        .arg(jlinkName, reconnect.error));
                m_probe->setDevice(QStringLiteral("Cortex-M4"));
                reconnect = m_probe->connectTarget();
                if (!reconnect) {
                    Logger::instance()->error(
                        QStringLiteral("通用 Cortex-M4 也无法重连: %1").arg(reconnect.error));
                }
            }
            // Whether or not the re-attach worked, `st` must describe the session
            // that is actually alive now.
            st = reconnect;
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

void ProbeWorker::doScanDeviceTree(const QString &svdPath)
{
    if (!m_probe || !m_probe->isConnected()) {
        emit deviceTreeScanned({}, QStringLiteral("目标未连接，无法读取设备树"));
        return;
    }
    if (svdPath.isEmpty()) {
        emit deviceTreeScanned({}, QStringLiteral("尚未加载 SVD，无法枚举外设"));
        return;
    }

    // Parse on this thread: the file is already in the OS cache and this keeps the
    // UI thread free (a family SVD takes ~20-50 ms).
    const auto parsed = library().load(svdPath);
    if (!parsed) {
        emit deviceTreeScanned({}, QStringLiteral("SVD 解析失败: %1").arg(parsed.error));
        return;
    }

    const QVector<PeripheralStatus> statuses =
        DeviceTreeScanner::scan(*m_probe, parsed.value,
                                [](int done, int total, const QString &name) {
                                    if (done % 20 == 0 || done == total)
                                        Logger::instance()->debug(
                                            QStringLiteral("设备树扫描 %1/%2 (%3)")
                                                .arg(done)
                                                .arg(total)
                                                .arg(name));
                                });
    emit deviceTreeScanned(statuses, QString());
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
    qRegisterMetaType<QVector<mrd::PeripheralStatus>>("QVector<mrd::PeripheralStatus>");

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
    connect(m_worker, &ProbeWorker::earlyIdentified, this,
            [this](bool ok, const QString &error, const IdentificationResult &result) {
                emit earlyIdentified(ok, error, result);
            }, Qt::QueuedConnection);

    connect(m_worker, &ProbeWorker::connectFinished, this,
            [this](bool ok, const QString &error, const ProbeInfo &info) {
                m_info = info;
                emit connectFinished(ok, error, info);
            });
    connect(m_worker, &ProbeWorker::identifyFinished, this, &ProbeController::identifyFinished);
    connect(m_worker, &ProbeWorker::readFinished, this, &ProbeController::readFinished);
    connect(m_worker, &ProbeWorker::writeFinished, this, &ProbeController::writeFinished);
    connect(m_worker, &ProbeWorker::probesListed, this, &ProbeController::probesListed);
    connect(m_worker, &ProbeWorker::deviceTreeScanned, this, &ProbeController::deviceTreeScanned);
    connect(m_worker, &ProbeWorker::rttLocated, this, &ProbeController::rttLocated);
    connect(m_worker, &ProbeWorker::registersRead, this, &ProbeController::registersRead);
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

void ProbeWorker::doFindRtt(quint32 hintAddress, quint32 ramStart, quint32 ramSize)
{
    if (!m_probe) {
        emit rttLocated(RttControlBlock(), QStringLiteral("探针未打开"));
        return;
    }
    const auto found = RttClient::find(*m_probe, hintAddress, ramStart, ramSize);
    if (!found) {
        emit rttLocated(RttControlBlock(), found.error);
        return;
    }
    emit rttLocated(found.value, QString());
}

void ProbeController::requestScanDeviceTree(const QString &svdPath)
{
    dispatch([worker = m_worker, svdPath] { worker->doScanDeviceTree(svdPath); });
}

void ProbeWorker::doReadRegisters()
{
    if (!m_probe) {
        emit registersRead({}, false, QStringLiteral("探针未打开"));
        return;
    }
    QVector<quint32> registers;
    const Status st = m_probe->readCoreRegisters(&registers);
    emit registersRead(registers, static_cast<bool>(st), st.error);
}

void ProbeController::requestReadRegisters()
{
    dispatch([worker = m_worker] { worker->doReadRegisters(); });
}

void ProbeController::requestFindRtt(quint32 hintAddress, quint32 ramStart, quint32 ramSize)
{
    dispatch([worker = m_worker, hintAddress, ramStart, ramSize] {
        worker->doFindRtt(hintAddress, ramStart, ramSize);
    });
}

void ProbeController::requestValidateChip(const QString &chipName)
{
    dispatch([worker = m_worker, chipName] { worker->doValidateChip(chipName); });
}

} // namespace mrd
