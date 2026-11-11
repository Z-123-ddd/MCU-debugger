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
    /// "jlink" (JLink.exe, official driver) or "probe-rs".
    static QString probeBackend();
    static void setProbeBackend(const QString &backend);

    static QString jlinkPath(); ///< empty = auto-detect the SEGGER install
    static void setJlinkPath(const QString &path);

    static QString probeRsPath(); ///< empty = auto-detect tools/probe-rs
    static void setProbeRsPath(const QString &path);

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
    static QString chipName();

    /// One-shot migration for the setting above.
    ///
    /// Builds up to 1.1.1.0 stored the auto-detected part here, so upgrading users
    /// would keep seeing (and connecting as) their previous chip. Clearing it once
    /// restores "auto" as the default without touching later manual choices.
    static void migrateChipNameSetting();
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

    static bool autoConnectOnStartup();
    static void setAutoConnectOnStartup(bool enabled);

    static QStringList pinnedPeripherals(); ///< "USART1" style names
    static void setPinnedPeripherals(const QStringList &names);

private:
    static QHash<QString, QString> bindings();
    static void setBindings(const QHash<QString, QString> &bindings);
};

} // namespace mrd
