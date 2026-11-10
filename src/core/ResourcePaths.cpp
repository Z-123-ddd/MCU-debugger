#include "core/ResourcePaths.h"

#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace mrd {

QStringList ResourcePaths::dataRoots()
{
    QStringList roots;
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList relative = {
        QStringLiteral("/data"),
        QStringLiteral("/../data"),
        QStringLiteral("/../../data"),
        QStringLiteral("/../../../data"),
    };
    for (const auto &rel : relative) {
        const QString dir = QDir::cleanPath(exeDir + rel);
        if (!roots.contains(dir))
            roots << dir;
    }

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    roots << QDir::cleanPath(appData + QStringLiteral("/data"));
    roots << QDir::cleanPath(QDir::currentPath() + QStringLiteral("/data"));
    return roots;
}

QString ResourcePaths::find(const QString &relativePath)
{
    const QString rel = relativePath;
    for (const auto &root : dataRoots()) {
        const QString candidate = QDir::cleanPath(root + QLatin1Char('/') + rel);
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

QString ResourcePaths::deviceIdDatabase()
{
    return find(QStringLiteral("device_ids.json"));
}

QString ResourcePaths::packageDirectory()
{
    const QString configured = AppSettings::packageDir();
    if (!configured.isEmpty() && QFileInfo::exists(configured))
        return configured;

    const QString bundled = find(QStringLiteral("packages"));
    if (!bundled.isEmpty())
        return bundled;

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            + QStringLiteral("/packages");
    QDir().mkpath(appData);
    return appData;
}

} // namespace mrd
