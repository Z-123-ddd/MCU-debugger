#pragma once

#include "core/SymbolFile.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

namespace mrd {

class ProbeController;

/// Keeps the numbers in the value columns current.
///
/// Two budgets, because a full refresh cannot run at the rate a human reads:
///
///   * **visible rows** every 200 ms -- the few dozen rows actually on screen;
///   * **everything else** swept in the background, one time-boxed chunk per tick,
///     round-robin, so it eventually covers the whole image without ever starving the
///     visible pass.
///
/// Why not "all variables every 200 ms": measured on a real 3777-leaf image, one sweep
/// costs ~692 ms even after merging neighbouring variables into contiguous blocks. The
/// bottleneck is the per-request protocol round trip (~1 ms), not the byte count -- 700
/// merged requests cannot fit in 200 ms. Refreshing only what is on screen is what makes
/// 200 ms honest.
///
/// Reads are non-intrusive. `JLINK_ReadMem` goes through the debug port while the core
/// keeps executing: measured `DHCSR.C_HALT` stays 0 throughout and the target's tick
/// counter keeps advancing. That is also why polling must stand aside during HSS live
/// capture and during a snapshot read -- all three share one debug link, and a sweep
/// would steal the bandwidth the sampler needs to avoid dropped samples.
class ValuePoller : public QObject
{
    Q_OBJECT

public:
    /// Period for the rows the user can see.
    static constexpr int kVisiblePeriodMs = 200;
    /// Wall time one background chunk may occupy before yielding to the next tick.
    static constexpr int kBackgroundBudgetMs = 120;
    /// Cap on one target's read, so a huge array cannot stall a sweep.
    static constexpr int kMaxTargetBytes = 64 * 1024;

    explicit ValuePoller(QObject *parent = nullptr);

    /// Not owned. Without it every request is a no-op.
    void setProbe(ProbeController *probe);

    /// Rows the user can actually see (expanded, on screen, or a register on display).
    /// Replaces the previous set; the next tick refreshes it.
    void setVisibleTargets(const QVector<WatchVariable> &targets);
    /// Every addressable leaf. Swept slowly in the background.
    void setBackgroundTargets(const QVector<WatchVariable> &targets);

    /// Stop issuing reads. Used while HSS samples or a snapshot capture is running.
    void setSuspended(bool suspended);
    bool isSuspended() const { return m_suspended; }

    /// Latest decoded values, keyed by WatchVariable::name.
    const QHash<QString, QVector<double>> &values() const { return m_values; }
    /// Drop everything (new symbol file, or a clear).
    void reset();

    double visibleSweepMs() const { return m_visibleSweepMs; }
    /// Test hook: pretend a sweep produced these numbers and notify the views, so the
    /// value column can be checked without a probe attached.
    void injectValuesForTest(const QHash<QString, QVector<double>> &values);
    /// Test hook: the request-coalescing rule, exposed because it is the whole reason a
    /// 200 ms visible sweep is affordable (request count, not byte count, is the cost).
    static void buildSpansForTest(const QVector<WatchVariable> &targets,
                                  QVector<QPair<quint32, int>> *spans);

    /// How far the background sweep has got, for the status line.
    int backgroundCursor() const { return m_backgroundCursor; }
    int backgroundTotal() const { return m_background.size(); }

signals:
    /// These keys have fresh values; repaint the affected cells only.
    void valuesUpdated(const QStringList &keys);
    /// One visible sweep completed, carrying its measured cost.
    void visibleSweepFinished(double milliseconds);

private:
    /// One coalesced memory request covering one or more targets.
    struct Span
    {
        quint32 address = 0;
        int length = 0;
    };
    /// Where a target's bytes live inside the span buffer that covers it.
    struct Placement
    {
        int spanIndex = -1;
        int offset = 0;
    };

    void tick();
    void startSweep(bool visible);
    void issueNext();
    void onReadFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok,
                        const QString &error);
    void finishSweep();
    /// Build merged spans for \a targets and each target's placement in them.
    static void buildSpans(const QVector<WatchVariable> &targets, QVector<Span> *spans,
                           QVector<Placement> *placements);

    ProbeController *m_probe = nullptr;
    QTimer m_timer;

    QVector<WatchVariable> m_visible;
    QVector<WatchVariable> m_background;
    int m_backgroundCursor = 0;

    bool m_suspended = false;
    /// One sweep at a time: visible and background share the debug link.
    bool m_busy = false;
    bool m_sweepIsVisible = false;
    QVector<WatchVariable> m_sweepTargets;
    QVector<Span> m_sweepSpans;
    QVector<Placement> m_sweepPlacements;
    QVector<QByteArray> m_sweepBuffers;
    int m_spanCursor = 0;
    quint64 m_pendingRequest = 0;

    QHash<QString, QVector<double>> m_values;
    double m_visibleSweepMs = 0.0;
    qint64 m_lastVisibleMs = 0;
    QElapsedTimer m_sweepClock;
};

} // namespace mrd
