#pragma once

#include "core/SymbolFile.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>

namespace mrd {

class ProbeController;

/// Reads a variable's memory into numbers, on demand.
///
/// This is a *snapshot* reader, not a sampler. Continuous acquisition belongs to
/// HssSampler; what this answers is the ad-hoc question "what is in this array right now?" and
/// "how did this scalar drift over the last two seconds?". Both are answered by a
/// bounded number of reads that finish, so there is no timer, no ring buffer and no
/// lost-sample problem -- the probe is asked N times and that is the end of it.
///
/// Two shapes fall out of the same read path:
///   - an array becomes one *column* of values (index on the X axis);
///   - a scalar is read once per tick, so N reads become N *rows* over time.
class SnapshotReader : public QObject
{
    Q_OBJECT

public:
    /// One captured variable.
    struct Series
    {
        QString name;            ///< dotted path, e.g. `config_.error_table`
        QString typeLabel;       ///< "float[64]", "int32_t"
        quint32 address = 0;
        int elementCount = 1;    ///< elements per read
        bool fromArray = false;  ///< true when elementCount > 1
        /// rows[t][element]: one row per capture round, one column per element.
        /// An array fills a single row; a scalar fills one column over many rows.
        QVector<QVector<double>> rows;
        QString error;           ///< set when this series could not be read

        bool isEmpty() const { return rows.isEmpty(); }
        /// Number of capture rounds that actually produced data.
        int roundCount() const { return rows.size(); }
        /// Flattened view: every element of every round, in capture order.
        QVector<double> flat() const;
    };

    /// Everything one capture produced, so the table and the plot agree.
    struct Snapshot
    {
        QVector<Series> series;
        int rounds = 0;          ///< capture rounds requested
        qint64 elapsedMs = 0;    ///< wall time the whole capture took
        QString error;           ///< non-empty when nothing could be read
        bool isEmpty() const { return series.isEmpty(); }
        /// Total values across all series (what the table will show).
        int valueCount() const;
    };

    explicit SnapshotReader(QObject *parent = nullptr);

    /// Not owned. Must be connected before capture() can succeed.
    void setProbe(ProbeController *probe);
    bool isBusy() const { return m_busy; }

    /// Start capturing \a variables.
    ///
    /// \a rounds is how many times to read each variable; it only matters for scalars
    /// (an array is complete after one read). \a intervalMs is the pause between
    /// rounds. Both are clamped: a capture must finish in bounded time.
    void capture(const QVector<WatchVariable> &variables, int rounds = 1,
                 int intervalMs = 20);

signals:
    /// Emitted once per capture, whether or not it produced data.
    void finished(const mrd::SnapshotReader::Snapshot &snapshot);

private:
    struct Pending
    {
        WatchVariable variable;
        int seriesIndex = -1;
        quint64 requestId = 0;
    };

    void startRound();
    void onReadFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok,
                        const QString &error);
    void finishCapture();

    ProbeController *m_probe = nullptr;
    bool m_busy = false;
    int m_rounds = 1;
    int m_intervalMs = 20;
    int m_round = 0;
    Snapshot m_snapshot;
    QVector<WatchVariable> m_variables;
    QVector<Pending> m_pending;
    int m_outstanding = 0;
    QElapsedTimer m_elapsed;
};

} // namespace mrd
