#include "core/PartTable.h"

#include "core/Logger.h"
#include "core/ResourcePaths.h"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QJsonArray>
#include <QFileInfo>
#include <QJsonObject>

namespace mrd {

namespace {

QHash<QString, PartTable::Entry> g_parts;
QString g_path;
bool g_loaded = false;

} // namespace

namespace {

/// Read data/parts.json into \a out.
bool loadTable(const QString &path, QHash<QString, PartTable::Entry> *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        if (error)
            *error = QStringLiteral("%1 解析失败: %2").arg(path, parseError.errorString());
        return false;
    }

    // { "parts": { "<part>": {...} } }
    const QJsonObject root = doc.object().value(QStringLiteral("parts")).toObject();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        PartTable::Entry entry;
        entry.part = it.key();
        entry.pack = o.value(QStringLiteral("pack")).toString();
        entry.package = o.value(QStringLiteral("package")).toString();
        entry.svd = o.value(QStringLiteral("svd")).toString();
        entry.sram = o.value(QStringLiteral("sram")).toString();
        entry.ccmSram = o.value(QStringLiteral("ccmSram")).toString();
        entry.backupSram = o.value(QStringLiteral("backupSram")).toString();
        entry.flash = o.value(QStringLiteral("flash")).toString();
        entry.clock = o.value(QStringLiteral("clock")).toString();
        entry.flashBytes = o.value(QStringLiteral("flashBytes")).toInt();
        entry.ramBytes = o.value(QStringLiteral("ramBytes")).toInt();
        entry.ioCount = o.value(QStringLiteral("ioCount")).toInt();
        const QJsonObject caps = o.value(QStringLiteral("capabilities")).toObject();
        for (auto cap = caps.constBegin(); cap != caps.constEnd(); ++cap) {
            if (cap.value().isDouble())
                entry.capabilities.insert(cap.key(), cap.value().toInt());
        }
        entry.valid = true;
        (*out)[entry.part] = entry;
    }
    return true;
}

} // namespace

int PartTable::load(QString *error)
{
    if (g_loaded)
        return g_parts.size();
    g_loaded = true;
    g_parts.clear();

    const QString path = ResourcePaths::find(QStringLiteral("parts.json"));
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("未找到 data/parts.json");
        return 0;
    }

    loadTable(path, &g_parts, error);
    g_path = path;

    if (g_parts.isEmpty()) {
        if (error)
            *error = QStringLiteral("型号表为空（%1）").arg(path);
        return 0;
    }

    Logger::instance()->debug(QStringLiteral("料号表: %1 个型号").arg(g_parts.size()));
    return g_parts.size();
}

bool PartTable::loaded()
{
    return g_loaded;
}

int PartTable::count()
{
    return g_parts.size();
}

QString PartTable::sourcePath()
{
    return g_path;
}

PartTable::Entry PartTable::lookup(const QString &partName)
{
    if (partName.isEmpty())
        return {};
    load();

    // 1. Exact hit ("N32H473CE").
    const auto exact = g_parts.constFind(partName);
    if (exact != g_parts.constEnd())
        return exact.value();

    // 2. Longest table entry that prefixes the queried name, so a full order code
    //    ("N32H473CEU7", "N32G452CBL7") resolves to its pack entry.
    const Entry *best = nullptr;
    for (auto it = g_parts.constBegin(); it != g_parts.constEnd(); ++it) {
        if (!partName.startsWith(it.key(), Qt::CaseInsensitive))
            continue;
        if (!best || it.key().size() > best->part.size())
            best = &it.value();
    }
    if (best)
        return *best;

    // 3. Name is a truncated form of table entries ("N32H473" -> many). Only
    //    usable when exactly one matches; a family is not a part.
    const QStringList candidates = matching(partName);
    if (candidates.size() == 1)
        return g_parts.value(candidates.first());
    return {};
}

QString PartTable::seriesOf(const QString &partName)
{
    // The series is the vendor prefix plus the model digits: N32 + one letter + 3-4
    // digits. Everything after that is package size, flash size and temperature code.
    // Kept as one regex so the rule is stated once instead of being re-derived per
    // caller (which is how "N32G455CEU7" ended up classified four different ways).
    static const QRegularExpression re(QStringLiteral("^(N32[A-Z]\\d{3,4})"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(partName.trimmed());
    if (!match.hasMatch())
        return QString();
    return match.captured(1).toUpper();
}

QStringList PartTable::series()
{
    load();
    QStringList out;
    for (const QString &part : g_parts.keys()) {
        if (!isOrderCode(part))
            continue;
        const QString s = seriesOf(part);
        if (!s.isEmpty() && !out.contains(s))
            out << s;
    }
    out.sort();
    return out;
}

QStringList PartTable::partsInSeries(const QString &series)
{
    load();
    const QString wanted = seriesOf(series);
    if (wanted.isEmpty())
        return {};
    QStringList out;
    for (auto it = g_parts.constBegin(); it != g_parts.constEnd(); ++it) {
        if (!isOrderCode(it.key()))
            continue;
        if (seriesOf(it.key()) == wanted)
            out << it.key();
    }
    out.sort();
    return out;
}

QStringList PartTable::packagesOf(const QString &partName)
{
    const Entry entry = lookup(partName);
    if (!entry.valid || entry.package.isEmpty())
        return {};
    QStringList out;
    for (const QString &piece : entry.package.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString name = piece.trimmed();
        if (!name.isEmpty() && !out.contains(name))
            out << name;
    }
    out.sort();
    return out;
}

bool PartTable::isOrderCode(const QString &partName)
{
    load();
    const auto it = g_parts.constFind(partName);
    if (it == g_parts.constEnd())
        return false;
    const Entry &entry = it.value();
    // Placeholder rows ("N32H785xIx7") and the bare ARM core templates ("ARMCM4") are
    // not order codes: they carry no package and cannot be selected as hardware.
    if (entry.part != partName)
        return false;
    if (partName.startsWith(QLatin1String("ARM")))
        return false;
    if (partName.contains(QLatin1Char('x')))
        return false;
    return true;
}

QString PartTable::formatSize(int bytes)
{
    if (bytes <= 0)
        return QString();
    // Flash/RAM sizes are powers of two; print one decimal only when the value is
    // not a whole number of KB (e.g. 1536 KB stays "1536 KB", 1.5 MB becomes "1.5 MB").
    if (bytes >= 1024 * 1024) {
        const double mb = double(bytes) / (1024.0 * 1024.0);
        if (qFuzzyCompare(mb, qRound(mb)))
            return QStringLiteral("%1 MB").arg(qRound(mb));
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }
    const double kb = double(bytes) / 1024.0;
    if (qFuzzyCompare(kb, qRound(kb)))
        return QStringLiteral("%1 KB").arg(qRound(kb));
    return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
}

QString PartTable::memorySummary(int flashBytes, int ramBytes)
{
    QStringList parts;
    if (flashBytes > 0)
        parts << QStringLiteral("Flash %1").arg(formatSize(flashBytes));
    if (ramBytes > 0)
        parts << QStringLiteral("RAM %1").arg(formatSize(ramBytes));
    return parts.join(QStringLiteral("  /  "));
}

QStringList PartTable::matching(const QString &stem)
{
    load();
    QStringList hits;
    for (auto it = g_parts.constBegin(); it != g_parts.constEnd(); ++it) {
        if (it.key().startsWith(stem, Qt::CaseInsensitive))
            hits << it.key();
    }
    hits.sort();
    return hits;
}

} // namespace mrd
