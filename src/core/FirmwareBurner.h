#pragma once

#include "core/FirmwareImage.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace mrd {

/// Writes a firmware image to the target's Flash.
///
/// Why J-Link Commander and not the DLL: programming Flash correctly needs the vendor's
/// flash algorithm (sector layout, timing, RMW handling), and `JLink.exe` ships exactly
/// that. Driving `JLINK_WriteMem` at a Flash address from the DLL only works when a
/// matching flash bank is configured, and getting that subtly wrong is how a target ends
/// up with a half-written sector. `loadbin` is the same path J-Flash uses.
///
/// The command sequence is deliberately narrow:
///
///   `loadbin <file> <start>`  erases only the sectors the image covers, then programs;
///   `verifybin <file> <start>` re-reads and compares -- a burn is not finished until it
///                             has been read back;
///   `r` / `g`                 reset and run, so the target leaves the debugger running.
///
/// Nothing here runs unless the caller asked for it: the class never writes on
/// construction, and `burn()` requires an explicit confirmation parameter.
struct BurnResult
{
    bool ok = false;
    QString error;
    /// Everything the Commander printed, for the log -- a failed burn is diagnosed from
    /// this, so it is kept verbatim rather than summarised.
    QString output;
    qint64 elapsedMs = 0;
    /// True when the verify step passed (a separate signal from `ok`: the write can
    /// succeed while the verify fails on a damaged sector).
    bool verified = false;
};

class FirmwareBurner
{
public:
    /// \a commanderExecutable comes from JLinkRunner::locate(). Passed in rather than
    /// looked up here so the caller controls which J-Link install is used.
    explicit FirmwareBurner(const QString &commanderExecutable);

    /// Build the command list without running it. Exposed so the self-test can assert
    /// the exact sequence, and so `--burn --dry-run` can print what it would do.
    /// \a forceErase inserts an `erase` of exactly the image range before programming.
    /// Without it J-Link skips the write when the content already matches, which is
    /// safe but means "burn" may perform no write at all.
    static QStringList buildCommands(const QString &device, const QString &imagePath,
                                     quint32 startAddress, quint32 endAddress, int speedKHz,
                                     bool forceErase);
    /// The same commands as a human-readable transcript.
    static QString describeCommands(const QString &device, const QString &imagePath,
                                    quint32 startAddress, quint32 endAddress, int speedKHz,
                                    bool forceErase);

    /// Perform the burn.
    ///
    /// \a device must be a name the J-Link software knows (see JLinkDeviceName); passing
    /// the raw order code fails on parts whose suffix the DLL does not list.
    BurnResult burn(const QString &device, const FirmwareImage &image, int speedKHz,
                    bool forceErase = false, int timeoutMs = 180000) const;

    /// Non-blocking burn: same commands, reported through \a onFinished.
    ///
    /// Use this from the UI. The blocking burn() is kept for the CLI, where waiting is the
    /// point -- on the UI thread it makes Windows paint "not responding" over the window
    /// for the duration of the flash cycle.
    ///
    /// \a onFinished runs on the caller's thread (the process signals are connected
    /// there), and receives the same BurnResult the blocking call would return.
    void burnAsync(const QString &device, const FirmwareImage &image, int speedKHz,
                   bool forceErase, int timeoutMs,
                   std::function<void(const BurnResult &)> onFinished) const;

    /// Turn a commander exit code + transcript into a BurnResult. Shared by both entry
    /// points so the two paths cannot disagree about what "success" means.
    static BurnResult interpret(int exitCode, const QString &output, bool timedOut,
                                qint64 elapsedMs);

private:
    QString m_commander;
};

} // namespace mrd
