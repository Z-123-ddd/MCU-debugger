#include "core/SvdTypes.h"

#include <QMap>

#include <algorithm>

namespace mrd {

QString svdAccessName(SvdAccess access)
{
    switch (access) {
    case SvdAccess::ReadOnly:
        return QStringLiteral("read-only");
    case SvdAccess::WriteOnly:
        return QStringLiteral("write-only");
    case SvdAccess::ReadWrite:
        return QStringLiteral("read-write");
    case SvdAccess::WriteOnce:
        return QStringLiteral("writeOnce");
    case SvdAccess::ReadWriteOnce:
        return QStringLiteral("read-writeOnce");
    case SvdAccess::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

SvdAccess svdAccessFromString(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("read-only"))
        return SvdAccess::ReadOnly;
    if (t == QLatin1String("write-only"))
        return SvdAccess::WriteOnly;
    if (t == QLatin1String("read-write"))
        return SvdAccess::ReadWrite;
    if (t == QLatin1String("writeonce"))
        return SvdAccess::WriteOnce;
    if (t == QLatin1String("read-writeonce"))
        return SvdAccess::ReadWriteOnce;
    return SvdAccess::Unknown;
}

bool svdAccessIsReadable(SvdAccess access)
{
    return access == SvdAccess::ReadOnly || access == SvdAccess::ReadWrite || access == SvdAccess::ReadWriteOnce;
}

bool svdAccessIsWritable(SvdAccess access)
{
    return access == SvdAccess::WriteOnly || access == SvdAccess::ReadWrite || access == SvdAccess::WriteOnce
           || access == SvdAccess::ReadWriteOnce;
}

// ---------------------------------------------------------------------------
// SvdField
// ---------------------------------------------------------------------------

bool SvdField::isReserved() const
{
    return name.compare(QLatin1String("reserved"), Qt::CaseInsensitive) == 0
           || name.compare(QLatin1String("RESERVED"), Qt::CaseInsensitive) == 0;
}

quint64 SvdField::mask64() const
{
    if (bitWidth <= 0 || bitOffset < 0 || bitOffset >= 64)
        return 0;
    const int width = std::min(bitWidth, 64 - bitOffset);
    const quint64 low = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
    return low << bitOffset;
}

quint32 SvdField::mask() const
{
    return static_cast<quint32>(mask64() & 0xFFFFFFFFull);
}

quint32 SvdField::extract(quint32 registerValue) const
{
    return static_cast<quint32>((static_cast<quint64>(registerValue) & mask64()) >> bitOffset);
}

quint32 SvdField::compose(quint32 registerValue, quint32 fieldValue) const
{
    const quint64 cleared = static_cast<quint64>(registerValue) & ~mask64();
    const quint64 shifted = (static_cast<quint64>(fieldValue) << bitOffset) & mask64();
    return static_cast<quint32>(cleared | shifted);
}

const SvdEnumValue *SvdField::enumFor(quint32 value) const
{
    for (const auto &e : enums) {
        if (e.value == value)
            return &e;
    }
    return nullptr;
}

QString SvdField::bitRangeText() const
{
    if (bitWidth <= 1)
        return QString::number(bitOffset);
    return QStringLiteral("%1:%2").arg(msb()).arg(bitOffset);
}

// ---------------------------------------------------------------------------
// SvdRegister
// ---------------------------------------------------------------------------

int SvdRegister::byteSize() const
{
    return size > 0 ? (size + 7) / 8 : 4;
}

quint32 SvdRegister::valueMask() const
{
    if (size <= 0)
        return 0xFFFFFFFFu;
    if (size >= 32)
        return 0xFFFFFFFFu;
    return static_cast<quint32>((1ull << size) - 1ull);
}

const SvdField *SvdRegister::findField(const QString &fieldName) const
{
    for (const auto &f : fields) {
        if (f.name.compare(fieldName, Qt::CaseInsensitive) == 0)
            return &f;
    }
    return nullptr;
}

SvdField *SvdRegister::findField(const QString &fieldName)
{
    for (auto &f : fields) {
        if (f.name.compare(fieldName, Qt::CaseInsensitive) == 0)
            return &f;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// SvdPeripheral
// ---------------------------------------------------------------------------

const SvdRegister *SvdPeripheral::findRegister(const QString &registerName) const
{
    for (const auto &r : registers) {
        if (r.name.compare(registerName, Qt::CaseInsensitive) == 0)
            return &r;
    }
    return nullptr;
}

SvdRegister *SvdPeripheral::findRegister(const QString &registerName)
{
    for (auto &r : registers) {
        if (r.name.compare(registerName, Qt::CaseInsensitive) == 0)
            return &r;
    }
    return nullptr;
}

int SvdPeripheral::indexOfRegister(const QString &registerName) const
{
    for (int i = 0; i < registers.size(); ++i) {
        if (registers.at(i).name.compare(registerName, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

quint32 SvdPeripheral::spanBytes() const
{
    quint32 span = 0;
    for (const auto &r : registers) {
        const quint32 end = r.addressOffset + static_cast<quint32>(r.byteSize());
        span = std::max(span, end);
    }
    return span;
}

// ---------------------------------------------------------------------------
// SvdDevice
// ---------------------------------------------------------------------------

int SvdDevice::totalRegisters() const
{
    int n = 0;
    for (const auto &p : peripherals)
        n += p.registers.size();
    return n;
}

int SvdDevice::totalFields() const
{
    int n = 0;
    for (const auto &p : peripherals) {
        for (const auto &r : p.registers)
            n += r.fields.size();
    }
    return n;
}

int SvdDevice::totalEnums() const
{
    int n = 0;
    for (const auto &p : peripherals) {
        for (const auto &r : p.registers) {
            for (const auto &f : r.fields)
                n += f.enums.size();
        }
    }
    return n;
}

const SvdPeripheral *SvdDevice::findPeripheral(const QString &peripheralName) const
{
    for (const auto &p : peripherals) {
        if (p.name.compare(peripheralName, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

SvdPeripheral *SvdDevice::findPeripheral(const QString &peripheralName)
{
    for (auto &p : peripherals) {
        if (p.name.compare(peripheralName, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

QStringList SvdDevice::peripheralNames() const
{
    QStringList names;
    names.reserve(peripherals.size());
    for (const auto &p : peripherals)
        names << p.name;
    return names;
}

QMap<QString, QStringList> SvdDevice::groupedPeripherals() const
{
    QMap<QString, QStringList> groups;
    for (const auto &p : peripherals)
        groups[p.groupLabel()] << p.name;
    return groups;
}

} // namespace mrd
