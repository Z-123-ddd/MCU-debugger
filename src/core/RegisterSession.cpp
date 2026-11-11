#include "core/RegisterSession.h"

#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeController.h"

#include <QDateTime>

#include <algorithm>
#include <cstring>

namespace mrd {

namespace {
constexpr int kMaxHistory = 100;
constexpr qint64 kReadStaleMs = 1500;

quint32 composeFromBytes(const QByteArray &data, int byteCount)
{
    quint32 value = 0;
    // Clamp to the destination: callers pass a whole register *window* length
    // (tens of bytes) when they just want the first word, and memcpy'ing that
    // into a quint32 overruns the stack.
    const int n = qMin(qMin(byteCount, data.size()), int(sizeof(value)));
    std::memcpy(&value, data.constData(), static_cast<size_t>(n));
    return value;
}
} // namespace

RegisterSession::RegisterSession(ProbeController *probe, QObject *parent)
    : QObject(parent)
    , m_probe(probe)
{
    if (m_probe) {
        connect(m_probe, &ProbeController::readFinished, this, &RegisterSession::onReadFinished);
        connect(m_probe, &ProbeController::writeFinished, this, &RegisterSession::onWriteFinished);
    }
}

RegisterSession::~RegisterSession() = default;

void RegisterSession::setDevice(const SvdDevice &device)
{
    m_device = device;
    m_values.clear();
    m_pendingReads.clear();
    m_history.clear();
    rebuildRegisterIndex();
    emit deviceChanged();
    emit historyChanged();
}

void RegisterSession::clearDevice()
{
    m_device = SvdDevice();
    m_values.clear();
    m_pendingReads.clear();
    m_history.clear();
    m_registerIndex.clear();
    emit deviceChanged();
    emit historyChanged();
}

void RegisterSession::setConnected(bool connected)
{
    m_connected = connected;
    if (!connected) {
        // Drop "live" flags: the next poll repopulates them.
        for (auto &v : m_values)
            v.valid = false;
    }
}

void RegisterSession::rebuildRegisterIndex()
{
    m_registerIndex.clear();
    m_registerIndex.reserve(m_device.totalRegisters());
    for (const auto &peripheral : m_device.peripherals) {
        for (const auto &reg : peripheral.registers) {
            const quint32 address = peripheral.baseAddress + reg.addressOffset;
            m_registerIndex.append({address, &reg});
        }
    }
    std::sort(m_registerIndex.begin(), m_registerIndex.end(),
              [](const QPair<quint32, const SvdRegister *> &a,
                 const QPair<quint32, const SvdRegister *> &b) { return a.first < b.first; });
}

quint32 RegisterSession::resetValueFor(const SvdRegister &reg) const
{
    return reg.resetValue;
}

RegisterValue RegisterSession::valueFor(quint32 address) const
{
    const auto it = m_values.constFind(address);
    if (it != m_values.constEnd())
        return it.value();

    RegisterValue value;
    value.offline = true;
    return value;
}

quint32 RegisterSession::effectiveValue(quint32 address) const
{
    const auto it = m_values.constFind(address);
    if (it != m_values.constEnd() && it->valid)
        return it->value;
    return it == m_values.constEnd() ? 0u : it->value;
}

QHash<quint32, quint32> RegisterSession::snapshot(const SvdPeripheral &peripheral) const
{
    QHash<quint32, quint32> out;
    for (const auto &reg : peripheral.registers) {
        const quint32 address = peripheral.baseAddress + reg.addressOffset;
        const auto it = m_values.constFind(address);
        out.insert(address, it != m_values.constEnd() ? it->value : reg.resetValue);
    }
    return out;
}

QHash<quint32, quint32> RegisterSession::allValues() const
{
    QHash<quint32, quint32> out;
    for (const auto &peripheral : m_device.peripherals) {
        for (const auto &reg : peripheral.registers) {
            const quint32 address = peripheral.baseAddress + reg.addressOffset;
            const auto it = m_values.constFind(address);
            out.insert(address, it != m_values.constEnd() ? it->value : reg.resetValue);
        }
    }
    return out;
}

void RegisterSession::applyRead(quint32 address, const QByteArray &data, bool ok, const QString &error)
{
    RegisterValue value;
    const auto it = m_values.find(address);
    if (it != m_values.end())
        value = *it;

    if (ok) {
        const int byteCount = data.size();
        value.value = composeFromBytes(data, byteCount);
        value.valid = true;
        value.offline = false;
        value.error.clear();
        value.timestampMs = QDateTime::currentMSecsSinceEpoch();
    } else {
        value.valid = false;
        value.error = error;
    }
    m_values.insert(address, value);

    if (!ok)
        noteReadFailure(address, error);
}

void RegisterSession::noteReadFailure(quint32 address, const QString &error)
{
    // A failure caused by a dead link is not a register problem: mark the session
    // unusable so the poll loop stops queueing requests, instead of producing one
    // "GDB Server 未连接" line per polled address for ever.
    const bool sessionLost =
        error.contains(QStringLiteral("未连接"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("not connected"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("探针未打开"), Qt::CaseInsensitive);
    if (sessionLost && m_connected) {
        Logger::instance()->warn(
            QStringLiteral("读取失败且提示未连接，标记会话为断开: %1").arg(error));
        setConnected(false);
    }
    emit readFailed(address, error);
}

void RegisterSession::setValueLocally(quint32 address, quint32 value)
{
    RegisterValue v = m_values.value(address);
    v.value = value;
    v.valid = true;
    v.offline = false;
    v.timestampMs = QDateTime::currentMSecsSinceEpoch();
    m_values.insert(address, v);
}

void RegisterSession::requestPeripheralRead(const SvdPeripheral &peripheral)
{
    if (!m_probe || !m_connected || peripheral.registers.isEmpty())
        return;

    // One window read per peripheral: much cheaper than one process per register.
    quint32 low = 0xFFFFFFFFu;
    quint32 high = 0;
    for (const auto &reg : peripheral.registers) {
        low = qMin(low, reg.addressOffset);
        high = qMax(high, reg.addressOffset + static_cast<quint32>(reg.byteSize()));
    }
    if (low == 0xFFFFFFFFu || high <= low)
        return;

    // Align to 4 bytes so `probe-rs read b32` can be used.
    const quint32 alignedLow = low & ~0x3u;
    const quint32 alignedHigh = (high + 3u) & ~0x3u;
    const int length = static_cast<int>(alignedHigh - alignedLow);

    const quint64 requestId = m_probe->nextRequestId();
    PendingRead pending;
    pending.address = peripheral.baseAddress + alignedLow;
    pending.length = length;
    pending.startMs = QDateTime::currentMSecsSinceEpoch();
    pending.poll = true;
    m_pendingReads.insert(requestId, pending);
    m_probe->requestRead(requestId, pending.address, length);
}

void RegisterSession::requestRegisterRead(const SvdPeripheral &peripheral, const SvdRegister &reg)
{
    if (!m_probe || !m_connected)
        return;
    const quint64 requestId = m_probe->nextRequestId();
    PendingRead pending;
    pending.address = peripheral.baseAddress + reg.addressOffset;
    pending.length = reg.byteSize();
    pending.startMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingReads.insert(requestId, pending);
    m_probe->requestRead(requestId, pending.address, pending.length);
}

void RegisterSession::requestRawRead(quint32 address)
{
    if (!m_probe || !m_connected)
        return;
    const quint64 requestId = m_probe->nextRequestId();
    PendingRead pending;
    pending.address = address;
    pending.length = 4;
    pending.startMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingReads.insert(requestId, pending);
    m_probe->requestRead(requestId, address, 4);
}

void RegisterSession::writeRawWord(quint32 address, quint32 value)
{
    if (!m_probe || !m_connected)
        return;
    QByteArray data(4, char(0));
    std::memcpy(data.data(), &value, 4);
    m_probe->requestWrite(m_probe->nextRequestId(), address, data);
    Logger::instance()->debug(QStringLiteral("raw write %1 = %2").arg(hexAddr(address), hex32(value)));
}

void RegisterSession::rememberWrite(quint32 address, quint32 oldValue, bool oldValid)
{
    WriteRecord record;
    record.address = address;
    record.oldValue = oldValue;
    record.oldValid = oldValid;
    m_history.append(record);
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
    emit historyChanged();
}

void RegisterSession::writeRegister(const SvdPeripheral &peripheral, const SvdRegister &reg, quint32 value)
{
    if (!m_probe || !m_connected) {
        emit writeFinished(peripheral.baseAddress + reg.addressOffset, false, QStringLiteral("目标未连接"));
        return;
    }

    const quint32 address = peripheral.baseAddress + reg.addressOffset;
    const quint32 clamped = value & reg.valueMask();
    const RegisterValue previous = m_values.value(address);
    rememberWrite(address, previous.value, previous.valid);

    const int byteCount = reg.byteSize();
    QByteArray data(byteCount, '\0');
    std::memcpy(data.data(), &clamped, static_cast<size_t>(byteCount));

    const quint64 requestId = m_probe->nextRequestId();
    m_probe->requestWrite(requestId, address, data);
    Logger::instance()->debug(QStringLiteral("write %1 = %2").arg(hexAddr(address), hex32(clamped)));
}

void RegisterSession::writeField(const SvdPeripheral &peripheral, const SvdRegister &reg, const SvdField &field,
                                 quint32 fieldValue)
{
    const quint32 address = peripheral.baseAddress + reg.addressOffset;
    const quint32 current = effectiveValue(address);
    const quint32 updated = field.compose(current, fieldValue);
    if (updated == current) {
        emit writeFinished(address, true, QString());
        return;
    }
    writeRegister(peripheral, reg, updated);
}

void RegisterSession::applyRegisterWrite(quint32 address, const QByteArray &data)
{
    if (!m_probe || !m_connected) {
        emit writeFinished(address, false, QStringLiteral("目标未连接"));
        return;
    }
    if (data.isEmpty())
        return;

    const RegisterValue previous = m_values.value(address);
    rememberWrite(address, previous.value, previous.valid);

    m_probe->requestWrite(m_probe->nextRequestId(), address, data);
    Logger::instance()->debug(QStringLiteral("write %1 = %2")
                                  .arg(hexAddr(address), hex32(composeFromBytes(data, data.size()))));
}

void RegisterSession::undoLastWrite()
{
    if (m_history.isEmpty() || !m_probe || !m_connected)
        return;
    const WriteRecord record = m_history.takeLast();
    emit historyChanged();
    if (!record.oldValid) {
        Logger::instance()->warn(QStringLiteral("撤销 %1 失败：没有原始值").arg(hexAddr(record.address)));
        return;
    }
    QByteArray data(4, '\0');
    std::memcpy(data.data(), &record.oldValue, 4);
    const quint64 requestId = m_probe->nextRequestId();
    m_probe->requestWrite(requestId, record.address, data);
    Logger::instance()->info(QStringLiteral("撤销写入 %1 = %2").arg(hexAddr(record.address), hex32(record.oldValue)));
}

void RegisterSession::onReadFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok,
                                     const QString &error)
{
    const auto pendingIt = m_pendingReads.find(requestId);
    if (pendingIt == m_pendingReads.end())
        return;
    m_pendingReads.erase(pendingIt);

    if (!ok) {
        noteReadFailure(address, error);
        return;
    }

    // Split the window into per-register values. Only the registers inside the
    // window are visited: the index is sorted, so both ends are found by bisection
    // instead of scanning the whole device (1455 registers) per completion.
    quint32 first = address;
    quint32 last = address;
    bool matched = false;
    const quint32 windowEnd = address + static_cast<quint32>(data.size());
    auto it = std::lower_bound(
        m_registerIndex.begin(), m_registerIndex.end(), address,
        [](const QPair<quint32, const SvdRegister *> &entry, quint32 wanted) {
            return entry.first < wanted;
        });
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (; it != m_registerIndex.end() && it->first < windowEnd; ++it) {
        const quint32 regAddress = it->first;
        const SvdRegister *reg = it->second;
        if (!reg)
            continue;
        const int offset = static_cast<int>(regAddress - address);
        const int byteCount = qMin(reg->byteSize(), data.size() - offset);
        if (byteCount <= 0)
            continue;
        const quint32 value = composeFromBytes(data.mid(offset, byteCount), byteCount);
        RegisterValue v = m_values.value(regAddress);
        v.value = value;
        v.valid = true;
        v.offline = false;
        v.error.clear();
        v.timestampMs = now;
        v.differsFromReset = (value != reg->resetValue);
        m_values.insert(regAddress, v);
        if (!matched) {
            first = regAddress;
            matched = true;
        }
        last = regAddress;
    }

    if (!matched) {
        // Unknown window (e.g. manual read): store raw at the start address.
        applyRead(address, data, ok, error);
        first = address;
        last = address;
    }
    emit valuesChanged(first, last);
}

void RegisterSession::onWriteFinished(quint64 requestId, quint32 address, bool ok, const QString &error)
{
    Q_UNUSED(requestId);
    if (ok) {
        // Reflect the new value immediately; the next poll confirms it.
        emit writeFinished(address, true, QString());
    } else {
        emit writeFinished(address, false, error);
        emit readFailed(address, error);
    }
}

} // namespace mrd
