#pragma once

#include "core/Result.h"
#include "core/SvdTypes.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mrd {

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

private:
    QStringList m_roots;
    QVector<SvdEntry> m_entries;
    QHash<QString, SvdEntry> m_cached;
};

} // namespace mrd
