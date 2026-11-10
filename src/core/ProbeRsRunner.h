#pragma once

#include <QString>
#include <QStringList>

namespace mrd {

/// Result of one `probe-rs` CLI invocation.
struct ProbeRsRun
{
    bool started = false;   ///< the process could actually be launched
    bool timedOut = false;
    int exitCode = -1;
    QString stdOut;
    QString stdErr;
    qint64 elapsedMs = 0;
    QString commandLine;

    bool ok() const { return started && !timedOut && exitCode == 0; }
    /// stderr when present, otherwise stdout (probe-rs reports errors on stderr).
    QString errorText() const;
};

/// Locates the `probe-rs` binary and runs CLI commands against it.
///
/// probe-rs is used as the debug backend (see README): the app never links a
/// probe driver, it drives the official CLI. Every call is a fresh process, so
/// nothing blocks the UI thread and no vendor driver conversion is needed.
class ProbeRsRunner
{
public:
    /// Candidate binary locations, in priority order.
    ///   explicit (settings) -> <app>/tools/probe-rs/probe-rs.exe
    ///   -> <app>/tools/*/probe-rs.exe -> <cwd>/tools/... -> PATH
    static QStringList candidatePaths(const QString &explicitPath = QString());
    static QString locate(const QString &explicitPath = QString());

    static QString defaultProbeRsDirectory();

    void setBinary(const QString &path) { m_binary = path; }
    QString binary() const { return m_binary; }

    /// Version string cached from the last `--version` call.
    QString cachedVersion() const { return m_version; }

    /// Resolve the binary if not set yet. Returns an error string on failure.
    QString ensureBinary(const QString &explicitPath = QString());

    /// Run `probe-rs <args>`; blocks up to \a timeoutMs (call from a worker thread).
    ProbeRsRun run(const QStringList &args, int timeoutMs = 30000);

    /// Version string (e.g. "probe-rs 0.32.0 (git commit ...)") or empty.
    QString version(int timeoutMs = 15000);

private:
    QString m_binary;
    QString m_version;
};

} // namespace mrd
