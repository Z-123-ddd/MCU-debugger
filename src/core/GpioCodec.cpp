#include "core/GpioCodec.h"

#include "core/SvdTypes.h"

namespace mrd {

namespace {

const SvdRegister *reg(const SvdPeripheral &peripheral, const char *name)
{
    return peripheral.findRegister(QString::fromLatin1(name));
}

quint32 valueOf(const SvdPeripheral &peripheral, const QHash<quint32, quint32> &values, const char *name,
                quint32 fallback, bool *valid)
{
    if (const SvdRegister *r = reg(peripheral, name)) {
        const auto it = values.constFind(peripheral.baseAddress + r->addressOffset);
        if (it != values.constEnd()) {
            if (valid)
                *valid = true;
            return it.value();
        }
    }
    return fallback;
}

/// Nations N32H47x style: register lookup with a list of accepted names.
const SvdRegister *regAny(const SvdPeripheral &peripheral, const QStringList &names)
{
    for (const QString &name : names) {
        if (const SvdRegister *r = peripheral.findRegister(name))
            return r;
    }
    return nullptr;
}

quint32 valueOfAny(const SvdPeripheral &peripheral, const QHash<quint32, quint32> &values,
                   const QStringList &names, quint32 fallback, bool *valid)
{
    if (const SvdRegister *r = regAny(peripheral, names)) {
        const auto it = values.constFind(peripheral.baseAddress + r->addressOffset);
        if (it != values.constEnd()) {
            if (valid)
                *valid = true;
            return it.value();
        }
    }
    return fallback;
}

QString modeForF1(quint32 cnf, quint32 mode)
{
    if (mode == 0)
        return QStringLiteral("输入");
    return QStringLiteral("输出");
}

QString subModeForF1Input(quint32 cnf)
{
    switch (cnf) {
    case 0:
        return QStringLiteral("模拟");
    case 1:
        return QStringLiteral("浮空");
    case 2:
        return QStringLiteral("上拉/下拉");
    default:
        return QStringLiteral("-");
    }
}

QString subModeForF1Output(quint32 cnf)
{
    switch (cnf) {
    case 0:
        return QStringLiteral("推挽");
    case 1:
        return QStringLiteral("开漏");
    case 2:
        return QStringLiteral("复用推挽");
    default:
        return QStringLiteral("复用开漏");
    }
}

QString speedForF1(quint32 mode)
{
    switch (mode) {
    case 1:
        return QStringLiteral("10MHz");
    case 2:
        return QStringLiteral("2MHz");
    case 3:
        return QStringLiteral("50MHz");
    default:
        return QStringLiteral("-");
    }
}

QString speedForModern(quint32 speed)
{
    switch (speed) {
    case 0:
        return QStringLiteral("低");
    case 1:
        return QStringLiteral("中");
    case 2:
        return QStringLiteral("高");
    case 3:
        return QStringLiteral("最高");
    default:
        return QStringLiteral("-");
    }
}

QString pullForModern(quint32 pull)
{
    switch (pull) {
    case 0:
        return QStringLiteral("无");
    case 1:
        return QStringLiteral("上拉");
    case 2:
        return QStringLiteral("下拉");
    default:
        return QStringLiteral("保留");
    }
}

} // namespace

bool GpioCodec::isGpioPort(const SvdPeripheral &peripheral)
{
    // STM32F1 / F4 style
    if ((reg(peripheral, "IDR") && reg(peripheral, "ODR")) || reg(peripheral, "MODER"))
        return true;
    // Nations N32H47x style
    return reg(peripheral, "GPIOx_PMODE") != nullptr
           || (reg(peripheral, "PMODE") != nullptr && (reg(peripheral, "PID") || reg(peripheral, "POD")));
}

QString GpioCodec::portName(const SvdPeripheral &peripheral)
{
    const QString name = peripheral.name.toUpper();
    if (name.startsWith(QLatin1String("GPIO"))) {
        const QString suffix = name.mid(4);
        if (!suffix.isEmpty())
            return QStringLiteral("P") + suffix.left(1);
    }
    if (name.startsWith(QLatin1String("P")) && name.size() >= 2 && name.at(1).isLetter())
        return QStringLiteral("P") + name.at(1);
    return QString();
}

QVector<GpioPin> GpioCodec::decode(const SvdPeripheral &peripheral, const QHash<quint32, quint32> &values,
                                   bool *valuesValid, const QSet<QString> &explicitlyConfigured,
                                   const QVector<int> &bondedBits)
{
    QVector<GpioPin> pins;
    const QString port = portName(peripheral);
    if (port.isEmpty() || !isGpioPort(peripheral))
        return pins;

    bool valid = false;
    const bool f1Layout = reg(peripheral, "CRL") != nullptr || reg(peripheral, "CRH") != nullptr;
    const bool n32Layout = !f1Layout
                           && (regAny(peripheral, {QStringLiteral("PMODE"), QStringLiteral("GPIOx_PMODE")}) != nullptr
                               || regAny(peripheral, {QStringLiteral("PID"), QStringLiteral("GPIOx_PID")})
                                      != nullptr);

    if (n32Layout) {
        const quint32 pmode = valueOfAny(peripheral, values,
                                         {QStringLiteral("PMODE"), QStringLiteral("GPIOx_PMODE")}, 0, &valid);
        const quint32 pupd = valueOfAny(peripheral, values,
                                        {QStringLiteral("PUPD"), QStringLiteral("GPIOx_PUPD")}, 0, &valid);
        const quint32 potype = valueOfAny(peripheral, values,
                                          {QStringLiteral("POTYPE"), QStringLiteral("GPIOx_POTYPE")}, 0, &valid);
        const quint32 ds = valueOfAny(peripheral, values, {QStringLiteral("DS"), QStringLiteral("GPIOx_DS")}, 0,
                                      &valid);
        const quint32 pid = valueOfAny(peripheral, values, {QStringLiteral("PID"), QStringLiteral("GPIOx_PID")}, 0,
                                       &valid);
        const quint32 pod = valueOfAny(peripheral, values, {QStringLiteral("POD"), QStringLiteral("GPIOx_POD")}, 0,
                                       &valid);

        // AFSEL0..3 hold 4 pins x 5 bits each.
        quint32 afsel[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            afsel[i] = valueOfAny(peripheral, values,
                                  {QStringLiteral("AFSEL%1").arg(i), QStringLiteral("GPIOx_AFSEL%1").arg(i)}, 0,
                                  &valid);
        }

        for (int i = 0; i < 16; ++i) {
            GpioPin pin;
            pin.index = i;
            pin.portName = port;

            const quint32 modeBits = (pmode >> (i * 2)) & 0x3;
            const quint32 pullBits = (pupd >> (i * 2)) & 0x3;
            const quint32 otype = (potype >> i) & 0x1;
            const quint32 drive = (ds >> (i * 2)) & 0x3;
            const quint32 af = (afsel[i / 4] >> ((i % 4) * 5)) & 0x1F;

            const QString pullText = (pullBits == 0) ? QStringLiteral("浮空")
                                     : (pullBits == 1) ? QStringLiteral("上拉")
                                     : (pullBits == 2) ? QStringLiteral("下拉")
                                                       : QStringLiteral("保留");
            // N32H47x resets PMODE to 0xFFFFFFFF, so `11` (= analog) is both the
            // reset value and a legitimate analog setting. Everything else at the
            // reset value means the firmware never touched the pin.
            const bool atResetValue = (modeBits == 0x3) && (pullBits == 0) && (otype == 0) && (af == 0);
            const QString pinKey = port + QString::number(i);
            switch (modeBits) {
            case 0:
                pin.kind = GpioPin::Input;
                pin.mode = QStringLiteral("输入");
                pin.subMode = pullText;
                break;
            case 1:
                pin.kind = GpioPin::Output;
                pin.mode = QStringLiteral("输出");
                pin.subMode = (otype == 0 ? QStringLiteral("推挽") : QStringLiteral("开漏"));
                break;
            case 2:
                pin.kind = GpioPin::Alternate;
                pin.mode = QStringLiteral("复用");
                pin.subMode = QStringLiteral("AF%1/%2").arg(af).arg(otype == 0 ? QStringLiteral("推挽")
                                                                             : QStringLiteral("开漏"));
                pin.function = QStringLiteral("AF%1").arg(af);
                break;
            default:
                pin.kind = GpioPin::Analog;
                pin.mode = QStringLiteral("模拟");
                pin.subMode = pullText;
                break;
            }
            if (atResetValue && !explicitlyConfigured.contains(pinKey)) {
                pin.kind = GpioPin::Uninitialized;
                pin.mode = QStringLiteral("未初始化");
                pin.subMode = QStringLiteral("复位默认");
                pin.function.clear();
            }
            pin.speed = QStringLiteral("DS%1").arg(drive);
            pin.outputHigh = ((pod >> i) & 1u) != 0;
            pin.inputHigh = ((pid >> i) & 1u) != 0;
            if (!bondedBits.isEmpty() && !bondedBits.contains(i)) {
                pin.bonded = false;
                pin.kind = GpioPin::Unknown;
                pin.mode = QStringLiteral("未引出");
                pin.subMode = QString();
                pin.function.clear();
            }
            pins << pin;
        }

        if (valuesValid)
            *valuesValid = valid;
        return pins;
    }

    if (f1Layout) {
        const quint32 crl = valueOf(peripheral, values, "CRL", 0x44444444u, &valid);
        quint32 crh = 0;
        if (reg(peripheral, "CRH")) {
            bool crhValid = false;
            crh = valueOf(peripheral, values, "CRH", 0x44444444u, &crhValid);
            valid = valid || crhValid;
        }
        const quint32 idr = valueOf(peripheral, values, "IDR", 0, &valid);
        const quint32 odr = valueOf(peripheral, values, "ODR", 0, &valid);

        for (int i = 0; i < 16; ++i) {
            GpioPin pin;
            pin.index = i;
            pin.portName = port;
            const quint32 nybble = (i < 8) ? ((crl >> (i * 4)) & 0xF) : ((crh >> ((i - 8) * 4)) & 0xF);
            const quint32 modeBits = (nybble & 0x3);
            const quint32 cnfBits = (nybble >> 2) & 0x3;
            pin.mode = modeForF1(cnfBits, modeBits);
            pin.speed = speedForF1(modeBits);
            if (modeBits == 0) {
                pin.kind = (cnfBits == 0) ? GpioPin::Analog : GpioPin::Input;
                pin.subMode = subModeForF1Input(cnfBits);
            } else if (cnfBits >= 2) {
                pin.kind = GpioPin::Alternate;
                pin.subMode = subModeForF1Output(cnfBits);
                pin.function = QStringLiteral("复用");
            } else {
                pin.kind = GpioPin::Output;
                pin.subMode = subModeForF1Output(cnfBits);
            }
            // STM32F1: CRL/CRH reset to 0x44444444, i.e. 0b0100 = floating input.
            const bool atF1Reset =
                (modeBits == 0 && cnfBits == 1) || (modeBits == 0 && cnfBits == 1);
            if (atF1Reset && !explicitlyConfigured.contains(port + QString::number(i))) {
                pin.kind = GpioPin::Uninitialized;
                pin.mode = QStringLiteral("未初始化");
                pin.subMode = QStringLiteral("复位默认");
                pin.function.clear();
            }
            pin.outputHigh = ((odr >> i) & 1u) != 0;
            pin.inputHigh = ((idr >> i) & 1u) != 0;
            if (!bondedBits.isEmpty() && !bondedBits.contains(i)) {
                pin.bonded = false;
                pin.kind = GpioPin::Unknown;
                pin.mode = QStringLiteral("未引出");
                pin.subMode = QString();
                pin.function.clear();
            }
            pins << pin;
        }
    } else {
        const quint32 moder = valueOf(peripheral, values, "MODER", 0, &valid);
        const quint32 otyper = valueOf(peripheral, values, "OTYPER", 0, &valid);
        const quint32 ospeedr = valueOf(peripheral, values, "OSPEEDR", 0, &valid);
        const quint32 pupdr = valueOf(peripheral, values, "PUPDR", 0, &valid);
        const quint32 idr = valueOf(peripheral, values, "IDR", 0, &valid);
        const quint32 odr = valueOf(peripheral, values, "ODR", 0, &valid);
        quint32 afrLow = 0;
        quint32 afrHigh = 0;
        if (reg(peripheral, "AFRL"))
            afrLow = valueOf(peripheral, values, "AFRL", 0, &valid);
        else if (reg(peripheral, "AFR0"))
            afrLow = valueOf(peripheral, values, "AFR0", 0, &valid);
        if (reg(peripheral, "AFRH"))
            afrHigh = valueOf(peripheral, values, "AFRH", 0, &valid);
        else if (reg(peripheral, "AFR1"))
            afrHigh = valueOf(peripheral, values, "AFR1", 0, &valid);

        for (int i = 0; i < 16; ++i) {
            GpioPin pin;
            pin.index = i;
            pin.portName = port;
            const quint32 mode = (moder >> (i * 2)) & 0x3;
            const quint32 otype = (otyper >> i) & 0x1;
            const quint32 speed = (ospeedr >> (i * 2)) & 0x3;
            const quint32 pull = (pupdr >> (i * 2)) & 0x3;
            const quint32 af = (i < 8 ? ((afrLow >> (i * 4)) & 0xF) : ((afrHigh >> ((i - 8) * 4)) & 0xF));

            switch (mode) {
            case 0:
                pin.kind = GpioPin::Input;
                pin.mode = QStringLiteral("输入");
                pin.subMode = pullForModern(pull);
                break;
            case 1:
                pin.kind = GpioPin::Output;
                pin.mode = QStringLiteral("输出");
                pin.subMode = (otype == 0 ? QStringLiteral("推挽") : QStringLiteral("开漏")) + QStringLiteral("/")
                              + pullForModern(pull);
                break;
            case 2:
                pin.kind = GpioPin::Alternate;
                pin.mode = QStringLiteral("复用");
                pin.subMode = QStringLiteral("AF%1/%2").arg(af).arg(otype == 0 ? QStringLiteral("推挽")
                                                                             : QStringLiteral("开漏"));
                pin.function = QStringLiteral("AF%1").arg(af);
                break;
            default:
                pin.kind = GpioPin::Analog;
                pin.mode = QStringLiteral("模拟");
                pin.subMode = QStringLiteral("-");
                break;
            }
            // STM32F4/G4 reset MODER to 0xFFFFFFFF as well: `11`+no pull+no AF is
            // the untouched pin unless this session configured it.
            if (mode == 0x3 && pull == 0 && otype == 0 && speed == 0 && af == 0
                && !explicitlyConfigured.contains(port + QString::number(i))) {
                pin.kind = GpioPin::Uninitialized;
                pin.mode = QStringLiteral("未初始化");
                pin.subMode = QStringLiteral("复位默认");
                pin.function.clear();
            }
            pin.speed = speedForModern(speed);
            pin.outputHigh = ((odr >> i) & 1u) != 0;
            pin.inputHigh = ((idr >> i) & 1u) != 0;
            if (!bondedBits.isEmpty() && !bondedBits.contains(i)) {
                pin.bonded = false;
                pin.kind = GpioPin::Unknown;
                pin.mode = QStringLiteral("未引出");
                pin.subMode = QString();
                pin.function.clear();
            }
            pins << pin;
        }
    }

    if (valuesValid)
        *valuesValid = valid;
    return pins;
}

QString GpioCodec::colorKey(const GpioPin &pin)
{
    switch (pin.kind) {
    case GpioPin::Uninitialized:
        return QStringLiteral("uninit");
    case GpioPin::Input:
        return QStringLiteral("in");
    case GpioPin::Output:
        return QStringLiteral("out");
    case GpioPin::Alternate:
        return QStringLiteral("af");
    case GpioPin::Analog:
        return QStringLiteral("ana");
    default:
        break;
    }
    return QStringLiteral("unknown");
}

// ---------------------------------------------------------------------------
// Pin (re)configuration
// ---------------------------------------------------------------------------

namespace {

/// Replace a 2-bit field inside accumulated.
void assign2(quint32 *accumulated, int shift, quint32 field)
{
    const quint32 mask = 0x3u << shift;
    *accumulated = (*accumulated & ~mask) | ((field & 0x3u) << shift);
}

} // namespace

QVector<GpioWrite> GpioCodec::encodePinConfig(const SvdPeripheral &peripheral, int bit,
                                              const GpioPinConfig &config,
                                              const QHash<quint32, quint32> &current)
{
    QVector<GpioWrite> writes;
    if (bit < 0 || bit > 15)
        return writes;

    const QString port = portName(peripheral);
    if (port.isEmpty())
        return writes;

    // Current register values (fall back to reset values when unread).
    auto snapshot = [&](const QStringList &names, quint32 fallback) -> quint32 {
        for (const QString &name : names) {
            if (const SvdRegister *r = peripheral.findRegister(name)) {
                const quint32 address = peripheral.baseAddress + r->addressOffset;
                const auto it = current.constFind(address);
                return it != current.constEnd() ? it.value() : r->resetValue;
            }
        }
        return fallback;
    };
    auto pushWrite = [&](const QStringList &names, quint32 value) {
        for (const QString &name : names) {
            if (const SvdRegister *r = peripheral.findRegister(name)) {
                GpioWrite w;
                w.address = peripheral.baseAddress + r->addressOffset;
                w.value = value;
                writes << w;
                return;
            }
        }
    };

    const bool f1Layout = peripheral.findRegister(QStringLiteral("CRL"))
                          || peripheral.findRegister(QStringLiteral("CRH"));
    const bool n32Layout =
        peripheral.findRegister(QStringLiteral("PMODE")) || peripheral.findRegister(QStringLiteral("GPIOx_PMODE"));

    if (n32Layout) {
        const QStringList pmodeNames = {QStringLiteral("PMODE"), QStringLiteral("GPIOx_PMODE")};
        const QStringList pupdNames = {QStringLiteral("PUPD"), QStringLiteral("GPIOx_PUPD")};
        const QStringList potypeNames = {QStringLiteral("POTYPE"), QStringLiteral("GPIOx_POTYPE")};

        const quint32 pmode = snapshot(pmodeNames, 0xFFFFFFFFu);
        const quint32 pupd = snapshot(pupdNames, 0);
        const quint32 potype = snapshot(potypeNames, 0);

        quint32 newPmode = pmode;
        quint32 newPupd = pupd;
        quint32 newPotype = potype;

        quint32 modeField = 0; // 0=in 1=out 2=AF 3=analog
        switch (config.mode) {
        case GpioPinConfig::AnalogInput:
            modeField = 3;
            break;
        case GpioPinConfig::DigitalInput:
            modeField = 0;
            break;
        case GpioPinConfig::DigitalOutput:
            modeField = 1;
            break;
        case GpioPinConfig::AlternateFunction:
            modeField = 2;
            break;
        }
        assign2(&newPmode, bit * 2, modeField);

        quint32 pullField = 0; // 0=none 1=up 2=down
        switch (config.pull) {
        case GpioPinConfig::PullUp:
            pullField = 1;
            break;
        case GpioPinConfig::PullDown:
            pullField = 2;
            break;
        case GpioPinConfig::NoPull:
            pullField = 0;
            break;
        }
        assign2(&newPupd, bit * 2, pullField);

        if (config.openDrain)
            newPotype |= (1u << bit);
        else
            newPotype &= ~(1u << bit);

        if (newPmode != pmode)
            pushWrite(pmodeNames, newPmode);
        if (newPupd != pupd)
            pushWrite(pupdNames, newPupd);
        if (newPotype != potype)
            pushWrite(potypeNames, newPotype);

        // Alternate function number lives in AFSEL0..3, 4 pins x 5 bits.
        if (config.mode == GpioPinConfig::AlternateFunction) {
            const int group = bit / 4;
            const int within = bit % 4;
            const QStringList afNames = {QStringLiteral("AFSEL%1").arg(group),
                                         QStringLiteral("GPIOx_AFSEL%1").arg(group)};
            const quint32 afsel = snapshot(afNames, 0);
            const quint32 mask = 0x1Fu << (within * 5);
            const quint32 updated = (afsel & ~mask) | ((quint32(config.alternateFunction) & 0x1Fu) << (within * 5));
            if (updated != afsel)
                pushWrite(afNames, updated);
        }

        // Drive strength: keep it sane for outputs (DS0 = lowest).
        return writes;
    }

    if (f1Layout) {
        // CRL covers pins 0..7, CRH covers 8..15; 4 bits per pin (CNF[1:0], MODE[1:0]).
        const bool low = bit < 8;
        const QStringList names = {low ? QStringLiteral("CRL") : QStringLiteral("CRH")};
        const quint32 cr = snapshot(names, 0x44444444u);
        const int shift = (bit % 8) * 4;

        quint32 modeBits = 0;
        quint32 cnfBits = 0;
        switch (config.mode) {
        case GpioPinConfig::AnalogInput:
            modeBits = 0;
            cnfBits = 0;
            break;
        case GpioPinConfig::DigitalInput:
            modeBits = 0;
            cnfBits = (config.pull == GpioPinConfig::NoPull) ? 1 : 2; // 1=float 2=pull
            break;
        case GpioPinConfig::DigitalOutput:
            modeBits = 1; // 10MHz
            cnfBits = config.openDrain ? 1 : 0;
            break;
        case GpioPinConfig::AlternateFunction:
            modeBits = 3; // 50MHz
            cnfBits = config.openDrain ? 3 : 2;
            break;
        }
        const quint32 nybble = (cnfBits << 2) | modeBits;
        const quint32 mask = 0xFu << shift;
        const quint32 updated = (cr & ~mask) | ((nybble & 0xFu) << shift);
        if (updated != cr)
            pushWrite(names, updated);
        return writes;
    }

    // STM32F4/G4/L4: MODER + OTYPER + PUPDR
    const QStringList moderNames = {QStringLiteral("MODER")};
    if (!peripheral.findRegister(QStringLiteral("MODER")))
        return writes;

    const quint32 moder = snapshot(moderNames, 0xFFFFFFFFu);
    const quint32 otyper = snapshot({QStringLiteral("OTYPER")}, 0);
    const quint32 pupdr = snapshot({QStringLiteral("PUPDR")}, 0);

    quint32 modeField = 0;
    switch (config.mode) {
    case GpioPinConfig::AnalogInput:
        modeField = 3;
        break;
    case GpioPinConfig::DigitalInput:
        modeField = 0;
        break;
    case GpioPinConfig::DigitalOutput:
        modeField = 1;
        break;
    case GpioPinConfig::AlternateFunction:
        modeField = 2;
        break;
    }
    quint32 newModer = moder;
    assign2(&newModer, bit * 2, modeField);

    quint32 newOtyper = otyper;
    if (config.openDrain)
        newOtyper |= (1u << bit);
    else
        newOtyper &= ~(1u << bit);

    quint32 pullField = 0;
    switch (config.pull) {
    case GpioPinConfig::PullUp:
        pullField = 1;
        break;
    case GpioPinConfig::PullDown:
        pullField = 2;
        break;
    case GpioPinConfig::NoPull:
        pullField = 0;
        break;
    }
    quint32 newPupdr = pupdr;
    assign2(&newPupdr, bit * 2, pullField);

    if (newModer != moder)
        pushWrite(moderNames, newModer);
    if (newOtyper != otyper)
        pushWrite({QStringLiteral("OTYPER")}, newOtyper);
    if (newPupdr != pupdr)
        pushWrite({QStringLiteral("PUPDR")}, newPupdr);

    if (config.mode == GpioPinConfig::AlternateFunction) {
        const bool low = bit < 8;
        const QStringList afNames = {low ? QStringLiteral("AFRL") : QStringLiteral("AFRH")};
        const quint32 afr = snapshot(afNames, 0);
        const int shift = (bit % 8) * 4;
        const quint32 mask = 0xFu << shift;
        const quint32 updated = (afr & ~mask) | ((quint32(config.alternateFunction) & 0xFu) << shift);
        if (updated != afr)
            pushWrite(afNames, updated);
    }
    return writes;
}

bool GpioCodec::encodePinLevel(const SvdPeripheral &peripheral, int bit, bool high,
                               const QHash<quint32, quint32> &current, GpioWrite *write,
                               QString *registerName)
{
    if (!write || bit < 0 || bit > 15)
        return false;

    auto findReg = [&peripheral](const QStringList &names) -> const SvdRegister * {
        for (const QString &name : names) {
            if (const SvdRegister *r = peripheral.findRegister(name))
                return r;
        }
        return nullptr;
    };
    auto currentOf = [&](const SvdRegister *r) -> quint32 {
        const quint32 address = peripheral.baseAddress + r->addressOffset;
        const auto it = current.constFind(address);
        return it != current.constEnd() ? it.value() : r->resetValue;
    };

    // --- N32H47x: PBSC is a combined set/clear register (set low half, clear
    //     high half), which is the cleanest single write. ---------------------
    if (const SvdRegister *pbsc = findReg({QStringLiteral("PBSC"), QStringLiteral("GPIOx_PBSC")})) {
        write->address = peripheral.baseAddress + pbsc->addressOffset;
        // Writing 1 to PBS<n> drives high, 1 to PBC<n> (bit 16+n) drives low.
        write->value = high ? (1u << bit) : (1u << (16 + bit));
        if (registerName)
            *registerName = pbsc->name;
        return true;
    }

    // --- STM32 BSRR / BRR --------------------------------------------------
    if (const SvdRegister *bsrr = findReg({QStringLiteral("BSRR")})) {
        write->address = peripheral.baseAddress + bsrr->addressOffset;
        write->value = high ? (1u << bit) : (1u << (16 + bit));
        if (registerName)
            *registerName = bsrr->name;
        return true;
    }
    if (const SvdRegister *brr = findReg({QStringLiteral("BRR")})) {
        if (!high) {
            write->address = peripheral.baseAddress + brr->addressOffset;
            write->value = (1u << bit);
            if (registerName)
                *registerName = brr->name;
            return true;
        }
    }

    // --- fallback: plain output data register ------------------------------
    if (const SvdRegister *pod = findReg({QStringLiteral("POD"), QStringLiteral("GPIOx_POD"),
                                          QStringLiteral("ODR")})) {
        quint32 value = currentOf(pod);
        value = high ? (value | (1u << bit)) : (value & ~(1u << bit));
        write->address = peripheral.baseAddress + pod->addressOffset;
        write->value = value;
        if (registerName)
            *registerName = pod->name;
        return true;
    }

    return false;
}

QString GpioCodec::describeConfig(const GpioPinConfig &config)
{
    switch (config.mode) {
    case GpioPinConfig::AnalogInput:
        return QStringLiteral("模拟量输入");
    case GpioPinConfig::DigitalInput:
        switch (config.pull) {
        case GpioPinConfig::PullUp:
            return QStringLiteral("数字输入(上拉)");
        case GpioPinConfig::PullDown:
            return QStringLiteral("数字输入(下拉)");
        case GpioPinConfig::NoPull:
            break;
        }
        return QStringLiteral("数字输入(浮空)");
    case GpioPinConfig::DigitalOutput:
        return config.openDrain ? QStringLiteral("数字输出(开漏)") : QStringLiteral("数字输出(推挽)");
    case GpioPinConfig::AlternateFunction:
        return QStringLiteral("复用功能 AF%1").arg(config.alternateFunction);
    }
    return QStringLiteral("未知");
}

} // namespace mrd
