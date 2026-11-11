#include "core/SampleCollector.h"

#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeController.h"

#include <QDateTime>

namespace mrd {

SampleCollector::SampleCollector(QObject *parent)
    : QObject(parent)
{
    m_history.setCapacity(8192);
}

void SampleCollector::setProbe(ProbeController *probe)
{
    if (m_probe == probe)
        return;
    if (m_probe)
        m_probe->disconnect(this);
    m_probe = probe;
    if (m_probe) {
        // Direct connections: every read completion must be handled on the UI
        // thread, and the chaining below re-issues the next read from there.
        connect(m_probe, &ProbeController::readFinished, this,
                &SampleCollector::onReadFinished);
        connect(m_probe, &ProbeController::writeFinished, this,
                &SampleCollector::onWriteFinished);
    }
}

void SampleCollector::setChannel(const RttControlBlock &block, int channelIndex)
{
    const RttBuffer *buffer = block.buffer(channelIndex);
    m_haveChannel = (buffer != nullptr);
    if (buffer) {
        m_buffer = *buffer;
        m_readOffsetKnown = false;
        m_readOffset = 0;
    }
    Logger::instance()->debug(
        QStringLiteral("采样通道: %1（缓冲 %2 字节 @ %3）")
            .arg(m_haveChannel ? m_buffer.name : QStringLiteral("无"))
            .arg(m_buffer.size)
            .arg(hexAddr(m_buffer.bufferAddress)));
}

int SampleCollector::payloadSize() const
{
    int bytes = 0;
    for (const WatchVariable &v : m_variables)
        bytes += v.span();
    return bytes;
}

QStringList SampleCollector::traceLabels() const
{
    QStringList labels;
    for (const WatchVariable &v : m_variables) {
        if (v.elementCount <= 1) {
            labels << v.name;
            continue;
        }
        for (int i = 0; i < v.elementCount; ++i)
            labels << QStringLiteral("%1[%2]").arg(v.name).arg(i);
    }
    return labels;
}

void SampleCollector::setVariables(const QVector<WatchVariable> &variables)
{
    const bool wasRunning = m_running;
    if (wasRunning)
        stop();

    m_variables = variables;
    m_parser.setPayloadSize(payloadSize());
    m_history.setTraceNames(traceLabels());
    m_frames = 0;
    m_dropped = 0;

    Logger::instance()->debug(QStringLiteral("采样布局: %1 个变量，%2 条曲线，帧长 %3 字节")
                                  .arg(m_variables.size())
                                  .arg(traceLabels().size())
                                  .arg(frameSize()));
    if (wasRunning && !m_variables.isEmpty())
        start();
}

void SampleCollector::clearHistory()
{
    m_history.clear();
}

void SampleCollector::start()
{
    if (m_running)
        return;
    if (!m_probe) {
        fail(QStringLiteral("探针未就绪"));
        return;
    }
    if (!m_haveChannel) {
        fail(QStringLiteral("没有可用的 RTT 上行通道"));
        return;
    }
    if (m_variables.isEmpty() || payloadSize() <= 0) {
        fail(QStringLiteral("没有选择要绘制的变量"));
        return;
    }

    m_running = true;
    m_parser.reset();
    m_stage = Stage::Idle;
    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    m_lastStatsMs = m_startedMs;
    m_lastStatsFrames = 0;
    emit runningChanged(true);
    pump();
}

void SampleCollector::stop()
{
    if (!m_running)
        return;
    m_running = false;
    m_stage = Stage::Idle;
    emit runningChanged(false);
}

void SampleCollector::fail(const QString &message)
{
    if (m_running) {
        m_running = false;
        m_stage = Stage::Idle;
        emit runningChanged(false);
    }
    emit failed(message);
}

void SampleCollector::pump()
{
    if (!m_running)
        return;
    switch (m_stage) {
    case Stage::Idle:
        issueWriteOffsetRead();
        break;
    case Stage::WaitWriteOffset:
    case Stage::WaitPayload:
    case Stage::WaitReadOffsetAck:
        break; // a request is already in flight
    }
}

void SampleCollector::issueWriteOffsetRead()
{
    m_stage = Stage::WaitWriteOffset;
    m_pendingRequest = m_probe->nextRequestId();
    m_probe->requestRead(m_pendingRequest, m_buffer.writeOffsetAddress, 4);
}

void SampleCollector::issuePayloadRead(quint32 writeOffset)
{
    if (!m_readOffsetKnown) {
        // First sample after start or after a target reset: jump to the writer's
        // position instead of replaying the whole ring, which would show a burst of
        // stale data as if it had just happened.
        m_readOffset = writeOffset < m_buffer.size ? writeOffset : 0;
        m_readOffsetKnown = true;
    }

    if (writeOffset == m_readOffset) {
        // Nothing new. Re-arm immediately: this is still not a timer-driven poll --
        // the loop runs as fast as the link allows and backs off not at all, so the
        // next write from the target is picked up as soon as it happens.
        m_stage = Stage::Idle;
        pump();
        return;
    }

    const quint32 pending = writeOffset > m_readOffset
        ? writeOffset - m_readOffset
        : m_buffer.size - m_readOffset;
    if (pending == 0 || pending > m_buffer.size) {
        m_readOffset = 0;
        m_readOffsetKnown = false;
        m_stage = Stage::Idle;
        pump();
        return;
    }

    m_pendingWriteOffset = writeOffset;
    m_stage = Stage::WaitPayload;
    m_pendingRequest = m_probe->nextRequestId();
    m_probe->requestRead(m_pendingRequest, m_buffer.bufferAddress + m_readOffset,
                         static_cast<int>(pending));
}

void SampleCollector::onReadFinished(quint64 requestId, quint32 address, const QByteArray &data,
                                     bool ok, const QString &error)
{
    if (!m_running || requestId != m_pendingRequest)
        return;
    Q_UNUSED(address);

    if (!ok) {
        fail(QStringLiteral("读取 RTT 失败: %1").arg(error));
        return;
    }

    if (m_stage == Stage::WaitWriteOffset) {
        if (data.size() < 4) {
            fail(QStringLiteral("RTT 写指针读取不完整"));
            return;
        }
        const uchar *b = reinterpret_cast<const uchar *>(data.constData());
        const quint32 writeOffset = static_cast<quint32>(b[0])
            | (static_cast<quint32>(b[1]) << 8) | (static_cast<quint32>(b[2]) << 16)
            | (static_cast<quint32>(b[3]) << 24);
        issuePayloadRead(writeOffset);
        return;
    }

    if (m_stage == Stage::WaitPayload) {
        QVector<QByteArray> payloads;
        m_parser.feed(data, &payloads);
        m_dropped = static_cast<quint64>(m_parser.resyncBytes());

        if (!payloads.isEmpty()) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (const QByteArray &payload : payloads) {
                QVector<double> values;
                values.reserve(payload.size() / 4 + 1);
                int offset = 0;
                for (const WatchVariable &v : m_variables) {
                    if (offset + v.span() > payload.size())
                        break;
                    const QByteArray slice = payload.mid(offset, v.span());
                    const QVector<double> decoded =
                        SymbolFile::decode(v, slice, v.elementCount);
                    for (double value : decoded)
                        values.append(value);
                    offset += v.span();
                }
                m_history.append(now, values);
                ++m_frames;
            }
            emit samplesAppended(payloads.size());

            const qint64 elapsed = now - m_lastStatsMs;
            if (elapsed >= 1000) {
                const double fps =
                    (m_frames - m_lastStatsFrames) * 1000.0 / static_cast<double>(elapsed);
                emit statistics(m_frames, m_dropped, fps);
                m_lastStatsMs = now;
                m_lastStatsFrames = m_frames;
            }
        }

        // Advance RdOff so the target keeps writing, then immediately look for more.
        m_readOffset = m_pendingWriteOffset;
        m_stage = Stage::WaitReadOffsetAck;
        m_pendingRequest = m_probe->nextRequestId();
        QByteArray encoded(4, Qt::Uninitialized);
        encoded[0] = static_cast<char>(m_readOffset & 0xffu);
        encoded[1] = static_cast<char>((m_readOffset >> 8) & 0xffu);
        encoded[2] = static_cast<char>((m_readOffset >> 16) & 0xffu);
        encoded[3] = static_cast<char>((m_readOffset >> 24) & 0xffu);
        m_probe->requestWrite(m_pendingRequest, m_buffer.readOffsetAddress, encoded);
        return;
    }

    if (m_stage == Stage::WaitReadOffsetAck) {
        // Whether the ack succeeded or not, keep going: a failed RdOff write only
        // means the ring will fill and the target will start dropping, which shows
        // up in the drop counter rather than silently stopping the plot.
        m_stage = Stage::Idle;
        pump();
    }
}

void SampleCollector::onWriteFinished(quint64 requestId, quint32 address, bool ok,
                                      const QString &error)
{
    if (!m_running || requestId != m_pendingRequest)
        return;
    Q_UNUSED(address);
    if (!ok) {
        Logger::instance()->debug(
            QStringLiteral("RTT 读指针回写失败（缓冲将逐渐填满）: %1").arg(error));
    }
}

} // namespace mrd
