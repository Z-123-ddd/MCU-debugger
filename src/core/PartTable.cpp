#include "core/PartTable.h"

#include "core/Logger.h"
#include "core/ResourcePaths.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace mrd {

namespace {

QHash<QString, PartTable::Entry> g_parts;
QString g_path;
bool g_loaded = false;

} // namespace

int PartTable::load(QString *error)
{
    if (g_loaded)
        return g_parts.size();
    g_loaded = true;
    g_parts.clear();

    g_path = ResourcePaths::find(QStringLiteral("parts.json"));
    if (g_path.isEmpty()) {
        // Not an error: without the table the app simply keeps using the family
        // data from device_ids.json and shows no flash/RAM figures.
        Logger::instance()->info(
            QStringLiteral("未找到 data/parts.json（运行 scripts/sync-part-table.py 生成）；"
                           "Flash/RAM 与逐型号能力将不可用"));
        if (error)
            *error = QStringLiteral("未找到 data/parts.json");
        return 0;
    }

    QFile file(g_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开 %1: %2").arg(g_path, file.errorString());
        return 0;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        if (error)
            *error = QStringLiteral("%1 解析失败: %2").arg(g_path, parseError.errorString());
        return 0;
    }

    const QJsonObject root = doc.object().value(QStringLiteral("parts")).toObject();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        Entry entry;
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
        g_parts.insert(entry.part, entry);
    }

    Logger::instance()->info(QStringLiteral("料号表: %1 个型号 (%2)")
                                 .arg(g_parts.size())
                                 .arg(g_path));
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
