#pragma once

#include "core/RttClient.h"
#include "core/SampleHistory.h"
#include "core/SymbolFile.h"

#include <QObject>
#include <QVector>

namespace mrd {

class ProbeController;

/// Reads RTT data from the target and turns it into plot samples.
///
/// The loop is deliberately **not** a timer: when a read completes, the next one
/// is issued immediately. That keeps acquisition at whatever rate the link can
/// sustain and makes it completely independent of how fast the UI repaints -- a
/// stalled or occluded window cannot lose samples, and a burst of samples cannot
/// block the event loop because each turn handles one read at a time.
///
/// Protocol (fixed frame, so no length field and nothing to mis-parse):
///     [0xA5][0x5A] <payload: one raw copy of each selected variable, in order>
/// Matching firmware code is produced by `FirmwareSnippet`, so the two ends cannot
/// disagree about the layout without the user seeing the generated struct.
class SampleCollector : public QObject
{
    Q_OBJECT

public:
    explicit SampleCollector(QObject *parent = nullptr);

    void setProbe(ProbeController *probe);

    /// RTT control block and which up-channel carries the frames.
    void setChannel(const RttControlBlock &block, int channelIndex);

    /// The variables whose values travel in every frame, in frame order.
    /// An array contributes all its elements, contiguous.
    void setVariables(const QVector<WatchVariable> &variables);
    QVector<WatchVariable> variables() const { return m_variables; }

    /// Trace labels, one per plotted curve (arrays fan out per element).
    QStringList traceLabels() const;

    int payloadSize() const;
    int frameSize() const { return payloadSize() > 0 ? payloadSize() + FrameParser::kSyncSize : 0; }

    bool isRunning() const { return m_running; }
    void start();
    void stop();

    /// Samples appended since the last call are in \a history; the caller owns it.
    SampleHistory *history() { return &m_history; }
    void clearHistory();

signals:
    /// A new batch of frames arrived. The widget only needs to repaint; the data is
    /// already in history(), so no work is coupled to this signal.
    void samplesAppended(int count);
    void failed(const QString &message);
    void runningChanged(bool running);
    /// Frame counter and drop counter, for the status line.
    void statistics(quint64 frames, quint64 droppedBytes, double framesPerSecond);

private slots:
    void onReadFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok,
                        const QString &error);
    void onWriteFinished(quint64 requestId, quint32 address, bool ok, const QString &error);

private:
    enum class Stage
    {
        Idle,
        WaitWriteOffset,
        WaitPayload,
        WaitReadOffsetAck,
    };

    void pump();
    void issueWriteOffsetRead();
    void issuePayloadRead(quint32 writeOffset);
    void fail(const QString &message);

    ProbeController *m_probe = nullptr;
    RttBuffer m_buffer;
    bool m_haveChannel = false;
    QVector<WatchVariable> m_variables;
    SampleHistory m_history{8192};
    FrameParser m_parser;

    bool m_running = false;
    Stage m_stage = Stage::Idle;
    quint32 m_readOffset = 0;
    bool m_readOffsetKnown = false;
    quint64 m_pendingRequest = 0;
    quint32 m_pendingWriteOffset = 0;

    quint64 m_frames = 0;
    quint64 m_dropped = 0;
    qint64 m_startedMs = 0;
    qint64 m_lastStatsMs = 0;
    quint64 m_lastStatsFrames = 0;
};

} // namespace mrd
