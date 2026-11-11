#include "core/FaultStatus.h"

#include <iterator>

namespace mrd {

QVector<quint32> faultProbeAddresses()
{
    return {FaultStatus::kCfsr, FaultStatus::kHfsr, FaultStatus::kDfsr,
            FaultStatus::kShcsr, FaultStatus::kMmfar, FaultStatus::kBfar};
}

namespace FaultStatus {

namespace {

/// ARMv7-M CFSR field layout (see the Architecture Reference Manual, B3.2.15):
///   [7:0]   MMFSR  mem manage
///   [15:8]  BFSR   bus fault
///   [31:16] UFSR   usage fault
struct BitName
{
    quint32 mask;
    const char *name;
};

// clang-format off
constexpr BitName kMmfsr[] = {
    {1u << 0, "IACCVIOL"},    {1u << 1, "DACCVIOL"},
    {1u << 3, "MUNSTKERR"},   {1u << 4, "MSTKERR"},
    {1u << 5, "MLSPERR"},     {1u << 7, "MMARVALID"},
};
constexpr BitName kBfsr[] = {
    {1u << 8,  "IBUSERR"},    {1u << 9,  "PRECISERR"},
    {1u << 10, "IMPRECISERR"}, {1u << 11, "UNSTKERR"},
    {1u << 12, "STKERR"},     {1u << 13, "LSPERR"},
    {1u << 15, "BFARVALID"},
};
constexpr BitName kUfsr[] = {
    {1u << 16, "UNDEFINSTR"}, {1u << 17, "INVSTATE"},
    {1u << 18, "INVPC"},      {1u << 19, "NOCP"},
    {1u << 20, "STKOF"},      {1u << 24, "UNALIGNED"},
    {1u << 25, "DIVBYZERO"},
};
constexpr BitName kHfsrBits[] = {
    {1u << 1,  "VECTTBL"},  {1u << 30, "FORCED"}, {1u << 31, "DEBUGEVT"},
};
constexpr BitName kDfsr[] = {
    {1u << 0, "HALTED"},    {1u << 1, "BKPT"},
    {1u << 2, "DWTTRAP"},   {1u << 3, "VCATCH"},   {1u << 4, "EXTERNAL"},
};
// clang-format on

void appendSet(const BitName *table, size_t count, quint32 value, QStringList *out)
{
    for (size_t i = 0; i < count; ++i) {
        if (value & table[i].mask)
            out->append(QString::fromLatin1(table[i].name));
    }
}

} // namespace

CpuHealth evaluate(const FaultSnapshot &snapshot, bool cpuHalted)
{
    if (!snapshot.hfsrValid && !snapshot.cfsrValid)
        return CpuHealth::Unknown;

    // HardFault first: it outranks everything except an explicit halt.
    if (snapshot.hfsrValid && (snapshot.hfsr & ((1u << 30) | (1u << 1))) != 0)
        return cpuHalted ? CpuHealth::HardFault : CpuHealth::HardFault;

    if (cpuHalted)
        return CpuHealth::Halted;

    if (snapshot.cfsrValid && snapshot.cfsr != 0)
        return CpuHealth::FaultFlags;
    return CpuHealth::Normal;
}

QString summary(CpuHealth health, const FaultSnapshot &snapshot)
{
    switch (health) {
    case CpuHealth::Unknown:
        return QStringLiteral("状态未知");
    case CpuHealth::Normal:
        return QStringLiteral("正常");
    case CpuHealth::Halted:
        return QStringLiteral("已暂停");
    case CpuHealth::FaultFlags: {
        const QStringList flags = decode(snapshot);
        return QStringLiteral("故障标志 %1").arg(flags.size());
    }
    case CpuHealth::HardFault: {
        // Say what escalated it: FORCED means another fault handler was unable to
        // run (typically a fault taken inside a fault handler).
        const bool forced = snapshot.hfsrValid && (snapshot.hfsr & (1u << 30)) != 0;
        const bool vecttbl = snapshot.hfsrValid && (snapshot.hfsr & (1u << 1)) != 0;
        QStringList why;
        if (forced)
            why << QStringLiteral("FORCED");
        if (vecttbl)
            why << QStringLiteral("VECTTBL");
        return why.isEmpty() ? QStringLiteral("HardFault")
                             : QStringLiteral("HardFault (%1)").arg(why.join(QLatin1Char('+')));
    }
    }
    return QString();
}

QStringList decode(const FaultSnapshot &snapshot)
{
    QStringList flags;
    if (snapshot.cfsrValid) {
        appendSet(kMmfsr, std::size(kMmfsr), snapshot.cfsr, &flags);
        appendSet(kBfsr, std::size(kBfsr), snapshot.cfsr, &flags);
        appendSet(kUfsr, std::size(kUfsr), snapshot.cfsr, &flags);
    }
    if (snapshot.hfsrValid)
        appendSet(kHfsrBits, std::size(kHfsrBits), snapshot.hfsr, &flags);
    return flags;
}

QVector<QPair<quint32, quint32>> clearWrites(const FaultSnapshot &snapshot)
{
    // CFSR and HFSR are write-1-to-clear: writing the current value clears exactly
    // the flags we saw and leaves anything raised in the meantime alone.
    QVector<QPair<quint32, quint32>> writes;
    if (snapshot.cfsrValid && snapshot.cfsr != 0)
        writes.append(qMakePair(kCfsr, snapshot.cfsr));
    if (snapshot.hfsrValid && (snapshot.hfsr & ~(1u << 31)) != 0)
        writes.append(qMakePair(kHfsr, snapshot.hfsr & ~(1u << 31))); // DEBUGEVT is not W1C
    return writes;
}

} // namespace FaultStatus

} // namespace mrd
