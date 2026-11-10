#include "core/DeviceIdentifier.h"

#include "core/HexUtil.h"
#include "core/IProbe.h"
#include "core/Logger.h"
#include "core/ResourcePaths.h"
#include "core/SvdLibrary.h"

#include <QFile>
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

    const QJsonArray devices = root.value(QStringLiteral("devices")).toArray();
    for (const auto &value : devices) {
        const QJsonObject o = value.toObject();
        DeviceIdEntry entry;
        entry.mcu = o.value(QStringLiteral("mcu")).toString();
        entry.family = o.value(QStringLiteral("family")).toString();
        entry.vendor = o.value(QStringLiteral("vendor")).toString();
        entry.svdHint = o.value(QStringLiteral("svdHint")).toString();
        entry.probeRsChip = o.value(QStringLiteral("probeRsChip")).toString();
        if (entry.probeRsChip.isEmpty())
            entry.probeRsChip = o.value(QStringLiteral("jlinkDevice")).toString(); // legacy key
        entry.devId = parseHexOrDec(o.value(QStringLiteral("devId")));
        entry.ioCount = o.value(QStringLiteral("ioCount")).toInt();
        entry.pinCount = o.value(QStringLiteral("pinCount")).toInt();
        entry.packageName = o.value(QStringLiteral("packageName")).toString();
        entry.verified = o.value(QStringLiteral("verified")).toBool(true);
        const quint32 idcodeAddress = parseHexOrDec(o.value(QStringLiteral("idcodeAddress")));
        if (idcodeAddress != 0)
            entry.idcodeAddress = idcodeAddress;
        if (entry.mcu.isEmpty())
            continue;
        if (entry.probeRsChip.isEmpty())
            entry.probeRsChip = entry.mcu;
        g_database << entry;
    }

    Logger::instance()->info(QStringLiteral("器件识别库: %1 条 (%2)").arg(g_database.size()).arg(g_databasePath));
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
                                                       const QString &overrideDevice, const QString &probeSerial)
{
    IdentificationResult result;
    result.probeSerial = probeSerial;

    if (!probe.isConnected())
        return Result<IdentificationResult>::failure(QStringLiteral("目标未连接，无法识别 MCU"));

    // 1. core identification
    quint32 cpuid = 0;
    if (const Status st = probe.readU32(0xE000ED00u, &cpuid); st) {
        result.cpuid = cpuid;
        result.coreName = coreNameFromCpuid(cpuid);
        Logger::instance()->info(QStringLiteral("CPUID = %1 (%2)")
                                     .arg(hex32(cpuid), result.coreName));
    } else {
        Logger::instance()->warn(QStringLiteral("读取 CPUID 失败: %1").arg(st.error));
    }

    DeviceIdEntry matchedEntry;
    bool haveEntry = false;

    const QString override = overrideDevice.trimmed();
    if (!override.isEmpty()) {
        result.manuallySelected = true;
        result.mcuName = override;
        result.probeRsChip = override;
        for (const auto &entry : g_database) {
            if (entry.mcu.compare(override, Qt::CaseInsensitive) == 0
                || entry.probeRsChip.compare(override, Qt::CaseInsensitive) == 0) {
                matchedEntry = entry;
                haveEntry = true;
                result.mcuName = entry.mcu;
                result.family = entry.family;
                result.vendor = entry.vendor;
                result.probeRsChip = entry.probeRsChip;
                break;
            }
        }
        result.note = QStringLiteral("手动指定芯片名");
    } else {
        // 2. probe the known DBGMCU_IDCODE addresses
        for (const quint32 address : g_idcodeAddresses) {
            quint32 value = 0;
            if (const Status st = probe.readU32(address, &value); !st)
                continue;
            if (!plausibleIdCode(value))
                continue;

            const quint32 devId = value & 0x00000FFFu;
            const quint32 revId = (value >> 16) & 0x0000FFFFu;
            Logger::instance()->info(QStringLiteral("IDCODE @%1 = %2 -> DEV_ID=0x%3 REV_ID=0x%4")
                                         .arg(hexAddr(address), hex32(value))
                                         .arg(devId, 3, 16, QLatin1Char('0'))
                                         .arg(revId, 4, 16, QLatin1Char('0')));
            result.devId = devId;
            result.revId = revId;

            const QVector<DeviceIdEntry> entries = lookupByDevId(devId);
            if (!entries.isEmpty()) {
                matchedEntry = entries.first();
                haveEntry = true;
                break;
            }
            result.note = QStringLiteral("DEV_ID 0x%1 不在识别库中，请手动选择器件")
                              .arg(devId, 3, 16, QLatin1Char('0'));
            break;
        }
    }

    if (haveEntry) {
        if (result.mcuName.isEmpty())
            result.mcuName = matchedEntry.mcu;
        if (result.family.isEmpty())
            result.family = matchedEntry.family;
        if (result.vendor.isEmpty())
            result.vendor = matchedEntry.vendor;
        if (result.probeRsChip.isEmpty())
            result.probeRsChip = matchedEntry.probeRsChip;
        result.ioCount = matchedEntry.ioCount;
        result.pinCount = matchedEntry.pinCount;
        result.packageName = matchedEntry.packageName;
    }

    // 3. SVD matching - prefer the hint from the table, then the MCU name.
    QStringList hints;
    if (haveEntry && !matchedEntry.svdHint.isEmpty())
        hints << matchedEntry.svdHint;
    if (!result.mcuName.isEmpty())
        hints << result.mcuName;
    if (haveEntry && !matchedEntry.family.isEmpty())
        hints << matchedEntry.family;

    for (const QString &hint : hints) {
        const QVector<SvdEntry> candidates = library.candidatesFor(hint);
        for (const auto &candidate : candidates) {
            if (!result.candidateSvdPaths.contains(candidate.path))
                result.candidateSvdPaths << candidate.path;
        }
        if (result.svdPath.isEmpty() && !candidates.isEmpty())
            result.svdPath = candidates.first().path;
    }

    if (!haveEntry && result.mcuName.isEmpty()) {
        return Result<IdentificationResult>::failure(
            result.note.isEmpty()
                ? QStringLiteral("无法识别 MCU 型号（DEV_ID/CPUID 均无匹配）")
                : result.note);
    }

    result.ok = true;
    if (result.svdPath.isEmpty())
        result.note = QStringLiteral("%1：未在 SVD 库中找到匹配文件，请手动指定")
                          .arg(result.note.isEmpty() ? QStringLiteral("提示") : result.note);

    return Result<IdentificationResult>::success(result);
}

} // namespace mrd
