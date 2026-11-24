#include "core/HssSampler.h"

#include "core/ProbeController.h"

#include <cstring>
#include <limits>

namespace mrd {

namespace {

/// Records to ask for per drain. The probe buffers far more than this while the UI is
/// not looking, so a shallow request only means more round trips.
constexpr int kDrainRecords = 2048;

} // namespace

HssSampler::HssSampler(QObject *parent)
    : QObject(parent)
{
}

void HssSampler::setProbe(ProbeController *probe)
{
    if (m_probe == probe)
        return;
    if (m_probe) {
        disconnect(m_probe, &ProbeController::hssStarted, this, &HssSampler::onHssStarted);
        disconnect(m_probe, &ProbeController::hssReadFinished, this,
                   &HssSampler::onHssReadFinished);
    }
    m_probe = probe;
    if (m_probe) {
        connect(m_probe, &ProbeController::hssStarted, this, &HssSampler::onHssStarted);
        connect(m_probe, &ProbeController::hssReadFinished, this,
                &HssSampler::onHssReadFinished);
    }
}

Status HssSampler::start(const QVector<WatchVariable> &variables, int periodUs,
                         qint64 timeOffsetUs)
{
    if (!m_probe)
        return Status::failure(QStringLiteral("探针尚未就绪"));
    if (m_running || m_starting)
        return Status::failure(QStringLiteral("采样已在进行中"));
    if (!m_probe->supportsHss())
        return Status::failure(
            QStringLiteral("当前后端不支持 HSS 实时采样（需要 J-Link DLL 后端）"));

    QVector<WatchVariable> usable;
    for (const WatchVariable &variable : variables) {
        if (!variable.usable())
            continue;
        if (variable.elementSize <= 0)
            continue;
        usable.append(variable);
    }
    if (usable.isEmpty())
        return Status::failure(
            QStringLiteral("勾选的变量里没有可直接采样的数值（结构体请展开后勾选成员）"));

    // The probe handles eight blocks; more is refused outright, so say so on the first
    // eight rather than silently showing a partial picture.
    if (usable.size() > maxVariables())
        usable.resize(maxVariables());

    m_periodUs = qBound(10, periodUs, 1000000);
    m_variables = usable;
    m_channels.clear();
    m_totalSamples = 0;
    // First record becomes timeOffsetUs, then keeps advancing from there.
    m_wrapBaseUs = qMax<qint64>(0, timeOffsetUs);
    m_lastRawTs = 0;
    m_haveLastRaw = false;
    m_measuredIntervalUs = 0.0;
    m_sampleBytes = 4; // timestamp
    m_lastError.clear();

    QVector<QPair<quint32, int>> blocks;
    blocks.reserve(usable.size());
    for (const WatchVariable &variable : usable) {
        const int width = qBound(1, variable.elementSize, 8);
        blocks.append(qMakePair(variable.address, width));

        Channel channel;
        channel.name = variable.name;
        channel.typeLabel = variable.typeLabel;
        channel.address = variable.address;
        channel.width = width;
        channel.source = variable;
        channel.source.elementSize = width;
        channel.source.elementCount = 1;
        channel.truncatedArray = variable.elementCount > 1;
        m_channels.append(channel);
        m_sampleBytes += width;
    }

    m_starting = true;
    m_probe->requestHssStart(blocks, m_periodUs);
    // The outcome is asynchronous; onHssStarted flips m_running. Reporting success
    // here would be a lie if the DLL refused.
    return Status::success();
}

void HssSampler::onHssStarted(bool ok, const QString &error)
{
    m_starting = false;
    if (!ok) {
        m_running = false;
        m_lastError = error;
        m_channels.clear();
        emit failed(error);
        emit started(false, error);
        return;
    }
    m_running = true;
    emit started(true, QString());
}

Status HssSampler::stop()
{
    if (!m_probe)
        return Status::success();
    m_running = false;
    m_starting = false;
    m_drainPending = false;
    // The drain is asynchronous: a read already queued on the worker can still arrive
    // after this returns. Dropping the target now is what keeps that late result from
    // writing into a history the caller has already destroyed.
    m_pendingHistory = nullptr;
    m_probe->requestHssStop();
    return Status::success();
}

bool HssSampler::requestDrain()
{
    if (!m_running || !m_probe || m_drainPending)
        return false;
    m_drainPending = true;
    m_drainId = m_probe->nextRequestId();
    m_probe->requestHssRead(m_drainId, kDrainRecords, m_sampleBytes);
    return true;
}

void HssSampler::onHssReadFinished(quint64 requestId, const QByteArray &records, int samples,
                                   int sampleBytes, bool ok, const QString &error)
{
    if (requestId != m_drainId)
        return; // a drain this sampler did not start
    m_drainPending = false;
    if (!m_running) {
        // Arrived after stop(): nothing to update, but a waiting caller still needs its
        // wake-up so it does not block until the timeout.
        emit drained(Result());
        return;
    }

    Result result;
    if (!ok) {
        result.error = error;
        m_lastError = error;
        emit failed(error);
        emit drained(result);
        return;
    }
    result.bytes = int(records.size());
    if (samples > 0 && sampleBytes > 0)
        applyRecords(records, samples, sampleBytes);
    result.samples = samples;
    // A full buffer means the probe still holds data we have not read yet. The caller
    // uses this to drain again immediately instead of waiting for the next tick -- that
    // wait is what made the curve trail behind the target.
    result.morePending = samples >= kDrainRecords;
    emit drained(result);
}

int HssSampler::unwrapTimestamps(const quint32 *raws, int count, qint64 *out,
                                 qint64 *wrapBase, quint32 *lastRaw, bool *haveLastRaw)
{
    if (!raws || !out || count <= 0)
        return 0;

    // 2^32 microseconds is where the 32-bit counter rolls over (~71.6 minutes).
    constexpr qint64 kSpan = qint64(1) << 32;
    // A backwards jump of nearly the full range is the counter rolling over (previous
    // was close to 2^32, current close to 0). A small backwards jump is the zeroed tail
    // of the DLL buffer. Half the range separates the two unambiguously.
    constexpr qint64 kHalfSpan = kSpan / 2;

    qint64 base = wrapBase ? *wrapBase : 0;
    quint32 previous = lastRaw ? *lastRaw : 0;
    bool havePrevious = haveLastRaw ? *haveLastRaw : false;
    int usable = 0;

    for (int i = 0; i < count; ++i) {
        const quint32 raw = raws[i];

        if (havePrevious) {
            const qint64 delta = qint64(raw) - qint64(previous);
            if (delta < 0) {
                if (qint64(previous) - qint64(raw) > kHalfSpan) {
                    base += kSpan; // rolled over: previous was near 2^32, now near 0
                } else {
                    break; // zeroed tail: not a record this session produced
                }
            }
            // Same-timestamp records are kept. They used to be dropped as "torn", but
            // two samples inside one microsecond are ordinary data -- dropping them
            // punched holes in the trace.
        }

        out[usable] = base + qint64(raw);
        previous = raw;
        havePrevious = true;
        ++usable;
    }

    if (wrapBase)
        *wrapBase = base;
    if (lastRaw)
        *lastRaw = previous;
    if (haveLastRaw)
        *haveLastRaw = havePrevious;
    return usable;
}

void HssSampler::applyRecords(const QByteArray &records, int samples, int sampleBytes)
{
    // The history is optional. A consumer that only wants the current value -- the
    // register view samples one register and shows its latest reading -- has no use for a
    // time series, and allocating one would mean buffer work on every drain that is
    // immediately thrown away. The channel statistics below are the payload it needs.
    SampleHistory *history = m_pendingHistory;
    if (sampleBytes <= 0)
        return;

    // --- pass 1: collect the raw timestamps and turn them into one timeline ---
    //
    // The DLL's report of how many samples it produced is not trustworthy (it covers
    // slots it touched, including an uninitialised tail), so the records are walked and
    // stopped at the first entry that is not a real record.
    QVector<quint32> raws;
    raws.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        if ((i + 1) * sampleBytes > records.size())
            break;
        quint32 raw = 0;
        std::memcpy(&raw, records.constData() + i * sampleBytes, 4);
        raws.append(raw);
    }
    if (raws.isEmpty())
        return;

    QVector<qint64> times(raws.size());
    const int usable = unwrapTimestamps(raws.constData(), int(raws.size()), times.data(),
                                        &m_wrapBaseUs, &m_lastRawTs, &m_haveLastRaw);
    if (usable <= 0)
        return;

    // --- pass 2: decode ------------------------------------------------------
    for (int i = 0; i < usable; ++i) {
        const char *record = records.constData() + i * sampleBytes;

        QVector<double> row;
        row.reserve(m_channels.size());
        int offset = 4;
        for (Channel &channel : m_channels) {
            const QByteArray slice(record + offset, channel.width);
            const QVector<double> values = SymbolFile::decode(channel.source, slice, 1);
            const double value = values.isEmpty()
                ? std::numeric_limits<double>::quiet_NaN()
                : values.first();
            row.append(value);

            if (std::isfinite(value)) {
                channel.latest = value;
                if (channel.count == 0) {
                    channel.min = value;
                    channel.max = value;
                } else {
                    channel.min = qMin(channel.min, value);
                    channel.max = qMax(channel.max, value);
                }
                channel.sum += value;
                ++channel.count;
            }
            offset += channel.width;
        }
        // Microseconds, full resolution: milliseconds collided neighbouring samples at
        // 1 kHz and drew the trace as vertical lines.
        if (history)
            history->append(times.at(i), row);
    }

    if (usable > 1 && times.at(usable - 1) > times.at(0)) {
        m_measuredIntervalUs =
            double(times.at(usable - 1) - times.at(0)) / double(usable - 1);
    }
    m_totalSamples += usable;
}

} // namespace mrd
