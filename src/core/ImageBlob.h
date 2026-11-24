#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace mrd {

/// One contiguous run of bytes destined for a flash address.
///
/// A firmware image is not necessarily one block: an ELF has several loadable
/// sections, and an Intel HEX file has records that may be sparse. Keeping the runs
/// separate until the final merge is what lets two images be combined without pretending
/// the holes between them are real data.
struct ImageSegment
{
    quint32 address = 0;
    QByteArray bytes;

    quint32 endAddress() const { return address + quint32(bytes.size()); }
};

/// A set of segments, in no particular order.
struct ImageBlob
{
    QVector<ImageSegment> segments;
    /// Where the bytes came from, for messages ("bootloader.hex").
    QString origin;

    bool isEmpty() const { return segments.isEmpty(); }
    int totalBytes() const;
    /// Lowest address across all segments (0 when empty).
    quint32 lowestAddress() const;
    /// Highest end address across all segments (0 when empty).
    quint32 highestEnd() const;
    /// Shift every segment by \a offset. Used to place an image at the address the
    /// user typed instead of the one it was linked for.
    void moveTo(quint32 baseAddress);
    QString rangeLabel() const;
};

/// Parse an Intel HEX file (`.hex`/`.ihx`).
///
/// Supported record types: 00 data, 01 EOF, 02 extended segment, 04 extended linear,
/// 05 start linear. Records with a bad checksum are refused rather than skipped -- a
/// corrupt bootloader is the one thing that must never be written silently.
Result<ImageBlob> parseIntelHex(const QString &path);

/// Load the loadable parts of an ELF (`.axf` / `.elf`).
///
/// Only sections that are allocated (SHF_ALLOC) and actually occupy file bytes are
/// taken: `.bss` has an address but no contents, and including it would write whatever
/// happened to be in the buffer over the target's RAM-initialisation area.
Result<ImageBlob> loadElfBlob(const QString &path);

/// Load either kind by extension, so the caller does not need to care.
Result<ImageBlob> loadImageBlob(const QString &path);

/// Flatten \a blobs into one buffer covering [\a start, \a end).
///
/// Gaps are filled with 0xFF (the erased state) so the buffer can be written as a single
/// image. Overlapping segments are an error: two images claiming the same address is
/// exactly the mistake that produces a bootloader with an application's vector table
/// spliced into it.
///
/// [\a start, \a end) must cover every segment; when \a start is 0 the lowest segment
/// address is used, and when \a end is 0 the highest end address.
Result<QByteArray> mergeBlobs(const QVector<ImageBlob> &blobs, quint32 *startInOut,
                              quint32 *endInOut, QStringList *notes);

} // namespace mrd
