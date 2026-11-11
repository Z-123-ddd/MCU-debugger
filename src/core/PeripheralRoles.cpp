#include "core/PeripheralRoles.h"

namespace mrd::PeripheralRoles {

bool isTimer(const SvdPeripheral &peripheral)
{
    // SVD naming across the N32 families: ATIM (advanced), BTIM (basic), GTIM and
    // plain TIM (general purpose), LPTIM (low power), SHRTIM (high resolution).
    static const QStringList kPrefixes = {
        QStringLiteral("ATIM"),  QStringLiteral("BTIM"), QStringLiteral("GTIM"),
        QStringLiteral("LPTIM"), QStringLiteral("SHRTIM"), QStringLiteral("TIM"),
    };
    for (const QString &prefix : kPrefixes) {
        if (peripheral.name.startsWith(prefix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

const SvdRegister *counterRegister(const SvdPeripheral &peripheral)
{
    for (const auto &reg : peripheral.registers) {
        const QString upper = reg.name.toUpper();
        if (!upper.endsWith(QLatin1String("CNT")))
            continue;
        if (upper.endsWith(QLatin1String("REPCNT")))
            continue; // repetition counter, not the running count
        return &reg;
    }
    return nullptr;
}

} // namespace mrd::PeripheralRoles
