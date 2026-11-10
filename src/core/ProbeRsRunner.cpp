#include "core/ProbeRsRunner.h"

#include "core/HexUtil.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace mrd {

namespace {

QString exeName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("probe-rs.exe");
#else
    return QStringLiteral("probe-rs");
#endif
}

QStringList subDirs(const QString &root)
{
    QStringList dirs;
    QDir dir(root);
    if (!dir.exists())
        return dirs;
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &entry : entries)
        dirs << QDir::cleanPath(root + QLatin1Char('/') + entry);
    // The plain "probe-rs" folder first, then any other versioned folder.
    const int plain = dirs.indexOf(QDir::cleanPath(root + QStringLiteral("/probe-rs")));
    if (plain > 0)
        dirs.move(plain, 0);
    return dirs;
}

} // namespace

QString ProbeRsRun::errorText() const
{
    if (timedOut)
        return QStringLiteral("命令超时: %1").arg(commandLine);
    const QString err = stdErr.trimmed();
    if (!err.isEmpty())
        return err;
    const QString out = stdOut.trimmed();
    if (!out.isEmpty())
        return out;
    if (!started)
        return QStringLiteral("无法启动 process: %1").arg(commandLine);
    return QStringLiteral("退出码 %1: %2").arg(exitCode).arg(commandLine);
}

QString ProbeRsRunner::defaultProbeRsDirectory()
{
    // Prefer an existing tools/ folder: deploy/ ships one next to the exe, while
    // build/bin needs to look two levels up into the source tree.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir::cleanPath(appDir + QStringLiteral("/tools")),
        QDir::cleanPath(appDir + QStringLiteral("/../tools")),
        QDir::cleanPath(appDir + QStringLiteral("/../../tools")),
    };
    for (const auto &candidate : candidates) {
        if (QFileInfo(candidate).isDir())
            return candidate;
    }
    return candidates.first();
}

QStringList ProbeRsRunner::candidatePaths(const QString &explicitPath)
{
    QStringList paths;
    const auto add = [&paths](const QString &path) {
        if (path.isEmpty())
            return;
        const QString cleaned = QDir::cleanPath(path);
        if (!paths.contains(cleaned) && QFileInfo(cleaned).isFile())
            paths << cleaned;
    };

    if (!explicitPath.isEmpty()) {
        if (QFileInfo(explicitPath).isDir())
            add(explicitPath + QLatin1Char('/') + exeName());
        else
            add(explicitPath);
    }

    // <app dir>/tools/... plus a few parent levels (dev tree, deploy dirs).
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList toolRoots;
    toolRoots << QDir::cleanPath(appDir + QStringLiteral("/tools"));
    toolRoots << QDir::cleanPath(appDir + QStringLiteral("/../tools"));
    toolRoots << QDir::cleanPath(appDir + QStringLiteral("/../../tools"));
    toolRoots << QDir::cleanPath(QDir::currentPath() + QStringLiteral("/tools"));
    for (const auto &root : toolRoots) {
        add(root + QLatin1Char('/') + exeName());
        add(root + QStringLiteral("/probe-rs/") + exeName());
        for (const auto &sub : subDirs(root))
            add(sub + QLatin1Char('/') + exeName());
    }

    // Last resort: rely on PATH.
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("probe-rs"));
    if (!onPath.isEmpty())
        add(onPath);

    return paths;
}

QString ProbeRsRunner::locate(const QString &explicitPath)
{
    const QStringList candidates = candidatePaths(explicitPath);
    return candidates.isEmpty() ? QString() : candidates.first();
}

QString ProbeRsRunner::ensureBinary(const QString &explicitPath)
{
    if (!m_binary.isEmpty() && QFileInfo(m_binary).isFile())
        return QString();

    m_binary = locate(explicitPath);
    if (m_binary.isEmpty()) {
        return QStringLiteral("未找到 probe-rs 可执行文件。请运行 scripts\\fetch-probe-rs.ps1 "
                              "下载到 tools\\probe-rs\\，或在设置中指定路径。（已搜索: %1）")
            .arg(candidatePaths(explicitPath).join(QStringLiteral(", ")));
    }
    Logger::instance()->info(QStringLiteral("probe-rs: %1").arg(m_binary));
    return QString();
}

ProbeRsRun ProbeRsRunner::run(const QStringList &args, int timeoutMs)
{
    ProbeRsRun result;
    if (m_binary.isEmpty()) {
        if (!ensureBinary().isEmpty())
            return result;
    }

    QProcess process;
    process.setProgram(m_binary);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setWorkingDirectory(QFileInfo(m_binary).absolutePath());

    result.commandLine = QStringLiteral("%1 %2").arg(QFileInfo(m_binary).fileName(), args.join(QLatin1Char(' ')));
    Logger::instance()->debug(QStringLiteral("exec: %1").arg(result.commandLine));

    QElapsedTimer timer;
    timer.start();
    process.start();
    if (!process.waitForStarted(10000)) {
        result.started = false;
        result.stdErr = process.errorString();
        result.elapsedMs = timer.elapsed();
        Logger::instance()->error(QStringLiteral("无法启动 probe-rs: %1").arg(result.stdErr));
        return result;
    }
    result.started = true;

    if (!process.waitForFinished(timeoutMs)) {
        result.timedOut = true;
        process.kill();
        process.waitForFinished(2000);
        result.stdOut = QString::fromLocal8Bit(process.readAllStandardOutput());
        result.stdErr = QString::fromLocal8Bit(process.readAllStandardError());
        result.elapsedMs = timer.elapsed();
        Logger::instance()->warn(QStringLiteral("probe-rs 超时 (%1 ms): %2").arg(timeoutMs).arg(result.commandLine));
        return result;
    }

    result.exitCode = process.exitCode();
    result.stdOut = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.stdErr = QString::fromLocal8Bit(process.readAllStandardError());
    result.elapsedMs = timer.elapsed();

    if (result.exitCode != 0) {
        Logger::instance()->warn(QStringLiteral("probe-rs 退出码 %1: %2").arg(result.exitCode).arg(result.errorText()));
    }
    return result;
}

QString ProbeRsRunner::version(int timeoutMs)
{
    const ProbeRsRun run = this->run({QStringLiteral("--version")}, timeoutMs);
    if (!run.ok())
        return QString();
    m_version = run.stdOut.trimmed();
    return m_version;
}

} // namespace mrd
