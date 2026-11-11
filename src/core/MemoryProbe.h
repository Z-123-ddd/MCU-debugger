#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QStringList>

namespace mrd {

class IProbe;

/// Works out how much flash / RAM the attached part really has, by probing the
/// memory map instead of asking anyone.
///
/// Why: a DEV_ID identifies the die, not the variant. `N32G455` ships as 15 order
/// codes that differ only in package and memory size, and no vendor file maps
/// DEV_ID to those (the CMSIS-Pack has `<description>/<memory>/<debug>` only, and
/// the datasheets have no ID table). Reading the last word of a range answers it
/// directly: inside the real flash it reads back, beyond it the bus faults.
///
/// This is read-only. Nothing is written, no clock is touched, and "beyond the
/// end" is exactly the case a debugger already handles for a missing peripheral.
class MemoryProbe
{
public:
    struct Result
    {
        int flashBytes = 0; ///< 0 = could not determine
        int ramBytes = 0;
        QString note;
        bool valid() const { return flashBytes > 0 || ramBytes > 0; }
    };

    /// Candidate sizes are tried largest first, so the first readable boundary is
    /// the real one. The lists cover every N32 part in `data/parts.json`.
    static QVector<int> flashCandidates();
    static QVector<int> ramCandidates();

    static Result detect(IProbe &probe);

    /// "Flash 512 KB / RAM 144 KB", for logs and the UI.
    static QString describe(const Result &result);

    /// GPIO base address each SVD uses, from `data/svd_gpio.json`.
    ///
    /// Probing these addresses names the family without any DEV_ID: the N32 part
    /// families keep their ports at distinct bases (0x40010800 for G45x,
    /// 0x4002C000 for H47x, 0x58032800 for H7xx, ...).
    struct GpioMap
    {
        QHash<QString, quint32> baseBySvd;      ///< "N32H473.svd" -> 0x4002C000
        QHash<quint32, QStringList> svdsAtBase; ///< 0x4002C000 -> ["N32H473.svd", ...]
        bool valid() const { return !baseBySvd.isEmpty(); }
    };
    static GpioMap gpioMap();

    /// True when a GPIO block answers at \a base (non-zero, no fault).
    static bool gpioBlockPresent(IProbe &probe, quint32 base);

    /// Flash-only consistency: the reliable half of the signature.
    static bool flashMatches(const Result &probe, int flashBytes);

    /// True when \a entry (from data/parts.json) is consistent with the probe.
    ///
    /// A zero size in either side means "unknown" and never rules a part out, so
    /// a backend that cannot read memory still lets the user pick.
    static bool matches(const Result &probe, int flashBytes, int ramBytes);
};

} // namespace mrd
