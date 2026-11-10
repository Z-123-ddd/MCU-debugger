#include "core/JLinkGdbProbe.h"

#include "core/HexUtil.h"
#include "core/JLinkRunner.h"
#include "core/Logger.h"

#include <QDeadlineTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>

#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpServer>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace mrd {

namespace {
constexpr quint32 kDhcsrAddress = 0xE000EDF0u;
}

JLinkGdbProbe::JLinkGdbProbe()
    : m_executable(JLinkRunner::locate())
{
}

JLinkGdbProbe::~JLinkGdbProbe()
{
    close();
}

QString JLinkGdbProbe::defaultGdbServerPath(const QString &jlinkExecutable)
{
    if (jlinkExecutable.isEmpty())
        return QString();
    const QString dir = QFileInfo(jlinkExecutable).absolutePath();
    const QStringList names = {QStringLiteral("/JLinkGDBServerCL.exe"), QStringLiteral("/JLinkGDBServerCL"),
                               QStringLiteral("/JLinkGDBServer")};
    for (const QString &name : names) {
        const QString candidate = dir + name;
        if (QFileInfo(candidate).isFile())
            return QDir::cleanPath(candidate);
    }
    return QString();
}

namespace {

QString pidFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/jlink-gdb.pid");
}

void recordServerPid(qint64 pid)
{
    QSettings settings(pidFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("pid"), pid);
    settings.setValue(QStringLiteral("startedAt"), QDateTime::currentSecsSinceEpoch());
}

void clearServerPid()
{
    QFile::remove(pidFilePath());
}

/// Image name of a running process (empty when it is gone).
QString processImageName(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                                      static_cast<DWORD>(pid));
    if (!handle)
        return QString();

    wchar_t buffer[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    QString name;
    if (QueryFullProcessImageNameW(handle, 0, buffer, &size))
        name = QString::fromWCharArray(buffer, static_cast<int>(size));
    CloseHandle(handle);
    return name;
#else
    Q_UNUSED(pid)
    return QString();
#endif
}

bool terminateProcess(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return false;
    const BOOL ok = TerminateProcess(handle, 1);
    CloseHandle(handle);
    return ok != FALSE;
#else
    Q_UNUSED(pid)
    return false;
#endif
}

/// True when the pid still refers to a JLinkGDBServerCL process (guards against
/// pid reuse).
bool isGdbServerPid(qint64 pid)
{
    const QString image = processImageName(pid);
    return image.contains(QLatin1String("JLinkGDBServer"), Qt::CaseInsensitive);
}

} // namespace

void JLinkGdbProbe::reapStaleServers()
{
    if (!QFile::exists(pidFilePath()))
        return;

    QSettings settings(pidFilePath(), QSettings::IniFormat);
    const qint64 pid = settings.value(QStringLiteral("pid")).toLongLong();
    if (pid > 0 && isGdbServerPid(pid)) {
        const bool killed = terminateProcess(pid);
        Logger::instance()->warn(QStringLiteral("清理上次遗留的 J-Link GDB Server (PID %1)%2")
                                     .arg(pid)
                                     .arg(killed ? QString() : QStringLiteral(" 失败")));
    }
    clearServerPid();
}

quint16 JLinkGdbProbe::pickFreePort() const
{
    QTcpServer probe;
    if (probe.listen(QHostAddress::LocalHost, 0)) {
        const quint16 port = probe.serverPort();
        probe.close();
        return port;
    }
    return 0;
}

Status JLinkGdbProbe::startServer()
{
    stopServer();

    const QString server = defaultGdbServerPath(m_executable);
    if (server.isEmpty()) {
        m_lastError = QStringLiteral("未找到 JLinkGDBServerCL.exe（应与 JLink.exe 在同一目录）");
        return Status::failure(m_lastError);
    }

    // Emulator identity (firmware / hardware / serial) comes from a short Commander
    // session: JLinkGDBServerCL does not write its banner into a pipe, so parsing
    // its stdout is not reliable. The probe is free at this point.
    if (m_serial.isEmpty() && !m_executable.isEmpty()) {
        const auto info = JLinkRunner::query(m_executable,
                                             m_deviceName.isEmpty() ? QStringLiteral("Cortex-M4") : m_deviceName);
        if (info) {
            m_serial = info.value.serial;
            m_commanderVersion = info.value.commander;
            m_firmware = info.value.firmware;
            m_hardwareVersion = info.value.hardware;
            m_product = info.value.product;
        }
    }

    const quint16 port = pickFreePort();
    if (port == 0) {
        m_lastError = QStringLiteral("无法分配本地端口");
        return Status::failure(m_lastError);
    }
    m_port = port;

    QStringList args;
    args << QStringLiteral("-device") << (m_deviceName.isEmpty() ? QStringLiteral("Cortex-M4") : m_deviceName)
         << QStringLiteral("-if") << (m_interfaceName == QLatin1String("JTAG") ? QStringLiteral("JTAG")
                                                                              : QStringLiteral("SWD"))
         << QStringLiteral("-speed") << QString::number(m_speedKHz)
         << QStringLiteral("-port") << QString::number(port)
         << QStringLiteral("-singlerun")
         << QStringLiteral("-localhostonly")
         << QStringLiteral("-nologtofile");

    auto *server_process = new QProcess();
    server_process->setProgram(server);
    server_process->setArguments(args);
    server_process->setProcessChannelMode(QProcess::MergedChannels);
    server_process->setWorkingDirectory(QFileInfo(server).absolutePath());
    server_process->start();

    if (!server_process->waitForStarted(10000)) {
        m_lastError = QStringLiteral("无法启动 JLinkGDBServerCL.exe: %1").arg(server_process->errorString());
        delete server_process;
        return Status::failure(m_lastError);
    }
    m_server = server_process;
    recordServerPid(static_cast<qint64>(server_process->processId()));

    // Wait until the port accepts connections (or the process dies).
    QDeadlineTimer deadline(30000);
    Status connected = Status::failure(QStringLiteral("GDB Server 未就绪"));
    while (!deadline.hasExpired()) {
        if (m_server->state() != QProcess::Running) {
            m_lastError = QStringLiteral("JLinkGDBServerCL.exe 已退出:\n%1").arg(serverLog().trimmed());
            stopServer();
            return Status::failure(m_lastError);
        }
        connected = m_client.connectToHost(QStringLiteral("127.0.0.1"), port, 1200);
        if (connected)
            break;
        QThread::msleep(200);
    }

    if (!connected) {
        m_lastError = QStringLiteral("无法连接 GDB Server:\n%1").arg(serverLog().trimmed());
        stopServer();
        return Status::failure(m_lastError);
    }

    m_serverRunning = true;

    // Ask the server what it actually connected to.
    const QString stop = m_client.stopReason(8000);
    if (stop.isEmpty()) {
        m_lastError = QStringLiteral("目标无响应（GDB 停止应答为空）。检查供电与 SWD 接线。");
        stopServer();
        return Status::failure(m_lastError);
    }

    // Let the server flush its startup banner, then parse the buffered text once
    // (serverLog() accumulates, so repeated calls no longer lose data).
    QThread::msleep(200);
    const QString startup = serverLog();
    Logger::instance()->debug(QStringLiteral("GDB server log (%1 bytes):\n%2")
                                  .arg(startup.size())
                                  .arg(startup));

    m_version = QStringLiteral("J-Link GDB Server");
    if (!m_commanderVersion.isEmpty())
        m_version = QStringLiteral("J-Link %1 (GDB Server)").arg(m_commanderVersion);
    if (m_serial.isEmpty())
        m_serial = JLinkRunner::parseSerial(startup);
    m_detectedDevice = JLinkRunner::parseSelectedDevice(startup);
    if (m_detectedDevice.isEmpty())
        m_detectedDevice = m_deviceName; // silent mode does not echo the selection
    m_coreName = JLinkRunner::parseCoreName(startup);
    if (m_serial.isEmpty())
        m_serial = JLinkRunner::parseSerial(startup);
    m_halted = stop.startsWith(QLatin1Char('T')) || stop.startsWith(QLatin1Char('S'));

    Logger::instance()->info(QStringLiteral("J-Link GDB Server 就绪: port=%1, %2, device=%3")
                                 .arg(port)
                                 .arg(m_version, m_detectedDevice.isEmpty() ? m_deviceName : m_detectedDevice));
    return Status::success();
}

QString JLinkGdbProbe::serverLog()
{
    if (m_server)
        m_logBuffer += QString::fromLocal8Bit(m_server->readAll());
    return m_logBuffer;
}

void JLinkGdbProbe::stopServer()
{
    m_client.disconnect();
    if (m_server) {
        if (m_server->state() == QProcess::Running) {
            m_server->terminate();
            if (!m_server->waitForFinished(3000))
                m_server->kill();
        }
        delete m_server;
        m_server = nullptr;
        clearServerPid();
    }
    m_serverRunning = false;
    m_halted = false;
}

/// "open" only validates the backend (JLink.exe + GDB server present) and applies
/// the configuration. The GDB server itself is started by connectTarget(), so the
/// device name is known before the server is launched (avoids a restart).
Status JLinkGdbProbe::open()
{
    if (m_executable.isEmpty())
        m_executable = JLinkRunner::locate();
    if (m_executable.isEmpty()) {
        m_lastError = QStringLiteral("未找到 JLink.exe。请安装 SEGGER J-Link 软件，或在设置中指定路径。");
        return Status::failure(m_lastError);
    }
    if (defaultGdbServerPath(m_executable).isEmpty()) {
        m_lastError = QStringLiteral("未找到 JLinkGDBServerCL.exe（应与 JLink.exe 在同一目录）");
        return Status::failure(m_lastError);
    }

    m_serverRunning = m_serverRunning || m_client.isConnected();
    return Status::success();
}

void JLinkGdbProbe::close()
{
    stopServer();
}

Status JLinkGdbProbe::setInterface(const QString &interfaceName)
{
    const QString upper = interfaceName.trimmed().toUpper();
    const QString normalized = (upper == QLatin1String("JTAG")) ? QStringLiteral("JTAG") : QStringLiteral("SWD");
    if (normalized != m_interfaceName) {
        m_interfaceName = normalized;
        stopServer(); // server args carry the interface
    }
    return Status::success();
}

Status JLinkGdbProbe::setSpeed(int speedKHz)
{
    const int speed = speedKHz > 0 ? speedKHz : 4000;
    if (speed != m_speedKHz) {
        m_speedKHz = speed;
        stopServer();
    }
    return Status::success();
}

Status JLinkGdbProbe::setDevice(const QString &deviceName)
{
    const QString name = deviceName.trimmed();
    if (name.isEmpty())
        return Status::failure(QStringLiteral("器件名为空"));
    if (name != m_deviceName) {
        m_deviceName = name;
        m_detectedDevice.clear();
        stopServer();
    }
    return Status::success();
}

Status JLinkGdbProbe::connectTarget()
{
    if (m_serverRunning && m_client.isConnected())
        return Status::success();
    if (const Status st = open(); !st)
        return st;
    return startServer();
}

void JLinkGdbProbe::disconnectTarget()
{
    stopServer();
}

Status JLinkGdbProbe::halt()
{
    if (!m_client.isConnected())
        return Status::failure(QStringLiteral("目标未连接"));
    QString console;
    Status st = m_client.monitor(QStringLiteral("halt"), &console);
    if (!st) {
        // Fall back to the RSP interrupt byte.
        st = m_client.interrupt();
        if (st) {
            const QString stop = m_client.stopReason(5000);
            st = stop.isEmpty() ? Status::failure(QStringLiteral("暂停失败")) : Status::success();
        }
    }
    if (st)
        m_halted = true;
    return st;
}

Status JLinkGdbProbe::resume()
{
    if (!m_client.isConnected())
        return Status::failure(QStringLiteral("目标未连接"));
    QString console;
    const Status st = m_client.monitor(QStringLiteral("go"), &console);
    if (st)
        m_halted = false;
    return st;
}

Status JLinkGdbProbe::resetTarget()
{
    if (!m_client.isConnected())
        return Status::failure(QStringLiteral("目标未连接"));
    QString console;
    const Status st = m_client.monitor(QStringLiteral("reset"), &console);
    if (st) {
        const QString stop = m_client.stopReason(8000);
        m_halted = stop.startsWith(QLatin1Char('T')) || stop.startsWith(QLatin1Char('S'));
    }
    return st;
}

Status JLinkGdbProbe::readMemory(quint32 address, int length, QByteArray *out)
{
    if (!out)
        return Status::failure(QStringLiteral("内部错误: 空输出缓冲区"));
    const auto result = m_client.readMemory(address, length);
    if (!result) {
        m_lastError = result.error;
        return Status::failure(result.error);
    }
    *out = result.value;
    return Status::success();
}

Status JLinkGdbProbe::writeMemory(quint32 address, const QByteArray &data)
{
    const Status st = m_client.writeMemory(address, data);
    if (!st)
        m_lastError = st.error;
    return st;
}

Status JLinkGdbProbe::readU32(quint32 address, quint32 *out)
{
    QByteArray data;
    if (const Status st = readMemory(address, 4, &data); !st)
        return st;
    quint32 value = 0;
    std::memcpy(&value, data.constData(), 4);
    if (out)
        *out = value;
    return Status::success();
}

Status JLinkGdbProbe::writeU32(quint32 address, quint32 value)
{
    QByteArray data(4, '\0');
    std::memcpy(data.data(), &value, 4);
    return writeMemory(address, data);
}

Status JLinkGdbProbe::exec(const QString &command)
{
    QString console;
    return m_client.monitor(command, &console);
}

Status JLinkGdbProbe::validateChipName(const QString &chip, QString *message)
{
    const QString name = chip.trimmed();
    if (name.isEmpty())
        return Status::failure(QStringLiteral("器件名为空"));

    const QString executable = m_executable.isEmpty() ? JLinkRunner::locate() : m_executable;
    if (executable.isEmpty())
        return Status::failure(QStringLiteral("未找到 JLink.exe"));

    const auto result = JLinkRunner::runScript(executable, {QStringLiteral("si SWD"), QStringLiteral("connect")},
                                               60000,
                                               {QStringLiteral("-device"), name, QStringLiteral("-if"),
                                                QStringLiteral("SWD"), QStringLiteral("-speed"),
                                                QStringLiteral("4000"), QStringLiteral("-autoconnect"),
                                                QStringLiteral("1")});
    if (!result) {
        if (message)
            *message = result.error;
        return Status::failure(result.error);
    }
    if (message)
        *message = result.value;
    if (!result.value.contains(QLatin1String("Cortex-M")))
        return Status::failure(QStringLiteral("J-Link 无法识别器件 '%1'").arg(name));
    return Status::success();
}

ProbeInfo JLinkGdbProbe::info() const
{
    ProbeInfo info;
    info.backend = QStringLiteral("jlink");
    info.binaryPath = m_executable;
    info.version = m_version.isEmpty() ? QString() : m_version;
    info.serialNumber = m_serial;
    if (!m_serial.isEmpty())
        info.probeSerials << m_serial;
    info.interfaceName = m_interfaceName;
    info.speedKHz = m_speedKHz;
    info.open = m_serverRunning;
    info.connected = m_client.isConnected();
    info.chipName = m_deviceName;
    info.detectedChip = m_detectedDevice;
    info.halted = m_halted;
    return info;
}

} // namespace mrd
