#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

/// Live CPU health read from the Armv7-M System Control Block.
///
/// The SCB fault registers are plain memory (no probe-specific register access
/// needed), which is what lets this work on both the J-Link/GDB and probe-rs
/// backends:
///
///   0xE000ED24 SHCSR   handler active/pending + enable bits
///   0xE000ED28 CFSR    MemManage / BusFault / UsageFault status (latched)
///   0xE000ED2C HFSR    HardFault status: FORCED = escalated from another fault
///   0xE000ED30 DFSR    debug events (halted, breakpoint)
///   0xE000ED34 MMFAR   faulting data address (valid when MMARVALID)
///   0xE000ED38 BFAR    faulting bus address  (valid when BFARVALID)
///
/// A fault that lands in the default `HardFault_Handler` (an infinite loop) keeps
/// HFSR.FORCED set until software clears it, so reading HFSR is enough to tell the
/// user "this MCU crashed and is stuck" without needing the PC.
struct FaultSnapshot
{
    bool cfsrValid = false;
    bool hfsrValid = false;
    bool dfsrValid = false;
    bool shcsrValid = false;
    quint32 cfsr = 0;
    quint32 hfsr = 0;
    quint32 dfsr = 0;
    quint32 shcsr = 0;
    bool mmfarValid = false;
    bool bfarValid = false;
    quint32 mmfar = 0;
    quint32 bfar = 0;
};

/// Overall verdict, ordered by severity.
enum class CpuHealth
{
    Unknown,    ///< nothing read yet (not connected / backend refused the read)
    Normal,     ///< no fault latched
    FaultFlags, ///< a configurable fault is latched but no HardFault escalation
    HardFault,  ///< HardFault occurred (FORCED or VECTTBL) - CPU is in the handler
    Halted,     ///< core halted by the debugger
};

/// Addresses to read, in the order `FaultStatus::snapshot()` expects them.
QVector<quint32> faultProbeAddresses();

namespace FaultStatus {

/// SCB addresses.
constexpr quint32 kShcsr = 0xE000ED24u;
constexpr quint32 kCfsr = 0xE000ED28u;
constexpr quint32 kHfsr = 0xE000ED2Cu;
constexpr quint32 kDfsr = 0xE000ED30u;
constexpr quint32 kMmfar = 0xE000ED34u;
constexpr quint32 kBfar = 0xE000ED38u;

/// Decided from the snapshot only (pure, so it is unit-testable).
CpuHealth evaluate(const FaultSnapshot &snapshot, bool cpuHalted);

/// One line for the status bar, e.g. "HardFault (FORCED)".
QString summary(CpuHealth health, const FaultSnapshot &snapshot);

/// Human-readable flag names that are set, e.g. "IMPRECISERR", "DIVBYZERO".
QStringList decode(const FaultSnapshot &snapshot);

/// Values to write back to clear the latched flags (write-1-to-clear).
/// Empty when there is nothing to clear.
QVector<QPair<quint32, quint32>> clearWrites(const FaultSnapshot &snapshot);

} // namespace FaultStatus

} // namespace mrd
