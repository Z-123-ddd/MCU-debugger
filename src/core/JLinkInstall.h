#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

/// One J-Link installation found on this machine.
///
/// SEGGER's software is *not* bundled with this application: it is commercial, and shipping
/// it would mean redistributing it. So at run time we have to locate whatever the user
/// installed -- and different machines have different versions, installed in different
/// places. That is what this type describes.
///
/// The distinction that matters most is `dllPath`: only the 64-bit `JLink_x64.dll` can be
/// loaded into this (64-bit) process. Older J-Link releases shipped just `JLinkARM.dll`,
/// which is 32-bit and fails with Windows error 193. A machine in that state can still be
/// driven perfectly well through `JLinkGDBServerCL.exe` (a separate process, so bitness is
/// irrelevant), which is why the fields are separate rather than one "usable" flag.
struct JLinkInstallation
{
    QString dir;            ///< install directory
    QString dllPath;        ///< JLink_x64.dll if present, else JLinkARM.dll
    QString cliPath;        ///< JLink.exe (or the older JLink_x64.exe)
    QString gdbServerPath;  ///< JLinkGDBServerCL.exe
    QString version;        ///< "V8.26" parsed from the directory, else empty
    QString source;         ///< where it was found, for the log/UI

    bool hasDll64() const;
    bool hasDll32Only() const;
    bool hasCli() const { return !cliPath.isEmpty(); }
    bool hasGdbServer() const { return !gdbServerPath.isEmpty(); }
    bool valid() const { return !dir.isEmpty() || !cliPath.isEmpty() || !dllPath.isEmpty(); }

    /// "C:\Program Files\SEGGER\JLink_V826 · V8.26 · 64 位 DLL · 注册表"
    QString describe() const;
};

/// Finds J-Link installations, best first.
///
/// Order (a private copy always wins, then the more explicit sources):
///   1. the path configured in the app's own settings;
///   2. environment variables -- `JLINK_PATH`, `JLINK_HOME`, `SEGGER_JLINK_PATH`,
///      `JLINK_INSTALL_DIR`, plus any directory on `PATH` that holds a JLink executable.
///      This exists because SEGGER's installer only adds itself to `PATH` and writes the
///      registry, while portable/office-deployed copies often just set a variable;
///   3. the registry (`HKLM`/`HKCU`, both native and 32-bit views);
///   4. `%ProgramFiles%\SEGGER\JLink*` and the (x86) equivalent, newest first;
///   5. `<exe>/tools/jlink` and nearby developer-tree levels, last -- a bundled copy should
///      never shadow a real installation the user keeps updated.
class JLinkInstall
{
public:
    static QVector<JLinkInstallation> discover();
    /// The installation to use, or an invalid one when J-Link is not installed.
    static JLinkInstallation best();

    /// Environment variables consulted, as "NAME=value" (for `--probe-info` style output).
    static QStringList environmentReport();

    /// One line naming what was found and from where, for the log.
    static QString summary();

    /// Parse "V8.26" / "JLink_V826" / "826" into "V8.26". Empty when unparseable.
    static QString versionFromName(const QString &name);
};

} // namespace mrd
