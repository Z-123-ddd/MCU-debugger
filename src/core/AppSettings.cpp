#include "core/AppSettings.h"

#include "core/Logger.h"

#include "core/SvdLibrary.h"

#include <QSettings>

namespace mrd {

namespace {
constexpr const char *kSvRoots = "svd/roots";
constexpr const char *kLastSvd = "svd/lastPath";
constexpr const char *kBackend = "probe/backend";
constexpr const char *kJlinkPath = "probe/jlinkPath";
constexpr const char *kProbeRsPath = "probe/probeRsPath";
constexpr const char *kProbeSelector = "probe/selector";
constexpr const char *kInterface = "probe/interface";
constexpr const char *kSpeed = "probe/speedKHz";
constexpr const char *kChip = "probe/chip";
constexpr const char *kBindings = "probe/bindings";
constexpr const char *kPollMs = "session/pollIntervalMs";
constexpr const char *kPackageDir = "session/packageDir";
constexpr const char *kTheme = "ui/theme";
constexpr const char *kAutoConnect = "ui/autoConnect";
constexpr const char *kPinned = "ui/pinned";
} // namespace

QStringList AppSettings::svdRoots()
{
    QSettings s;
    const QStringList stored = s.value(QLatin1String(kSvRoots)).toStringList();
    if (stored.isEmpty())
        return SvdLibrary::defaultRoots();
    return stored;
}

void AppSettings::setSvdRoots(const QStringList &roots)
{
    QSettings s;
    s.setValue(QLatin1String(kSvRoots), roots);
}

QString AppSettings::lastSvdPath()
{
    return QSettings().value(QLatin1String(kLastSvd)).toString();
}

void AppSettings::setLastSvdPath(const QString &path)
{
    QSettings().setValue(QLatin1String(kLastSvd), path);
}

QString AppSettings::probeBackend()
{
    const QString stored = QSettings().value(QLatin1String(kBackend)).toString();
    if (stored == QLatin1String("probe-rs") || stored == QLatin1String("jlink"))
        return stored;
    return QStringLiteral("jlink"); // J-Link keeps the official USB driver in place
}

void AppSettings::setProbeBackend(const QString &backend)
{
    QSettings().setValue(QLatin1String(kBackend), backend);
}

QString AppSettings::jlinkPath()
{
    return QSettings().value(QLatin1String(kJlinkPath)).toString();
}

void AppSettings::setJlinkPath(const QString &path)
{
    QSettings().setValue(QLatin1String(kJlinkPath), path);
}

QString AppSettings::probeRsPath()
{
    return QSettings().value(QLatin1String(kProbeRsPath)).toString();
}

void AppSettings::setProbeRsPath(const QString &path)
{
    QSettings().setValue(QLatin1String(kProbeRsPath), path);
}

QString AppSettings::probeSelector()
{
    return QSettings().value(QLatin1String(kProbeSelector)).toString();
}

void AppSettings::setProbeSelector(const QString &selector)
{
    QSettings().setValue(QLatin1String(kProbeSelector), selector);
}

QString AppSettings::interfaceName()
{
    const QString name = QSettings().value(QLatin1String(kInterface), QStringLiteral("SWD")).toString();
    return name.compare(QLatin1String("JTAG"), Qt::CaseInsensitive) == 0 ? QStringLiteral("JTAG")
                                                                        : QStringLiteral("SWD");
}

void AppSettings::setInterfaceName(const QString &name)
{
    QSettings().setValue(QLatin1String(kInterface), name);
}

int AppSettings::speedKHz()
{
    return QSettings().value(QLatin1String(kSpeed), 4000).toInt();
}

void AppSettings::setSpeedKHz(int speed)
{
    QSettings().setValue(QLatin1String(kSpeed), speed);
}

QString AppSettings::chipName()
{
    return QSettings().value(QLatin1String(kChip)).toString();
}

void AppSettings::migrateChipNameSetting()
{
    // Version marker so this runs exactly once per installation.
    constexpr const char *kMarker = "probe/chipModeVersion";
    constexpr int kCurrent = 2; // 2 = "empty means auto"

    QSettings settings;
    if (settings.value(QLatin1String(kMarker), 1).toInt() >= kCurrent)
        return;

    // Older builds wrote the detected part here unconditionally, so the stored
    // value is the last board rather than a deliberate choice. Drop it.
    settings.remove(QLatin1String(kChip));
    settings.setValue(QLatin1String(kMarker), kCurrent);
    Logger::instance()->info(
        QStringLiteral("已重置保存的芯片名：旧版本会把它当成手动指定，导致换芯片后仍按上一次的型号"));
}

void AppSettings::setChipName(const QString &name)
{
    QSettings().setValue(QLatin1String(kChip), name);
}

QHash<QString, QString> AppSettings::bindings()
{
    QHash<QString, QString> out;
    QSettings s;
    const QStringList list = s.value(QLatin1String(kBindings)).toStringList();
    for (const QString &item : list) {
        const int sep = item.indexOf(QLatin1Char('|'));
        if (sep > 0)
            out.insert(item.left(sep), item.mid(sep + 1));
    }
    return out;
}

void AppSettings::setBindings(const QHash<QString, QString> &bindings)
{
    QStringList list;
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it)
        list << QStringLiteral("%1|%2").arg(it.key(), it.value());
    QSettings().setValue(QLatin1String(kBindings), list);
}

QString AppSettings::bindingForSn(const QString &sn)
{
    return bindings().value(sn);
}

void AppSettings::setBindingForSn(const QString &sn, const QString &svdPath)
{
    auto map = bindings();
    map.insert(sn, svdPath);
    setBindings(map);
}

int AppSettings::pollIntervalMs()
{
    return QSettings().value(QLatin1String(kPollMs), 200).toInt();
}

void AppSettings::setPollIntervalMs(int ms)
{
    QSettings().setValue(QLatin1String(kPollMs), ms);
}

QString AppSettings::packageDir()
{
    return QSettings().value(QLatin1String(kPackageDir)).toString();
}

void AppSettings::setPackageDir(const QString &dir)
{
    QSettings().setValue(QLatin1String(kPackageDir), dir);
}

QString AppSettings::theme()
{
    return QSettings().value(QLatin1String(kTheme), QStringLiteral("light")).toString();
}

void AppSettings::setTheme(const QString &theme)
{
    QSettings().setValue(QLatin1String(kTheme), theme);
}

bool AppSettings::autoConnectOnStartup()
{
    return QSettings().value(QLatin1String(kAutoConnect), false).toBool();
}

void AppSettings::setAutoConnectOnStartup(bool enabled)
{
    QSettings().setValue(QLatin1String(kAutoConnect), enabled);
}

QStringList AppSettings::pinnedPeripherals()
{
    return QSettings().value(QLatin1String(kPinned)).toStringList();
}

void AppSettings::setPinnedPeripherals(const QStringList &names)
{
    QSettings().setValue(QLatin1String(kPinned), names);
}

} // namespace mrd
