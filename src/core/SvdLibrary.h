#pragma once

#include "core/Result.h"
#include "core/SvdTypes.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

/// Vendor fact sheet for one exact part number, from `svd/manifest.json`.
///
/// A DFP pack ships one SVD per *sub-series* (N32H473.svd covers ten packages),
/// so the manifest is the only place that ties a part number to the SVD the
/// vendor assigned *and* to that particular package's pin/IO counts.
struct SvdManifestEntry
{
    QString part;
    QString svdPath;      ///< absolute path to the .svd
    QString series;
    QString package;      ///< empty when the vendor lists several packages
    QStringList packages; ///< every package the part is offered in
    int pinCount = 0;     ///< 0 when ambiguous (e.g. LQFP100/LQFP128)
    int ioCount = 0;
};

/// Lightweight metadata for one `.svd` file in the library.
struct SvdEntry
{
    QString path;
    QString deviceName;
    QString vendor;
    QString version;
    qint64 size = 0;
    qint64 mtime = 0;

    /// "STM32F103xx (STM32F1xx_DFP)"
    QString displayName() const;
};

/// Index over every CMSIS-SVD file found in the configured roots.
///
/// The index is cached in `<AppData>/svd-index.json` and invalidated per file by
/// (size, mtime), so scanning a large Keil pack directory stays fast.
class SvdLibrary
{
public:
    /// Built-in search roots: `<exe>/svd`, `<AppData>/svd`, and the local Keil
    /// CMSIS pack directory when it exists.
    static QStringList defaultRoots();
    static QString cacheFilePath();

    SvdLibrary() = default;

    void setRoots(const QStringList &roots);
    void addRoot(const QString &root);
    QStringList roots() const { return m_roots; }

    bool loadCache();
    bool saveCache() const;

    /// Walk all roots; returns the number of indexed files.
    int scan(bool force = false);

    const QVector<SvdEntry> &entries() const { return m_entries; }
    int count() const { return m_entries.size(); }

    QVector<SvdEntry> search(const QString &text) const;

    /// Rank SVD files that may describe \a deviceName (e.g. "STM32F103C8T6").
    QVector<SvdEntry> candidatesFor(const QString &deviceName) const;

    Result<SvdDevice> load(const QString &path) const;
    Result<SvdDevice> load(const SvdEntry &entry) const { return load(entry.path); }

    // --- vendor manifest ---------------------------------------------------
    /// Read `<root>/manifest.json` (written by scripts/extract-svd-packs.py).
    int loadManifest();
    int manifestCount() const { return m_manifest.size(); }
    /// Exact match first, then a unique prefix match ("N32G452CB" against the
    /// manifest's "N32G452CBL7").
    bool lookupPart(const QString &part, SvdManifestEntry *out = nullptr,
                    QStringList *ambiguous = nullptr) const;

private:
    QStringList m_roots;
    QVector<SvdEntry> m_entries;
    QHash<QString, SvdManifestEntry> m_manifest;
    QHash<QString, SvdEntry> m_cached;
};

} // namespace mrd
