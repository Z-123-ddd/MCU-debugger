#pragma once

#include "core/SvdTypes.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace mrd {

/// Decides which peripheral *instances* a specific part really implements.
///
/// The SVD describes the die/family, so it lists blocks that a given model does
/// not have: `N32H473.svd` declares SPI1..SPI6 while the 48-pin N32H473CE only
/// offers five SPIs. Two pieces of static vendor data settle which is which,
/// without touching the target at all:
///
///  1. **Capability counts** from the CMSIS-Pack `.pdsc` `<description>` block,
///     which is written per exact part (`Dname="N32H473CE"`): `SPI/I2S:5/2`,
///     `FDCAN:2`, `I2C:4`, ... This is the authoritative "how many".
///  2. **Package pin signals** from `data/packages/*.json`: an instance whose
///     mandatory signals are not bonded cannot be used on this package.
///
/// Proving absence by *reading* the peripheral is not an option: a gated
/// peripheral cannot answer on the bus, and opening its clock could disturb a
/// running bus (a gated-but-configured timer would resume driving its pins, and
/// enabling DMA1/DMA2 could restart a stalled transfer).
class PeripheralPresence
{
public:
    /// Mandatory signals per peripheral class, used for the pin-based check.
    struct SignalRule
    {
        QString classPrefix;              ///< "SPI", "I2C", "USART", "UART", "FDCAN"
        QStringList requiredSignals;      ///< e.g. {"NSS","SCK","MISO","MOSI"}
        /// True when the datasheet says TX/RX can be mapped to *any* IO pin, so a
        /// missing fixed pin proves nothing (N32H47x notes 5 and 6).
        bool anyPinMappable = false;
    };

    struct Inputs
    {
        /// Peripheral class -> how many instances this part has (0/absent = unknown).
        QHash<QString, int> limits;
        /// Instance name -> signals bonded out by this package ("SPI1" -> {NSS,...}).
        ///
        /// Empty when no pin table exists for this part; the pin test is then
        /// skipped and only the vendor counts decide (never hide a whole class
        /// just because the package description is missing).
        QHash<QString, QSet<QString>> bondedSignals;
    };

    struct Result
    {
        /// Instances this part does not have -> reason text.
        QHash<QString, QString> absent;
        /// Classes where the SVD declares more instances than the vendor pack, but
        /// the package has no pin table so we cannot tell *which* one to drop.
        /// Nothing is hidden in that case; these lines are for the log/UI.
        QStringList undecidable;
    };

    /// Decide which instances this part does not have.
    ///
    /// Nothing is ever hidden on a guess:
    ///  * with a package pin table, an instance missing a mandatory signal is
    ///    absent (SPI5 has no MISO on the 48-pin part) and the count trims any
    ///    remainder;
    ///  * without a pin table the counts alone cannot say *which* instance is
    ///    extra, and hiding a usable peripheral is worse than listing one too
    ///    many, so the class is left visible and reported via `undecidable`.
    static Result analyse(const SvdDevice &device, const Inputs &inputs);

    /// Convenience: just the hidden instances (`analyse().absent`).
    static QHash<QString, QString> absent(const SvdDevice &device, const Inputs &inputs);

    /// Classes covered by a signal rule.
    static QVector<SignalRule> signalRules();

    /// Split "SPI12" into ("SPI", 12); class empty for names without a numeric tail.
    static void splitInstanceName(const QString &name, QString *classPrefix, int *index);
};

} // namespace mrd
