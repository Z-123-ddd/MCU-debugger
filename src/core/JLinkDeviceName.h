#pragma once

#include <QString>
#include <QStringList>

namespace mrd {

/// Maps an order code to the device name SEGGER's tools accept.
///
/// Why this exists: J-Flash and JLinkGDBServer *do* support these parts -- the
/// names live inside `JLinkARM.dll` -- but they are spelled without the trailing
/// package/temperature suffix. Measured against JLink V8.26:
///
///   -device N32G455CC     -> works
///   -device N32G455CCL7   -> "Failed to get index for device name"
///
/// So `N32G455CCL7` (order code) must become `N32G455CC` before it is handed to
/// J-Link. That is the whole reason J-Flash "supports G455" while this tool kept
/// falling back to a generic Cortex-M4.
///
/// The rule is derived from the catalogue, not hard-coded per part: SEGGER keeps
/// `<family><package letter><flash letter>` and drops the rest (`L7`/`Q7`/`S7`
/// suffix = package finish + temperature range).
class JLinkDeviceName
{
public:
    /// Candidate J-Link device names for \a orderCode, best first.
    ///
    /// Usually one entry. More when the name is ambiguous, so the caller can try
    /// them in order; empty when nothing plausible can be derived.
    static QStringList candidates(const QString &orderCode);

    /// True when \a jlinkName looks like a valid SEGGER device name for this
    /// family (i.e. it was accepted by the resolver above).
    static bool isValidFor(const QString &jlinkName, const QString &orderCode);

    /// Names known to exist in JLinkARM.dll for \a family ("N32G455"), as measured.
    ///
    /// Empty for families SEGGER does not ship; the caller then keeps the generic
    /// core instead of pretending.
    static QStringList knownNames(const QString &family);
};

} // namespace mrd
