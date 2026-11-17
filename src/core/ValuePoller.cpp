#include "core/ValuePoller.h"

#include "core/ProbeController.h"

#include <QDateTime>

#include <algorithm>

namespace mrd {
namespace {

/// Bytes to read for one target, capped so a huge array cannot stall a sweep.
int cappedSpan(const WatchVariable &variable)
{
    const int span = variable.span();
    return qBound(0, span, ValuePoller::kMaxTargetBytes);
}

} // namespace

ValuePoller::ValuePoller(QObject *parent)
    : QObject(parent)
{
    // The tick is only a scheduler: it decides whether to run the visible pass or a
    // background chunk. Work itself is driven by read completions.
    m_timer.setInterval(ValuePoller::kVisiblePeriodMs);
    connect(&m_timer, &QTimer::timeout, this, &ValuePoller::tick);
}

void ValuePoller::setProbe(ProbeController *probe)
{
    m_probe = probe;
    if (m_probe) {
        connect(m_probe, &ProbeController::readFinished, this,
                &ValuePoller::onReadFinished, Qt::UniqueConnection);
    }
}

void ValuePoller::setVisibleTargets(const QVector<WatchVariable> &targets)
{
    m_visible = targets;
    if (m_suspended || targets.isEmpty())
        return;
    if (!m_timer.isActive())
        m_timer.start(); // interval stays kVisiblePeriodMs
    if (!m_busy)
        QTimer::singleShot(0, this, &ValuePoller::tick); // first number without the wait
}

void ValuePoller::setBackgroundTargets(const QVector<WatchVariable> &targets)
{
    m_background = targets;
    m_backgroundCursor = 0;
}

void ValuePoller::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;
    if (!suspended) {
        if (!m_timer.isActive())
            m_timer.start();
        QTimer::singleShot(0, this, &ValuePoller::tick); // refresh at once
        return;
    }
    m_timer.stop();
    // Abandon the in-flight sweep. Its read still completes; onReadFinished drops the
    // result because m_busy was cleared.
    m_busy = false;
    m_pendingRequest = 0;
}

void ValuePoller::reset()
{
    m_values.clear();
    m_background.clear();
    m_backgroundCursor = 0;
    m_visible.clear();
    m_busy = false;
    m_pendingRequest = 0;
    m_sweepBuffers.clear();
    m_visibleSweepMs = 0.0;
    m_lastVisibleMs = 0;
}

void ValuePoller::injectValuesForTest(const QHash<QString, QVector<double>> &values)
{
    QStringList keys;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        m_values.insert(it.key(), it.value());
        keys << it.key();
    }
    if (!keys.isEmpty())
        emit valuesUpdated(keys);
}

void ValuePoller::buildSpansForTest(const QVector<WatchVariable> &targets,
                                    QVector<QPair<quint32, int>> *spans)
{
    QVector<Span> merged;
    QVector<Placement> placements;
    buildSpans(targets, &merged, &placements);
    spans->clear();
    for (const Span &span : merged)
        spans->append(qMakePair(span.address, span.length));
}

void ValuePoller::buildSpans(const QVector<WatchVariable> &targets, QVector<Span> *spans,
                             QVector<Placement> *placements)
{
    spans->clear();
    placements->clear();
    placements->resize(targets.size());

    // Sort by address so neighbouring variables can share one request. The per-request
    // round trip dominates the cost (~1 ms against a few hundred bytes), so cutting the
    // request count is the only optimisation that matters.
    QVector<int> order(targets.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&targets](int a, int b) {
        return targets.at(a).address < targets.at(b).address;
    });

    for (int index : order) {
        const WatchVariable &variable = targets.at(index);
        const int length = cappedSpan(variable);
        if (length <= 0)
            continue;
        const quint32 begin = variable.address;
        const quint32 end = begin + quint32(length);
        if (!spans->isEmpty()) {
            Span &last = spans->last();
            const quint32 lastEnd = last.address + quint32(last.length);
            // Merge when overlapping or adjacent; a gap would need its own request.
            if (begin <= lastEnd) {
                if (end > lastEnd)
                    last.length = int(end - last.address);
                (*placements)[index] = {int(spans->size()) - 1, int(begin - last.address)};
                continue;
            }
        }
        Span span;
        span.address = begin;
        span.length = length;
        spans->append(span);
        (*placements)[index] = {int(spans->size()) - 1, 0};
    }
}

void ValuePoller::tick()
{
    if (m_suspended || m_busy || !m_probe)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool visibleDue = !m_visible.isEmpty()
        && (m_lastVisibleMs == 0 || now - m_lastVisibleMs >= kVisiblePeriodMs);

    if (visibleDue) {
        startSweep(true);
        return;
    }
    if (!m_background.isEmpty())
        startSweep(false);
}

void ValuePoller::startSweep(bool visible)
{
    if (visible) {
        m_sweepTargets = m_visible;
    } else {
        // One time-boxed slice of the background set: a few hundred targets per chunk,
        // picked round-robin so every variable is eventually covered.
        constexpr int kChunkTargets = 256;
        m_sweepTargets.clear();
        if (m_background.isEmpty())
            return;
        const int total = m_background.size();
        const int count = qMin(kChunkTargets, total);
        for (int i = 0; i < count; ++i)
            m_sweepTargets.append(m_background.at((m_backgroundCursor + i) % total));
        m_backgroundCursor = (m_backgroundCursor + count) % total;
    }

    buildSpans(m_sweepTargets, &m_sweepSpans, &m_sweepPlacements);
    if (m_sweepSpans.isEmpty())
        return;

    m_sweepBuffers.clear();
    m_sweepBuffers.resize(m_sweepSpans.size());
    m_spanCursor = 0;
    m_busy = true;
    m_sweepIsVisible = visible;
    m_sweepClock.start();
    issueNext();
}

void ValuePoller::issueNext()
{
    if (!m_probe || m_spanCursor >= m_sweepSpans.size()) {
        finishSweep();
        return;
    }

    // The background pass yields once its budget is spent, so the visible refresh keeps
    // its 200 ms cadence instead of queueing behind a long sweep.
    if (!m_sweepIsVisible && m_sweepClock.elapsed() > kBackgroundBudgetMs) {
        finishSweep();
        return;
    }

    const Span &span = m_sweepSpans.at(m_spanCursor);
    m_pendingRequest = m_probe->nextRequestId();
    m_probe->requestRead(m_pendingRequest, span.address, span.length);
    // A backend that never answers would wedge the poller forever; the tick restarts it
    // because m_busy is cleared here.
    QTimer::singleShot(2000, this, [this, request = m_pendingRequest]() {
        if (m_busy && m_pendingRequest == request) {
            m_busy = false;
            m_pendingRequest = 0;
        }
    });
}

void ValuePoller::onReadFinished(quint64 requestId, quint32 address, const QByteArray &data,
                                 bool ok, const QString &error)
{
    Q_UNUSED(address);
    Q_UNUSED(error);
    if (!m_busy || requestId != m_pendingRequest)
        return; // a stray or abandoned reply

    if (ok && m_spanCursor < m_sweepBuffers.size())
        m_sweepBuffers[m_spanCursor] = data;
    ++m_spanCursor;
    m_pendingRequest = 0;
    issueNext();
}

void ValuePoller::finishSweep()
{
    m_busy = false;
    m_pendingRequest = 0;

    QStringList updated;
    for (int i = 0; i < m_sweepTargets.size(); ++i) {
        const Placement &placement = m_sweepPlacements.at(i);
        if (placement.spanIndex < 0 || placement.spanIndex >= m_sweepBuffers.size())
            continue;
        const QByteArray &buffer = m_sweepBuffers.at(placement.spanIndex);
        if (buffer.isEmpty())
            continue;
        const WatchVariable &variable = m_sweepTargets.at(i);
        const int length = cappedSpan(variable);
        if (placement.offset < 0 || placement.offset + length > buffer.size())
            continue;
        const QByteArray slice =
            buffer.mid(placement.offset, qMin(length, buffer.size() - placement.offset));
        const int elements = variable.elementSize > 0
            ? qMax(1, slice.size() / variable.elementSize)
            : 1;
        m_values.insert(variable.name,
                        SymbolFile::decode(variable, slice, elements));
        updated << variable.name;
    }

    if (m_sweepIsVisible) {
        m_visibleSweepMs = double(m_sweepClock.elapsed());
        m_lastVisibleMs = QDateTime::currentMSecsSinceEpoch();
        emit visibleSweepFinished(m_visibleSweepMs);
    }
    if (!updated.isEmpty())
        emit valuesUpdated(updated);

    // Chain into the next background chunk without waiting for the tick, so a slow
    // sweep still makes progress; the visible pass preempts it in tick().
    if (!m_sweepIsVisible && !m_suspended && !m_background.isEmpty())
        QTimer::singleShot(0, this, [this]() {
            if (!m_suspended && !m_busy)
                startSweep(false);
        });
}

} // namespace mrd
