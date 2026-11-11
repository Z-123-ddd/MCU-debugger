#include "core/MemoryProbe.h"

#include "core/HexUtil.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include "core/ResourcePaths.h"

#include "core/IProbe.h"
#include "core/Logger.h"

namespace mrd {

namespace {

constexpr quint32 kFlashBase = 0x08000000u;
constexpr quint32 kRamBase = 0x20000000u;

/// Largest candidate whose last word is readable.
///
/// Reading the *last word* rather than the first is what makes this work: every
/// part can read its own base address, only the real size has its top word.
int probeTop(IProbe &probe, quint32 base, const QVector<int> &candidates, const char *label)
{
    for (int size : candidates) {
        quint32 value = 0;
        const quint32 address = base + static_cast<quint32>(size) - 4u;
        if (!probe.readU32(address, &value))
            continue;
        // A plain "readable" is not proof of the size:
        //   0xFFFFFFFF -- erased flash reads this, and so does an unmapped window;
        //   0x00000000 -- a reserved/aliased region reads this.
        // Measured on N32H473CC: 0x0807FFFC (the 512K boundary of a 512K-max die)
        // returns 0xFFFFFFFF even though the part has 256K, and 0x2002FFFC (192K)
        // returns 0 although its RAM is 144K. Treating those as "present" picked
        // 512K/192K for a 256K/144K part. Both patterns are therefore inconclusive
        // and the search continues downward.
        if (value == 0xFFFFFFFFu || value == 0x00000000u) {
            Logger::instance()->debug(
                QStringLiteral("%1 探测: %2 = 0x%3（无法作为依据，继续向下找）")
                    .arg(QString::fromLatin1(label),
                         QStringLiteral("0x%1").arg(address, 8, 16, QLatin1Char('0')),
                         QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'))));
            continue;
        }
        Logger::instance()->debug(QStringLiteral("%1 探测: %2 可读 -> %3 KB")
                                     .arg(QString::fromLatin1(label),
                                          QStringLiteral("0x%1").arg(address, 8, 16, QLatin1Char('0')))
                                     .arg(size / 1024));
        return size;
    }
    Logger::instance()->debug(
        QStringLiteral("%1 探测: 所有候选边界都读不到，内存大小未知").arg(QString::fromLatin1(label)));
    return 0;
}

} // namespace

QVector<int> MemoryProbe::flashCandidates()
{
    // Every flash size present in data/parts.json, largest first.
    return {2048 * 1024, 1024 * 1024, 512 * 1024, 384 * 1024, 256 * 1024,
            128 * 1024,  64 * 1024,   32 * 1024,  16 * 1024};
}

QVector<int> MemoryProbe::ramCandidates()
{
    // N32 parts top out at 512K (H7) and go down to 8K.
    return {512 * 1024, 448 * 1024, 384 * 1024, 256 * 1024, 192 * 1024, 160 * 1024,
            144 * 1024, 128 * 1024, 112 * 1024, 80 * 1024,  64 * 1024,  48 * 1024,
            32 * 1024,  16 * 1024,  8 * 1024};
}

MemoryProbe::Result MemoryProbe::detect(IProbe &probe)
{
    Result result;
    if (!probe.isConnected()) {
        result.note = QStringLiteral("目标未连接");
        return result;
    }
    result.flashBytes = probeTop(probe, kFlashBase, flashCandidates(), "Flash");
    result.ramBytes = probeTop(probe, kRamBase, ramCandidates(), "RAM");
    result.note = describe(result);
    Logger::instance()->debug(QStringLiteral("内存探测: %1").arg(result.note));
    return result;
}

QString MemoryProbe::describe(const Result &result)
{
    if (!result.valid())
        return QStringLiteral("内存大小未知");
    QStringList parts;
    if (result.flashBytes > 0)
        parts << QStringLiteral("Flash %1 KB").arg(result.flashBytes / 1024);
    if (result.ramBytes > 0)
        parts << QStringLiteral("RAM %1 KB").arg(result.ramBytes / 1024);
    return parts.join(QStringLiteral(" / "));
}

bool MemoryProbe::matches(const Result &probe, int flashBytes, int ramBytes)
{
    // Flash first: it is the reliable half. Erased flash reads 0xFFFFFFFF, which
    // detect() already rejects, so a measured flash size is trustworthy.
    if (probe.flashBytes > 0 && flashBytes > 0 && probe.flashBytes != flashBytes)
        return false;

    // RAM is only a tie-breaker. Measured on real silicon, the byte right past the
    // real end can still read back data (an alias or a neighbouring block), so a
    // mismatch here is weaker evidence than a flash mismatch. An entry with no RAM
    // data at all cannot be judged.
    if (probe.ramBytes > 0 && ramBytes > 0 && probe.ramBytes != ramBytes)
        return false;
    return true;
}

MemoryProbe::GpioMap MemoryProbe::gpioMap()
{
    static GpioMap map;
    static bool loaded = false;
    if (loaded)
        return map;
    loaded = true;

    const QString path = ResourcePaths::find(QStringLiteral("svd_gpio.json"));
    if (path.isEmpty())
        return map;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return map;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();

    const QJsonObject bySvd = root.value(QStringLiteral("svd")).toObject();
    for (auto it = bySvd.constBegin(); it != bySvd.constEnd(); ++it) {
        const QString baseText = it.value().toObject().value(QStringLiteral("gpioBase")).toString();
        quint32 base = 0;
        if (!parseHex(baseText, &base))
            continue;
        map.baseBySvd.insert(it.key(), base);
    }
    const QJsonObject byBase = root.value(QStringLiteral("byBase")).toObject();
    for (auto it = byBase.constBegin(); it != byBase.constEnd(); ++it) {
        quint32 base = 0;
        if (!parseHex(it.key(), &base))
            continue;
        QStringList names;
        for (const auto &value : it.value().toArray())
            names << value.toString();
        names.sort();
        map.svdsAtBase.insert(base, names);
    }
    Logger::instance()->debug(
        QStringLiteral("SVD GPIO 基地址表: %1 个地址").arg(map.svdsAtBase.size()));
    return map;
}

bool MemoryProbe::gpioBlockPresent(IProbe &probe, quint32 base)
{
    // "First register is non-zero" is not enough: measured on an N32H473,
    // 0x40010800 (where the G-series puts GPIOA) reads 0x02008000 from an
    // unrelated peripheral, so a single word proves nothing.
    //
    // What does hold: every N32 GPIO block carries a read-only port-ID register,
    // at +0x08 on the G-series layout and +0x2C on the H-series one, and it is
    // never zero on silicon. A wrong address has zeros in both places (verified on
    // the real board: 0x40010800 -> +0x08=0, +0x2C=0; 0x4002C000 -> +0x2C=0xD000).
    const quint32 idOffsets[] = {0x08u, 0x2Cu};
    for (const quint32 offset : idOffsets) {
        quint32 value = 0;
        if (probe.readU32(base + offset, &value) && value != 0)
            return true;
    }
    return false;
}

bool MemoryProbe::flashMatches(const Result &probe, int flashBytes)
{
    if (probe.flashBytes <= 0 || flashBytes <= 0)
        return true; // nothing to compare
    return probe.flashBytes == flashBytes;
}

} // namespace mrd
