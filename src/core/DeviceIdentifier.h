#pragma once

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
    QString probeRsChip; ///< human-readable alias, used only for matching typed names
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

    /// Series of the selected order code ("N32G455"). Comes from the picker, never from
    /// silicon: a DEV_ID cannot name a package or a flash size.
    QString series;

    /// Device name J-Link accepts for this part (see JLinkDeviceName), and the
    /// alternatives in case the first is rejected.
    QString jlinkDeviceName;
    QStringList jlinkDeviceCandidates;
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

/// Turns "what is physically attached" plus "which order code the user picked" into
/// "which SVD describes it".
///
/// The part is never inferred. Identification does two things only:
///
///   1. read CPUID (0xE000ED00) -> core name, which is what makes register access work;
///   2. turn the *selected* order code into SVD / capacity / package / J-Link device name.
///
/// Everything about the part comes from the order code the user picks out of
/// `data/parts.json` (see PartTable). The automatic narrowing this replaced -- memory
/// probing, GPIO-port presence, port-set matching -- could not be made reliable: reading
/// a blank upper flash half and reading past the end of a smaller part both answer 0xFF,
/// so a 512 KB part holding a 235 KB image was reported as 256 KB and the pinout then
/// described a package the board does not use.
///
/// The check that the pick matches the silicon is `validatePeripherals()`, run against a
/// freshly scanned device tree right after connecting.
class DeviceIdentifier
{
public:
    static bool loadDatabase(QString *error = nullptr);
    static const QVector<DeviceIdEntry> &database();
    static QString databasePath();
    static QVector<DeviceIdEntry> lookupByDevId(quint32 devId);

    /// Part names in the database that start with  prefix (UI suggestions).
    static QStringList matchingNames(const QString &prefix);

    /// DBGMCU_IDCODE-like addresses probed when DEV_ID is unknown.
    static QVector<quint32> idcodeAddresses();

    static QString coreNameFromCpuid(quint32 cpuid);

    static Result<IdentificationResult> identify(IProbe &probe, SvdLibrary &library,
                                                 const QString &overrideDevice = QString(),
                                                 const QString &probeSerial = QString());
};

} // namespace mrd

Q_DECLARE_METATYPE(mrd::IdentificationResult)
