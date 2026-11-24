#include "core/DeviceIdentifier.h"

#include "core/DeviceTreeScanner.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/IProbe.h"
#include "core/JLinkDeviceName.h"
#include "core/Logger.h"
#include "core/PartTable.h"
#include "core/ResourcePaths.h"
#include "core/SvdLibrary.h"
#include "core/SvdParser.h"
#include "ui/PackageView.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mrd {

namespace {

QVector<DeviceIdEntry> g_database;
QString g_databasePath;
QVector<quint32> g_idcodeAddresses = {0xE0042000u, 0x5C001000u, 0x40015800u};

quint32 parseHexOrDec(const QJsonValue &value)
{
    if (value.isDouble())
        return static_cast<quint32>(value.toDouble());
    quint32 parsed = 0;
    if (parseHex(value.toString(), &parsed))
        return parsed;
    return 0;
}

/// Parse one identification row and append it, skipping duplicates.
///
/// A row is considered present when both MCU name and DEV_ID match, so the same
/// DEV_ID under a different part (N32H473CE vs N32H473KE) still loads.
void appendEntry(const QJsonObject &o, QVector<DeviceIdEntry> *database)
{
    DeviceIdEntry entry;
    entry.mcu = o.value(QStringLiteral("mcu")).toString();
    entry.family = o.value(QStringLiteral("family")).toString();
    entry.vendor = o.value(QStringLiteral("vendor")).toString();
    entry.svdHint = o.value(QStringLiteral("svdHint")).toString();
    // Kept as a human-readable alias used when matching what the user typed. It is
    // NOT a J-Link device name: handing it to the DLL crashes V8.26.
    entry.probeRsChip = o.value(QStringLiteral("probeRsChip")).toString();
    if (entry.probeRsChip.isEmpty())
        entry.probeRsChip = o.value(QStringLiteral("jlinkDevice")).toString(); // legacy key
    entry.devId = parseHexOrDec(o.value(QStringLiteral("devId")));
    entry.ioCount = o.value(QStringLiteral("ioCount")).toInt();
    entry.pinCount = o.value(QStringLiteral("pinCount")).toInt();
    entry.packageName = o.value(QStringLiteral("packageName")).toString();
    entry.verified = o.value(QStringLiteral("verified")).toBool(true);
    // Per-model peripheral counts ("SPI": 5). Absent for parts the vendor pack
    // does not describe individually -> no trimming, show the SVD as-is.
    const QJsonObject caps = o.value(QStringLiteral("capabilities")).toObject();
    for (auto it = caps.constBegin(); it != caps.constEnd(); ++it) {
        if (it.value().isDouble())
            entry.capabilities.insert(it.key(), it.value().toInt());
    }
    const quint32 idcodeAddress = parseHexOrDec(o.value(QStringLiteral("idcodeAddress")));
    if (idcodeAddress != 0)
        entry.idcodeAddress = idcodeAddress;
    if (entry.mcu.isEmpty())
        return;
    if (entry.probeRsChip.isEmpty())
        entry.probeRsChip = entry.mcu;
    for (const auto &existing : *database) {
        if (existing.devId == entry.devId
            && existing.mcu.compare(entry.mcu, Qt::CaseInsensitive) == 0)
            return;
    }
    database->append(entry);
}

/// DEV_ID/REV_ID candidates: 0x00000000 and 0xFFFFFFFF mean "nothing there".
bool plausibleIdCode(quint32 value)
{
    return value != 0u && value != 0xFFFFFFFFu && (value & 0x00000FFFu) != 0u;
}

} // namespace

QString DeviceIdentifier::databasePath()
{
    return g_databasePath;
}

namespace {

/// Load a package table by its series group and package name.
PackageDescription loadPackageTable(const QString &chip, const QString &packageName)
{
    const QString dir = ResourcePaths::packageDirectory();
    if (dir.isEmpty())
        return {};
    const QDir folder(dir);
    for (const QString &name : folder.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const PackageDescription description =
            PackageDescription::loadFromFile(dir + QLatin1Char('/') + name);
        // compare() returns 0 when equal, so "!compare" was true *for a match* and
        // the loop returned the first table that did NOT match. Skipping on != 0.
        if (!description.isValid()
            || description.packageName.compare(packageName, Qt::CaseInsensitive) != 0)
            continue;
        // `chip` here is the part/family name ("N32G455"); package tables are keyed
        // by series group ("N32G45x"), which matchesPart handles via the wildcard.
        if (PackageView::matchesPart(description.chip, chip))
            return description;
    }
    return {};
}

} // namespace

bool DeviceIdentifier::loadDatabase(QString *error)
{
    g_database.clear();
    g_databasePath = ResourcePaths::deviceIdDatabase();
    if (g_databasePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("未找到 data/device_ids.json（搜索目录: %1）")
                         .arg(ResourcePaths::dataRoots().join(QStringLiteral(", ")));
        return false;
    }

    QFile file(g_databasePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开 %1: %2").arg(g_databasePath, file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        if (error)
            *error = QStringLiteral("%1 解析失败: %2").arg(g_databasePath, parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonArray addresses = root.value(QStringLiteral("idcodeAddresses")).toArray();
    if (!addresses.isEmpty()) {
        g_idcodeAddresses.clear();
        for (const auto &value : addresses) {
            const quint32 address = parseHexOrDec(value);
            if (address != 0 && !g_idcodeAddresses.contains(address))
                g_idcodeAddresses << address;
        }
    }

    for (const auto &value : root.value(QStringLiteral("devices")).toArray())
        appendEntry(value.toObject(), &g_database);

    // Supplementary tables (data/device_ids.d/*.json). The built-in table wins,
    // so a supplement can never silently redefine a measured entry.
    const QStringList supplements = ResourcePaths::deviceIdSupplements();
    for (const QString &path : supplements) {
        QFile extra(path);
        if (!extra.open(QIODevice::ReadOnly)) {
            Logger::instance()->warn(QStringLiteral("无法打开补充识别表 %1: %2")
                                         .arg(path, extra.errorString()));
            continue;
        }
        QJsonParseError extraError{};
        const QJsonDocument extraDoc = QJsonDocument::fromJson(extra.readAll(), &extraError);
        if (extraDoc.isNull()) {
            Logger::instance()->warn(QStringLiteral("补充识别表 %1 解析失败: %2")
                                         .arg(path, extraError.errorString()));
            continue;
        }
        const int before = g_database.size();
        for (const auto &value : extraDoc.object().value(QStringLiteral("devices")).toArray())
            appendEntry(value.toObject(), &g_database);
        Logger::instance()->info(QStringLiteral("补充识别表 %1: +%2 条")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(g_database.size() - before));
    }

    Logger::instance()->info(QStringLiteral("器件识别库: %1 条 (%2)")
                                 .arg(g_database.size())
                                 .arg(g_databasePath));
    return !g_database.isEmpty();
}

const QVector<DeviceIdEntry> &DeviceIdentifier::database()
{
    return g_database;
}

QVector<quint32> DeviceIdentifier::idcodeAddresses()
{
    return g_idcodeAddresses;
}

QVector<DeviceIdEntry> DeviceIdentifier::lookupByDevId(quint32 devId)
{
    QVector<DeviceIdEntry> hits;
    if (devId == 0)
        return hits;
    for (const auto &entry : g_database) {
        if (entry.devId == devId)
            hits << entry;
    }
    return hits;
}

QStringList DeviceIdentifier::matchingNames(const QString &prefix)
{
    QStringList names;
    for (const auto &entry : g_database) {
        if (prefix.isEmpty() || entry.mcu.startsWith(prefix, Qt::CaseInsensitive))
            names << entry.mcu;
    }
    names.removeDuplicates();
    names.sort();
    return names;
}

/// Register offsets that identify a GPIO port's existence.
///
/// The configuration register sits at 0x00 and the port-ID/data register around
/// 0x08 on both layouts, and neither is all-zero on a real port (the reset value
/// of the N32 config register is 0x00080800). Probing offsets that never carry a
/// meaningful zero keeps "all zeros" a reliable absence signal.
QVector<quint32> portProbeOffsets(const SvdPeripheral &peripheral)
{
    QVector<quint32> offsets;
    // Prefer the configuration register, then the id/data register.
    for (const char *suffix : {"PL_CFG", "PMODE", "CRL", "MODER"}) {
        for (const auto &reg : peripheral.registers) {
            if (reg.name.endsWith(QLatin1String(suffix))) {
                offsets << reg.addressOffset;
                break;
            }
        }
        if (!offsets.isEmpty())
            break;
    }
    for (const char *suffix : {"PID", "IDR", "POD", "ODR"}) {
        for (const auto &reg : peripheral.registers) {
            if (reg.name.endsWith(QLatin1String(suffix))) {
                const quint32 offset = reg.addressOffset;
                if (!offsets.contains(offset))
                    offsets << offset;
                break;
            }
        }
    }
    if (offsets.isEmpty()) {
        // Last resort: the first two registers of the block.
        for (int i = 0; i < peripheral.registers.size() && i < 2; ++i)
            offsets << peripheral.registers.at(i).addressOffset;
    }
    return offsets;
}

/// Offset of the read-only port-ID register, or `kNoPortId` when the SVD has none.
///
/// This is the one register that answers "does this port exist on the die?"
/// without guessing, because it is unique per port and reads exactly zero on a
/// port the die does not implement -- even with the port's clock forced on.
/// Measured on an N32H473 (48-pin): with the GPIO clock enabled, the missing
/// GPIOD..GPIOH read back 0xFFFFFFFF at +0x00 (the reset value of PMODE, i.e.
/// indistinguishable from a real port) but 0x00000000 at +0x2C, while the real
/// GPIOA/GPIOB/GPIOC report 0x0000C800 / 0x00007DD8 / 0x00004040.
constexpr quint32 kNoPortId = 0xFFFFFFFFu;

quint32 portIdOffset(const SvdPeripheral &peripheral)
{
    for (const auto &reg : peripheral.registers) {
        if (reg.name.compare(QStringLiteral("PID"), Qt::CaseInsensitive) == 0
            || reg.name.endsWith(QStringLiteral("_PID")))
            return reg.addressOffset;
    }
    return kNoPortId;
}

QString DeviceIdentifier::coreNameFromCpuid(quint32 cpuid)
{
    // CPUID layout: [31:24] implementer, [23:20] variant, [19:16] constant,
    // [15:4] part number.
    const quint32 part = (cpuid >> 4) & 0xFFFu;
    switch (part) {
    case 0xC20:
        return QStringLiteral("Cortex-M0");
    case 0xC21:
        return QStringLiteral("Cortex-M1");
    case 0xC23:
        return QStringLiteral("Cortex-M3");
    case 0xC24:
        return QStringLiteral("Cortex-M4");
    case 0xC27:
        return QStringLiteral("Cortex-M7");
    case 0xC60:
        return QStringLiteral("Cortex-M0+");
    case 0xD20:
        return QStringLiteral("Cortex-M23");
    case 0xD21:
        return QStringLiteral("Cortex-M33");
    case 0xD22:
        return QStringLiteral("Cortex-M55");
    case 0xD23:
        return QStringLiteral("Cortex-M85");
    default:
        break;
    }
    return QStringLiteral("未知核 (CPUID=0x%1)").arg(QString::number(cpuid, 16).toUpper());
}

Result<IdentificationResult> DeviceIdentifier::identify(IProbe &probe, SvdLibrary &library,
                                                      const QString &overrideDevice,
                                                      const QString &probeSerial)
{
    IdentificationResult result;
    result.probeSerial = probeSerial;

    if (!probe.isConnected())
        return Result<IdentificationResult>::failure(QStringLiteral("目标未连接，无法识别 MCU"));

    // --- 1. core identification ------------------------------------------
    // The only part that has to be automatic: CPUID tells us the core, which is what
    // makes register and memory access possible at all. Everything above it (which exact
    // part, which package) cannot be read from silicon reliably.
    quint32 cpuid = 0;
    if (const Status st = probe.readU32(0xE000ED00u, &cpuid); st) {
        result.cpuid = cpuid;
        result.coreName = coreNameFromCpuid(cpuid);
        Logger::instance()->debug(QStringLiteral("CPUID = %1 (%2)")
                                      .arg(hex32(cpuid), result.coreName));
    } else {
        Logger::instance()->warn(QStringLiteral("读取 CPUID 失败: %1").arg(st.error));
    }

    // --- 2. no part inference ---------------------------------------------
    // Nothing is read to guess the part. One DEV_ID covers every package and flash size
    // of a family, and the memory probe that used to break that tie cannot tell blank
    // flash from an unmapped window (both answer 0xFF) -- which is how a 512 KB board
    // ended up reported as 256 KB with the wrong pinout. DEV_ID is no longer read at all:
    // the order code is the single source, and validatePeripherals() on connect is what
    // checks that the pick matches the silicon.

    // --- 3. the order code decides everything else ------------------------
    // Only an exact order code from data/parts.json is accepted. A bare series
    // ("N32G455") is not: it spans four packages, and guessing one is how the pinout
    // ends up describing a package the board does not use.
    const QString chosen = overrideDevice.trimmed();
    if (!chosen.isEmpty() && PartTable::isOrderCode(chosen)) {
        result.manuallySelected = true;
        result.partNumber = chosen;
        result.mcuName = chosen;
        result.series = PartTable::seriesOf(chosen);

        const PartTable::Entry part = PartTable::lookup(chosen);
        result.flashBytes = part.flashBytes;
        result.ramBytes = part.ramBytes;
        result.flashText = part.flash;
        result.sramText = part.sram;
        result.ccmText = part.ccmSram;
        result.packageName = part.package;
        result.ioCount = part.ioCount;
        result.capabilities = part.capabilities;
        result.family = part.pack;
        result.note = QStringLiteral("按所选料号 %1").arg(chosen);

        // The SVD is whatever the vendor manifest assigned to this order code.
        library.loadManifest();
        SvdManifestEntry manifestEntry;
        QStringList ambiguous;
        if (library.lookupPart(chosen, &manifestEntry, &ambiguous)) {
            result.svdPath = manifestEntry.svdPath;
            if (manifestEntry.pinCount > 0)
                result.pinCount = manifestEntry.pinCount;
            if (manifestEntry.ioCount > 0)
                result.ioCount = manifestEntry.ioCount;
        } else {
            // Not in the manifest: every order code of one series shares one SVD, so any
            // sibling resolves it. This is not a guess about the package -- the part and
            // its package already came from parts.json; only the file name is borrowed.
            for (const QString &sibling : PartTable::partsInSeries(result.series)) {
                if (library.lookupPart(sibling, &manifestEntry, &ambiguous)) {
                    result.svdPath = manifestEntry.svdPath;
                    Logger::instance()->debug(
                        QStringLiteral("%1 不在厂商清单中，SVD 借用同系列 %2 的 %3")
                            .arg(chosen, sibling, QFileInfo(manifestEntry.svdPath).fileName()));
                    break;
                }
            }
        }
    } else if (!chosen.isEmpty()) {
        // A series or an unknown string: remember it as the hint, but do not pretend it
        // identifies a package.
        result.series = PartTable::seriesOf(chosen).isEmpty() ? chosen
                                                             : PartTable::seriesOf(chosen);
        result.note = QStringLiteral("已选系列 %1，请选择具体料号").arg(result.series);
    }

    // --- 4. the name J-Link needs ----------------------------------------
    // SEGGER's catalogue keys on order code minus the temperature/package-finish suffix,
    // and handing it a name it does not know crashes V8.26 rather than failing softly.
    if (!result.partNumber.isEmpty()) {
        QStringList names;
        for (const QString &candidate : JLinkDeviceName::candidates(result.partNumber)) {
            if (JLinkDeviceName::isKnown(candidate) || !JLinkDeviceName::hasCatalogue())
                names << candidate;
        }
        if (names.isEmpty() && JLinkDeviceName::hasCatalogue()) {
            Logger::instance()->debug(
                QStringLiteral("J-Link 目录中没有 %1 的条目，保持通用内核")
                    .arg(result.partNumber));
        }
        if (!names.isEmpty()) {
            result.jlinkDeviceName = names.first();
            result.jlinkDeviceCandidates = names;
        }
    }

    result.ok = true;

    // --- 5. one summary line ---------------------------------------------
    {
        QStringList summary;
        summary << QStringLiteral("内核 %1").arg(result.coreName.isEmpty()
                                                    ? QStringLiteral("未知")
                                                    : result.coreName);
        if (result.devId != 0)
            summary << QStringLiteral("DEV_ID 0x%1").arg(result.devId, 3, 16, QLatin1Char('0'));
        if (!result.partNumber.isEmpty()) {
            const QString memory = PartTable::memorySummary(result.flashBytes, result.ramBytes);
            summary << QStringLiteral("料号 %1").arg(result.partNumber);
            if (!memory.isEmpty())
                summary << memory;
            if (!result.packageName.isEmpty())
                summary << result.packageName;
            if (!result.svdPath.isEmpty())
                summary << QFileInfo(result.svdPath).fileName();
        } else if (!result.series.isEmpty()) {
            summary << QStringLiteral("系列 %1（未选料号）").arg(result.series);
        }
        Logger::instance()->info(QStringLiteral("已连接: %1").arg(summary.join(QStringLiteral("  |  "))));
    }

    return Result<IdentificationResult>::success(result);
}

} // namespace mrd
