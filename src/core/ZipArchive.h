#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QHash>
#include <QString>

namespace mrd {

/// Minimal read-only ZIP reader.
///
/// Exists because FMU packages are ZIP archives and the tool must not depend on Qt's
/// private `QZipReader` (its header is explicitly "may change or be removed"). Only what
/// an FMU needs is implemented: the central directory, stored and deflated entries, and
/// raw extraction to memory. No encryption, no ZIP64, no writing.
///
/// ZIP64 is deliberately unsupported: an FMU with a >4 GB member is not a thing, and
/// silently mis-reading a ZIP64 archive is worse than refusing it.
class ZipArchive
{
public:
    struct Entry
    {
        QString path;        ///< archive path, forward slashes, as stored
        quint32 compressedBytes = 0;
        quint32 uncompressedBytes = 0;
        quint16 method = 0;  ///< 0 = stored, 8 = deflate
        quint32 localHeaderOffset = 0;
    };

    /// Open an archive from disk. Fails with a message rather than returning a half-valid
    /// object, so a corrupt .fmu is reported at the point it is selected.
    static Result<ZipArchive> open(const QString &path);

    bool isValid() const { return m_valid; }
    QString path() const { return m_path; }
    /// Every entry, in central-directory order.
    QVector<Entry> entries() const { return m_entries; }
    QStringList paths() const;

    /// True when an entry with this exact path exists.
    bool contains(const QString &entryPath) const;
    /// Extract one entry to memory (empty on failure).
    QByteArray read(const QString &entryPath) const;
    /// Extract the first entry whose path matches \a prefix and ends with \a suffix.
    /// Used to find `binaries/win64/*.dll` without hard-coding the library name.
    QByteArray readFirstMatching(const QString &prefix, const QString &suffix,
                                 QString *matchedPath = nullptr) const;
    /// Entry names under a directory prefix ("binaries/win64/").
    QStringList entriesUnder(const QString &prefix) const;

private:
    bool load(const QByteArray &bytes);

    QString m_path;
    QByteArray m_bytes;
    QVector<Entry> m_entries;
    QHash<QString, int> m_index;
    bool m_valid = false;
};

/// Inflate a raw deflate stream (as stored in a ZIP member). Exposed so the FMU loader
/// and the tests can share one implementation.
Result<QByteArray> inflateRaw(const QByteArray &input, int expectedSize);

} // namespace mrd
