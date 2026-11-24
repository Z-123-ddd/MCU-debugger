#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace mrd {

/// Thin typed wrapper around QSettings for everything the app persists.
class AppSettings
{
public:
    // --- SVD library -------------------------------------------------------
    static QStringList svdRoots();
    static void setSvdRoots(const QStringList &roots);

    static QString lastSvdPath();
    static void setLastSvdPath(const QString &path);

    // --- probe backend ------------------------------------------------------
    /// "jlink" (GDB server) or "jlink-dll" (DLL directly, needed for HSS).

    static QString jlinkPath(); ///< empty = auto-detect the SEGGER install
    /// Explicit path to JLink_x64.dll / JLinkARM.dll; empty = auto-detect.
    static QString jlinkDllPath();
    static void setJlinkDllPath(const QString &path);
    static void setJlinkPath(const QString &path);


    static QString probeSelector(); ///< optional "vid:pid:serial" / serial number
    static void setProbeSelector(const QString &selector);

    static QString interfaceName(); ///< "SWD" or "JTAG"
    static void setInterfaceName(const QString &name);

    static int speedKHz();
    static void setSpeedKHz(int speed);

    /// Explicitly chosen device name; empty means "identify from DBGMCU_IDCODE".
    ///
    /// It used to hold the last *detected* part as well, which turned every new
    /// connection into a manual override for the previous board.
    /// Package chosen for \a chipName ("LQFP48"), empty when none was picked.
    ///
    /// Kept separate from the order code because one code can be bonded into several
    /// packages and the pinout depends on which: "UQFN48/LQFP48" on N32H473CE means the
    /// same silicon in two different pin layouts.
    static QString chipPackage();
    static void setChipPackage(const QString &package);

    static QString chipName();

    /// One-shot migration for the setting above.
    ///
    /// Builds up to 1.1.1.0 stored the auto-detected part here, so upgrading users
    /// would keep seeing (and connecting as) their previous chip. Clearing it once
    /// restores "auto" as the default without touching later manual choices.
    static void migrateChipNameSetting();
    /// Remove settings that belonged to the removed GDB backend / auto-connect.
    static void dropObsoleteBackendKeys();
    static void setChipName(const QString &name);

    /// Device/SVD binding remembered per J-Link serial number.
    static QString bindingForSn(const QString &sn);
    static void setBindingForSn(const QString &sn, const QString &svdPath);

    // --- session -----------------------------------------------------------
    static int pollIntervalMs();
    static void setPollIntervalMs(int ms);

    static QString packageDir();
    static void setPackageDir(const QString &dir);

    static QString theme();
    static void setTheme(const QString &theme);


    static QStringList pinnedPeripherals(); ///< "USART1" style names
    static void setPinnedPeripherals(const QStringList &names);

private:
    static QHash<QString, QString> bindings();
    static void setBindings(const QHash<QString, QString> &bindings);
};

} // namespace mrd
