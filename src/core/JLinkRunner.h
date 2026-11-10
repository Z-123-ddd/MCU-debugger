#pragma once

#include "core/Result.h"

#include <QString>
#include <QStringList>

namespace mrd {

/// Identity of the local SEGGER J-Link installation (no target access needed).
struct JLinkInstallInfo
{
    QString executable;   ///< JLink.exe used
    QString commander;    ///< "V8.26 (Compiled ...)"
    QString dllVersion;   ///< "V8.26"
    QString firmware;     ///< "J-Link V9 compiled ..."
    QString hardware;     ///< "V9.70"
    QString serial;       ///< "602717758"
    QString product;      ///< "J-Link PLUS"
    bool probeFound = false;
};

/// Locates `JLink.exe` (J-Link Commander) and runs short one-shot scripts.
///
/// The app talks to the J-Link through the vendor CLI so the official USB
/// driver stays in place (probe-rs would require WinUSB instead). Long-lived
/// register access uses JLinkProbe's persistent session; this class only covers
/// discovery and version/emulator queries.
class JLinkRunner
{
public:
    /// Candidate `JLink.exe` paths, highest version first.
    static QStringList candidatePaths(const QString &explicitPath = QString());
    static QString locate(const QString &explicitPath = QString());

    /// Directory for a private copy (`<exe>/tools/jlink`).
    static QString defaultInstallDirectory();

    /// Run a one-shot script (lines joined with '\n'); returns stdout.
    static Result<QString> runScript(const QString &executable, const QStringList &lines, int timeoutMs = 30000,
                                     const QStringList &extraArgs = {});

    /// Version + connected-emulator information. A short connect is performed so
    /// the DLL reports firmware/hardware/serial (they are not printed before).
    static Result<JLinkInstallInfo> query(const QString &executable,
                                          const QString &device = QStringLiteral("Cortex-M4"),
                                          int timeoutMs = 60000);

    // --- parsing helpers (shared with JLinkProbe) --------------------------
    /// Parse `4002C000 = 028BFFFD 64100002 ...` lines into values in address order.
    static QVector<quint32> parseMemoryValues(const QString &output);
    /// Parse `Device "N32H473CE" selected.`
    static QString parseSelectedDevice(const QString &output);
    /// Parse `Found Cortex-M4 r0p1, Little endian.`
    static QString parseCoreName(const QString &output);
    /// Parse `S/N: 602717758` or `Serial number: 602717758`
    static QString parseSerial(const QString &output);
    /// Parse commander banner `SEGGER J-Link Commander V8.26 (Compiled ...)`.
    static QString parseCommanderVersion(const QString &output);
};

} // namespace mrd
