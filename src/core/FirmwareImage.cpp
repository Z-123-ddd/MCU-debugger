#include "core/FirmwareImage.h"

#include "core/PartTable.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace mrd {
namespace {

/// Cortex-M: Flash lives at 0x08000000 and SRAM at 0x20000000 on essentially every
/// vendor part. These are only the *fallback* window; whenever the part table knows the
/// real sizes they replace it.
constexpr quint32 kDefaultFlashBase = 0x08000000u;
constexpr quint32 kDefaultRamBase = 0x20000000u;
constexpr int kDefaultFlashBytes = 256 * 1024;
constexpr int kDefaultRamBytes = 64 * 1024;

quint32 roundUp(quint32 value, quint32 granularity)
{
    const quint32 remainder = value % granularity;
    return remainder == 0 ? value : value + (granularity - remainder);
}

/// Validate a prepared image against the part's memory map and derive its end.
///
/// Shared by the single-file and the merged path: after merging there is only one image
/// to write, so both must apply exactly the same rules or one of them would be the weak
/// link.
Status validateAndFinish(FirmwareImage *image, quint32 flashBase, int flashBytes,
                         quint32 ramBase, int ramBytes)
{
    if (image->bytes.size() < 8)
        return Status::failure(QStringLiteral("镜像太小，不像固件（%1 字节）").arg(image->bytes.size()));

    // The vector table sits at the *content* start: when the erase range was widened
    // downwards, the buffer begins with filler that is not part of any image.
    const quint32 vectorOffset = image->contentStartAddress > image->startAddress
        ? image->contentStartAddress - image->startAddress
        : 0u;
    if (vectorOffset + 8u > quint32(image->bytes.size()))
        return Status::failure(QStringLiteral("镜像里找不到向量表"));

    std::memcpy(&image->initialSp, image->bytes.constData() + vectorOffset, 4);
    quint32 rawReset = 0;
    std::memcpy(&rawReset, image->bytes.constData() + vectorOffset + 4, 4);
    image->resetHandler = rawReset & ~1u;

    const quint32 ramEnd = ramBase + quint32(qMax(0, ramBytes));
    const quint32 flashEnd = flashBase + quint32(qMax(0, flashBytes));

    if (image->initialSp < ramBase || image->initialSp > ramEnd) {
        return Status::failure(
            QStringLiteral("初始堆栈指针 0x%1 不在本型号 SRAM 范围 0x%2..0x%3 内")
                .arg(image->initialSp, 8, 16, QLatin1Char('0'))
                .arg(ramBase, 8, 16, QLatin1Char('0'))
                .arg(ramEnd, 8, 16, QLatin1Char('0')));
    }
    if (image->resetHandler < flashBase || image->resetHandler >= flashEnd) {
        return Status::failure(
            QStringLiteral("复位向量 0x%1 不在本型号 Flash 范围 0x%2..0x%3 内")
                .arg(image->resetHandler, 8, 16, QLatin1Char('0'))
                .arg(flashBase, 8, 16, QLatin1Char('0'))
                .arg(flashEnd, 8, 16, QLatin1Char('0')));
    }
    if ((rawReset & 1u) == 0) {
        image->notes << QStringLiteral("复位向量的 Thumb 位为 0，通常应为奇数地址");
    }

    // The image is already positioned (startAddress fixed by the caller for a merged
    // build), so the end comes from the buffer, not from re-deriving the start.
    const quint32 rawEnd = image->startAddress + quint32(image->bytes.size());
    image->endAddress = roundUp(rawEnd, quint32(FirmwareImage::kEraseGranularity));
    if (image->endAddress != rawEnd) {
        image->notes << QStringLiteral("结束地址按 %1 KB 擦除粒度上对齐：0x%2 -> 0x%3")
                            .arg(FirmwareImage::kEraseGranularity / 1024)
                            .arg(rawEnd, 8, 16, QLatin1Char('0'))
                            .arg(image->endAddress, 8, 16, QLatin1Char('0'));
    }

    if (image->endAddress > flashEnd) {
        const int overflow = int(image->endAddress - flashEnd);
        return Status::failure(
            QStringLiteral("需要 %1（%2），但本型号 Flash 只有 %3，超出 %4 字节。拒绝烧录。")
                .arg(image->rangeLabel(), QStringLiteral("%1 字节").arg(image->span()))
                .arg(PartTable::formatSize(flashBytes))
                .arg(overflow));
    }
    return Status::success();
}

} // namespace

QString FirmwareImage::rangeLabel() const
{
    if (!isValid())
        return QStringLiteral("(无效)");
    return QStringLiteral("%1 .. %2（%3）")
        .arg(QStringLiteral("0x%1").arg(startAddress, 8, 16, QLatin1Char('0')))
        .arg(QStringLiteral("0x%1").arg(endAddress, 8, 16, QLatin1Char('0')))
        .arg(PartTable::formatSize(span()));
}

QString FirmwareImage::writeBackOverSource(QString *error) const
{
    if (error)
        error->clear();
    if (!isMerged()) {
        if (error)
            *error = QStringLiteral("只有合并过的镜像才能回写");
        return QString();
    }
    if (path.isEmpty() || bytes.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有可回写的目标文件");
        return QString();
    }
    // A .hex source cannot be replaced by raw bytes: the file would still be named .hex
    // but no longer be Intel HEX, and every tool downstream would fail to parse it.
    if (QFileInfo(path).suffix().compare(QLatin1String("bin"), Qt::CaseInsensitive) != 0) {
        if (error)
            *error = QStringLiteral("源文件不是 .bin（%1），不覆盖")
                         .arg(QFileInfo(path).fileName());
        return QString();
    }

    // Keep the previous content: this overwrites the artifact the operator ships.
    const QString backup = path + QStringLiteral(".bak");
    if (QFile::exists(backup))
        QFile::remove(backup);
    if (!QFile::copy(path, backup)) {
        if (error)
            *error = QStringLiteral("无法备份原文件到 %1").arg(backup);
        return QString();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return QString();
    }
    const qint64 written = file.write(bytes);
    file.close();
    if (written != bytes.size()) {
        if (error)
            *error = QStringLiteral("写入 %1 只完成 %2/%3 字节")
                         .arg(path).arg(written).arg(bytes.size());
        return QString();
    }
    return path;
}

void FirmwareImage::discardScratch()
{
    if (scratchPath.isEmpty())
        return;
    QFile::remove(scratchPath);
    scratchPath.clear();
}

Result<FirmwareImage> loadFirmwareImage(const QString &path, quint32 flashBase, int flashBytes,
                                        quint32 ramBase, int ramBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<FirmwareImage>::failure(
            QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
    }
    FirmwareImage image;
    image.path = path;
    image.bytes = file.readAll();
    file.close();
    image.startAddress = flashBase;
    image.contentStartAddress = flashBase;

    // A raw .bin may carry trailing 0xFF padding; trimming it keeps the erase range
    // honest (a 256 KB file that is really 232 KB should not erase 256 KB).
    int lastMeaningful = -1;
    for (int i = image.bytes.size() - 1; i >= 0; --i) {
        if (quint8(image.bytes.at(i)) != FirmwareImage::kErasedByte) {
            lastMeaningful = i;
            break;
        }
    }
    if (lastMeaningful < 0) {
        return Result<FirmwareImage>::failure(
            QStringLiteral("%1 全部是 0xFF（空白镜像），没有可烧录的内容")
                .arg(QFileInfo(path).fileName()));
    }
    const int padding = image.bytes.size() - (lastMeaningful + 1);
    if (padding > 0) {
        image.bytes.truncate(lastMeaningful + 1);
        image.notes << QStringLiteral("文件末尾有 %1 字节 0xFF 填充，不会写入").arg(padding);
    }

    const Status status = validateAndFinish(&image, flashBase, flashBytes, ramBase, ramBytes);
    if (!status)
        return Result<FirmwareImage>::failure(status.error);
    return Result<FirmwareImage>::success(image);
}

Result<FirmwareImage> buildFirmwareImage(const FirmwarePlan &plan, quint32 flashBase,
                                        int flashBytes, quint32 ramBase, int ramBytes)
{
    if (plan.applicationPath.isEmpty())
        return Result<FirmwareImage>::failure(QStringLiteral("未指定固件文件"));

    QVector<ImageBlob> blobs;
    QStringList notes;

    // --- bootloader -------------------------------------------------------
    if (plan.hasBootloader()) {
        auto boot = loadImageBlob(plan.bootloaderPath);
        if (!boot) {
            return Result<FirmwareImage>::failure(
                QStringLiteral("bootloader：%1").arg(boot.error));
        }
        if (plan.bootloaderBase != 0) {
            boot.value.moveTo(plan.bootloaderBase);
            notes << QStringLiteral("bootloader 已按输入地址放到 0x%1")
                         .arg(plan.bootloaderBase, 8, 16, QLatin1Char('0'));
        }
        notes << QStringLiteral("bootloader %1：%2")
                     .arg(boot.value.origin, boot.value.rangeLabel());
        blobs.append(boot.value);
    }

    // --- application ------------------------------------------------------
    const quint32 appBase = plan.effectiveApplicationBase(flashBase);
    const QString appSuffix = QFileInfo(plan.applicationPath).suffix().toLower();
    const bool appIsRawBinary = appSuffix == QLatin1String("bin");

    ImageBlob appBlob;
    if (appIsRawBinary) {
        // A .bin carries no addresses at all, so its position is entirely decided by the
        // plan. Reading it here (rather than through loadImageBlob) is what makes the
        // application base meaningful instead of an error.
        QFile raw(plan.applicationPath);
        if (!raw.open(QIODevice::ReadOnly)) {
            return Result<FirmwareImage>::failure(
                QStringLiteral("无法读取 %1：%2").arg(plan.applicationPath, raw.errorString()));
        }
        QByteArray payload = raw.readAll();
        raw.close();

        int lastMeaningful = -1;
        for (int i = payload.size() - 1; i >= 0; --i) {
            if (quint8(payload.at(i)) != FirmwareImage::kErasedByte) {
                lastMeaningful = i;
                break;
            }
        }
        if (lastMeaningful < 0) {
            return Result<FirmwareImage>::failure(
                QStringLiteral("%1 全部是 0xFF（空白镜像）")
                    .arg(QFileInfo(plan.applicationPath).fileName()));
        }
        const int padding = payload.size() - (lastMeaningful + 1);
        if (padding > 0) {
            payload.truncate(lastMeaningful + 1);
            notes << QStringLiteral("固件末尾有 %1 字节 0xFF 填充，不会写入").arg(padding);
        }

        ImageSegment segment;
        segment.address = appBase;
        segment.bytes = payload;
        appBlob.segments.append(segment);
        appBlob.origin = QFileInfo(plan.applicationPath).fileName();
    } else {
        // .hex / .axf: the file knows its own addresses, but the plan still wins -- an
        // operator pointing the application at a different base is an override, not a
        // mistake to be ignored.
        auto loaded = loadImageBlob(plan.applicationPath);
        if (!loaded) {
            return Result<FirmwareImage>::failure(QStringLiteral("固件：%1").arg(loaded.error));
        }
        appBlob = loaded.value;
        appBlob.moveTo(appBase);
    }

    notes << QStringLiteral("固件 %1：放到 0x%2，%3 字节")
                 .arg(appBlob.origin)
                 .arg(appBase, 8, 16, QLatin1Char('0'))
                 .arg(appBlob.totalBytes());

    // Does the application actually belong at appBase? Its first two words are
    // [initial SP][reset handler], and the reset handler is an absolute address baked in
    // at link time. If it does not fall inside the region the image now occupies, the
    // image was linked for a different base and would jump into whatever sits there --
    // for a bootloader layout, into the bootloader itself.
    {
        const ImageSegment *lowest = nullptr;
        for (const ImageSegment &segment : appBlob.segments) {
            if (!lowest || segment.address < lowest->address)
                lowest = &segment;
        }
        if (lowest && lowest->address == appBase && lowest->bytes.size() >= 8) {
            quint32 reset = 0;
            std::memcpy(&reset, lowest->bytes.constData() + 4, 4);
            reset &= ~1u;
            const quint32 appEnd = appBase + quint32(appBlob.totalBytes());
            if (reset < appBase || reset >= appEnd) {
                return Result<FirmwareImage>::failure(
                    QStringLiteral(
                        "固件被链接在 %1，但要求放到 %2：它的复位向量指向 %1，"
                        "不在放置范围内。\n"
                        "裸 .bin 里的绝对地址是链接时定死的，直接搬到别的地址会让 MCU "
                        "跳到错误的位置（这里会跳进 bootloader）。\n"
                        "请用 %2 重新链接后再烧录。")
                        .arg(QStringLiteral("0x%1").arg(reset, 8, 16, QLatin1Char('0')),
                             QStringLiteral("0x%1").arg(appBase, 8, 16, QLatin1Char('0'))));
            }
        }
    }

    blobs.append(appBlob);

    // --- merge ------------------------------------------------------------
    quint32 start = plan.overallStart;
    quint32 end = plan.overallEnd;
    const auto merged = mergeBlobs(blobs, &start, &end, &notes);
    if (!merged) {
        return Result<FirmwareImage>::failure(merged.error);
    }

    FirmwareImage image;
    image.startAddress = start;
    image.bytes = merged.value;
    {
        // Where content actually begins, which the plan's start may precede.
        quint32 contentStart = 0;
        bool have = false;
        for (const ImageBlob &blob : blobs) {
            if (blob.isEmpty())
                continue;
            const quint32 lowest = blob.lowestAddress();
            contentStart = have ? qMin(contentStart, lowest) : lowest;
            have = true;
        }
        image.contentStartAddress = have ? contentStart : start;
    }

    // The merged buffer is what `loadbin` must point at, and it is not the application
    // file any more. Written next to the application so it is easy to find afterwards;
    // FirmwareBurner removes it once the burn is done.
    const QString stem = QFileInfo(plan.applicationPath).completeBaseName();
    const QString scratch = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("mrd_merged_%1_%2.bin")
                                              .arg(stem)
                                              .arg(QCoreApplication::applicationPid()));
    QFile scratchFile(scratch);
    if (!scratchFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Result<FirmwareImage>::failure(
            QStringLiteral("无法写合并后的镜像 %1：%2").arg(scratch, scratchFile.errorString()));
    }
    scratchFile.write(image.bytes);
    scratchFile.close();
    image.scratchPath = scratch;
    image.path = plan.applicationPath;

    image.notes = notes;
    image.notes << QStringLiteral("合并结果：%1（%2 字节）")
                       .arg(QStringLiteral("0x%1 .. 0x%2").arg(start, 8, 16, QLatin1Char('0'))
                                .arg(end, 8, 16, QLatin1Char('0')))
                       .arg(image.bytes.size());

    // The vector table of the merged image belongs to whichever image starts lowest.
    // Writing it at `start` is exactly what the target will boot from, so the same
    // validation applies.
    const Status status = validateAndFinish(&image, flashBase, flashBytes, ramBase, ramBytes);
    if (!status) {
        QFile::remove(scratch);
        return Result<FirmwareImage>::failure(status.error);
    }
    return Result<FirmwareImage>::success(image);
}

Result<FirmwareImage> loadFirmwareImageForPart(const QString &path, const QString &partName)
{
    const PartTable::Entry entry = PartTable::lookup(partName);

    const quint32 flashBase = kDefaultFlashBase;
    const quint32 ramBase = kDefaultRamBase;
    const int flashBytes = entry.valid && entry.flashBytes > 0 ? entry.flashBytes
                                                              : kDefaultFlashBytes;
    const int ramBytes = entry.valid && entry.ramBytes > 0 ? entry.ramBytes
                                                          : kDefaultRamBytes;

    Result<FirmwareImage> result =
        loadFirmwareImage(path, flashBase, flashBytes, ramBase, ramBytes);
    if (result.ok) {
        if (entry.valid) {
            result.value.notes.prepend(
                QStringLiteral("按型号 %1 校验：Flash %2 / RAM %3")
                    .arg(entry.part, PartTable::formatSize(flashBytes),
                         PartTable::formatSize(ramBytes)));
        } else {
            result.value.notes.prepend(
                QStringLiteral("型号 %1 不在器件表里，按 Cortex-M 默认窗口校验"
                               "（Flash %2 / RAM %3）")
                    .arg(partName.isEmpty() ? QStringLiteral("(未知)") : partName,
                         PartTable::formatSize(flashBytes), PartTable::formatSize(ramBytes)));
        }
    }
    return result;
}

Result<FirmwareImage> buildFirmwareImageForPart(const FirmwarePlan &plan, const QString &partName)
{
    const PartTable::Entry entry = PartTable::lookup(partName);

    const quint32 flashBase = kDefaultFlashBase;
    const quint32 ramBase = kDefaultRamBase;
    const int flashBytes = entry.valid && entry.flashBytes > 0 ? entry.flashBytes
                                                              : kDefaultFlashBytes;
    const int ramBytes = entry.valid && entry.ramBytes > 0 ? entry.ramBytes
                                                          : kDefaultRamBytes;

    Result<FirmwareImage> result =
        buildFirmwareImage(plan, flashBase, flashBytes, ramBase, ramBytes);
    if (result.ok) {
        result.value.notes.prepend(
            entry.valid
                ? QStringLiteral("按型号 %1 校验：Flash %2 / RAM %3")
                      .arg(entry.part, PartTable::formatSize(flashBytes),
                           PartTable::formatSize(ramBytes))
                : QStringLiteral("型号 %1 不在器件表里，按 Cortex-M 默认窗口校验")
                      .arg(partName.isEmpty() ? QStringLiteral("(未知)") : partName));
    }
    return result;
}

} // namespace mrd
