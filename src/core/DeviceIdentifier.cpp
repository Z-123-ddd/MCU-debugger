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

bool DeviceIdentifier::rememberDevId(quint32 devId, const QString &partName, QString *error)
{
    if (devId == 0 || partName.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("DEV_ID 或型号为空");
        return false;
    }
    // A row we already have is authoritative; never override a measured entry
    // with a guess.
    for (const auto &entry : g_database) {
        if (entry.devId == devId && entry.mcu.compare(partName, Qt::CaseInsensitive) == 0)
            return true; // already known
        if (entry.devId == devId) {
            if (error)
                *error = QStringLiteral("DEV_ID 0x%1 已属于 %2")
                             .arg(devId, 3, 16, QLatin1Char('0'))
                             .arg(entry.mcu);
            return false;
        }
    }

    const QString path = ResourcePaths::learnedDeviceIdPath();
    QJsonObject root;
    QJsonArray devices;
    {
        QFile existing(path);
        if (existing.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(existing.readAll());
            root = doc.object();
            devices = root.value(QStringLiteral("devices")).toArray();
            existing.close();
        }
    }

    // Replace a previous learning for the same part instead of appending twice.
    QJsonArray kept;
    for (const auto &value : devices) {
        const QJsonObject o = value.toObject();
        if (o.value(QStringLiteral("mcu")).toString().compare(partName, Qt::CaseInsensitive) == 0)
            continue;
        kept.append(value);
    }

    QJsonObject row;
    row.insert(QStringLiteral("mcu"), partName);
    row.insert(QStringLiteral("devId"), QStringLiteral("0x%1").arg(devId, 3, 16, QLatin1Char('0')));
    // Learned by connecting real hardware, so it counts as measured.
    row.insert(QStringLiteral("verified"), true);
    row.insert(QStringLiteral("learnedFrom"), QStringLiteral("manual selection on hardware"));
    kept.append(row);

    root.insert(QStringLiteral("comment"),
                QStringLiteral("Learned identification rows (DEV_ID -> part). Written by the app "
                               "when a part was picked manually; the built-in table wins."));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("devices"), kept);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 %1: %2").arg(path, out.errorString());
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();

    Logger::instance()->info(
        QStringLiteral("已记住 DEV_ID 0x%1 -> %2（%3）")
            .arg(devId, 3, 16, QLatin1Char('0'))
            .arg(partName, path));
    // Reload so the new row is live for this session too.
    loadDatabase();
    return true;
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

PortProbe DeviceIdentifier::probeGpioPorts(IProbe &probe, const QString &svdPath)
{
    PortProbe result;
    if (!probe.isConnected() || svdPath.isEmpty()) {
        result.note = QStringLiteral("未连接或缺少 SVD");
        return result;
    }

    const auto parsed = SvdParser::parseFile(svdPath);
    if (!parsed) {
        result.note = QStringLiteral("SVD 解析失败: %1").arg(parsed.error);
        return result;
    }
    const SvdDevice &device = parsed.value;

    // Clock gates decide whether a failed read is meaningful: with the gate closed
    // a fault is expected, so it proves nothing about the port existing.
    //
    // Only the GPIO gates are opened, and only for the duration of the probe. That
    // is the safe subset: a GPIO clock supplies nothing but the port's own register
    // block, so it cannot make a pin drive anything (the pin's state comes from
    // PMODE/AFSEL/ODR, which are untouched). Timers/DMA are deliberately NOT
    // touched -- those would resume real output.
    const QHash<QString, DeviceTreeScanner::ClockBit> clocks = DeviceTreeScanner::clockMap(device);
    QHash<QString, quint32> clockValues;
    QVector<QPair<quint32, quint32>> gateRestore; // address, original value
    {
        QHash<QString, quint32> wanted; // RCC register -> bits to raise
        QHash<QString, quint32> addresses;
        for (const auto &peripheral : device.peripherals) {
            if (!GpioCodec::isGpioPort(peripheral))
                continue;
            const DeviceTreeScanner::ClockBit bit = clocks.value(peripheral.name);
            if (!bit.valid() || bit.bit < 0 || bit.bit > 31)
                continue;
            if (!addresses.contains(bit.registerName))
                addresses.insert(bit.registerName, bit.address);
            wanted[bit.registerName] |= (1u << bit.bit);
        }
        for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
            const quint32 address = addresses.value(it.key());
            quint32 original = 0;
            if (!probe.readU32(address, &original))
                continue;
            const quint32 raised = original | it.value();
            if (raised == original)
                continue; // already clocked
            if (!probe.writeU32(address, raised))
                continue; // read-only backend: stay conservative
            // Read back: a protected register accepts the write and drops it.
            quint32 check = 0;
            if (!probe.readU32(address, &check) || (check & it.value()) != it.value()) {
                probe.writeU32(address, original); // undo partial
                continue;
            }
            gateRestore.append(qMakePair(address, original));
        }
        if (!gateRestore.isEmpty())
            Logger::instance()->debug(
                QStringLiteral("端口探测：临时开启 %1 个 GPIO 时钟寄存器").arg(gateRestore.size()));
    }

    struct GateRestorer
    {
        IProbe *probe = nullptr;
        const QVector<QPair<quint32, quint32>> *list = nullptr;
        ~GateRestorer()
        {
            if (!probe || !list)
                return;
            for (const auto &entry : *list)
                probe->writeU32(entry.first, entry.second);
        }
    } restorer{&probe, &gateRestore};

    int readable = 0;
    int missing = 0;
    int unknown = 0;
    for (const auto &peripheral : device.peripherals) {
        if (!GpioCodec::isGpioPort(peripheral))
            continue;
        const QString port = GpioCodec::portName(peripheral);
        if (port.isEmpty())
            continue;

        // Whether this port's gate is open decides what an unreadable block means:
        // with the clock running, a refused read can only be "no such port".
        bool gateKnown = false;
        bool gateOpen = false;
        const DeviceTreeScanner::ClockBit bit = clocks.value(peripheral.name);
        if (bit.valid()) {
            if (!clockValues.contains(bit.registerName)) {
                quint32 value = 0;
                if (probe.readU32(bit.address, &value))
                    clockValues.insert(bit.registerName, value);
            }
            if (clockValues.contains(bit.registerName)) {
                gateKnown = true;
                gateOpen = ((clockValues.value(bit.registerName) >> bit.bit) & 1u) != 0;
            }
        }

        // A port is detected by its register block. Two traps, both measured on
        // real silicon:
        //
        //   * a missing port does NOT fault -- it reads back as zeros, so a bus
        //     error cannot be used as the signal;
        //   * once the port's clock is running, a missing port reads 0xFFFFFFFF at
        //     +0x00, which is exactly PMODE's reset value on a *real* port, so
        //     "non-zero means present" is wrong too.
        //
        // So both 0x00000000 (blank) and 0xFFFFFFFF (stub) count as "no
        // information". A real port still shows up somewhere: N32H473 GPIOA/GPIOB/
        // GPIOC return real values (PID 0xC800/0x7DD8/0x4040), and GPIOH -- whose
        // PID reads 0 -- still has 0x3FFF in PMODE. GPIOD..GPIOG are blank at every
        // offset. Anything unreadable stays unknown rather than guessed.
        const QVector<quint32> offsets = portProbeOffsets(peripheral);
        bool decided = false;
        bool present = false;
        for (const quint32 offset : offsets) {
            quint32 value = 0;
            if (!probe.readU32(peripheral.baseAddress + offset, &value))
                continue;
            decided = true;
            if (value != 0 && value != 0xFFFFFFFFu) {
                present = true;
                break;
            }
        }

        if (decided && present) {
            result.present.insert(port);
            ++readable;
        } else if (decided || (gateKnown && gateOpen)) {
            // Blank, or refused even with the port clocked: the die has no such
            // port. (Simulated backends report a fault here; real silicon reports
            // zeros. Both mean absent.)
            result.absent.insert(port);
            ++missing;
        } else {
            ++unknown; // clock closed and unreadable -> cannot tell
        }
    }

    result.note = QStringLiteral("端口探测: 存在 %1，不存在 %2，未知 %3")
                      .arg(readable)
                      .arg(missing)
                      .arg(unknown);
    // Connection detail: only interesting when diagnosing, so DEBUG (--verbose).
    Logger::instance()->debug(result.note);
    if (!result.absent.isEmpty())
        Logger::instance()->debug(
            QStringLiteral("die 上没有的 GPIO 端口: %1")
                .arg(QStringList(result.absent.constBegin(), result.absent.constEnd())
                         .join(QStringLiteral(", "))));
    return result;
}

QStringList DeviceIdentifier::familyCandidates(const QString &family,
                                                const MemoryProbe::Result &memory)
{
    if (family.trimmed().isEmpty())
        return {};

    PartTable::load();
    QStringList candidates;
    for (const QString &part : PartTable::matching(family)) {
        const PartTable::Entry entry = PartTable::lookup(part);
        if (!entry.valid || entry.part != part)
            continue; // keep exact order codes only, not prefixes
        if (entry.flashBytes <= 0 && entry.ramBytes <= 0)
            continue;
        if (!MemoryProbe::matches(memory, entry.flashBytes, entry.ramBytes))
            continue;
        candidates << part;
    }
    candidates.removeDuplicates();
    candidates.sort();
    return candidates;
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
        Logger::instance()->debug(QStringLiteral("CPUID = %1 (%2)")
                                      .arg(hex32(cpuid), result.coreName));
    } else {
        Logger::instance()->warn(QStringLiteral("读取 CPUID 失败: %1").arg(st.error));
    }

    DeviceIdEntry matchedEntry;
    bool haveEntry = false;

    // Read DEV_ID even when the user named the chip: without it there is nothing
    // to learn from, and every future connection of the same board stays manual.
    quint32 probedDevId = 0;
    quint32 probedRevId = 0;
    for (const quint32 address : g_idcodeAddresses) {
        quint32 value = 0;
        if (!probe.readU32(address, &value))
            continue;
        if (!plausibleIdCode(value))
            continue;
        probedDevId = value & 0x00000FFFu;
        probedRevId = (value >> 16) & 0x0000FFFFu;
        result.devId = probedDevId;
        result.revId = probedRevId;
        Logger::instance()->debug(QStringLiteral("IDCODE @%1 = %2 -> DEV_ID=0x%3 REV_ID=0x%4")
                                      .arg(hexAddr(address), hex32(value))
                                      .arg(probedDevId, 3, 16, QLatin1Char('0'))
                                      .arg(probedRevId, 4, 16, QLatin1Char('0')));
        break;
    }

    const QString override = overrideDevice.trimmed();

    // --- DEV_ID beats a typed/remembered name -------------------------------
    // The name in the toolbar is sticky: it gets filled from the last successful
    // identification, so it describes the *previous* board. If it were allowed to
    // short-circuit detection, plugging in a different part would keep reporting
    // the old one -- which is exactly the "plugged a G455, log still says H473"
    // symptom. DEV_ID comes from silicon, so it wins; the typed name is only used
    // when DEV_ID resolves to nothing (e.g. a part newer than the database).
    if (probedDevId != 0) {
        const QVector<DeviceIdEntry> byDevId = lookupByDevId(probedDevId);
        if (!byDevId.isEmpty()) {
            matchedEntry = byDevId.first();
            haveEntry = true;
            result.mcuName = matchedEntry.mcu;
            result.family = matchedEntry.family;
            result.vendor = matchedEntry.vendor;
            result.probeRsChip = matchedEntry.probeRsChip;
            // A name that merely differs in the order-code suffix ("N32G455CC"
            // vs "N32G455") describes the same part, so it is not a mismatch --
            // warning about it would be noise on every re-connect.
            // Compare on what J-Link would actually load. "N32G455CC" (a family +
            // package) and "N32G455" (the database row for the same silicon) are
            // the same device, so this must not be reported as a conflict.
            const QString overrideName =
                JLinkDeviceName::candidates(override).value(0).toUpper();
            const QString entryName =
                JLinkDeviceName::candidates(matchedEntry.probeRsChip).value(0).toUpper();
            const bool nameMatches =
                !overrideName.isEmpty() && !entryName.isEmpty()
                && (overrideName.startsWith(entryName) || entryName.startsWith(overrideName));
            const bool samePart =
                nameMatches
                || override.compare(matchedEntry.probeRsChip, Qt::CaseInsensitive) == 0;
            if (!override.isEmpty() && !samePart) {
                result.note = QStringLiteral("界面上的芯片名是 %1，但 DEV_ID 0x%2 实际是 %3 —— "
                                             "已按实际器件识别")
                                  .arg(override)
                                  .arg(probedDevId, 3, 16, QLatin1Char('0'))
                                  .arg(matchedEntry.mcu);
                Logger::instance()->warn(result.note);
            } else {
                result.note = QStringLiteral("按 DEV_ID 0x%1 识别")
                                  .arg(probedDevId, 3, 16, QLatin1Char('0'));
            }
        }
    }

    if (!haveEntry && !override.isEmpty()) {
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
        if (result.note.isEmpty())
            result.note = QStringLiteral("手动指定芯片名（DEV_ID 0x%1 未登记）")
                              .arg(probedDevId, 3, 16, QLatin1Char('0'));

        // Learn the pairing so the next connection needs no hand-holding.
        if (probedDevId != 0 && !result.mcuName.isEmpty()) {
            QString learnError;
            if (!rememberDevId(probedDevId, result.mcuName, &learnError) && !learnError.isEmpty())
                Logger::instance()->debug(
                    QStringLiteral("未记录 DEV_ID 映射: %1").arg(learnError));
        }
    } else if (probedDevId != 0 && !haveEntry) {
        result.note = QStringLiteral("DEV_ID 0x%1 不在识别库中，请选择具体料号")
                          .arg(probedDevId, 3, 16, QLatin1Char('0'));
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
        result.capabilities = matchedEntry.capabilities;
    }

    // 2b. Exact-part facts from the vendor pack (data/parts.json).
    //     A DEV_ID identifies a family, not a package/flash variant, so flash and
    //     RAM size and the per-model peripheral counts only exist at part level.
    //     The part table wins over the family row when it has the answer.
    {
        const PartTable::Entry part = PartTable::lookup(result.mcuName);
        if (part.valid) {
            result.partNumber = part.part;
            if (part.flashBytes > 0)
                result.flashBytes = part.flashBytes;
            if (part.ramBytes > 0)
                result.ramBytes = part.ramBytes;
            result.sramText = part.sram;
            result.ccmText = part.ccmSram;
            result.flashText = part.flash;
            if (!part.capabilities.isEmpty())
                result.capabilities = part.capabilities;
            if (result.ioCount <= 0 && part.ioCount > 0)
                result.ioCount = part.ioCount;
            if (result.packageName.isEmpty() && !part.package.isEmpty())
                result.packageName = part.package;
            Logger::instance()->debug(
                QStringLiteral("料号 %1: Flash %2 / RAM %3，能力清单 %4 项")
                    .arg(part.part,
                         part.flashBytes > 0 ? QStringLiteral("%1 KB").arg(part.flashBytes / 1024)
                                             : QStringLiteral("?"),
                         part.ramBytes > 0 ? QStringLiteral("%1 KB").arg(part.ramBytes / 1024)
                                           : QStringLiteral("?"))
                    .arg(part.capabilities.size()));
            // Pin tables are written per package and name the exact order code,
            // so prefer that over the family name for the package lookup.
            result.mcuName = part.part;
        }
    }

    // 2c. Narrow the family to a concrete order code by probing memory.
    //
    //     When DEV_ID only gives us a family (or nothing at all) the order codes of
    //     that family differ by package + flash/RAM size. Reading the top word of
    //     each candidate size tells us the real memory, which is usually enough to
    //     land on one part without asking the user anything.
    //     This runs whenever the part number is still a *family*: either DEV_ID was
    //     unknown, or it resolved to a database row whose name is a family
    //     ("N32G455") rather than a full order code. Both cases leave several
    //     packages open, and the memory tells them apart.
    const bool partIsFamily =
        !result.mcuName.isEmpty() && !PartTable::lookup(result.mcuName).valid;
    if (result.mcuName.isEmpty() || !haveEntry || partIsFamily) {
        const QString familyHint =
            !result.mcuName.isEmpty() ? result.mcuName : override.trimmed();

        // No family at all (unknown DEV_ID, nothing typed): fall back to the memory
        // signature. Flash/RAM addresses are identical on every Cortex-M part, so
        // this needs no device model -- and it turns "DEV_ID not in the database"
        // into a short candidate list instead of a dead end.
        //
        // Flash is the reliable half (erased flash reads 0xFFFFFFFF, which detect()
        // rejects); RAM is used only to refine, because the byte past the real end
        // can still read back data on real silicon.
        if (familyHint.isEmpty()) {
            const MemoryProbe::Result memory = MemoryProbe::detect(probe);
            result.memoryProbe = memory;
            if (memory.valid()) {
                // Entries without any memory data (placeholder rows such as
                // N32H785xIx7) can never be verified, so they are not candidates.
                QStringList byFlash;
                for (const QString &part : PartTable::matching(QStringLiteral("N32"))) {
                    const PartTable::Entry entry = PartTable::lookup(part);
                    if (!entry.valid || entry.part != part)
                        continue;
                    if (entry.flashBytes <= 0 && entry.ramBytes <= 0)
                        continue; // no data: unverifiable
                    if (!MemoryProbe::flashMatches(memory, entry.flashBytes))
                        continue;
                    byFlash << part;
                }

                // Refine with RAM when that still leaves something; otherwise keep
                // the flash-only set (a RAM alias must not hide the real part).
                QStringList refined;
                for (const QString &part : byFlash) {
                    const PartTable::Entry entry = PartTable::lookup(part);
                    if (MemoryProbe::matches(memory, entry.flashBytes, entry.ramBytes))
                        refined << part;
                }
                QStringList candidates = refined.isEmpty() ? byFlash : refined;

                // Name the family by *where its GPIO blocks live*. Flash alone left
                // candidates from six different families (G451..G457, H473, H474),
                // and picking the first one's SVD produced a completely wrong device
                // model. Probing the GPIO base separates them with no DEV_ID at all.
                const MemoryProbe::GpioMap gpio = MemoryProbe::gpioMap();
                if (gpio.valid() && !candidates.isEmpty()) {
                    // Which SVDs do the candidates need?
                    QSet<QString> neededSvds;
                    library.loadManifest();
                    for (const QString &part : candidates) {
                        SvdManifestEntry entry;
                        QStringList ambiguous;
                        if (library.lookupPart(part, &entry, &ambiguous))
                            neededSvds.insert(QFileInfo(entry.svdPath).fileName());
                    }
                    // Probe each distinct base once, then keep only reachable SVDs.
                    QHash<quint32, bool> reachable;
                    QSet<QString> liveSvds;
                    for (const QString &svdName : neededSvds) {
                        const quint32 base = gpio.baseBySvd.value(svdName, 0);
                        if (base == 0) {
                            liveSvds.insert(svdName); // no address known: keep
                            continue;
                        }
                        if (!reachable.contains(base))
                            reachable.insert(base, MemoryProbe::gpioBlockPresent(probe, base));
                        if (reachable.value(base))
                            liveSvds.insert(svdName);
                    }
                    if (!liveSvds.isEmpty() && liveSvds.size() < neededSvds.size()) {
                        QStringList kept;
                        for (const QString &part : candidates) {
                            SvdManifestEntry entry;
                            QStringList ambiguous;
                            if (!library.lookupPart(part, &entry, &ambiguous))
                                continue;
                            if (liveSvds.contains(QFileInfo(entry.svdPath).fileName()))
                                kept << part;
                        }
                        if (!kept.isEmpty()) {
                            Logger::instance()->debug(
                                QStringLiteral("按 GPIO 基地址排除 %1 个料号（系列不符）")
                                    .arg(candidates.size() - kept.size()));
                            candidates = kept;
                        }
                    }
                }

                candidates.sort();
                result.familyPartCandidates = candidates;

                if (!candidates.isEmpty()) {
                    // All order codes of one family share an SVD, so the first
                    // candidate answers "which SVD", which the port probe needs.
                    library.loadManifest();
                    for (const QString &part : candidates) {
                        SvdManifestEntry entry;
                        QStringList ambiguous;
                        if (library.lookupPart(part, &entry, &ambiguous)) {
                            result.svdPath = entry.svdPath;
                            break;
                        }
                    }
                    result.note = QStringLiteral("DEV_ID 0x%1 未登记；%2 匹配到 %3 个料号")
                                      .arg(result.devId, 3, 16, QLatin1Char('0'))
                                      .arg(MemoryProbe::describe(memory))
                                      .arg(candidates.size());
                    // Note only -- step 4 still gets to narrow this by port presence,
                    // otherwise a die with PA..PC+PH would keep all 12 H47x order
                    // codes even though only the 48-pin ones can match.
                    Logger::instance()->debug(result.note);
                }
            }
        }
        if (!familyHint.isEmpty()) {
            const MemoryProbe::Result memory = MemoryProbe::detect(probe);
            result.memoryProbe = memory;
            const QStringList candidates = familyCandidates(familyHint, memory);
            result.familyPartCandidates = candidates;
            if (candidates.size() == 1) {
                result.mcuName = candidates.first();
                result.packageHint = QStringLiteral("料号由内存探测确定: %1")
                                         .arg(MemoryProbe::describe(memory));
                Logger::instance()->debug(
                    QStringLiteral("由内存探测确定为 %1").arg(result.mcuName));
            } else if (candidates.size() > 1) {
                result.note = QStringLiteral("%1 系列内存为 %2，仍匹配到 %3 个料号: %4")
                                  .arg(familyHint, MemoryProbe::describe(memory))
                                  .arg(candidates.size())
                                  .arg(candidates.join(QStringLiteral(", ")));
                Logger::instance()->debug(result.note);
            } else {
                Logger::instance()->info(
                    QStringLiteral("%1 系列未能按内存探测缩小料号（探测: %2）")
                        .arg(familyHint, MemoryProbe::describe(memory)));
            }
        }
    }

    // 3. SVD matching.
    //    The vendor manifest is authoritative: it maps the exact part number to
    //    the SVD the pack assigned, plus that package's pin/IO counts. Only when
    //    the part is not in the manifest do we fall back to name scoring.
    library.loadManifest();
    if (!result.mcuName.isEmpty()) {
        SvdManifestEntry entry;
        QStringList ambiguous;
        bool found = library.lookupPart(result.mcuName, &entry, &ambiguous);

        // The manifest only lists exact order codes, so a family-level result
        // ("N32G455" from a known DEV_ID, before the package is settled) misses.
        // Every order code of one family shares the same SVD, so any candidate
        // answers the question -- take the first one.
        if (!found && !result.familyPartCandidates.isEmpty()) {
            for (const QString &candidate : result.familyPartCandidates) {
                if (library.lookupPart(candidate, &entry, &ambiguous)) {
                    found = true;
                    Logger::instance()->debug(
                        QStringLiteral("按系列内料号 %1 定位 SVD").arg(candidate));
                    break;
                }
            }
        }
        if (found) {
            result.svdPath = entry.svdPath;
            // The pin/IO counts and the package only describe a *specific* order
            // code. When the part is still a family with several candidates, taking
            // the first one's package would present a guess as a finding -- the
            // summary then claimed "LQFP48" while four packages were still open.
            const bool partPinned = result.familyPartCandidates.size() == 1;
            const bool nameIsExact = entry.part.compare(result.mcuName, Qt::CaseInsensitive) == 0;
            if (entry.ioCount > 0 && (partPinned || nameIsExact))
                result.ioCount = entry.ioCount;
            if (entry.pinCount > 0 && (partPinned || nameIsExact))
                result.pinCount = entry.pinCount;
            if (partPinned || nameIsExact) {
                if (!entry.package.isEmpty())
                    result.packageName = entry.package;
                else if (!entry.packages.isEmpty())
                    result.packageName = entry.packages.first();
            } else if (result.familyPartCandidates.size() > 1) {
                result.packageName.clear(); // unknown, do not imply one
            }
            result.note = QStringLiteral("SVD 由厂商清单按型号 %1 指定（%2）")
                              .arg(entry.part,
                                   entry.packages.isEmpty() ? entry.package : entry.packages.join(QLatin1Char('/')));
            Logger::instance()->debug(QStringLiteral("厂商清单: %1 -> %2 (IO %3 / 脚 %4)")
                                          .arg(result.mcuName, entry.svdPath)
                                          .arg(result.ioCount)
                                          .arg(result.pinCount));
        } else if (!ambiguous.isEmpty()) {
            result.note = QStringLiteral("型号 %1 在厂商清单中有多个匹配: %2")
                              .arg(result.mcuName, ambiguous.join(QStringLiteral(", ")));
            Logger::instance()->warn(result.note);
        }
    }

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

    // Unknown DEV_ID with no part number: only fail when there is *nothing* to show.
    // With memory-derived candidates the caller can offer them (and remember the
    // DEV_ID after the user picks), which beats "identification failed".
    if (!haveEntry && result.mcuName.isEmpty()) {
        if (result.familyPartCandidates.isEmpty()) {
            return Result<IdentificationResult>::failure(
                result.note.isEmpty()
                    ? QStringLiteral("无法识别 MCU 型号（DEV_ID/CPUID 均无匹配）")
                    : result.note);
        }
        result.ok = true;
        result.note = QStringLiteral("DEV_ID 0x%1 未登记；按内存 %2 匹配到 %3 个料号，请选择")
                          .arg(result.devId, 3, 16, QLatin1Char('0'))
                          .arg(MemoryProbe::describe(result.memoryProbe))
                          .arg(result.familyPartCandidates.size());
    }

    // 4. Narrow further by which GPIO ports the die implements.
    //
    //    Memory already cut this to "same flash/RAM", which on N32G455 still leaves
    //    four packages (LQFP48/64/80/100). Their port sets differ (PA..PC, +PD,
    //    +PE), so reading whether each port answers removes the ones whose ports
    //    are simply not on the die -- no marking-reading required.
    if (result.familyPartCandidates.size() > 1 && !result.svdPath.isEmpty()) {
        const PortProbe ports = probeGpioPorts(probe, result.svdPath);
        result.portProbe = ports;
        if (ports.valid() && !ports.absent.isEmpty()) {
            QStringList kept;
            QStringList rejected;
            for (const QString &candidate : result.familyPartCandidates) {
                const PartTable::Entry entry = PartTable::lookup(candidate);
                if (!entry.valid || entry.package.isEmpty()) {
                    kept << candidate; // nothing to check against
                    continue;
                }
                // A package cannot bond a port the die does not have, so a port the
                // die lacks rules the package out.
                bool impossible = false;
                for (const QString &name : entry.package.split(QLatin1Char('/'),
                                                               Qt::SkipEmptyParts)) {
                    // Key the table by the family the SVD belongs to ("N32H473"
                    // from "N32H473.svd"), not by result.mcuName: with an unknown
                    // DEV_ID no part has been chosen yet, so the old lookup always
                    // failed and not a single candidate was ever rejected.
                    const QString svdFamily =
                        QFileInfo(result.svdPath).completeBaseName();
                    const PackageDescription table =
                        loadPackageTable(svdFamily, name.trimmed());
                    if (!table.isValid())
                        continue;
                    const QHash<QString, QVector<int>> gpio = table.gpioPorts();
                    for (auto it = gpio.constBegin(); it != gpio.constEnd(); ++it) {
                        if (ports.absent.contains(it.key()))
                            impossible = true;
                    }
                }
                if (impossible)
                    rejected << candidate;
                else
                    kept << candidate;
            }
            if (!rejected.isEmpty()) {
                Logger::instance()->debug(
                    QStringLiteral("按 GPIO 端口存在性排除 %1 个料号: %2")
                        .arg(rejected.size())
                        .arg(rejected.join(QStringLiteral(", "))));
                result.familyPartCandidates = kept;
                if (kept.size() == 1) {
                    result.mcuName = kept.first();
                    result.packageHint = QStringLiteral("料号由端口探测确定: %1")
                                             .arg(kept.first());
                    Logger::instance()->debug(
                        QStringLiteral("由端口探测确定为 %1").arg(result.mcuName));
                }
            }
        }
    }

    // 4a. Candidates that survive the port probe can still belong to different
    //     SVDs -- on a real N32H473 that left {H473CC, H473KC, H474CC, H474KC}.
    //     Sibling dies are not distinguishable by memory or port set, but they do
    //     not implement the same peripherals (H474 adds the eight SHRTIM blocks,
    //     H473 has none), and a peripheral block that is not on the die reads back
    //     blank. Probing one such block settles which die this is.
    if (result.familyPartCandidates.size() > 1 && !result.svdPath.isEmpty()) {
        library.loadManifest();
        // Group the survivors by the SVD they resolve to.
        QMap<QString, QStringList> bySvd; // svd path -> candidates
        for (const QString &candidate : result.familyPartCandidates) {
            SvdManifestEntry entry;
            QStringList ambiguous;
            if (!library.lookupPart(candidate, &entry, &ambiguous))
                continue;
            bySvd[entry.svdPath] << candidate;
        }

        if (bySvd.size() > 1) {
            const QString firstPath = bySvd.firstKey();
            const QString firstSvd = QFileInfo(firstPath).fileName();
            const auto firstParsed = SvdParser::parseFile(firstPath);
            if (firstParsed) {
                // A peripheral that only one side implements.
                for (auto it = bySvd.constBegin(); it != bySvd.constEnd(); ++it) {
                    if (it.key() == firstPath)
                        continue;
                    const auto otherParsed = SvdParser::parseFile(it.key());
                    if (!otherParsed)
                        continue;

                    QSet<QString> mine;
                    for (const auto &peripheral : firstParsed.value.peripherals)
                        mine.insert(peripheral.name);
                    QSet<QString> theirs;
                    for (const auto &peripheral : otherParsed.value.peripherals)
                        theirs.insert(peripheral.name);

                    // Only-in-other is the useful direction: if that block answers,
                    // the die is the other one.
                    for (const QString &name : theirs - mine) {
                        quint32 base = 0;
                        for (const auto &peripheral : otherParsed.value.peripherals) {
                            if (peripheral.name == name) {
                                base = peripheral.baseAddress;
                                break;
                            }
                        }
                        if (base == 0)
                            continue;
                        // Blank is 0 (ported off) or 0xFFFFFFFF (clock-gated stub).
                        bool answers = false;
                        bool readable = false;
                        for (quint32 offset = 0; offset <= 0x0C; offset += 4) {
                            quint32 value = 0;
                            if (!probe.readU32(base + offset, &value))
                                continue;
                            readable = true;
                            if (value != 0 && value != 0xFFFFFFFFu) {
                                answers = true;
                                break;
                            }
                        }
                        if (!readable)
                            continue; // no evidence either way

                        const QString winner = answers ? it.key() : firstPath;
                        const QString loser = answers ? firstPath : it.key();
                        Logger::instance()->debug(
                            QStringLiteral("按外设 %1 判定为 %2（该外设%3）")
                                .arg(name, QFileInfo(winner).fileName(),
                                     answers ? QStringLiteral("存在")
                                             : QStringLiteral("不存在")));
                        // Rebuild the survivor list from the winning SVD, plus any
                        // candidate whose SVD was never in question.
                        QStringList kept;
                        for (const QString &candidate : result.familyPartCandidates) {
                            SvdManifestEntry entry;
                            QStringList ambiguous;
                            if (!library.lookupPart(candidate, &entry, &ambiguous))
                                continue;
                            const QString svd = entry.svdPath;
                            if (svd == winner
                                || (svd != loser && svd != winner
                                    && QFileInfo(svd).fileName() == firstSvd))
                                kept << candidate;
                        }
                        if (!kept.isEmpty()) {
                            result.familyPartCandidates = kept;
                            result.svdPath = winner;
                        }
                        break;
                    }
                    break; // one discriminator is enough
                }
            }
        }
    }

    // 4b. Now that the candidate list is final, pin down the exact-part facts.
    //
    //     Step 3 ran *before* the port probe narrowed the family, so at that point
    //     several packages were still open and it deliberately left the package
    //     unknown. Once a single order code survives, its package / pin / IO counts
    //     are no longer a guess and must be filled in -- otherwise the pinout page
    //     sees "no package known" and asks the user to pick, even though there is
    //     nothing left to choose (reported as "still asks me to select").
    if (result.familyPartCandidates.size() == 1
        && result.mcuName == result.familyPartCandidates.first()) {
        const PartTable::Entry exact = PartTable::lookup(result.mcuName);
        if (exact.valid) {
            if (result.packageName.isEmpty() && !exact.package.isEmpty())
                result.packageName = exact.package;
            if (result.ioCount <= 0 && exact.ioCount > 0)
                result.ioCount = exact.ioCount;
        }
        // The manifest is authoritative for the pin count of an exact part.
        if (!result.svdPath.isEmpty()) {
            SvdManifestEntry entry;
            QStringList ambiguous;
            if (library.lookupPart(result.mcuName, &entry, &ambiguous)) {
                if (result.pinCount <= 0 && entry.pinCount > 0)
                    result.pinCount = entry.pinCount;
                if (result.ioCount <= 0 && entry.ioCount > 0)
                    result.ioCount = entry.ioCount;
                if (result.packageName.isEmpty()) {
                    if (!entry.package.isEmpty())
                        result.packageName = entry.package;
                    else if (!entry.packages.isEmpty())
                        result.packageName = entry.packages.first();
                }
            }
        }
        Logger::instance()->debug(
            QStringLiteral("料号确定后补齐: 封装 %1 / IO %2 / 脚 %3")
                .arg(result.packageName.isEmpty() ? QStringLiteral("?") : result.packageName)
                .arg(result.ioCount)
                .arg(result.pinCount));
    }

    // 5. Translate the order code into the device name J-Link actually accepts.
    //
    //    SEGGER *does* ship these parts -- the names are inside JLinkARM.dll -- but
    //    without the order-code suffix: "-device N32G455CC" works while
    //    "-device N32G455CCL7" fails with "Failed to get index for device name".
    //    That mismatch is why this tool kept falling back to a generic Cortex-M4
    //    while J-Flash (which never sees the order code) worked fine.
    {
        QStringList names = JLinkDeviceName::candidates(result.mcuName);
        if (!result.partNumber.isEmpty() && result.partNumber != result.mcuName) {
            for (const QString &extra : JLinkDeviceName::candidates(result.partNumber)) {
                if (!names.contains(extra))
                    names << extra;
            }
        }
        if (!names.isEmpty()) {
            result.jlinkDeviceName = names.first();
            result.jlinkDeviceCandidates = names;
            if (result.jlinkDeviceName.compare(result.mcuName, Qt::CaseInsensitive) != 0) {
                Logger::instance()->debug(
                    QStringLiteral("J-Link 器件名: %1（料号 %2 的后缀 J-Link 不接受）")
                        .arg(result.jlinkDeviceName, result.mcuName));
            }
        }
    }

    result.ok = true;
    if (result.svdPath.isEmpty())
        result.note = QStringLiteral("%1：未在 SVD 库中找到匹配文件，请手动指定")
                          .arg(result.note.isEmpty() ? QStringLiteral("提示") : result.note);

    // One line that says everything needed. The individual steps above (CPUID,
    // raw IDCODE, manifest lookup, memory probe) stay at DEBUG: they were useful
    // while bringing this up, but on every connection they buried the answer.
    {
        QStringList summary;
        summary << QStringLiteral("器件 %1").arg(result.mcuName.isEmpty()
                                                    ? QStringLiteral("未知")
                                                    : result.mcuName);
        if (!result.coreName.isEmpty())
            summary << result.coreName;
        if (result.devId != 0)
            summary << QStringLiteral("DEV_ID 0x%1").arg(result.devId, 3, 16, QLatin1Char('0'));
        const QString memory = PartTable::memorySummary(result.flashBytes, result.ramBytes);
        if (!memory.isEmpty())
            summary << memory;
        if (!result.packageName.isEmpty())
            summary << result.packageName;
        if (!result.svdPath.isEmpty())
            summary << QFileInfo(result.svdPath).fileName();
        Logger::instance()->info(QStringLiteral("已连接: %1").arg(summary.join(QStringLiteral("  |  "))));
    }

    return Result<IdentificationResult>::success(result);
}

} // namespace mrd
