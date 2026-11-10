#include "core/SvdParser.h"

#include "core/HexUtil.h"
#include "core/Logger.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>

namespace mrd {
namespace {

struct Defaults
{
    int size = 32;
    SvdAccess access = SvdAccess::Unknown;
    quint32 resetValue = 0;
    quint32 resetMask = 0xFFFFFFFFu;
};

quint32 parseNumber(const QString &text)
{
    QString s = text.trimmed();
    if (s.isEmpty())
        return 0;

    bool ok = false;
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) || s.startsWith(QLatin1String("0X")))
        return static_cast<quint32>(s.mid(2).toULongLong(&ok, 16));
    if (s.startsWith(QLatin1String("0b"), Qt::CaseInsensitive))
        return static_cast<quint32>(s.mid(2).toULongLong(&ok, 2));
    if (s.startsWith(QLatin1Char('#'))) {
        QString bits = s.mid(1);
        bits.replace(QLatin1Char('x'), QLatin1Char('0'), Qt::CaseInsensitive);
        return static_cast<quint32>(bits.toULongLong(&ok, 2));
    }
    quint64 v = s.toULongLong(&ok, 10);
    if (!ok)
        v = s.toULongLong(&ok, 16);
    return static_cast<quint32>(v);
}

QStringList dimLabels(const QString &dimIndex, int dim)
{
    QStringList out;
    const QString t = dimIndex.trimmed();
    if (!t.isEmpty()) {
        if (t.contains(QLatin1Char('-'))) {
            const QStringList parts = t.split(QLatin1Char('-'));
            if (parts.size() == 2) {
                bool okA = false;
                bool okB = false;
                const int a = parts.at(0).trimmed().toInt(&okA);
                const int b = parts.at(1).trimmed().toInt(&okB);
                if (okA && okB && b >= a) {
                    for (int i = a; i <= b; ++i)
                        out << QString::number(i);
                }
            }
        }
        if (out.isEmpty()) {
            const QStringList parts = t.split(QLatin1Char(','));
            for (const QString &p : parts) {
                const QString s = p.trimmed();
                if (!s.isEmpty())
                    out << s;
            }
        }
    }
    if (out.size() != dim) {
        out.clear();
        for (int i = 0; i < dim; ++i)
            out << QString::number(i);
    }
    return out;
}

QString applyIndex(const QString &nameTemplate, const QString &label, int index)
{
    if (nameTemplate.contains(QLatin1String("%s")))
        return QString(nameTemplate).replace(QLatin1String("%s"), label);
    return nameTemplate + label;
}

class Impl
{
public:
    Impl(QXmlStreamReader &reader, SvdDevice &device)
        : r(reader)
        , dev(device)
    {
    }

    bool parse();

private:
    void warn(const QString &msg);
    bool parseDevice();
    void parsePeripherals(const Defaults &defaults);
    void parsePeripheral(const Defaults &defaults);
    void parseRegisters(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out);
    void parseCluster(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out);
    void parseRegister(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out);
    void parseFields(SvdRegister &reg, const Defaults &defaults);
    void parseField(SvdRegister &reg, const Defaults &defaults);
    void parseEnums(SvdField &field);
    void parseAddressBlock(SvdPeripheral &peripheral);
    void parseInterrupt(SvdPeripheral &peripheral);

    QXmlStreamReader &r;
    SvdDevice &dev;
};

void Impl::warn(const QString &msg)
{
    dev.warnings << msg;
    Logger::instance()->debug(QStringLiteral("[svd] %1").arg(msg));
}

bool Impl::parse()
{
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("device")) {
            return parseDevice();
        }
        r.skipCurrentElement();
    }
    return false;
}

bool Impl::parseDevice()
{
    Defaults defaults;
    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (name == QLatin1String("vendor")) {
            dev.vendor = r.readElementText();
        } else if (name == QLatin1String("vendorID")) {
            dev.vendorId = r.readElementText();
        } else if (name == QLatin1String("name")) {
            dev.name = r.readElementText();
        } else if (name == QLatin1String("version")) {
            dev.version = r.readElementText();
        } else if (name == QLatin1String("description")) {
            dev.description = r.readElementText().trimmed();
        } else if (name == QLatin1String("addressUnitBits")) {
            dev.addressUnitBits = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("width")) {
            dev.width = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("size")) {
            dev.defaultSize = static_cast<int>(parseNumber(r.readElementText()));
            defaults.size = dev.defaultSize;
        } else if (name == QLatin1String("access")) {
            dev.access = svdAccessFromString(r.readElementText());
            defaults.access = dev.access;
        } else if (name == QLatin1String("resetValue")) {
            dev.resetValue = parseNumber(r.readElementText());
            defaults.resetValue = dev.resetValue;
        } else if (name == QLatin1String("resetMask")) {
            dev.resetMask = parseNumber(r.readElementText());
            defaults.resetMask = dev.resetMask;
        } else if (name == QLatin1String("cpu")) {
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("name"))
                    dev.cpuName = r.readElementText();
                else
                    r.skipCurrentElement();
            }
        } else if (name == QLatin1String("peripherals")) {
            parsePeripherals(defaults);
        } else {
            r.skipCurrentElement();
        }
    }
    return true;
}

void Impl::parsePeripherals(const Defaults &defaults)
{
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("peripheral"))
            parsePeripheral(defaults);
        else
            r.skipCurrentElement();
    }
}

void Impl::parsePeripheral(const Defaults &defaults)
{
    SvdPeripheral peripheral;
    Defaults peripheralDefaults = defaults;
    int dim = 0;
    quint32 dimIncrement = 0;
    QString dimIndex;
    bool sawBase = false;

    const auto attributes = r.attributes();
    if (attributes.hasAttribute(QLatin1String("derivedFrom")))
        peripheral.derivedFrom = attributes.value(QLatin1String("derivedFrom")).toString();

    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (name == QLatin1String("name")) {
            peripheral.name = r.readElementText().trimmed();
        } else if (name == QLatin1String("description")) {
            peripheral.description = r.readElementText().trimmed();
        } else if (name == QLatin1String("groupName")) {
            peripheral.groupName = r.readElementText().trimmed();
        } else if (name == QLatin1String("baseAddress")) {
            peripheral.baseAddress = parseNumber(r.readElementText());
            sawBase = true;
        } else if (name == QLatin1String("size")) {
            peripheralDefaults.size = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("access")) {
            peripheralDefaults.access = svdAccessFromString(r.readElementText());
        } else if (name == QLatin1String("resetValue")) {
            peripheralDefaults.resetValue = parseNumber(r.readElementText());
        } else if (name == QLatin1String("resetMask")) {
            peripheralDefaults.resetMask = parseNumber(r.readElementText());
        } else if (name == QLatin1String("dim")) {
            dim = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("dimIncrement")) {
            dimIncrement = parseNumber(r.readElementText());
        } else if (name == QLatin1String("dimIndex")) {
            dimIndex = r.readElementText();
        } else if (name == QLatin1String("addressBlock")) {
            parseAddressBlock(peripheral);
        } else if (name == QLatin1String("interrupt")) {
            parseInterrupt(peripheral);
        } else if (name == QLatin1String("registers")) {
            parseRegisters(peripheralDefaults, 0, peripheral.registers);
        } else {
            r.skipCurrentElement();
        }
    }

    if (!sawBase && peripheral.derivedFrom.isEmpty())
        warn(QStringLiteral("外设 %1 缺少 baseAddress").arg(peripheral.name));

    if (dim > 1) {
        const QStringList labels = dimLabels(dimIndex, dim);
        for (int i = 0; i < dim; ++i) {
            SvdPeripheral copy = peripheral;
            copy.name = applyIndex(peripheral.name, labels.at(i), i);
            copy.baseAddress = peripheral.baseAddress + static_cast<quint32>(i) * dimIncrement;
            dev.peripherals << copy;
        }
    } else {
        dev.peripherals << peripheral;
    }
}

void Impl::parseAddressBlock(SvdPeripheral &peripheral)
{
    SvdAddressBlock block;
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("offset"))
            block.offset = parseNumber(r.readElementText());
        else if (r.name() == QLatin1String("size"))
            block.size = parseNumber(r.readElementText());
        else if (r.name() == QLatin1String("usage"))
            block.usage = r.readElementText().trimmed();
        else
            r.skipCurrentElement();
    }
    peripheral.addressBlocks << block;
}

void Impl::parseInterrupt(SvdPeripheral &peripheral)
{
    SvdInterrupt irq;
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("name"))
            irq.name = r.readElementText().trimmed();
        else if (r.name() == QLatin1String("description"))
            irq.description = r.readElementText().trimmed();
        else if (r.name() == QLatin1String("value"))
            irq.value = static_cast<int>(parseNumber(r.readElementText()));
        else
            r.skipCurrentElement();
    }
    peripheral.interrupts << irq;
}

void Impl::parseRegisters(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out)
{
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("register"))
            parseRegister(defaults, baseOffset, out);
        else if (r.name() == QLatin1String("cluster"))
            parseCluster(defaults, baseOffset, out);
        else
            r.skipCurrentElement();
    }
}

void Impl::parseCluster(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out)
{
    quint32 clusterOffset = 0;
    int dim = 0;
    quint32 dimIncrement = 0;
    QString dimIndex;
    QVector<SvdRegister> inner;

    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (name == QLatin1String("addressOffset"))
            clusterOffset = parseNumber(r.readElementText());
        else if (name == QLatin1String("dim"))
            dim = static_cast<int>(parseNumber(r.readElementText()));
        else if (name == QLatin1String("dimIncrement"))
            dimIncrement = parseNumber(r.readElementText());
        else if (name == QLatin1String("dimIndex"))
            dimIndex = r.readElementText();
        else if (name == QLatin1String("register"))
            parseRegister(defaults, 0, inner);
        else if (name == QLatin1String("cluster"))
            parseCluster(defaults, 0, inner);
        else
            r.skipCurrentElement();
    }

    if (dim > 1) {
        const QStringList labels = dimLabels(dimIndex, dim);
        for (int i = 0; i < dim; ++i) {
            for (SvdRegister reg : inner) {
                reg.addressOffset += baseOffset + clusterOffset + static_cast<quint32>(i) * dimIncrement;
                reg.name = applyIndex(reg.name, labels.at(i), i);
                out << reg;
            }
        }
    } else {
        for (SvdRegister reg : inner) {
            reg.addressOffset += baseOffset + clusterOffset;
            out << reg;
        }
    }
}

void Impl::parseRegister(const Defaults &defaults, quint32 baseOffset, QVector<SvdRegister> &out)
{
    SvdRegister reg;
    Defaults d = defaults;
    int dim = 0;
    quint32 dimIncrement = 0;
    QString dimIndex;
    bool sawSize = false;
    bool sawAccess = false;
    bool sawResetValue = false;
    bool sawResetMask = false;

    const auto attributes = r.attributes();
    if (attributes.hasAttribute(QLatin1String("derivedFrom")))
        reg.derivedFrom = attributes.value(QLatin1String("derivedFrom")).toString();

    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (name == QLatin1String("name")) {
            reg.name = r.readElementText().trimmed();
        } else if (name == QLatin1String("displayName")) {
            reg.displayName = r.readElementText().trimmed();
        } else if (name == QLatin1String("description")) {
            reg.description = r.readElementText().trimmed();
        } else if (name == QLatin1String("groupName")) {
            reg.groupName = r.readElementText().trimmed();
        } else if (name == QLatin1String("addressOffset")) {
            reg.addressOffset = parseNumber(r.readElementText());
        } else if (name == QLatin1String("size")) {
            reg.size = static_cast<int>(parseNumber(r.readElementText()));
            d.size = reg.size;
            sawSize = true;
        } else if (name == QLatin1String("access")) {
            reg.access = svdAccessFromString(r.readElementText());
            d.access = reg.access;
            sawAccess = true;
        } else if (name == QLatin1String("resetValue")) {
            reg.resetValue = parseNumber(r.readElementText());
            d.resetValue = reg.resetValue;
            sawResetValue = true;
        } else if (name == QLatin1String("resetMask")) {
            reg.resetMask = parseNumber(r.readElementText());
            d.resetMask = reg.resetMask;
            sawResetMask = true;
        } else if (name == QLatin1String("dim")) {
            dim = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("dimIncrement")) {
            dimIncrement = parseNumber(r.readElementText());
        } else if (name == QLatin1String("dimIndex")) {
            dimIndex = r.readElementText();
        } else if (name == QLatin1String("fields")) {
            parseFields(reg, d);
        } else {
            r.skipCurrentElement();
        }
    }

    if (!sawSize)
        reg.size = d.size;
    if (!sawAccess)
        reg.access = d.access;
    if (!sawResetValue)
        reg.resetValue = d.resetValue;
    if (!sawResetMask)
        reg.resetMask = d.resetMask;

    reg.addressOffset += baseOffset;

    if (dim > 1) {
        const QStringList labels = dimLabels(dimIndex, dim);
        for (int i = 0; i < dim; ++i) {
            SvdRegister copy = reg;
            copy.name = applyIndex(reg.name, labels.at(i), i);
            if (!reg.displayName.isEmpty())
                copy.displayName = applyIndex(reg.displayName, labels.at(i), i);
            copy.addressOffset = reg.addressOffset + static_cast<quint32>(i) * dimIncrement;
            out << copy;
        }
    } else {
        out << reg;
    }
}

void Impl::parseFields(SvdRegister &reg, const Defaults &defaults)
{
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("field"))
            parseField(reg, defaults);
        else
            r.skipCurrentElement();
    }
}

void Impl::parseField(SvdRegister &reg, const Defaults &defaults)
{
    SvdField field;
    Defaults d = defaults;
    int msb = -1;
    bool sawAccess = false;
    bool sawOffset = false;
    bool sawWidth = false;

    const auto attributes = r.attributes();
    if (attributes.hasAttribute(QLatin1String("derivedFrom")))
        field.derivedFrom = attributes.value(QLatin1String("derivedFrom")).toString();

    while (r.readNextStartElement()) {
        const auto name = r.name();
        if (name == QLatin1String("name")) {
            field.name = r.readElementText().trimmed();
        } else if (name == QLatin1String("description")) {
            field.description = r.readElementText().trimmed();
        } else if (name == QLatin1String("bitOffset")) {
            field.bitOffset = static_cast<int>(parseNumber(r.readElementText()));
            sawOffset = true;
        } else if (name == QLatin1String("bitWidth")) {
            field.bitWidth = static_cast<int>(parseNumber(r.readElementText()));
            sawWidth = true;
        } else if (name == QLatin1String("lsb")) {
            field.bitOffset = static_cast<int>(parseNumber(r.readElementText()));
            sawOffset = true;
        } else if (name == QLatin1String("msb")) {
            msb = static_cast<int>(parseNumber(r.readElementText()));
        } else if (name == QLatin1String("bitRange")) {
            QString text = r.readElementText().trimmed();
            text.remove(QLatin1Char('['));
            text.remove(QLatin1Char(']'));
            const QStringList parts = text.split(QLatin1Char(':'));
            if (parts.size() == 2) {
                msb = parts.at(0).trimmed().toInt();
                field.bitOffset = parts.at(1).trimmed().toInt();
            } else if (parts.size() == 1) {
                msb = parts.at(0).trimmed().toInt();
                field.bitOffset = msb;
            }
            sawOffset = true;
            sawWidth = true;
        } else if (name == QLatin1String("access")) {
            field.access = svdAccessFromString(r.readElementText());
            d.access = field.access;
            sawAccess = true;
        } else if (name == QLatin1String("resetValue")) {
            field.resetValue = parseNumber(r.readElementText());
        } else if (name == QLatin1String("resetMask")) {
            field.resetMask = parseNumber(r.readElementText());
        } else if (name == QLatin1String("enumeratedValues")) {
            parseEnums(field);
        } else {
            r.skipCurrentElement();
        }
    }

    if (msb >= 0 && sawOffset)
        field.bitWidth = msb - field.bitOffset + 1;
    if (!sawWidth && msb < 0)
        field.bitWidth = 1;
    if (field.bitWidth <= 0)
        field.bitWidth = 1;
    if (!sawOffset) {
        warn(QStringLiteral("寄存器 %1 的字段 %2 缺少位偏移，已置 0").arg(reg.name, field.name));
        field.bitOffset = 0;
    }
    if (!sawAccess)
        field.access = d.access;
    if (field.bitOffset + field.bitWidth > 64)
        warn(QStringLiteral("寄存器 %1 的字段 %2 越界: %3 bit").arg(reg.name, field.name).arg(field.bitWidth));

    reg.fields << field;
}

void Impl::parseEnums(SvdField &field)
{
    while (r.readNextStartElement()) {
        if (r.name() == QLatin1String("enumeratedValue")) {
            SvdEnumValue value;
            while (r.readNextStartElement()) {
                if (r.name() == QLatin1String("name"))
                    value.name = r.readElementText().trimmed();
                else if (r.name() == QLatin1String("description"))
                    value.description = r.readElementText().trimmed();
                else if (r.name() == QLatin1String("value"))
                    value.value = parseNumber(r.readElementText());
                else if (r.name() == QLatin1String("isDefault")) {
                    const QString t = r.readElementText().trimmed().toLower();
                    value.isDefault = (t == QLatin1String("true") || t == QLatin1String("1"));
                } else {
                    r.skipCurrentElement();
                }
            }
            field.enums << value;
        } else {
            r.skipCurrentElement();
        }
    }
}

// ---------------------------------------------------------------------------
// derivedFrom resolution
// ---------------------------------------------------------------------------

void mergeRegister(SvdRegister &target, const SvdRegister &source)
{
    if (target.fields.isEmpty())
        target.fields = source.fields;
    if (target.description.isEmpty())
        target.description = source.description;
    if (target.displayName.isEmpty())
        target.displayName = source.displayName;
    if (target.groupName.isEmpty())
        target.groupName = source.groupName;
    if (target.access == SvdAccess::Unknown)
        target.access = source.access;
    if (target.size == 0)
        target.size = source.size;
    if (target.resetValue == 0)
        target.resetValue = source.resetValue;
}

void mergePeripheral(SvdPeripheral &target, const SvdPeripheral &source)
{
    if (target.registers.isEmpty()) {
        target.registers = source.registers;
    } else {
        for (auto &reg : target.registers) {
            if (const SvdRegister *src = source.findRegister(reg.name))
                mergeRegister(reg, *src);
        }
    }
    if (target.addressBlocks.isEmpty())
        target.addressBlocks = source.addressBlocks;
    if (target.interrupts.isEmpty())
        target.interrupts = source.interrupts;
    if (target.groupName.isEmpty())
        target.groupName = source.groupName;
    if (target.description.isEmpty())
        target.description = source.description;
}

void resolveFieldDerived(SvdDevice &dev, SvdPeripheral &peripheral, SvdRegister &reg)
{
    for (auto &field : reg.fields) {
        if (field.derivedFrom.isEmpty())
            continue;

        const SvdField *source = nullptr;
        const QString ref = field.derivedFrom;

        if (ref.contains(QLatin1Char('.'))) {
            const QStringList parts = ref.split(QLatin1Char('.'));
            if (parts.size() >= 3) {
                if (const SvdPeripheral *per = dev.findPeripheral(parts.at(0))) {
                    if (const SvdRegister *r2 = per->findRegister(parts.at(1)))
                        source = r2->findField(parts.at(2));
                }
            } else if (parts.size() == 2) {
                if (const SvdRegister *r2 = peripheral.findRegister(parts.at(0)))
                    source = r2->findField(parts.at(1));
            }
        } else {
            source = reg.findField(ref);
            if (!source) {
                for (const auto &r2 : peripheral.registers) {
                    if (const SvdField *f2 = r2.findField(ref)) {
                        source = f2;
                        break;
                    }
                }
            }
        }

        if (!source) {
            dev.warnings << QStringLiteral("字段 derivedFrom 无法解析: %1.%2 -> %3")
                                .arg(peripheral.name, reg.name, ref);
            continue;
        }
        if (field.description.isEmpty())
            field.description = source->description;
        if (field.enums.isEmpty())
            field.enums = source->enums;
        if (field.access == SvdAccess::Unknown)
            field.access = source->access;
        if (field.bitWidth == 1 && source->bitWidth != 1) {
            field.bitOffset = source->bitOffset;
            field.bitWidth = source->bitWidth;
        }
    }
}

void resolveRegisterDerived(SvdDevice &dev, SvdPeripheral &peripheral)
{
    for (int i = 0; i < peripheral.registers.size(); ++i) {
        auto &reg = peripheral.registers[i];
        if (reg.derivedFrom.isEmpty())
            continue;

        const SvdRegister *source = nullptr;
        const QString ref = reg.derivedFrom;
        if (ref.contains(QLatin1Char('.'))) {
            const QStringList parts = ref.split(QLatin1Char('.'));
            if (parts.size() == 2) {
                if (const SvdPeripheral *per = dev.findPeripheral(parts.at(0)))
                    source = per->findRegister(parts.at(1));
            }
        } else {
            source = peripheral.findRegister(ref);
        }

        if (!source || source == &reg) {
            dev.warnings << QStringLiteral("寄存器 derivedFrom 无法解析: %1.%2 -> %3")
                                .arg(peripheral.name, reg.name, ref);
            continue;
        }
        mergeRegister(reg, *source);
    }

    for (auto &reg : peripheral.registers)
        resolveFieldDerived(dev, peripheral, reg);
}

void resolveDerived(SvdDevice &dev)
{
    QHash<QString, int> index;
    for (int i = 0; i < dev.peripherals.size(); ++i) {
        const QString key = dev.peripherals.at(i).name.toUpper();
        if (index.contains(key))
            dev.warnings << QStringLiteral("外设名重复: %1").arg(dev.peripherals.at(i).name);
        else
            index.insert(key, i);
    }

    QSet<QString> resolved;
    QSet<QString> visiting;

    std::function<void(int)> resolveOne = [&](int i) {
        auto &peripheral = dev.peripherals[i];
        const QString key = peripheral.name.toUpper();
        if (peripheral.derivedFrom.isEmpty() || resolved.contains(key))
            return;
        if (visiting.contains(key)) {
            dev.warnings << QStringLiteral("derivedFrom 循环: %1").arg(peripheral.name);
            return;
        }
        visiting.insert(key);

        if (index.contains(peripheral.derivedFrom.toUpper())) {
            const int srcIndex = index.value(peripheral.derivedFrom.toUpper());
            if (srcIndex != i) {
                resolveOne(srcIndex);
                mergePeripheral(peripheral, dev.peripherals.at(srcIndex));
            }
        } else {
            dev.warnings << QStringLiteral("外设 derivedFrom 无法解析: %1 -> %2")
                                .arg(peripheral.name, peripheral.derivedFrom);
        }

        visiting.remove(key);
        resolved.insert(key);
    };

    for (int i = 0; i < dev.peripherals.size(); ++i)
        resolveOne(i);

    for (auto &peripheral : dev.peripherals)
        resolveRegisterDerived(dev, peripheral);
}

} // namespace

Result<SvdDevice> SvdParser::parseFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Result<SvdDevice>::failure(QStringLiteral("无法打开 SVD 文件: %1 (%2)").arg(path, file.errorString()));

    const QByteArray data = file.readAll();
    file.close();
    return parseData(data, path);
}

Result<SvdDevice> SvdParser::parseData(const QByteArray &data, const QString &label)
{
    QXmlStreamReader reader(data);
    SvdDevice device;
    device.sourcePath = label;

    Impl impl(reader, device);
    if (!impl.parse() || reader.hasError()) {
        const QString detail = reader.hasError()
                                   ? QStringLiteral("第 %1 行: %2").arg(reader.lineNumber()).arg(reader.errorString())
                                   : QStringLiteral("找不到 <device> 根元素");
        return Result<SvdDevice>::failure(QStringLiteral("%1 解析失败 - %2").arg(label, detail));
    }

    resolveDerived(device);

    // Keep registers sorted by offset: the block layout relies on it.
    for (auto &peripheral : device.peripherals) {
        std::stable_sort(peripheral.registers.begin(), peripheral.registers.end(),
                         [](const SvdRegister &a, const SvdRegister &b) {
                             return a.addressOffset < b.addressOffset;
                         });
    }

    if (device.name.isEmpty())
        device.name = QFileInfo(label).completeBaseName();

    return Result<SvdDevice>::success(device);
}

} // namespace mrd
