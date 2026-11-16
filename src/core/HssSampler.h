#pragma once

#include "core/SampleHistory.h"
#include "core/SymbolFile.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

class ProbeController;

/// Background sampler built on the J-Link DLL's HSS engine.
///
/// This is the piece the GDB path cannot provide. With HSS the *probe* reads the
/// configured addresses on its own timebase while the core keeps running, and the host
/// only drains a buffer -- so sampling does not steal execution time from the target,
/// and the curve is live rather than a frozen snapshot.
///
/// All DLL access happens on the probe worker thread: the session lives there, and HSS
/// is stateful (start once, read repeatedly). This class therefore talks to
/// ProbeController, never to the DLL. Touching the DLL from the UI thread would mean
/// racing the worker for one non-thread-safe session.
///
/// Layout facts, verified against a live J-Link PLUS:
///   - `JLINK_HSS_Start(blocks, numBlocks, periodUs, flags)` with flags = 1;
///   - the block array is walked until a zeroed terminator, so one must be present;
///   - each record is [4-byte timestamp][block0][block1]...;
///   - `JLINK_HSS_Read` returns a sample *count*, not a byte count, and its value can
///     exceed the buffer, so the tail has to be validated by timestamp.
class HssSampler : public QObject
{
    Q_OBJECT

public:
    /// Result of one drain, so callers can report what actually arrived.
    struct Result
    {
        int samples = 0;
        int bytes = 0;
        QString error;
        /// True when the read limit was reached: more records are waiting.
        bool morePending = false;
    };

    /// Running statistics per sampled variable: the value now, plus min/max/average,
    /// which is what makes a moving number readable without staring at the curve.
    struct Channel
    {
        QString name;
        QString typeLabel;
        quint32 address = 0;
        int width = 4;
        /// The exact variable this channel samples. Kept rather than looked up by
        /// address later: two selected variables can share an address in this project
        /// (armclang merged-globals blocks), and decoding one as the other's type is
        /// how a stable signal turns into a garbage spike.
        WatchVariable source;
        double latest = 0.0;
        double min = 0.0;
        double max = 0.0;
        double sum = 0.0;
        qint64 count = 0;
        bool truncatedArray = false;

        double average() const { return count > 0 ? sum / double(count) : 0.0; }
    };

    explicit HssSampler(QObject *parent = nullptr);

    void setProbe(ProbeController *probe);
    bool isRunning() const { return m_running; }
    int periodUs() const { return m_periodUs; }
    qint64 totalSamples() const { return m_totalSamples; }
    double measuredIntervalUs() const { return m_measuredIntervalUs; }
    const QVector<Channel> &channels() const { return m_channels; }
    QString lastError() const { return m_lastError; }

    /// Turn the probe's 32-bit microsecond timestamps into one monotonic timeline.
    ///
    /// \a wrapBase and \a lastRaw are carried across drains. \a wrapBase counts how
    /// many times the 32-bit counter has wrapped (each wrap adds 2^32 microseconds);
    /// \a lastRaw is the previous record's raw timestamp, needed to recognise a wrap
    /// that happens *between* two drains.
    ///
    /// Returns how many records are usable. The loop stops at the first record that is
    /// the zeroed tail of the DLL buffer rather than real data.
    static int unwrapTimestamps(const quint32 *raws, int count, qint64 *out,
                                qint64 *wrapBase, quint32 *lastRaw, bool *haveLastRaw);

    /// Maximum variables per session: the DLL refuses more than eight blocks.
    static int maxVariables() { return 8; }

    /// Configure and start. Arrays are streamed as element 0 only.
    ///
    /// \a timeOffsetUs shifts the whole timeline. A restarted HSS session begins its
    /// 32-bit timestamp at 0 again, so resuming a paused capture passes the last
    /// timestamp already plotted -- otherwise the new samples would land in the past.
    Status start(const QVector<WatchVariable> &variables, int periodUs,
                 qint64 timeOffsetUs = 0);
    Status stop();

    /// Request one drain. Asynchronous: the result arrives via drainFinished.
    bool requestDrain();
    bool isDrainPending() const { return m_drainPending; }

signals:
    void failed(const QString &error);
    /// The worker confirmed (or refused) the HSS start. Sampling is asynchronous, so
    /// this is the only reliable "can I drain now?" signal.
    void started(bool ok, const QString &error);
    /// Emitted when a requested drain completes (history already updated).
    void drained(const mrd::HssSampler::Result &result);

private slots:
    void onHssStarted(bool ok, const QString &error);
    void onHssReadFinished(quint64 requestId, const QByteArray &records, int samples,
                           int sampleBytes, bool ok, const QString &error);

private:
    void applyRecords(const QByteArray &records, int samples, int sampleBytes);

    ProbeController *m_probe = nullptr;
    bool m_running = false;
    bool m_starting = false;
    bool m_drainPending = false;
    QString m_lastError;

    int m_sampleBytes = 0;
    int m_periodUs = 1000;
    qint64 m_totalSamples = 0;
    double m_measuredIntervalUs = 0.0;
    /// Previous raw probe timestamp and wrap count, for unwrapping.
    /// Multiples of 2^32 already added; the unwrap base.
    qint64 m_wrapBaseUs = 0;
    /// Last raw timestamp seen, for detecting a wrap between drains.
    quint32 m_lastRawTs = 0;
    bool m_haveLastRaw = false;

    QVector<WatchVariable> m_variables;
    QVector<Channel> m_channels;
    /// Set by applyRecords, consumed by whoever listens to `drained`.
    SampleHistory *m_pendingHistory = nullptr;
    quint64 m_drainId = 0;

public:
    /// The drain writes into a caller-owned history; set right before requestDrain().
    void setDrainTarget(SampleHistory *history) { m_pendingHistory = history; }
};

} // namespace mrd
