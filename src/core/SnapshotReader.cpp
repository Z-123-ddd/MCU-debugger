#include "core/SnapshotReader.h"

#include "core/ProbeController.h"

#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>

namespace mrd {

namespace {

/// A capture is meant to answer a question about the target, not to stream it, so
/// every dimension is capped. 64 Ki elements covers any table worth eyeballing, and
/// 5000 rounds at the shortest interval still finishes in a few seconds.
constexpr int kMaxElements = 65536;
constexpr int kMaxRounds = 5000;
constexpr int kMinIntervalMs = 0;
constexpr int kMaxIntervalMs = 5000;

} // namespace

QVector<double> SnapshotReader::Series::flat() const
{
    QVector<double> out;
    out.reserve(rows.size() * elementCount);
    for (const QVector<double> &row : rows)
        out.append(row);
    return out;
}

int SnapshotReader::Snapshot::valueCount() const
{
    int total = 0;
    for (const Series &s : series)
        total += int(s.rows.size()) * qMax(1, s.elementCount);
    return total;
}

SnapshotReader::SnapshotReader(QObject *parent)
    : QObject(parent)
{
}

void SnapshotReader::setProbe(ProbeController *probe)
{
    if (m_probe == probe)
        return;
    if (m_probe)
        disconnect(m_probe, &ProbeController::readFinished, this,
                   &SnapshotReader::onReadFinished);
    m_probe = probe;
    if (m_probe) {
        connect(m_probe, &ProbeController::readFinished, this,
                &SnapshotReader::onReadFinished);
    }
}

void SnapshotReader::capture(const QVector<WatchVariable> &variables, int rounds,
                             int intervalMs)
{
    if (!m_probe) {
        Snapshot failed;
        failed.error = QStringLiteral("探针尚未就绪");
        emit finished(failed);
        return;
    }
    if (m_busy) {
        // Restarting mid-flight would leave stale requestIds matched to new series,
        // so the old capture is simply dropped and the caller tries again.
        return;
    }

    m_variables.clear();
    m_snapshot = Snapshot();
    m_round = 0;
    m_outstanding = 0;
    m_pending.clear();

    for (const WatchVariable &variable : variables) {
        // Non-samplable rows (a struct row, a forward declaration) are skipped rather
        // than captured as zeros -- a made-up zero would look like real target data.
        if (!variable.usable())
            continue;
        if (variable.elementSize <= 0)
            continue;
        m_variables.append(variable);
    }
    if (m_variables.isEmpty()) {
        Snapshot failed;
        failed.error = QStringLiteral("勾选的变量里没有可直接读取的数值"
                                      "（结构体请展开后勾选成员，或直接勾选整个数组）");
        emit finished(failed);
        return;
    }

    m_rounds = qBound(1, rounds, kMaxRounds);
    m_intervalMs = qBound(kMinIntervalMs, intervalMs, kMaxIntervalMs);
    m_busy = true;
    m_elapsed.start();

    // One Series per variable, created up front so the table can show a row per
    // variable even if its read later fails.
    for (const WatchVariable &variable : m_variables) {
        Series series;
        series.name = variable.name;
        series.typeLabel = variable.typeLabel;
        series.address = variable.address;
        series.elementCount = qMax(1, qMin(variable.elementCount, kMaxElements));
        series.fromArray = series.elementCount > 1;
        // An array read once is already complete; only a scalar needs repeating to
        // show a trend. Reporting that up front keeps the status line honest.
        m_snapshot.series.append(series);
    }
    m_snapshot.rounds = m_rounds;

    startRound();
}

void SnapshotReader::startRound()
{
    if (!m_probe || !m_busy)
        return;

    m_pending.clear();
    for (int i = 0; i < m_variables.size(); ++i) {
        const WatchVariable &variable = m_variables.at(i);
        Series &series = m_snapshot.series[i];
        // An array is fully described by its first read; re-reading it every round
        // would multiply memory by the round count for no extra information.
        if (series.fromArray && m_round > 0)
            continue;

        const int elements = series.fromArray ? series.elementCount : 1;
        const int bytes = elements * variable.elementSize;
        if (bytes <= 0)
            continue;

        Pending pending;
        pending.variable = variable;
        pending.seriesIndex = i;
        pending.requestId = m_probe->nextRequestId();
        m_pending.append(pending);
    }

    if (m_pending.isEmpty()) {
        finishCapture();
        return;
    }

    m_outstanding = int(m_pending.size());
    for (const Pending &pending : m_pending) {
        const int elements =
            m_snapshot.series[pending.seriesIndex].fromArray
            ? m_snapshot.series[pending.seriesIndex].elementCount
            : 1;
        m_probe->requestRead(pending.requestId, pending.variable.address,
                             elements * pending.variable.elementSize);
    }
}

void SnapshotReader::onReadFinished(quint64 requestId, quint32 address, const QByteArray &data,
                                    bool ok, const QString &error)
{
    Q_UNUSED(address);
    if (!m_busy)
        return;

    int index = -1;
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending.at(i).requestId == requestId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return; // a read this reader did not start (the register view, say)

    const Pending pending = m_pending.takeAt(index);
    if (pending.seriesIndex < 0 || pending.seriesIndex >= m_snapshot.series.size()) {
        if (--m_outstanding <= 0)
            finishCapture();
        return;
    }

    Series &series = m_snapshot.series[pending.seriesIndex];
    if (!ok) {
        if (series.error.isEmpty())
            series.error = error;
    } else {
        const int elements = series.fromArray ? series.elementCount : 1;
        const QVector<double> values =
            SymbolFile::decode(pending.variable, data, elements);
        if (values.isEmpty()) {
            if (series.error.isEmpty())
                series.error = QStringLiteral("字节数不足，无法解码");
        } else {
            series.rows.append(values);
        }
    }

    if (--m_outstanding <= 0) {
        ++m_round;
        const bool moreRounds = m_round < m_rounds;
        if (moreRounds && m_intervalMs > 0) {
            // Let the UI breathe between rounds instead of blocking the event loop;
            // a tight loop would also starve the probe's own signal delivery.
            QTimer::singleShot(m_intervalMs, this, [this] { startRound(); });
        } else if (moreRounds) {
            startRound();
        } else {
            finishCapture();
        }
    }
}

void SnapshotReader::finishCapture()
{
    m_busy = false;
    m_pending.clear();
    m_outstanding = 0;
    m_snapshot.elapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;

    bool anyData = false;
    QString firstError;
    for (const Series &series : m_snapshot.series) {
        if (!series.rows.isEmpty())
            anyData = true;
        if (!series.error.isEmpty() && firstError.isEmpty())
            firstError = series.error;
    }
    if (!anyData)
        m_snapshot.error = firstError.isEmpty() ? QStringLiteral("未读到数据") : firstError;

    const Snapshot result = m_snapshot;
    m_snapshot = Snapshot();
    emit finished(result);
}

} // namespace mrd
