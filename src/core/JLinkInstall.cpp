#include "core/JLinkInstall.h"

#include "core/AppSettings.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace mrd {

namespace {

/// Names under which a directory or file can hold a J-Link installation.
const char *kDll64 = "JLink_x64.dll";
const char *kDll32 = "JLinkARM.dll";
const char *kCli = "JLink.exe";
const char *kCli64 = "JLink_x64.exe"; // older releases named the 64-bit CLI this way
const char *kGdb = "JLinkGDBServerCL.exe";

QString firstExisting(const QString &dir, const QStringList &names)
{
    for (const QString &name : names) {
        const QString path = QDir(dir).filePath(name);
        if (QFileInfo(path).isFile())
            return QDir::cleanPath(path);
    }
    return QString();
}

/// Fill in the paths for \a dir, or leave it invalid when nothing J-Link-ish is there.
JLinkInstallation probeDir(const QString &dirPath, const QString &source)
{
    JLinkInstallation install;
    if (dirPath.isEmpty())
        return install;
    const QDir dir(QDir::cleanPath(dirPath));
    if (!dir.exists())
        return install;

    install.dir = dir.absolutePath();
    install.source = source;
    install.dllPath = firstExisting(install.dir, {QString::fromLatin1(kDll64),
                                                  QString::fromLatin1(kDll32)});
    install.cliPath = firstExisting(install.dir, {QString::fromLatin1(kCli),
                                                  QString::fromLatin1(kCli64)});
    install.gdbServerPath = firstExisting(install.dir, {QString::fromLatin1(kGdb)});
    install.version = JLinkInstall::versionFromName(QFileInfo(install.dir).fileName());
    if (install.version.isEmpty())
        install.version = JLinkInstall::versionFromName(dir.dirName());
    return install;
}

/// Accept either a directory or the path of one of the executables inside it.
JLinkInstallation probePathLike(const QString &path, const QString &source)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (info.isFile())
        return probeDir(info.absolutePath(), source);
    return probeDir(path, source);
}

QStringList environmentCandidates()
{
    // Deliberately generous: there is no official variable, so cover the spellings people
    // actually use. Each may hold either an install directory or a path to JLink.exe.
    static const QStringList names = {
        QStringLiteral("JLINK_PATH"),        QStringLiteral("JLINK_HOME"),
        QStringLiteral("JLINK_INSTALL_DIR"), QStringLiteral("SEGGER_JLINK_PATH"),
        QStringLiteral("SEGGER_JLINK_HOME"), QStringLiteral("JLINK_DIR"),
    };
    QStringList out;
    for (const QString &name : names) {
        const QString value = qEnvironmentVariable(name.toUtf8().constData()).trimmed();
        if (!value.isEmpty())
            out << value;
    }
    return out;
}

/// Directories on PATH that actually contain a JLink executable.
QStringList pathCandidates()
{
    QStringList out;
    const QString pathVar = qEnvironmentVariable("PATH");
    for (const QString &entry : pathVar.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        const QString dir = entry.trimmed();
        if (dir.isEmpty())
            continue;
        if (QFileInfo(QDir(dir).filePath(QString::fromLatin1(kCli))).isFile()
            || QFileInfo(QDir(dir).filePath(QString::fromLatin1(kCli64))).isFile())
            out << dir;
    }
    return out;
}

QStringList registryCandidates()
{
    QStringList out;
#ifdef Q_OS_WIN
    // A 64-bit SEGGER installer writes the native view; a 32-bit one lands in
    // WOW6432Node, and a per-user install goes to HKCU. Read all of them.
    static const QStringList keys = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\SEGGER\\J-Link"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\SEGGER\\J-Link"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\SEGGER\\J-Link"),
    };
    for (const QString &key : keys) {
        QSettings reg(key, QSettings::NativeFormat);
        const QString value = reg.value(QStringLiteral("InstallPath")).toString();
        if (!value.isEmpty() && QFileInfo::exists(value))
            out << QDir::cleanPath(value);
    }
#endif
    return out;
}

QStringList seggerDirectories()
{
    QStringList out;
    const QStringList roots = {
        qEnvironmentVariable("ProgramFiles") + QStringLiteral("/SEGGER"),
        qEnvironmentVariable("ProgramFiles(x86)") + QStringLiteral("/SEGGER"),
        qEnvironmentVariable("ProgramW6432") + QStringLiteral("/SEGGER"),
    };
    for (const QString &root : roots) {
        const QDir base(root);
        if (root.isEmpty() || !base.exists())
            continue;
        QStringList names = base.entryList({QStringLiteral("JLink*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        // Newest first: V826 before V796. The version parser ignores names it cannot read,
        // which conveniently sorts "JLink" (the plain symlink-ish dir) last.
        std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
            return JLinkInstall::versionFromName(a) > JLinkInstall::versionFromName(b);
        });
        for (const QString &name : names)
            out << QDir::cleanPath(base.filePath(name));
    }
    return out;
}

QStringList localCandidates()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return {QDir::cleanPath(appDir + QStringLiteral("/tools/jlink")),
            QDir::cleanPath(appDir + QStringLiteral("/../tools/jlink")),
            QDir::cleanPath(appDir + QStringLiteral("/../../tools/jlink")),
            QDir::cleanPath(QDir::currentPath() + QStringLiteral("/tools/jlink"))};
}

} // namespace

bool JLinkInstallation::hasDll64() const
{
    return !dllPath.isEmpty()
           && QFileInfo(dllPath).fileName().compare(QString::fromLatin1(kDll64),
                                                    Qt::CaseInsensitive) == 0;
}

bool JLinkInstallation::hasDll32Only() const
{
    return !dllPath.isEmpty() && !hasDll64();
}

QString JLinkInstallation::describe() const
{
    if (!valid())
        return QStringLiteral("未安装 J-Link");
    QStringList parts;
    parts << dir;
    if (!version.isEmpty())
        parts << version;
    if (hasDll64())
        parts << QStringLiteral("64 位 DLL");
    else if (hasDll32Only())
        parts << QStringLiteral("仅 32 位 DLL（本程序为 64 位，不能用）");
    else
        parts << QStringLiteral("无 DLL");
    if (hasCli())
        parts << QStringLiteral("有 JLink.exe");
    if (hasGdbServer())
        parts << QStringLiteral("有 GDB Server");
    if (!source.isEmpty())
        parts << source;
    return parts.join(QStringLiteral(" · "));
}

QString JLinkInstall::versionFromName(const QString &name)
{
    // "JLink_V826" -> "V8.26", "V796c" -> "V7.96", "JLink" -> "".
    static const QRegularExpression re(QStringLiteral("V(\\d)(\\d{2})"));
    const QRegularExpressionMatch match = re.match(name);
    if (match.hasMatch())
        return QStringLiteral("V%1.%2").arg(match.captured(1), match.captured(2));
    // A bare number, as in "826".
    static const QRegularExpression bare(QStringLiteral("^(\\d)(\\d{2})$"));
    const QRegularExpressionMatch bareMatch = bare.match(name);
    if (bareMatch.hasMatch())
        return QStringLiteral("V%1.%2").arg(bareMatch.captured(1), bareMatch.captured(2));
    return QString();
}

QVector<JLinkInstallation> JLinkInstall::discover()
{
    QVector<JLinkInstallation> out;
    QHash<QString, bool> seen;
    const auto add = [&out, &seen](const JLinkInstallation &install) {
        if (!install.valid() || install.dir.isEmpty())
            return;
        const QString key = install.dir.toLower();
        if (seen.contains(key))
            return; // first source wins, and the list is built best-first
        seen.insert(key, true);
        out.append(install);
    };

    // 1. the app's own setting
    const QString configured = AppSettings::jlinkPath();
    if (!configured.isEmpty())
        add(probePathLike(configured, QStringLiteral("设置")));

    // 2. environment variables, then PATH
    for (const QString &value : environmentCandidates())
        add(probePathLike(value, QStringLiteral("环境变量")));
    for (const QString &dir : pathCandidates())
        add(probeDir(dir, QStringLiteral("PATH")));

    // 3. registry
    for (const QString &dir : registryCandidates())
        add(probeDir(dir, QStringLiteral("注册表")));

    // 4. Program Files
    for (const QString &dir : seggerDirectories())
        add(probeDir(dir, QStringLiteral("Program Files")));

    // 5. a copy shipped or dropped next to the app
    for (const QString &dir : localCandidates())
        add(probeDir(dir, QStringLiteral("随程序")));

    return out;
}

JLinkInstallation JLinkInstall::best()
{
    const QVector<JLinkInstallation> all = discover();
    if (all.isEmpty())
        return {};
    // Prefer an installation that can serve BOTH backends; then one with a 64-bit DLL;
    // then one with only the CLI/GDB server (still perfectly usable, via the GDB backend).
    for (const auto &install : all) {
        if (install.hasDll64() && install.hasGdbServer())
            return install;
    }
    for (const auto &install : all) {
        if (install.hasDll64())
            return install;
    }
    for (const auto &install : all) {
        if (install.hasGdbServer() || install.hasCli())
            return install;
    }
    return all.first();
}

QStringList JLinkInstall::environmentReport()
{
    QStringList out;
    for (const QString &name : {QStringLiteral("JLINK_PATH"), QStringLiteral("JLINK_HOME"),
                                QStringLiteral("JLINK_INSTALL_DIR"),
                                QStringLiteral("SEGGER_JLINK_PATH"),
                                QStringLiteral("SEGGER_JLINK_HOME"),
                                QStringLiteral("JLINK_DIR")}) {
        const QString value = qEnvironmentVariable(name.toUtf8().constData());
        if (!value.isEmpty())
            out << QStringLiteral("%1=%2").arg(name, value);
    }
    const QStringList onPath = pathCandidates();
    if (!onPath.isEmpty())
        out << QStringLiteral("PATH 中: %1").arg(onPath.join(QStringLiteral(", ")));
    return out;
}

QString JLinkInstall::summary()
{
    const QVector<JLinkInstallation> all = discover();
    if (all.isEmpty())
        return QStringLiteral("未找到 J-Link 安装（环境变量 / 注册表 / PATH / Program Files 都没有）");
    QStringList lines;
    for (const auto &install : all)
        lines << install.describe();
    return lines.join(QStringLiteral(" | "));
}

} // namespace mrd
