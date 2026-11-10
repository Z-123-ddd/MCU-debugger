#include "core/JLinkRunner.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace mrd {

namespace {

QString exeName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("JLink.exe");
#else
    return QStringLiteral("JLinkExe");
#endif
}

/// "JLink_V826" -> 826 (0 when unparsable).
int versionNumber(const QString &dirName)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)"));
    const auto match = re.match(dirName);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString installPathFromRegistry()
{
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\SEGGER\\J-Link"),
                  QSettings::NativeFormat);
    const QString value = reg.value(QStringLiteral("InstallPath")).toString();
    if (!value.isEmpty() && QFileInfo::exists(value))
        return QDir::cleanPath(value);
#endif
    return QString();
}

QStringList seggerDirectories()
{
    QStringList dirs;
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    const QString seggerRoot = programFiles + QStringLiteral("/SEGGER");
    QDir root(seggerRoot);
    if (!root.exists())
        return dirs;

    struct Entry
    {
        QString path;
        int version = 0;
        bool plain = false;
    };
    QVector<Entry> entries;
    const QStringList names = root.entryList(QStringList() << QStringLiteral("JLink*"), QDir::Dirs);
    for (const QString &name : names) {
        Entry entry;
        entry.path = QDir::cleanPath(seggerRoot + QLatin1Char('/') + name);
        entry.plain = (name.compare(QLatin1String("JLink"), Qt::CaseInsensitive) == 0);
        entry.version = versionNumber(name);
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.version != b.version ? a.version > b.version : a.path < b.path;
    });
    for (const auto &entry : entries)
        dirs << entry.path;
    return dirs;
}

} // namespace

QString JLinkRunner::defaultInstallDirectory()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/tools/jlink"));
}

QStringList JLinkRunner::candidatePaths(const QString &explicitPath)
{
    QStringList paths;
    const auto addFile = [&paths](const QString &path) {
        if (path.isEmpty())
            return;
        const QString cleaned = QDir::cleanPath(path);
        if (!paths.contains(cleaned) && QFileInfo(cleaned).isFile())
            paths << cleaned;
    };
    const auto addDir = [&addFile](const QString &dir) {
        if (dir.isEmpty())
            return;
        addFile(dir + QLatin1Char('/') + exeName());
        addFile(dir + QStringLiteral("/JLink_x64.exe"));
    };

    if (!explicitPath.isEmpty()) {
        if (QFileInfo(explicitPath).isDir())
            addDir(explicitPath);
        else
            addFile(explicitPath);
    }

    addDir(installPathFromRegistry());
    for (const auto &dir : seggerDirectories())
        addDir(dir);

    // A private copy next to the app (deploy/tools/jlink), plus dev-tree levels.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList toolRoots = {
        QDir::cleanPath(appDir + QStringLiteral("/tools")),
        QDir::cleanPath(appDir + QStringLiteral("/../tools")),
        QDir::cleanPath(appDir + QStringLiteral("/../../tools")),
        QDir::cleanPath(QDir::currentPath() + QStringLiteral("/tools")),
    };
    for (const auto &root : toolRoots) {
        addDir(root + QStringLiteral("/jlink"));
        addDir(root);
    }

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("JLink"));
    if (!onPath.isEmpty())
        addFile(onPath);

    return paths;
}

QString JLinkRunner::locate(const QString &explicitPath)
{
    const QStringList candidates = candidatePaths(explicitPath);
    return candidates.isEmpty() ? QString() : candidates.first();
}

Result<QString> JLinkRunner::runScript(const QString &executable, const QStringList &lines, int timeoutMs,
                                       const QStringList &extraArgs)
{
    if (!QFileInfo(executable).isFile())
        return Result<QString>::failure(QStringLiteral("JLink.exe 不存在: %1").arg(executable));

    QTemporaryDir tempDir;
    if (!tempDir.isValid())
        return Result<QString>::failure(QStringLiteral("无法创建临时目录"));
    const QString scriptPath = tempDir.path() + QStringLiteral("/cmd.jlink");

    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text))
        return Result<QString>::failure(QStringLiteral("无法写入脚本: %1").arg(scriptPath));
    scriptFile.write(QStringLiteral("q\n").prepend(lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toLocal8Bit());
    scriptFile.close();

    QStringList args = extraArgs;
    args << QStringLiteral("-CommanderScript") << scriptPath;

    QProcess process;
    process.setProgram(executable);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(tempDir.path()); // keep J-Link logs out of Program Files
    process.start();
    if (!process.waitForStarted(10000))
        return Result<QString>::failure(QStringLiteral("无法启动 JLink.exe: %1").arg(process.errorString()));

    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return Result<QString>::failure(QStringLiteral("J-Link 命令超时 (%1 ms)").arg(timeoutMs));
    }

    const QString output = QString::fromLocal8Bit(process.readAll());
    return Result<QString>::success(output);
}

Result<JLinkInstallInfo> JLinkRunner::query(const QString &executable, const QString &device,
                                          int timeoutMs)
{
    JLinkInstallInfo info;
    info.executable = executable;

    // A connect makes the DLL print firmware/hardware/serial information.
    const auto result = runScript(
        executable, {QStringLiteral("ShowEmuList"), QStringLiteral("si SWD"),
                     QStringLiteral("speed 4000"), QStringLiteral("connect")},
        timeoutMs,
        {QStringLiteral("-device"), device.isEmpty() ? QStringLiteral("Cortex-M4") : device,
         QStringLiteral("-if"), QStringLiteral("SWD"), QStringLiteral("-speed"),
         QStringLiteral("4000"), QStringLiteral("-autoconnect"), QStringLiteral("1")});
    if (!result) {
        return Result<JLinkInstallInfo>::failure(result.error);
    }

    const QString &output = result.value;
    info.commander = parseCommanderVersion(output);

    static const QRegularExpression dllRe(QStringLiteral("DLL version\\s+(V[0-9.]+)"));
    if (const auto m = dllRe.match(output); m.hasMatch())
        info.dllVersion = m.captured(1);

    static const QRegularExpression fwRe(QStringLiteral("Firmware:\\s+(.+)"));
    if (const auto m = fwRe.match(output); m.hasMatch())
        info.firmware = m.captured(1).trimmed();

    static const QRegularExpression hwRe(QStringLiteral("Hardware version:\\s+(\\S+)"));
    if (const auto m = hwRe.match(output); m.hasMatch())
        info.hardware = m.captured(1);

    static const QRegularExpression productRe(QStringLiteral("ProductName:\\s+(.+)$"),
                                              QRegularExpression::MultilineOption);
    if (const auto m = productRe.match(output); m.hasMatch())
        info.product = m.captured(1).trimmed();

    info.serial = parseSerial(output);
    Logger::instance()->info(QStringLiteral("J-Link: %1/%2 SN=%3 %4")
                                 .arg(info.firmware, info.hardware, info.serial, info.product));
    info.probeFound = !info.serial.isEmpty();
    if (!info.probeFound)
        info.probeFound = output.contains(QLatin1String("J-Link["));

    return Result<JLinkInstallInfo>::success(info);
}

QVector<quint32> JLinkRunner::parseMemoryValues(const QString &output)
{
    QVector<quint32> values;
    static const QRegularExpression lineRe(QStringLiteral("([0-9A-Fa-f]{1,8})\\s*=\\s*(.+)$"));

    for (const QString &rawLine : output.split(QLatin1Char('\n'))) {
        QString line = rawLine.trimmed();
        // Interactive mode prefixes the prompt: "J-Link>4002C000 = 028BFFFD"
        const int prompt = line.indexOf(QLatin1String("J-Link>"));
        if (prompt >= 0)
            line = line.mid(prompt + 7).trimmed();
        if (line.isEmpty())
            continue;

        const auto match = lineRe.match(line);
        if (!match.hasMatch())
            continue;

        const QStringList tokens = match.captured(2).split(QRegularExpression(QStringLiteral("\\s+")),
                                                          Qt::SkipEmptyParts);
        for (const QString &token : tokens) {
            bool ok = false;
            const quint32 value = token.toUInt(&ok, 16);
            if (ok)
                values.append(value);
        }
    }
    return values;
}

QString JLinkRunner::parseSelectedDevice(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("Device \"([^\"]+)\" selected"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString JLinkRunner::parseCoreName(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("Found (Cortex-[A-Za-z0-9+]+)"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString JLinkRunner::parseSerial(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("S/N:\\s*(\\d+)"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString JLinkRunner::parseCommanderVersion(const QString &output)
{
    static const QRegularExpression re(QStringLiteral("J-Link Commander\\s+(V[0-9.]+)"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

} // namespace mrd
