#pragma once

#include "core/Result.h"
#include "core/SvdTypes.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <QMetaType>

#include <functional>

namespace mrd {

class IProbe;

/// What the target actually reports about one SVD peripheral.
struct PeripheralStatus
{
    enum Reachability {
        /// A register was read successfully: the bus decodes this address.
        Readable,
        /// The bus rejected the access, and the clock is not known to be enabled.
        Unreachable,
        /// The bus rejected the access although the clock is enabled, so the SVD
        /// lists a peripheral this particular part does not implement.
        Missing,
    };

    QString name;
    QString group;
    quint32 baseAddress = 0;
    int registerCount = 0;

    Reachability reachability = Unreachable;
    /// Clock gate state from RCC; unknown when the SVD exposes no enable bit.
    bool clockKnown = false;
    bool clockEnabled = false;
    /// "RCC_AHB1PCLKEN[0]" style description of where the gate lives.
    QString clockSource;

    /// Register used as the presence probe and the value it returned.
    QString probeRegister;
    quint32 probeValue = 0;
    QString error;

    /// GPIO pins this *package* bonds out; -1 when not known (non-GPIO peripheral,
    /// or no package table). A GPIO port whose registers answer on the bus but
    /// which has zero bonded pins is not usable on this part even though the bus
    /// decodes it, so bonding has to override the read result.
    int bondedPins = -1;

    /// Does this *part* implement the peripheral, judged from static vendor data
    /// (CMSIS-Pack capability counts + package pin signals)?
    ///
    /// This is the primary filter: the SVD describes the die/family, so it lists
    /// blocks this particular model does not have (e.g. SPI5 on a 48-pin
    /// N32H473CE). Defaults to true so "no data" never hides anything.
    bool exists = true;
    /// Why a peripheral was judged absent, for the UI tooltip.
    QString absenceReason;

    bool accessible() const { return reachability == Readable; }
    /// False only when we know the package brings no pin out for this peripheral.
    bool bonded() const { return bondedPins != 0; }
    /// True when this part really offers the peripheral: static vendor data says
    /// the model implements it and the package exposes a pin for it.
    ///
    /// Live read results deliberately do not enter this decision: a gated
    /// peripheral cannot answer on the bus, so "the read failed" says nothing
    /// about whether the block exists.
    bool onChip() const { return exists && bonded(); }
    /// Registers respond but no pin is bonded out (typical for larger packages
    /// being cut down to a smaller one).
    bool unbonded() const { return bondedPins == 0; }
};

/// Derives the *real* peripheral tree of the attached part.
///
/// Two independent sources are combined:
///   * **RCC clock-gate bits** from the SVD (`RCC_AHB1PCLKEN` etc.) tell whether
///     firmware actually clocked the peripheral;
///   * **a single read** of one register per peripheral tells whether the bus
///     decodes that address at all.
///
/// The combination resolves the ambiguity a single read cannot: a bus error means
/// "absent" only when the clock is known to be on (a clock-gated peripheral also
/// faults on some parts). Everything else is reported as unreachable rather than
/// silently dropped.
class DeviceTreeScanner
{
public:
    /// Where a peripheral's clock gate lives.
    struct ClockBit
    {
        QString registerName;
        quint32 address = 0;
        int bit = -1;
        bool valid() const { return bit >= 0; }
    };

    /// Map peripheral name -> clock gate, built from the SVD's RCC peripheral.
    static QHash<QString, ClockBit> clockMap(const SvdDevice &device);

    /// Register chosen as the presence probe.
    ///
    /// Prefers identifiers/control registers and avoids names that typically have
    /// read-side effects (clear/flag/data registers), because the SVD gives us no
    /// `modifiedWriteValues` / `readAction` information to rely on.
    static const SvdRegister *presenceProbeRegister(const SvdPeripheral &peripheral);

    /// Pure classification from an already-performed read; testable without a probe.
    static PeripheralStatus classify(const SvdPeripheral &peripheral, const ClockBit &clock,
                                     bool gateOn, bool readSucceeded, quint32 value,
                                     const QString &error);

    /// Number of peripherals whose clock state the SVD lets us determine.
    static int clockResolvableCount(const SvdDevice &device);

    /// Walk every peripheral through \a probe (blocking: call from a worker thread).
    /// \a progress is invoked as (done, total, name) so the caller can log.
    ///
    /// Read-only: nothing on the target is written. Which instances this part
    /// actually implements comes from static vendor data instead (see
    /// PeripheralPresence), because proving absence by reading would require
    /// enabling the peripheral's clock, and that can disturb a running bus.
    static QVector<PeripheralStatus> scan(IProbe &probe, const SvdDevice &device,
                                          const std::function<void(int, int, const QString &)> &progress = {});
};

} // namespace mrd

Q_DECLARE_METATYPE(mrd::PeripheralStatus)
Q_DECLARE_METATYPE(QVector<mrd::PeripheralStatus>)
