#include "core/JLinkDeviceName.h"

#include "core/Logger.h"
#include "core/ResourcePaths.h"

#include <QFile>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace mrd {

namespace {

/// Names harvested from JLinkARM.dll (`data/jlink_devices.json`), lower-cased.
/// Loaded lazily; empty means "no list", in which case the pure rule is used.
QSet<QString> &knownSet()
{
    static QSet<QString> set;
    static bool loaded = false;
    if (loaded)
        return set;
    loaded = true;

    const QString path = ResourcePaths::find(QStringLiteral("jlink_devices.json"));
    if (path.isEmpty())
        return set;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return set;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    for (const auto &value : doc.object().value(QStringLiteral("devices")).toArray())
        set.insert(value.toString().toLower());
    Logger::instance()->debug(
        QStringLiteral("J-Link 器件名表: %1 条 (%2)").arg(set.size()).arg(path));
    return set;
}

/// Trailing suffix SEGGER does not use: package finish + temperature ("L7",
/// "Q7", "S7", "T7", "U7", "W7", "E7"...). Two chars, letter+digit.
QString stripSuffix(const QString &orderCode)
{
    static const QRegularExpression suffix(QStringLiteral("[A-Z][0-9]$"));
    if (orderCode.size() > 4 && suffix.match(orderCode).hasMatch())
        return orderCode.left(orderCode.size() - 2);
    return orderCode;
}

} // namespace

QStringList JLinkDeviceName::candidates(const QString &orderCode)
{
    const QString code = orderCode.trimmed().toUpper();
    if (code.isEmpty())
        return {};

    QStringList out;

    // 1. The catalogue name itself, in case SEGGER kept the full order code.
    out << code;

    // 2. Without the finish/temperature suffix -- what SEGGER actually stores.
    const QString stripped = stripSuffix(code);
    if (stripped != code && !out.contains(stripped))
        out << stripped;

    // 3. If a known-name list is available, drop anything it does not contain so
    //    the caller never hands J-Link a name that will fail.
    const QSet<QString> &known = knownSet();
    if (!known.isEmpty()) {
        QStringList filtered;
        for (const QString &name : out) {
            if (known.contains(name.toLower()))
                filtered << name;
        }
        if (!filtered.isEmpty())
            return filtered;
        // Nothing matched: rather than returning a wrong name, return the stripped
        // form so the caller still has something to try and logs the miss.
        return stripped == code ? QStringList{code} : QStringList{stripped};
    }
    return out;
}

bool JLinkDeviceName::isValidFor(const QString &jlinkName, const QString &orderCode)
{
    const QSet<QString> &known = knownSet();
    if (known.isEmpty())
        return !jlinkName.trimmed().isEmpty();
    return known.contains(jlinkName.trimmed().toLower());
}

QStringList JLinkDeviceName::knownNames(const QString &family)
{
    const QString stem = family.trimmed().toUpper();
    QStringList out;
    for (const QString &name : knownSet()) {
        if (name.toUpper().startsWith(stem))
            out << name.toUpper();
    }
    out.sort();
    return out;
}

} // namespace mrd
