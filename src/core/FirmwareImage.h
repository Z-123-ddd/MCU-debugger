#pragma once

#include "core/ImageBlob.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace mrd {

/// A raw firmware image plus the address range it has to be written to.
///
/// A `.bin` carries no address information, so the range has to be *derived*. Doing it
/// from measurement rather than from a filename convention is what lets the same rule
/// work for another part number: the part table supplies the candidate window (RAM and
/// Flash extents) and the image's own vector table either confirms it or does not.
///
/// The rule, verified on a real SG6010C image and its live target:
///
///   * **start** -- a Cortex-M image begins with `[initial SP][reset handler]`. The
///     start address is accepted when that SP lands inside the part's SRAM and the
///     reset handler lands inside its Flash. This is a real check, not a default: an
///     image built for a different layout fails it instead of being written to the
///     wrong place.
///   * **end** -- the last byte that is not the erased value 0xFF, rounded up to the
///     part's erase granularity. The file may be shorter than the part, and may carry
///     trailing padding; neither should decide where the write stops.
///
/// The range is then checked against the part's Flash size, because a 281 KB image on a
/// 256 KB part must be refused *before* anything is erased.
struct FirmwareImage
{
    /// Erase granularity used to round the end up. N32 parts erase in 4 KB sectors,
    /// which is also the most common size on Cortex-M; rounding to it means the write
    /// never ends inside a partially-erased sector.
    static constexpr int kEraseGranularity = 4 * 1024;
    /// The erased state of NOR flash.
    static constexpr quint8 kErasedByte = 0xFF;

    QString path;
    /// When the image was assembled (bootloader + application), the merged bytes live in
    /// a temporary file: `loadbin` needs a real path, not a buffer. Empty for a plain
    /// single-file burn.
    QString scratchPath;
    QByteArray bytes;          ///< the whole file
    quint32 startAddress = 0;  ///< where byte 0 has to go (erase range start)
    quint32 endAddress = 0;    ///< one past the last meaningful byte, sector-aligned
    /// Lowest address that actually carries data. Equal to \a startAddress unless the
    /// user widened the erase range downwards; the vector table lives here, not at
    /// \a startAddress, which is why validation uses it.
    quint32 contentStartAddress = 0;
    quint32 initialSp = 0;     ///< first vector table word
    quint32 resetHandler = 0;  ///< second word, Thumb bit cleared
    /// Non-fatal notes worth showing: trailing padding found, unaligned end, etc.
    QStringList notes;

    /// Payload size actually written (end - start).
    int span() const { return int(endAddress - startAddress); }
    /// Raw file size, which may exceed the payload when the file has padding.
    int fileSize() const { return bytes.size(); }
    bool isValid() const { return startAddress != 0 && endAddress > startAddress; }

    /// "0x08000000 .. 0x08039610（230 KB）"
    QString rangeLabel() const;

    /// The file to hand to the flash loader: the merged temp file when there is one,
    /// otherwise the original.
    QString burnPath() const { return scratchPath.isEmpty() ? path : scratchPath; }
    /// True when this image was assembled from more than one source.
    bool isMerged() const { return !scratchPath.isEmpty(); }
    /// Delete the merged temp file. Called once the burn (or the report) is done.
    void discardScratch();

    /// Overwrite the *original* firmware file with this (merged) image, keeping the name.
    ///
    /// Only for a merged image whose bytes were just verified on the target: replacing the
    /// source with anything less would silently change the release artifact. The previous
    /// content is copied to "<name>.bak" first, because an in-place overwrite of the file
    /// an operator is about to ship has to be reversible.
    ///
    /// Returns the path written, or empty with \a error set.
    QString writeBackOverSource(QString *error) const;
};

/// How a firmware image is assembled before burning.
///
/// The reason this exists: a board with a bootloader needs two images written in one
/// pass -- the bootloader at the start of flash and the application at an offset it was
/// linked for. Merging them here (rather than burning twice) means one erase, one
/// program, one verify, and no window where the board has a new bootloader but the old
/// application.
///
/// Every address is overridable because a second board revision may use a different
/// split. The defaults are what the supplied images actually use.
struct FirmwarePlan
{
    /// The application image: normally a raw `.bin`. Also accepts `.hex`/`.axf`.
    QString applicationPath;
    /// Where the application has to sit. 0 means "decide from context": the flash base
    /// when there is no bootloader, otherwise `kDefaultAppOffset` past it.
    quint32 applicationBase = 0;

    /// Optional bootloader (`.axf`, `.elf` or `.hex`). Empty means a plain burn.
    QString bootloaderPath;
    /// Where the bootloader goes. 0 means "keep the address it was linked for".
    quint32 bootloaderBase = 0;

    /// Overall range actually erased and written. 0 means "derive from the content".
    /// Both are exposed in the UI so an operator can extend or pin them.
    quint32 overallStart = 0;
    quint32 overallEnd = 0;

    /// Offset used when  applicationBase is left at 0 with a bootloader present.
    /// 32 KB is the conventional boundary on these parts and matches the supplied
    /// bootloader, which occupies 0x08000000..0x08002ED8.
    static constexpr quint32 kDefaultAppOffset = 0x8000;

    bool hasBootloader() const { return !bootloaderPath.isEmpty(); }
    /// The application base that will actually be used for a given flash base.
    quint32 effectiveApplicationBase(quint32 flashBase) const
    {
        if (applicationBase != 0)
            return applicationBase;
        return hasBootloader() ? flashBase + kDefaultAppOffset : flashBase;
    }
};

/// Build the image described by \a plan and validate it against the part.
///
/// The checks are the same ones the single-file path performs -- vector table inside the
/// memory map, image fits the flash -- applied to the *merged* result, because after
/// merging there is only one image to write.
Result<FirmwareImage> buildFirmwareImage(const FirmwarePlan &plan, quint32 flashBase,
                                        int flashBytes, quint32 ramBase, int ramBytes);

/// Same, using the part table entry for \a partName.
Result<FirmwareImage> buildFirmwareImageForPart(const FirmwarePlan &plan,
                                                const QString &partName);

/// Load and analyse a firmware file for a given part.
///
/// \a flashBase  candidate start address (Cortex-M parts use 0x08000000);
/// \a flashBytes part's Flash size from the part table -- the range is validated
///               against it, and a too-large image is reported as an error, not a warning;
/// \a ramBase / \a ramBytes the SRAM window the initial stack pointer must fall in.
///
/// Never touches the target: this is pure file analysis, so it is safe to run before
/// asking the user to confirm a burn.
Result<FirmwareImage> loadFirmwareImage(const QString &path, quint32 flashBase, int flashBytes,
                                        quint32 ramBase, int ramBytes);

/// Same, using the part table entry for \a partName (falls back to the Cortex-M
/// defaults 0x08000000 / 0x20000000 when the part is unknown).
Result<FirmwareImage> loadFirmwareImageForPart(const QString &path, const QString &partName);

} // namespace mrd
