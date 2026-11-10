#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>

class QTcpSocket;

namespace mrd {

/// Minimal GDB Remote Serial Protocol client (the subset a register debugger
/// needs: memory read/write, halt/run/reset via monitor commands, stop reason).
///
/// Packet framing: `$<payload>#<checksum>` with '+/-' acknowledgements; binary
/// data is escaped as '}' + (byte ^ 0x20).
class GdbRspClient
{
public:
    GdbRspClient() = default;
    ~GdbRspClient();

    Status connectToHost(const QString &host, quint16 port, int timeoutMs = 8000);
    void disconnect();
    bool isConnected() const;

    /// Read \a length bytes from \a address (target byte order).
    Result<QByteArray> readMemory(quint32 address, int length, int timeoutMs = 15000);
    Status writeMemory(quint32 address, const QByteArray &data, int timeoutMs = 15000);

    /// Stop reason of the last `?` (`S05`/`T05...`); empty on failure.
    QString stopReason(int timeoutMs = 8000);

    /// Send a monitor command (J-Link: "halt", "go", "reset", "semihosting enable").
    Status monitor(const QString &command, QString *console = nullptr, int timeoutMs = 15000);

    /// Asynchronous interrupt (0x03) - halts a running target.
    Status interrupt();

    /// Last transport/protocol error, for UI messages.
    QString lastError() const { return m_lastError; }

private:
    QString sendPacket(const QByteArray &payload, int timeoutMs);
    bool waitForAck(int timeoutMs);

    QTcpSocket *m_socket = nullptr;
    QString m_lastError;
    QByteArray m_rxBuffer;
};

} // namespace mrd
