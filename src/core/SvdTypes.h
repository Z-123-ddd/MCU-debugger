#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

namespace mrd {

/// CMSIS-SVD <access> plus the SVD 1.3 "writeOnce"/"read-writeOnce" variants.
enum class SvdAccess {
    Unknown = 0,
    ReadOnly,
    WriteOnly,
    ReadWrite,
    WriteOnce,
    ReadWriteOnce,
};

QString svdAccessName(SvdAccess access);
SvdAccess svdAccessFromString(const QString &text);
bool svdAccessIsReadable(SvdAccess access);
bool svdAccessIsWritable(SvdAccess access);

struct SvdEnumValue
{
    QString name;
    QString description;
    quint32 value = 0;
    bool isDefault = false;
};

/// One bit field inside a register.
struct SvdField
{
    QString name;
    QString description;
    int bitOffset = 0;
    int bitWidth = 1;
    SvdAccess access = SvdAccess::Unknown;
    quint32 resetValue = 0;
    quint32 resetMask = 0xFFFFFFFFu;
    QVector<SvdEnumValue> enums;
    QString derivedFrom;

    int msb() const { return bitOffset + bitWidth - 1; }
    bool isReserved() const;
    quint64 mask64() const;
    quint32 mask() const;
    quint32 extract(quint32 registerValue) const;
    /// Read-modify-write: replace only this field's bits.
    quint32 compose(quint32 registerValue, quint32 fieldValue) const;
    const SvdEnumValue *enumFor(quint32 value) const;
    /// "31:16" for multi-bit fields, "5" for single bits.
    QString bitRangeText() const;
};

struct SvdRegister
{
    QString name;
    QString displayName;
    QString description;
    QString groupName;
    quint32 addressOffset = 0;
    int size = 32; ///< bits
    SvdAccess access = SvdAccess::Unknown;
    quint32 resetValue = 0;
    quint32 resetMask = 0xFFFFFFFFu;
    QVector<SvdField> fields;
    QString derivedFrom;

    QString label() const { return displayName.isEmpty() ? name : displayName; }
    int byteSize() const;
    quint32 valueMask() const; ///< (1 << size) - 1
    const SvdField *findField(const QString &fieldName) const;
    SvdField *findField(const QString &fieldName);
    bool hasField(const QString &fieldName) const { return findField(fieldName) != nullptr; }
};

struct SvdAddressBlock
{
    quint32 offset = 0;
    quint32 size = 0;
    QString usage;
};

struct SvdInterrupt
{
    QString name;
    QString description;
    int value = -1;
};

struct SvdPeripheral
{
    QString name;
    QString description;
    QString groupName;
    QString derivedFrom;
    quint32 baseAddress = 0;
    QVector<SvdRegister> registers;
    QVector<SvdAddressBlock> addressBlocks;
    QVector<SvdInterrupt> interrupts;

    QString groupLabel() const { return groupName.isEmpty() ? name : groupName; }
    const SvdRegister *findRegister(const QString &registerName) const;
    SvdRegister *findRegister(const QString &registerName);
    int indexOfRegister(const QString &registerName) const;
    /// Highest register offset + size, in bytes (0 when empty).
    quint32 spanBytes() const;
};

struct SvdDevice
{
    QString vendor;
    QString vendorId;
    QString name;
    QString version;
    QString description;
    QString cpuName;
    int addressUnitBits = 8;
    int width = 32;
    int defaultSize = 32;
    SvdAccess access = SvdAccess::Unknown;
    quint32 resetValue = 0;
    quint32 resetMask = 0xFFFFFFFFu;
    QVector<SvdPeripheral> peripherals;
    QString sourcePath;
    QStringList warnings;

    int totalRegisters() const;
    int totalFields() const;
    int totalEnums() const;
    const SvdPeripheral *findPeripheral(const QString &peripheralName) const;
    SvdPeripheral *findPeripheral(const QString &peripheralName);
    QStringList peripheralNames() const;
    /// Peripheral groups as "groupName -> {peripheral names}" (CubeMX-like tree).
    QMap<QString, QStringList> groupedPeripherals() const;
};

} // namespace mrd
