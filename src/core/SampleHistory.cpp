#include "core/SampleHistory.h"

#include <QtGlobal>

#include <algorithm>

namespace mrd {

// --- SampleHistory -----------------------------------------------------------

SampleHistory::SampleHistory(int capacity)
    : m_capacity(qMax(16, capacity))
{
    m_times.resize(m_capacity);
}

void SampleHistory::setCapacity(int capacity)
{
    m_capacity = qMax(16, capacity);
    clear();
    m_times.resize(m_capacity);
}

void SampleHistory::setTraceNames(const QStringList &names)
{
    if (m_names == names)
        return;
    m_names = names;
    m_series.clear();
    m_series.resize(m_names.size());
    for (QVector<double> &series : m_series)
        series.resize(m_capacity);
    clear();
}

void SampleHistory::clear()
{
    m_head = 0;
    m_count = 0;
    for (QVector<double> &series : m_series)
        std::fill(series.begin(), series.end(), 0.0);
}

int SampleHistory::slot(int index) const
{
    if (m_count <= 0)
        return 0;
    return (m_head + index) % m_capacity;
}

void SampleHistory::append(qint64 timeMs, const QVector<double> &values)
{
    if (m_names.isEmpty())
        return;
    if (m_series.size() != m_names.size()) {
        m_series.resize(m_names.size());
        for (QVector<double> &series : m_series) {
            if (series.size() != m_capacity)
                series.assign(m_capacity, 0.0);
        }
    }
    const int writeIndex = m_count < m_capacity ? slot(m_count) : m_head;
    if (m_count >= m_capacity)
        m_head = (m_head + 1) % m_capacity; // overwrite the oldest
    else
        ++m_count;

    m_times[writeIndex] = timeMs;
    for (int t = 0; t < m_series.size(); ++t) {
        if (m_series[t].size() != m_capacity)
            m_series[t].assign(m_capacity, 0.0);
        m_series[t][writeIndex] = t < values.size() ? values.at(t) : 0.0;
    }
}

qint64 SampleHistory::timeAt(int index) const
{
    if (index < 0 || index >= m_count)
        return 0;
    return m_times.at(slot(index));
}

double SampleHistory::valueAt(int trace, int index) const
{
    if (trace < 0 || trace >= m_series.size() || index < 0 || index >= m_count)
        return 0.0;
    return m_series.at(trace).at(slot(index));
}

qint64 SampleHistory::firstTime() const
{
    return m_count > 0 ? m_times.at(slot(0)) : 0;
}

qint64 SampleHistory::lastTime() const
{
    return m_count > 0 ? m_times.at(slot(m_count - 1)) : 0;
}

double SampleHistory::minValue(int trace) const
{
    if (trace < 0 || trace >= m_series.size() || m_count == 0)
        return 0.0;
    double lowest = m_series.at(trace).at(slot(0));
    for (int i = 1; i < m_count; ++i)
        lowest = qMin(lowest, m_series.at(trace).at(slot(i)));
    return lowest;
}

double SampleHistory::maxValue(int trace) const
{
    if (trace < 0 || trace >= m_series.size() || m_count == 0)
        return 0.0;
    double highest = m_series.at(trace).at(slot(0));
    for (int i = 1; i < m_count; ++i)
        highest = qMax(highest, m_series.at(trace).at(slot(i)));
    return highest;
}

double SampleHistory::minAll() const
{
    double lowest = 0.0;
    bool first = true;
    for (int t = 0; t < m_series.size(); ++t) {
        for (int i = 0; i < m_count; ++i) {
            const double v = m_series.at(t).at(slot(i));
            if (first || v < lowest) {
                lowest = v;
                first = false;
            }
        }
    }
    return lowest;
}

double SampleHistory::maxAll() const
{
    double highest = 0.0;
    bool first = true;
    for (int t = 0; t < m_series.size(); ++t) {
        for (int i = 0; i < m_count; ++i) {
            const double v = m_series.at(t).at(slot(i));
            if (first || v > highest) {
                highest = v;
                first = false;
            }
        }
    }
    return highest;
}

// --- FrameParser -------------------------------------------------------------

QByteArray FrameParser::syncWord()
{
    return QByteArray::fromHex("A55A");
}

void FrameParser::setPayloadSize(int payloadBytes)
{
    m_payloadBytes = qMax(0, payloadBytes);
    reset();
}

void FrameParser::reset()
{
    m_buffer.clear();
    m_resyncBytes = 0;
}

int FrameParser::feed(const QByteArray &bytes, QVector<QByteArray> *payloads)
{
    const int frameSize = this->frameSize();
    if (frameSize <= 0 || bytes.isEmpty())
        return 0;

    m_buffer.append(bytes);
    const QByteArray sync = syncWord();
    int produced = 0;

    while (m_buffer.size() >= frameSize) {
        // Align on a sync word. If the leading bytes are not a sync, drop them --
        // this is the only recovery path, and it also handles the case where the
        // target restarted mid-frame.
        if (!m_buffer.startsWith(sync)) {
            const int found = m_buffer.indexOf(sync, 1);
            if (found < 0) {
                // Keep the last sync-size-1 bytes in case a sync straddles the
                // boundary, discard the rest.
                const int keep = qMin(m_buffer.size(), kSyncSize - 1);
                m_resyncBytes += m_buffer.size() - keep;
                m_buffer = m_buffer.right(keep);
                break;
            }
            m_resyncBytes += found;
            m_buffer.remove(0, found);
            continue;
        }
        if (payloads)
            payloads->append(m_buffer.mid(kSyncSize, m_payloadBytes));
        m_buffer.remove(0, frameSize);
        ++produced;
    }
    return produced;
}

} // namespace mrd
