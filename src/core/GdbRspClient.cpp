#include "core/GdbRspClient.h"

#include "core/Logger.h"

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QThread>

namespace mrd {

namespace {

QByteArray escapeBinary(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size());
    for (const char c : data) {
        const char ch = c;
        if (ch == '$' || ch == '#' || ch == '}' || ch == '*') {
            out.append('}');
            out.append(char(static_cast<unsigned char>(ch) ^ 0x20));
        } else {
            out.append(ch);
        }
    }
    return out;
}

QByteArray unescapeBinary(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size());
    for (int i = 0; i < data.size(); ++i) {
        if (data.at(i) == '}' && i + 1 < data.size()) {
            out.append(char(static_cast<unsigned char>(data.at(++i)) ^ 0x20));
        } else {
            out.append(data.at(i));
        }
    }
    return out;
}

/// GDB replies are lowercase hex; convert to bytes, return empty on bad input.
QByteArray hexToBytes(const QByteArray &hex)
{
    if (hex.isEmpty())
        return QByteArray();
    QByteArray out;
    out.reserve(hex.size() / 2);
    for (int i = 0; i + 1 < hex.size(); i += 2) {
        bool ok = false;
        const int value = hex.mid(i, 2).toInt(&ok, 16);
        if (!ok)
            return QByteArray();
        out.append(char(value));
    }
    return out;
}

} // namespace

GdbRspClient::~GdbRspClient()
{
    disconnect();
}

bool GdbRspClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

Status GdbRspClient::connectToHost(const QString &host, quint16 port, int timeoutMs)
{
    disconnect();
    m_socket = new QTcpSocket();

    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        m_socket->connectToHost(host, port);
        if (m_socket->waitForConnected(1500))
            break;
        // The server may still be enumerating USB; retry until the deadline.
        m_socket->abort();
        QThread::msleep(150);
    }

    if (!isConnected()) {
        m_lastError = QStringLiteral("无法连接 GDB Server %1:%2 (%3)")
                          .arg(host)
                          .arg(port)
                          .arg(m_socket->errorString());
        disconnect();
        return Status::failure(m_lastError);
    }

    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_rxBuffer.clear();
    Logger::instance()->debug(QStringLiteral("GDB Server 已连接 %1:%2").arg(host).arg(port));
    return Status::success();
}

void GdbRspClient::disconnect()
{
    if (!m_socket)
        return;
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected(500);
    }
    delete m_socket;
    m_socket = nullptr;
    m_rxBuffer.clear();
}

bool GdbRspClient::waitForAck(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (!m_rxBuffer.isEmpty()) {
            const char c = m_rxBuffer.at(0);
            if (c == '+') {
                m_rxBuffer.remove(0, 1);
                return true;
            }
            if (c == '-') {
                m_rxBuffer.remove(0, 1);
                return false;
            }
            if (c == '$') {
                // A packet arrived before the ack; leave it for the next read.
                return true;
            }
            m_rxBuffer.remove(0, 1);
            continue;
        }
        if (!m_socket->waitForReadyRead(int(qMin<qint64>(deadline.remainingTime(), 500))))
            continue;
        m_rxBuffer += m_socket->readAll();
    }
    m_lastError = QStringLiteral("等待 ACK 超时");
    return false;
}

QString GdbRspClient::sendPacket(const QByteArray &payload, int timeoutMs)
{
    if (!isConnected()) {
        m_lastError = QStringLiteral("GDB Server 未连接");
        return QString();
    }

    const QByteArray escaped = escapeBinary(payload);
    unsigned char checksum = 0;
    for (const char c : escaped)
        checksum = static_cast<unsigned char>(checksum + static_cast<unsigned char>(c));

    QByteArray packet;
    packet.append('$');
    packet.append(escaped);
    packet.append('#');
    packet.append(QStringLiteral("%1").arg(checksum, 2, 16, QLatin1Char('0')).toLatin1());

    m_socket->write(packet);
    if (!m_socket->waitForBytesWritten(3000)) {
        m_lastError = QStringLiteral("发送 GDB 包失败: %1").arg(m_socket->errorString());
        return QString();
    }

    if (!waitForAck(timeoutMs))
        return QString(); // some servers answer without '+' - the packet read will tell

    // Read the reply packet.
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        // Look for a complete $...#xx frame in the buffer.
        const int start = m_rxBuffer.indexOf('$');
        if (start >= 0) {
            const int hash = m_rxBuffer.indexOf('#', start);
            if (hash >= 0 && m_rxBuffer.size() >= hash + 3) {
                const QByteArray body = m_rxBuffer.mid(start + 1, hash - start - 1);
                m_rxBuffer.remove(0, hash + 3);
                m_socket->write("+");
                m_socket->flush();
                if (!m_rxBuffer.isEmpty() && m_rxBuffer.at(0) == '-') {
                    m_lastError = QStringLiteral("GDB Server 拒绝了请求");
                    m_rxBuffer.remove(0, 1);
                    return QString();
                }
                return QString::fromLatin1(unescapeBinary(body));
            }
        }
        if (!m_socket->waitForReadyRead(int(qMin<qint64>(deadline.remainingTime(), 500))))
            continue;
        m_rxBuffer += m_socket->readAll();
    }

    m_lastError = QStringLiteral("GDB 响应超时");
    return QString();
}

Result<QByteArray> GdbRspClient::readMemory(quint32 address, int length, int timeoutMs)
{
    if (length <= 0)
        return Result<QByteArray>::failure(QStringLiteral("读取长度非法"));

    // The server limits packet size; chunk reads to stay well below it.
    constexpr int kChunk = 1024;
    QByteArray out;
    out.reserve(length);

    for (int offset = 0; offset < length; offset += kChunk) {
        const int chunk = qMin(kChunk, length - offset);
        const QString request = QStringLiteral("m%1,%2")
                                    .arg(address + quint32(offset), 0, 16)
                                    .arg(chunk, 0, 16);
        const QString reply = sendPacket(request.toLatin1(), timeoutMs);
        if (reply.isEmpty()) {
            return Result<QByteArray>::failure(
                m_lastError.isEmpty() ? QStringLiteral("读取 0x%1 无响应").arg(QString::number(address, 16))
                                      : m_lastError);
        }
        if (reply.startsWith(QLatin1Char('E'))) {
            return Result<QByteArray>::failure(
                QStringLiteral("读取 0x%1 失败 (GDB 错误 %2)")
                    .arg(QString::number(address + quint32(offset), 16), reply));
        }
        const QByteArray bytes = hexToBytes(reply.toLatin1());
        if (bytes.size() != chunk) {
            return Result<QByteArray>::failure(
                QStringLiteral("读取 0x%1 返回 %2/%3 字节")
                    .arg(QString::number(address + quint32(offset), 16))
                    .arg(bytes.size())
                    .arg(chunk));
        }
        out += bytes;
    }
    return Result<QByteArray>::success(out);
}

Status GdbRspClient::writeMemory(quint32 address, const QByteArray &data, int timeoutMs)
{
    if (data.isEmpty())
        return Status::failure(QStringLiteral("写入数据为空"));

    constexpr int kChunk = 512;
    for (int offset = 0; offset < data.size(); offset += kChunk) {
        const QByteArray chunk = data.mid(offset, kChunk);
        const QString request = QStringLiteral("M%1,%2:%3")
                                    .arg(address + quint32(offset), 0, 16)
                                    .arg(chunk.size(), 0, 16)
                                    .arg(QString::fromLatin1(chunk.toHex()));
        const QString reply = sendPacket(request.toLatin1(), timeoutMs);
        if (reply.isEmpty())
            return Status::failure(m_lastError.isEmpty() ? QStringLiteral("写入无响应") : m_lastError);
        if (reply != QLatin1String("OK")) {
            return Status::failure(QStringLiteral("写入 0x%1 失败 (GDB: %2)")
                                       .arg(QString::number(address + quint32(offset), 16), reply));
        }
    }
    return Status::success();
}

QString GdbRspClient::stopReason(int timeoutMs)
{
    const QString reply = sendPacket("?", timeoutMs);
    if (reply.isEmpty())
        return QString();
    return reply;
}

Status GdbRspClient::monitor(const QString &command, QString *console, int timeoutMs)
{
    const QByteArray payload = "qRcmd," + command.toLatin1().toHex();
    const QString reply = sendPacket(payload, timeoutMs);
    if (reply.isEmpty())
        return Status::failure(m_lastError.isEmpty() ? QStringLiteral("监控命令无响应") : m_lastError);

    if (reply == QLatin1String("OK")) {
        if (console)
            console->clear();
        return Status::success();
    }
    if (reply.startsWith(QLatin1Char('E')))
        return Status::failure(QStringLiteral("J-Link 拒绝命令 '%1' (%2)").arg(command, reply));

    // J-Link answers monitor commands with hex-encoded console text.
    if (console) {
        const QByteArray text = hexToBytes(reply.toLatin1());
        *console = QString::fromLocal8Bit(text).trimmed();
    }
    return Status::success();
}

Status GdbRspClient::interrupt()
{
    if (!isConnected())
        return Status::failure(QStringLiteral("GDB Server 未连接"));
    m_socket->write(QByteArray(1, char(0x03)));
    m_socket->flush();
    return Status::success();
}

} // namespace mrd
