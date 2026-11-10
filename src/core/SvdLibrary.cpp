#include "core/SvdLibrary.h"

#include "core/Logger.h"
#include "core/SvdParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <algorithm>

namespace mrd {

namespace {

/// Uppercase, keep only [A-Z0-9] - used for fuzzy device name matching.
QString normalize(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c.isLetterOrNumber())
            out.append(c.toUpper());
    }
    return out;
}

/// Read only the header of an SVD file (name/vendor/version live before
/// <peripherals>), so indexing a 200-file pack stays cheap.
bool probeSvdHeader(const QString &path, SvdEntry *entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QXmlStreamReader reader(&file);
    bool sawName = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token != QXmlStreamReader::StartElement)
            continue;

        const auto name = reader.name();
        if (name == QLatin1String("peripherals"))
            break; // header finished
        if (name == QLatin1String("name") && !sawName) {
            entry->deviceName = reader.readElementText().trimmed();
            sawName = true;
        } else if (name == QLatin1String("vendor")) {
            entry->vendor = reader.readElementText().trimmed();
        } else if (name == QLatin1String("version")) {
            entry->version = reader.readElementText().trimmed();
        }
    }

    if (!sawName)
        entry->deviceName = QFileInfo(path).completeBaseName();
    return true;
}

} // namespace

QString SvdEntry::displayName() const
{
    if (vendor.isEmpty())
        return deviceName;
    return QStringLiteral("%1  (%2)").arg(deviceName, vendor);
}

QString SvdLibrary::cacheFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/svd-index.json");
}

QStringList SvdLibrary::defaultRoots()
{
    QStringList roots;
    const QString exeDir = QCoreApplication::applicationDirPath();

    // <exe>/svd plus the development/deploy layouts: scripts/fetch-n32-svds.py
    // writes to <repo>/svd, which sits two levels above build/bin.
    const QStringList relativeSvd = {
        QStringLiteral("/svd"),
        QStringLiteral("/../svd"),
        QStringLiteral("/../../svd"),
        QStringLiteral("/../../../svd"),
    };
    for (const QString &rel : relativeSvd) {
        const QString candidate = QDir::cleanPath(exeDir + rel);
        if (QFileInfo::exists(candidate) && !roots.contains(candidate))
            roots << candidate;
    }

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    roots << appData + QStringLiteral("/svd");

    const QStringList keilCandidates = {
        QStringLiteral("C:/Keil_v5/ARM/PACK"),
        QStringLiteral("C:/ProgramData/Keil_v5/ARM/PACK"),
    };
    for (const auto &candidate : keilCandidates) {
        if (QFileInfo::exists(candidate))
            roots << candidate;
    }
    return roots;
}

void SvdLibrary::setRoots(const QStringList &roots)
{
    m_roots.clear();
    for (const auto &root : roots) {
        const QString cleaned = QDir::cleanPath(root.trimmed());
        if (!cleaned.isEmpty() && !m_roots.contains(cleaned))
            m_roots << cleaned;
    }
}

void SvdLibrary::addRoot(const QString &root)
{
    const QString cleaned = QDir::cleanPath(root.trimmed());
    if (!cleaned.isEmpty() && !m_roots.contains(cleaned))
        m_roots << cleaned;
}

bool SvdLibrary::loadCache()
{
    QFile file(cacheFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return false;

    m_cached.clear();
    for (const auto &value : doc.array()) {
        const QJsonObject o = value.toObject();
        SvdEntry entry;
        entry.path = o.value(QStringLiteral("path")).toString();
        entry.deviceName = o.value(QStringLiteral("device")).toString();
        entry.vendor = o.value(QStringLiteral("vendor")).toString();
        entry.version = o.value(QStringLiteral("version")).toString();
        entry.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
        entry.mtime = static_cast<qint64>(o.value(QStringLiteral("mtime")).toDouble());
        if (!entry.path.isEmpty())
            m_cached.insert(entry.path, entry);
    }
    return !m_cached.isEmpty();
}

bool SvdLibrary::saveCache() const
{
    QJsonArray array;
    for (const auto &entry : m_entries) {
        QJsonObject o;
        o.insert(QStringLiteral("path"), entry.path);
        o.insert(QStringLiteral("device"), entry.deviceName);
        o.insert(QStringLiteral("vendor"), entry.vendor);
        o.insert(QStringLiteral("version"), entry.version);
        o.insert(QStringLiteral("size"), static_cast<double>(entry.size));
        o.insert(QStringLiteral("mtime"), static_cast<double>(entry.mtime));
        array.append(o);
    }

    const QString path = cacheFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    return true;
}

int SvdLibrary::scan(bool force)
{
    if (!force)
        loadCache();

    m_entries.clear();
    QSet<QString> seen;

    for (const auto &root : m_roots) {
        const QFileInfo rootInfo(root);
        if (!rootInfo.exists() || !rootInfo.isDir())
            continue;

        QDirIterator it(root, {QStringLiteral("*.svd"), QStringLiteral("*.SVD")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = QDir::cleanPath(it.next());
            if (seen.contains(path))
                continue;
            seen.insert(path);

            const QFileInfo info(path);
            SvdEntry entry;
            entry.path = path;
            entry.size = info.size();
            entry.mtime = static_cast<qint64>(info.lastModified().toMSecsSinceEpoch());

            const auto cached = m_cached.constFind(path);
            if (!force && cached != m_cached.constEnd() && cached->size == entry.size
                && cached->mtime == entry.mtime && !cached->deviceName.isEmpty()) {
                entry.deviceName = cached->deviceName;
                entry.vendor = cached->vendor;
                entry.version = cached->version;
            } else if (!probeSvdHeader(path, &entry)) {
                continue;
            }

            m_entries << entry;
        }
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const SvdEntry &a, const SvdEntry &b) {
        const int cmp = a.deviceName.compare(b.deviceName, Qt::CaseInsensitive);
        return cmp == 0 ? a.path < b.path : cmp < 0;
    });

    m_cached.clear();
    for (const auto &entry : m_entries)
        m_cached.insert(entry.path, entry);

    saveCache();
    Logger::instance()->info(QStringLiteral("SVD 库: %1 个文件 / %2 个根目录")
                                 .arg(m_entries.size())
                                 .arg(m_roots.size()));
    return m_entries.size();
}

QVector<SvdEntry> SvdLibrary::search(const QString &text) const
{
    const QString needle = text.trimmed();
    if (needle.isEmpty())
        return m_entries;

    QVector<SvdEntry> hits;
    for (const auto &entry : m_entries) {
        if (entry.deviceName.contains(needle, Qt::CaseInsensitive)
            || entry.vendor.contains(needle, Qt::CaseInsensitive)
            || entry.path.contains(needle, Qt::CaseInsensitive))
            hits << entry;
    }
    return hits;
}

QVector<SvdEntry> SvdLibrary::candidatesFor(const QString &deviceName) const
{
    const QString target = normalize(deviceName);
    if (target.isEmpty())
        return {};

    struct Scored
    {
        int score = 0;
        SvdEntry entry;
    };
    QVector<Scored> scored;

    for (const auto &entry : m_entries) {
        QString candidate = normalize(entry.deviceName);
        if (candidate.isEmpty())
            continue;

        // SVD naming convention: family files end in "xx" (STM32F103xx).
        while (candidate.endsWith(QLatin1Char('X')) && candidate.size() > 3)
            candidate.chop(1);

        int score = 0;
        if (target == candidate)
            score = 1000;
        else if (target.startsWith(candidate))
            score = 500 + candidate.size();
        else if (candidate.startsWith(target))
            score = 400 + target.size();
        else if (candidate.contains(target) || target.contains(candidate))
            score = 100 + std::min(candidate.size(), target.size());

        if (score > 0)
            scored.append({score, entry});
    }

    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        return a.score > b.score;
    });

    QVector<SvdEntry> out;
    out.reserve(scored.size());
    for (const auto &item : scored)
        out << item.entry;
    return out;
}

// ---------------------------------------------------------------------------
// Vendor manifest (scripts/extract-svd-packs.py writes svd/manifest.json)
// ---------------------------------------------------------------------------

int SvdLibrary::loadManifest()
{
    m_manifest.clear();
    if (m_roots.isEmpty())
        return 0;

    for (const QString &root : m_roots) {
        const QString path = QDir::cleanPath(root + QStringLiteral("/manifest.json"));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (doc.isNull()) {
            Logger::instance()->warn(QStringLiteral("%1 解析失败: %2").arg(path, parseError.errorString()));
            continue;
        }

        const QJsonObject devices = doc.object().value(QStringLiteral("devices")).toObject();
        for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
            const QJsonObject o = it.value().toObject();
            SvdManifestEntry entry;
            entry.part = o.value(QStringLiteral("part")).toString(it.key());
            entry.series = o.value(QStringLiteral("pack")).toString();
            entry.package = o.value(QStringLiteral("package")).toString();
            entry.pinCount = o.value(QStringLiteral("pinCount")).toInt();
            entry.ioCount = o.value(QStringLiteral("ioCount")).toInt();
            for (const auto &pkg : o.value(QStringLiteral("packages")).toArray())
                entry.packages << pkg.toString();

            // The manifest path is relative to the repository root; resolve it
            // against every ancestor of the svd/ directory that contains it.
            const QString relative = o.value(QStringLiteral("svdPath")).toString();
            if (relative.isEmpty())
                continue;
            entry.svdPath = QDir::cleanPath(root + QStringLiteral("/../") + relative);
            if (!QFileInfo::exists(entry.svdPath))
                entry.svdPath = QDir::cleanPath(root + QLatin1Char('/') + QFileInfo(relative).fileName());

            m_manifest.insert(entry.part, entry);
        }
        Logger::instance()->info(QStringLiteral("SVD 厂商清单: %1 条 (%2)")
                                     .arg(m_manifest.size())
                                     .arg(path));
        return m_manifest.size();
    }
    return 0;
}

bool SvdLibrary::lookupPart(const QString &part, SvdManifestEntry *out, QStringList *ambiguous) const
{
    if (ambiguous)
        ambiguous->clear();
    const QString target = normalize(part);
    if (target.isEmpty() || m_manifest.isEmpty())
        return false;

    // 1) exact part number
    for (auto it = m_manifest.constBegin(); it != m_manifest.constEnd(); ++it) {
        if (normalize(it.key()) == target) {
            if (out)
                *out = it.value();
            return true;
        }
    }

    // 2) unique prefix match: J-Link may report "N32G452CB" while the pack spells
    //    the full ordering code "N32G452CBL7".
    QStringList hits;
    for (auto it = m_manifest.constBegin(); it != m_manifest.constEnd(); ++it) {
        const QString key = normalize(it.key());
        if (key.startsWith(target) || target.startsWith(key))
            hits << it.key();
    }
    if (hits.size() == 1) {
        if (out)
            *out = m_manifest.value(hits.first());
        return true;
    }
    if (hits.size() > 1 && ambiguous)
        *ambiguous = hits;
    return false;
}

Result<SvdDevice> SvdLibrary::load(const QString &path) const
{
    return SvdParser::parseFile(path);
}

} // namespace mrd
