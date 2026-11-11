#pragma once

#include "core/MemoryProbe.h"
#include "core/Result.h"

#include <QHash>
#include <QSet>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

class IProbe;
class SvdLibrary;

/// One row of `data/device_ids.json`.
struct DeviceIdEntry
{
    QString mcu;         ///< "STM32F103C8"
    QString family;      ///< "STM32F103"
    QString vendor;      ///< "ST"
    QString svdHint;     ///< SVD device name, e.g. "STM32F103xx"
    QString probeRsChip; ///< name passed to `probe-rs --chip`
    quint32 devId = 0;   ///< DBGMCU_IDCODE DEV_ID; 0 = not verified yet
    quint32 idcodeAddress = 0xE0042000u;
    bool verified = true;
    /// Bonded IO count / physical pin count, from the CMSIS-Pack `.pdsc`.
    /// 0 = unknown, which keeps the package view from inventing a layout.
    int ioCount = 0;
    int pinCount = 0;
    QString packageName; ///< e.g. "UQFN48"
    /// Per-model peripheral counts from the CMSIS-Pack `.pdsc` description
    /// ("SPI" -> 5, "FDCAN" -> 2, ...). Empty when the pack has no entry for this
    /// part, in which case nothing is trimmed (the SVD is shown as-is).
    QHash<QString, int> capabilities;
};

/// Which GPIO ports the attached die implements.
///
/// Packages differ in their port set: on N32G455 the LQFP48 bonds PA/PB/PC,
/// LQFP64 adds PD, LQFP80/100 add PE. Reading a port's register answers whether
/// the port is there at all, which narrows the package without making the user
/// read a marking off the chip.
struct PortProbe
{
    QSet<QString> present; ///< "PA", "PB", ...
    QSet<QString> absent;  ///< proven not implemented (clock running, read failed)
    QString note;
    bool valid() const { return !present.isEmpty() || !absent.isEmpty(); }
};

/// Outcome of the automatic MCU identification.
struct IdentificationResult
{
    bool ok = false;
    QString error;

    QString mcuName;
    QString family;
    QString vendor;
    QString coreName;
    QString probeRsChip;
    QString svdPath;
    QStringList candidateSvdPaths;
    QString note;

    quint32 devId = 0;
    quint32 revId = 0;
    quint32 cpuid = 0;
    QString probeSerial;
    bool manuallySelected = false;

    /// Package facts needed to draw the pinout *after* identification.
    int ioCount = 0;
    int pinCount = 0;
    QString packageName;
    /// Vendor per-model peripheral counts; drives PeripheralPresence.
    QHash<QString, int> capabilities;

    /// Result of the flash/RAM probe used to narrow a family to one order code.
    MemoryProbe::Result memoryProbe;
    /// Which GPIO ports the die implements (see probeGpioPorts).
    PortProbe portProbe;

    /// Order codes of the identified family that match the probed memory.
    QStringList familyPartCandidates;
    /// Human-readable reason when the part number was derived rather than looked up.
    QString packageHint;

    /// Device name J-Link accepts for this part (see JLinkDeviceName), and the
    /// alternatives in case the first is rejected.
    QString jlinkDeviceName;
    QStringList jlinkDeviceCandidates;
    /// SVD file name inferred from the memory signature when DEV_ID is unknown.
    QString svdHintFromMemory;
    /// Flash / RAM of this exact part, from the CMSIS-Pack linker memory rows.
    /// 0 = unknown (no data/parts.json entry for this part).
    int flashBytes = 0;
    int ramBytes = 0;
    /// Prose breakdown from the pack, e.g. SRAM "160KB" + CCM "32KB".
    QString sramText;
    QString ccmText;
    QString flashText;
    /// The exact pack part the figures came from ("N32H473CE"), for the UI.
    QString partNumber;
};

/// Turns "what is physically attached" into "which SVD describes it".
///
/// Strategy:
///   1. read CPUID (0xE000ED00) to learn the core,
///   2. probe the known DBGMCU_IDCODE addresses and decode DEV_ID/REV_ID,
///   3. look the DEV_ID up in `data/device_ids.json`,
///   4. ask SvdLibrary for the best matching SVD by device name.
/// A manual chip name (`--device`, or picking in the UI) short-circuits 2-3.
class DeviceIdentifier
{
public:
    static bool loadDatabase(QString *error = nullptr);
    static const QVector<DeviceIdEntry> &database();
    static QString databasePath();
    static QVector<DeviceIdEntry> lookupByDevId(quint32 devId);

    /// Remember that  devId belongs to  partName, in a user-writable
    /// supplement (`<AppData>/data/device_ids.d/learned.json`).
    ///
    /// The vendor datasheets do not publish DEV_ID, so the database can only grow
    /// from real hardware: when the user picks the chip by hand for an unknown
    /// DEV_ID, that pair is worth keeping so the next connection is automatic.
    /// Existing rows win (the built-in table is authoritative).
    static bool rememberDevId(quint32 devId, const QString &partName, QString *error = nullptr);

    /// Part names in the database that start with  prefix (UI suggestions).
    static QStringList matchingNames(const QString &prefix);

    /// Order codes of  family, narrowed to those whose memory matches reality.
    ///
    /// A DEV_ID identifies the die; the order codes of one family differ only in
    /// package and memory size (N32G455 has 15 of them). Probing flash/RAM cuts
    /// that list down, so the user usually has to choose nothing at all.
    /// Empty when the family is not present in `data/parts.json`.
    static QStringList familyCandidates(const QString &family, const MemoryProbe::Result &memory);

    /// Which GPIO ports the attached die actually implements.
    ///
    /// Packages differ in their port set: on N32G455 the LQFP48 bonds PA/PB/PC,
    /// LQFP64 adds PD, LQFP80/100 add PE. Reading a port's register answers
    /// whether the port is there at all, which narrows the package without the
    /// user having to read a marking off the chip.
    ///
    /// A port counts as present only when a read succeeds (the clock may be off,
    /// so a failure alone proves nothing). Ports whose clock is known to be closed
    /// are reported as "unknown", never as absent.
    /// Probe the GPIO ports described by \a svdPath. Needs the SVD only to learn
    /// where each port lives; returns an invalid result when it cannot be read.
    static PortProbe probeGpioPorts(IProbe &probe, const QString &svdPath);

    /// DBGMCU_IDCODE-like addresses probed when DEV_ID is unknown.
    static QVector<quint32> idcodeAddresses();

    static QString coreNameFromCpuid(quint32 cpuid);

    static Result<IdentificationResult> identify(IProbe &probe, SvdLibrary &library,
                                                 const QString &overrideDevice = QString(),
                                                 const QString &probeSerial = QString());
};

} // namespace mrd

Q_DECLARE_METATYPE(mrd::IdentificationResult)
