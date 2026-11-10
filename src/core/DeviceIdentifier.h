#pragma once

#include "core/Result.h"

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

    /// DBGMCU_IDCODE-like addresses probed when DEV_ID is unknown.
    static QVector<quint32> idcodeAddresses();

    static QString coreNameFromCpuid(quint32 cpuid);

    static Result<IdentificationResult> identify(IProbe &probe, SvdLibrary &library,
                                                 const QString &overrideDevice = QString(),
                                                 const QString &probeSerial = QString());
};

} // namespace mrd

Q_DECLARE_METATYPE(mrd::IdentificationResult)
