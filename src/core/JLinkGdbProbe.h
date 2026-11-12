#pragma once

#include "core/GdbRspClient.h"
#include "core/IProbe.h"

#include <QString>

class QProcess;

namespace mrd {

/// IProbe implementation on top of `JLinkGDBServerCL.exe` + GDB remote protocol.
///
/// Why this and not the J-Link DLL or Commander:
///   * the official SEGGER USB driver keeps working (probe-rs would need WinUSB);
///   * J-Link Commander buffers stdout when piped, so it cannot be driven
///     interactively;
///   * a GDB server session is persistent and unbuffered, so register polling
///     costs milliseconds instead of seconds.
///
/// The server process is a child of the app; `open()` starts it on a free
/// localhost port, `close()` terminates it.
class JLinkGdbProbe : public IProbe
{
public:
    JLinkGdbProbe();
    ~JLinkGdbProbe() override;

    Status open() override;
    void close() override;
    bool isOpen() const override { return m_serverRunning; }

    Status setInterface(const QString &interfaceName) override;
    Status setSpeed(int speedKHz) override;
    Status setDevice(const QString &deviceName) override;
    QString deviceName() const override { return m_deviceName; }

    Status connectTarget() override;
    void disconnectTarget() override;
    bool isConnected() const override { return m_client.isConnected(); }

    Status halt() override;
    Status resume() override;
    Status resetTarget() override;
    bool isHalted() const override { return m_halted; }

    Status readMemory(quint32 address, int length, QByteArray *out) override;
    Status writeMemory(quint32 address, const QByteArray &data) override;
    Status readU32(quint32 address, quint32 *out) override;
    Status writeU32(quint32 address, quint32 value) override;

    Status readCoreRegisters(QVector<quint32> *out) override;
    Status exec(const QString &command) override;

    ProbeInfo info() const override;
    QString lastError() const { return m_lastError; }

    /// Device name the server reports (J-Link resolves families to part numbers).
    QString detectedDevice() const { return m_detectedDevice; }
    QString coreName() const { return m_coreName; }

    /// Verify a chip name by asking the GDB server to start with it.
    Status validateChipName(const QString &chip, QString *message = nullptr) override;

    void setExecutable(const QString &path) { m_executable = path; }
    QString executable() const { return m_executable; }

    /// Path of the GDB server executable next to JLink.exe.
    static QString defaultGdbServerPath(const QString &jlinkExecutable);

    /// Kill a JLinkGDBServerCL orphan left behind by a previous (crashed) run.
    /// Only processes recorded in our pid file are touched, and the image name is
    /// verified first, so unrelated J-Link sessions are never affected.
    static void reapStaleServers();

private:
    Status startServer();
    void stopServer();
    quint16 pickFreePort() const;
    QString serverLog();          ///< non-destructive: accumulates the server output
    QString m_logBuffer;

    QString m_executable;
    QProcess *m_server = nullptr;
    GdbRspClient m_client;
    QString m_lastError;
    QString m_deviceName;
    QString m_detectedDevice;
    QString m_coreName;
    QString m_interfaceName = QStringLiteral("SWD");
    int m_speedKHz = 4000;
    quint16 m_port = 0;
    bool m_serverRunning = false;
    bool m_halted = false;
    QString m_version;
    QString m_serial;
    QString m_commanderVersion;
    QString m_firmware;
    QString m_hardwareVersion;
    QString m_product;
};

} // namespace mrd
