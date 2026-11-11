#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

/// Ring-buffered sample history for the plot.
///
/// Storage is one flat array per trace and time is kept alongside, so drawing is a
/// pure read: the collector appends, the widget reads, and neither waits for the
/// other. That separation is the point -- rendering never drives sampling.
class SampleHistory
{
public:
    explicit SampleHistory(int capacity = 8192);

    void setCapacity(int capacity);
    int capacity() const { return m_capacity; }

    /// Names are the curve labels; setting them resets the history because a
    /// different trace set means the old samples no longer line up.
    void setTraceNames(const QStringList &names);
    QStringList traceNames() const { return m_names; }
    int traceCount() const { return m_names.size(); }

    void clear();

    /// Append one sample. \a values is truncated/padded to the trace count.
    void append(qint64 timeMs, const QVector<double> &values);

    int count() const { return m_count; }
    bool isEmpty() const { return m_count == 0; }
    int headIndex() const { return m_head; } ///< index of the oldest sample

    qint64 timeAt(int index) const;
    double valueAt(int trace, int index) const;

    qint64 firstTime() const;
    qint64 lastTime() const;

    /// Min/max over the visible window for auto-scaling. \a ignoreZeros keeps a
    /// mostly-zero trace from flattening the others.
    double minValue(int trace) const;
    double maxValue(int trace) const;

    /// Global limits across all traces (what the Y axis actually uses).
    double minAll() const;
    double maxAll() const;

private:
    int slot(int index) const;

    int m_capacity = 8192;
    QStringList m_names;
    QVector<QVector<double>> m_series;
    QVector<qint64> m_times;
    int m_head = 0;
    int m_count = 0;
};

/// Reassembles fixed-size frames out of a byte stream.
///
/// RTT hands over an unstructured byte stream: a read can end mid-frame, and a
/// target reset can leave a partial frame behind. Because the frame length is
/// derived from the symbol table (each selected variable's width times its element
/// count), the parser only has to find the sync word and then step in fixed
/// strides -- no length field, no escaping, nothing the firmware can get wrong.
class FrameParser
{
public:
    /// Frame = 2 sync bytes + \a payloadBytes of variable data.
    void setPayloadSize(int payloadBytes);
    int payloadSize() const { return m_payloadBytes; }
    int frameSize() const { return m_payloadBytes > 0 ? m_payloadBytes + kSyncSize : 0; }

    void reset();

    /// Feed newly received bytes; complete payloads are appended to \a payloads
    /// (sync word stripped). Returns the number of frames produced.
    int feed(const QByteArray &bytes, QVector<QByteArray> *payloads);

    static constexpr int kSyncSize = 2;
    static QByteArray syncWord(); ///< \x A5 \x5A

    /// Diagnostics: bytes discarded while hunting for a sync word.
    int resyncBytes() const { return m_resyncBytes; }

private:
    int m_payloadBytes = 0;
    QByteArray m_buffer;
    int m_resyncBytes = 0;
};

/// Split a selection of variables into individual plotted traces.
///
/// An array variable becomes one trace per element (`g_adc_raw[2]`), because that
/// is what a user wants to see; a scalar stays one trace.
struct PlotTrace
{
    int variableIndex = 0; ///< index into the selection
    QString label;
    int elementIndex = 0;
    int offsetInFrame = 0; ///< byte offset of this element inside the payload
    int elementSize = 0;
    int kind = 0; ///< ScalarKind, kept as int so this header stays dependency-free
};

} // namespace mrd
