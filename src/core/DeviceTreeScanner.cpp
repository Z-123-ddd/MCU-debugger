#include "core/DeviceTreeScanner.h"

#include "core/HexUtil.h"
#include "core/IProbe.h"
#include "core/Logger.h"

#include <QRegularExpression>
#include <QStringList>

namespace mrd {

namespace {

/// Upper-case and drop separators so "USB_FS" and "USBFSEN" compare equal.
QString normalize(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c.isLetterOrNumber())
            out.append(c.toUpper());
    }
    return out;
}

/// Field names are "<PERIPH>EN"; strip that and apply the known naming quirks.
QString clockFieldToPeripheral(const QString &fieldName)
{
    QString name = fieldName;

    // Only enable bits carry clock information.
    if (!name.endsWith(QLatin1String("EN"), Qt::CaseInsensitive))
        return QString();
    // "FDCAN1OFFEN" enables the transceiver-off feature, not the peripheral.
    if (name.endsWith(QLatin1String("OFFEN"), Qt::CaseInsensitive))
        return QString();
    name.chop(2);

    // N32 names the GPIO gates IOPA..IOPH.
    if (name.startsWith(QLatin1String("IOP"), Qt::CaseInsensitive) && name.size() == 4)
        name = QStringLiteral("GPIO") + name.mid(3);

    return normalize(name);
}

/// Registers whose read side typically has side effects; never used as a probe.
bool looksReadSensitive(const QString &name)
{
    static const QStringList kBad = {
        QStringLiteral("CLR"),  QStringLiteral("ICR"),   QStringLiteral("FLAG"), QStringLiteral("DAT"),
        QStringLiteral("RX"),   QStringLiteral("TX"),    QStringLiteral("POD"),  QStringLiteral("PBSC"),
        QStringLiteral("PBC"),  QStringLiteral("PBS"),   QStringLiteral("FIFO"), QStringLiteral("POP"),
        QStringLiteral("PUSH"), QStringLiteral("CAP"),   QStringLiteral("CMP"),  QStringLiteral("W1C"),
    };
    const QString upper = name.toUpper();
    for (const QString &bad : kBad) {
        if (upper.contains(bad))
            return true;
    }
    return false;
}

int probePriority(const QString &name)
{
    const QString upper = name.toUpper();
    if (upper.contains(QLatin1String("ID")))
        return 100; // identifier registers are pure reads
    if (upper.contains(QLatin1String("CFG")) || upper.contains(QLatin1String("CTRL")))
        return 80;
    if (upper.contains(QLatin1String("CR")))
        return 70;
    if (upper.contains(QLatin1String("STS")) || upper.contains(QLatin1String("SR"))
        || upper.contains(QLatin1String("STAT")))
        return 40; // may be read-to-clear, use only if nothing better exists
    if (upper.contains(QLatin1String("PMODE")) || upper.contains(QLatin1String("MODER")))
        return 60;
    return 10;
}

} // namespace

QHash<QString, DeviceTreeScanner::ClockBit> DeviceTreeScanner::clockMap(const SvdDevice &device)
{
    QHash<QString, ClockBit> map;

    // Locate the clock-controller peripheral (SVD names it RCC on both vendors).
    const SvdPeripheral *rcc = device.findPeripheral(QStringLiteral("RCC"));
    if (!rcc) {
        for (const auto &peripheral : device.peripherals) {
            if (peripheral.groupName.compare(QLatin1String("RCC"), Qt::CaseInsensitive) == 0) {
                rcc = &peripheral;
                break;
            }
        }
    }
    if (!rcc)
        return map;

    // Index peripherals by their normalized name so field names can be matched.
    QHash<QString, QString> byNormalized;
    for (const auto &peripheral : device.peripherals)
        byNormalized.insert(normalize(peripheral.name), peripheral.name);

    // Scan every RCC register, not just the *CLKEN ones: this family also gates
    // peripherals from RCC_BDCTRL (LPTIM1EN at 0x34), which a name filter would
    // miss and the peripheral would then look clock-less.
    for (const auto &reg : rcc->registers) {
        if (reg.fields.isEmpty())
            continue;

        for (const auto &field : reg.fields) {
            const QString want = clockFieldToPeripheral(field.name);
            if (want.isEmpty())
                continue;
            const auto it = byNormalized.constFind(want);
            if (it == byNormalized.constEnd())
                continue;

            ClockBit bit;
            bit.registerName = reg.name;
            bit.address = rcc->baseAddress + reg.addressOffset;
            bit.bit = field.bitOffset;
            if (bit.valid())
                map.insert(it.value(), bit);
        }
    }
    return map;
}

int DeviceTreeScanner::clockResolvableCount(const SvdDevice &device)
{
    const QHash<QString, ClockBit> map = clockMap(device);
    int count = 0;
    for (const auto &peripheral : device.peripherals) {
        if (map.contains(peripheral.name))
            ++count;
    }
    return count;
}

const SvdRegister *DeviceTreeScanner::presenceProbeRegister(const SvdPeripheral &peripheral)
{
    const SvdRegister *best = nullptr;
    int bestScore = -1;

    for (const auto &reg : peripheral.registers) {
        if (looksReadSensitive(reg.name))
            continue;
        if (!svdAccessIsReadable(reg.access))
            continue;
        const int score = probePriority(reg.name);
        if (score > bestScore) {
            bestScore = score;
            best = &reg;
        }
    }

    // Everything looked read-sensitive: fall back to the first register anyway so
    // presence detection still works (documented as a possible side effect).
    if (!best && !peripheral.registers.isEmpty())
        best = &peripheral.registers.first();
    return best;
}

PeripheralStatus DeviceTreeScanner::classify(const SvdPeripheral &peripheral, const ClockBit &clock,
                                             bool gateOn, bool readSucceeded, quint32 value,
                                             const QString &error)
{
    PeripheralStatus status;
    status.name = peripheral.name;
    status.group = peripheral.groupLabel();
    status.baseAddress = peripheral.baseAddress;
    status.registerCount = peripheral.registers.size();
    status.error = error;

    status.clockKnown = clock.valid();
    status.clockEnabled = clock.valid() && gateOn;
    if (status.clockKnown)
        status.clockSource = QStringLiteral("%1[%2]").arg(clock.registerName).arg(clock.bit);

    if (readSucceeded) {
        // A decoded address proves the peripheral is implemented. For a gated
        // peripheral it also proves the gate is open.
        status.reachability = PeripheralStatus::Readable;
        status.probeValue = value;
        if (status.clockKnown)
            status.clockEnabled = true;
        return status;
    }

    // The access faulted. That only means "not implemented" when we can prove the
    // clock is running; with the gate closed a fault is the expected response, so
    // we refuse to draw a conclusion.
    status.reachability = status.clockEnabled ? PeripheralStatus::Missing
                                              : PeripheralStatus::Unreachable;
    return status;
}

QVector<PeripheralStatus> DeviceTreeScanner::scan(
    IProbe &probe, const SvdDevice &device,
    const std::function<void(int, int, const QString &)> &progress)
{
    QVector<PeripheralStatus> result;
    result.reserve(device.peripherals.size());

    const QHash<QString, ClockBit> clocks = clockMap(device);

    // The clock-enable registers are shared by many peripherals: read each once.
    QHash<QString, quint32> clockRegisterValues;
    if (probe.isConnected()) {
        for (auto it = clocks.constBegin(); it != clocks.constEnd(); ++it) {
            const ClockBit &bit = it.value();
            if (clockRegisterValues.contains(bit.registerName))
                continue;
            quint32 value = 0;
            if (probe.readU32(bit.address, &value))
                clockRegisterValues.insert(bit.registerName, value);
        }
    }

    const int total = device.peripherals.size();
    int done = 0;

    for (const auto &peripheral : device.peripherals) {
        ClockBit bit = clocks.value(peripheral.name);

        // Gate state must come from a successfully read enable register; if that
        // read failed we know nothing about the clock.
        bool gateOn = false;
        if (bit.valid()) {
            const auto valueIt = clockRegisterValues.constFind(bit.registerName);
            if (valueIt != clockRegisterValues.constEnd())
                gateOn = (((valueIt.value()) >> bit.bit) & 1u) != 0;
            else
                bit = ClockBit();
        }

        const SvdRegister *probeReg = presenceProbeRegister(peripheral);
        bool readOk = false;
        quint32 value = 0;
        QString error;
        if (!probeReg) {
            error = QStringLiteral("该外设没有任何寄存器");
        } else if (!probe.isConnected()) {
            error = QStringLiteral("目标未连接");
        } else {
            const quint32 address = peripheral.baseAddress + probeReg->addressOffset;
            readOk = static_cast<bool>(probe.readU32(address, &value));
            if (!readOk)
                error = QStringLiteral("读取 %1 失败").arg(hexAddr(address));
        }

        PeripheralStatus status = classify(peripheral, bit, gateOn, readOk, value, error);
        if (probeReg)
            status.probeRegister = probeReg->name;
        result << status;

        ++done;
        if (progress)
            progress(done, total, peripheral.name);
    }

    int readable = 0;
    int missing = 0;
    int clocked = 0;
    for (const auto &status : result) {
        if (status.accessible())
            ++readable;
        if (status.reachability == PeripheralStatus::Missing)
            ++missing;
        if (status.clockKnown && status.clockEnabled)
            ++clocked;
    }
    Logger::instance()->info(QStringLiteral("设备树: %1 个外设，可访问 %2，缺失 %3，时钟已开 %4")
                                 .arg(total)
                                 .arg(readable)
                                 .arg(missing)
                                 .arg(clocked));
    return result;
}

} // namespace mrd
