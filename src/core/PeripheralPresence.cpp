#include "core/PeripheralPresence.h"

#include <QMap>

#include <algorithm>

namespace mrd {

namespace {

/// "SPI12" -> ("SPI", 12). Trailing digits only; "SPI" alone yields index -1.
bool splitInstance(const QString &name, QString *prefix, int *index)
{
    int cut = name.size();
    while (cut > 0 && name.at(cut - 1).isDigit())
        --cut;
    if (cut == name.size() || cut == 0)
        return false; // no numeric tail / no name part
    bool ok = false;
    const int value = name.mid(cut).toInt(&ok);
    if (!ok)
        return false;
    if (prefix)
        *prefix = name.left(cut);
    if (index)
        *index = value;
    return true;
}

} // namespace

QVector<PeripheralPresence::SignalRule> PeripheralPresence::signalRules()
{
    // Mandatory signals per class. Timers/analog blocks have no mandatory pin, so
    // they are capped by count alone when a limit is known.
    return {
        {QStringLiteral("SPI"), {QStringLiteral("NSS"), QStringLiteral("SCK"),
                                 QStringLiteral("MISO"), QStringLiteral("MOSI")},
         false},
        {QStringLiteral("I2C"), {QStringLiteral("SCL"), QStringLiteral("SDA")}, false},
        // N32H47x datasheet note 5: USART3 / UART5 / UART8 TX+RX are mappable to
        // any IO, and note 6 says the same for FDCAN1 / FDCAN2. A missing fixed
        // pin therefore says nothing about those blocks.
        {QStringLiteral("USART"), {QStringLiteral("TX"), QStringLiteral("RX")}, true},
        {QStringLiteral("UART"), {QStringLiteral("TX"), QStringLiteral("RX")}, true},
        {QStringLiteral("FDCAN"), {QStringLiteral("TX"), QStringLiteral("RX")}, true},
    };
}

void PeripheralPresence::splitInstanceName(const QString &name, QString *classPrefix, int *index)
{
    if (!splitInstance(name, classPrefix, index)) {
        if (classPrefix)
            classPrefix->clear();
        if (index)
            *index = -1;
    }
}

PeripheralPresence::Result PeripheralPresence::analyse(const SvdDevice &device, const Inputs &inputs)
{
    Result result;
    if (inputs.limits.isEmpty())
        return result; // no vendor data -> hide nothing

    const QVector<SignalRule> rules = signalRules();

    // Group the SVD's instances per class, so the count can be compared.
    QMap<QString, QVector<QPair<int, QString>>> byClass; // class -> [(index, name)]
    for (const auto &peripheral : device.peripherals) {
        QString prefix;
        int index = -1;
        if (!splitInstance(peripheral.name, &prefix, &index))
            continue; // RCC, USB_FS, xSPI ... not an indexed instance
        if (!inputs.limits.contains(prefix))
            continue; // this class has no vendor count -> keep
        byClass[prefix].append({index, peripheral.name});
    }

    for (auto it = byClass.begin(); it != byClass.end(); ++it) {
        const QString &cls = it.key();
        auto &instances = it.value();
        std::sort(instances.begin(), instances.end());

        const int limit = inputs.limits.value(cls, -1);
        if (limit == 0) {
            // Vendor says the part has none of this class ("FEMC: -").
            for (const auto &entry : instances)
                result.absent.insert(entry.second, QStringLiteral("本型号没有 %1").arg(cls));
            continue;
        }
        if (limit < 0)
            continue; // unknown count -> hide nothing
        if (instances.size() <= limit)
            continue; // SVD does not over-declare this class

        const SignalRule *rule = nullptr;
        for (const auto &candidate : rules) {
            if (candidate.classPrefix.compare(cls, Qt::CaseInsensitive) == 0) {
                rule = &candidate;
                break;
            }
        }

        // Keep the instances that the package can actually wire up.
        //
        // Without a pin table for this part we know nothing about bonding, so the
        // pin test must be skipped entirely -- otherwise every instance would look
        // "unbonded" and we would hide the whole class. Only the vendor count is
        // authoritative then.
        const bool havePinData = !inputs.bondedSignals.isEmpty();

        QStringList usable;
        QStringList partial;
        for (const auto &entry : instances) {
            const QString &name = entry.second;
            if (!rule || !havePinData) {
                usable << name; // count-only class, or no pin data available
                continue;
            }
            const QSet<QString> bonded = inputs.bondedSignals.value(name);
            if (bonded.isEmpty()) {
                // Nothing bonded. Fine only when the class allows any-pin mapping.
                if (rule->anyPinMappable)
                    usable << name;
                else
                    partial << QStringLiteral("%1（本封装未引出）").arg(name);
                continue;
            }
            int have = 0;
            QStringList missing;
            for (const QString &signal : rule->requiredSignals) {
                if (bonded.contains(signal))
                    ++have;
                else
                    missing << signal;
            }
            if (have == rule->requiredSignals.size())
                usable << name;
            else if (have == 0 && rule->anyPinMappable)
                usable << name;
            else
                partial << QStringLiteral("%1（本封装缺 %2）")
                               .arg(name, missing.join(QLatin1Char('/')));
        }

        // Without a pin table we cannot tell which instance is the extra one.
        // Hiding a usable peripheral would be worse than listing one too many, so
        // record the discrepancy and keep everything.
        if (!havePinData && usable.size() > limit) {
            result.undecidable << QStringLiteral("%1: SVD 有 %2 个实例，本型号只有 %3 个；"
                                                 "缺少封装引脚表，无法确定该去掉哪一个")
                                      .arg(cls)
                                      .arg(instances.size())
                                      .arg(limit);
            continue;
        }

        // Count-based trim: drop the highest indices until the vendor count fits.
        QStringList dropped;
        while (usable.size() > limit && !usable.isEmpty())
            dropped << usable.takeLast();

        // Anything not kept is absent, with the reason.
        for (const auto &entry : instances) {
            const QString &name = entry.second;
            if (usable.contains(name))
                continue;
            QString reason;
            for (const QString &text : partial) {
                if (text.startsWith(name)) {
                    reason = text;
                    break;
                }
            }
            if (reason.isEmpty()) {
                reason = QStringLiteral("本型号只有 %1 个 %2，%3 是多出的实例")
                             .arg(limit)
                             .arg(cls, name);
            }
            result.absent.insert(name, reason);
        }
    }
    return result;
}

QHash<QString, QString> PeripheralPresence::absent(const SvdDevice &device, const Inputs &inputs)
{
    return analyse(device, inputs).absent;
}

} // namespace mrd
