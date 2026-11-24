#include "core/ImageBlob.h"

#include "core/ElfFile.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace mrd {
namespace {

/// ELF section flags/type we care about.
constexpr quint32 kShfAlloc = 0x2;
constexpr quint32 kShtNobits = 8;

/// Intel HEX record types.
enum HexRecord : int {
    kHexData = 0x00,
    kHexEof = 0x01,
    kHexExtendedSegment = 0x02,
    kHexStartSegment = 0x03,
    kHexExtendedLinear = 0x04,
    kHexStartLinear = 0x05,
};

int hexNibble(QChar c)
{
    const ushort u = c.unicode();
    if (u >= '0' && u <= '9')
        return int(u - '0');
    if (u >= 'A' && u <= 'F')
        return int(u - 'A') + 10;
    if (u >= 'a' && u <= 'f')
        return int(u - 'a') + 10;
    return -1;
}

/// Append \a bytes at \a address, merging with the previous segment when they are
/// contiguous. Intel HEX files are usually already contiguous, but a linker can emit
/// records out of order, so both cases have to work.
void appendSegment(ImageBlob *blob, quint32 address, const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    for (ImageSegment &segment : blob->segments) {
        if (address >= segment.address && address + quint32(bytes.size()) <= segment.endAddress()) {
            // Wholly inside an existing run: overwrite in place (later record wins,
            // which is what a linker means when it emits the same address twice).
            const int offset = int(address - segment.address);
            std::memcpy(segment.bytes.data() + offset, bytes.constData(), size_t(bytes.size()));
            return;
        }
        if (address == segment.endAddress()) {
            segment.bytes.append(bytes);
            return;
        }
        if (address + quint32(bytes.size()) == segment.address) {
            segment.bytes.prepend(bytes);
            segment.address = address;
            return;
        }
    }
    ImageSegment segment;
    segment.address = address;
    segment.bytes = bytes;
    blob->segments.append(segment);
}

} // namespace

int ImageBlob::totalBytes() const
{
    int total = 0;
    for (const ImageSegment &segment : segments)
        total += segment.bytes.size();
    return total;
}

quint32 ImageBlob::lowestAddress() const
{
    if (segments.isEmpty())
        return 0;
    quint32 lowest = segments.first().address;
    for (const ImageSegment &segment : segments)
        lowest = qMin(lowest, segment.address);
    return lowest;
}

quint32 ImageBlob::highestEnd() const
{
    quint32 highest = 0;
    for (const ImageSegment &segment : segments)
        highest = qMax(highest, segment.endAddress());
    return highest;
}

void ImageBlob::moveTo(quint32 baseAddress)
{
    const quint32 current = lowestAddress();
    if (current == 0 || segments.isEmpty())
        return;
    const qint64 delta = qint64(baseAddress) - qint64(current);
    for (ImageSegment &segment : segments)
        segment.address = quint32(qint64(segment.address) + delta);
}

QString ImageBlob::rangeLabel() const
{
    if (segments.isEmpty())
        return QStringLiteral("(空)");
    return QStringLiteral("0x%1 .. 0x%2（%3）")
        .arg(lowestAddress(), 8, 16, QLatin1Char('0'))
        .arg(highestEnd(), 8, 16, QLatin1Char('0'))
        .arg(QStringLiteral("%1 字节").arg(totalBytes()));
}

Result<ImageBlob> parseIntelHex(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<ImageBlob>::failure(
            QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
    }

    ImageBlob blob;
    blob.origin = QFileInfo(path).fileName();

    quint32 base = 0;
    bool sawEof = false;
    int lineNumber = 0;

    while (!file.atEnd()) {
        const QByteArray rawLine = file.readLine();
        ++lineNumber;
        const QString line = QString::fromLatin1(rawLine).trimmed();
        if (line.isEmpty())
            continue;
        if (!line.startsWith(QLatin1Char(':'))) {
            // Some tools emit blank trailing lines; anything else is not HEX.
            continue;
        }

        // Byte count, 4-hex address, 2-hex type, then payload + checksum.
        QByteArray record;
        record.reserve(line.size() / 2);
        for (int i = 1; i < line.size(); i += 2) {
            const int high = hexNibble(line.at(i));
            const int low = i + 1 < line.size() ? hexNibble(line.at(i + 1)) : -1;
            if (high < 0 || low < 0) {
                return Result<ImageBlob>::failure(
                    QStringLiteral("%1 第 %2 行不是十六进制")
                        .arg(QFileInfo(path).fileName()).arg(lineNumber));
            }
            record.append(char((high << 4) | low));
        }
        if (record.size() < 5) {
            return Result<ImageBlob>::failure(
                QStringLiteral("%1 第 %2 行太短").arg(QFileInfo(path).fileName()).arg(lineNumber));
        }

        const int count = quint8(record.at(0));
        const quint32 address = quint32((quint8(record.at(1)) << 8) | quint8(record.at(2)));
        const int type = quint8(record.at(3));
        if (record.size() != count + 5) {
            return Result<ImageBlob>::failure(
                QStringLiteral("%1 第 %2 行的长度字段与实际不符")
                    .arg(QFileInfo(path).fileName()).arg(lineNumber));
        }

        // Checksum: two's complement of the sum of all bytes but the last.
        int sum = 0;
        for (int i = 0; i < record.size() - 1; ++i)
            sum += quint8(record.at(i));
        const quint8 expected = quint8((0x100 - (sum & 0xFF)) & 0xFF);
        if (expected != quint8(record.at(record.size() - 1))) {
            // Refused, not skipped: a corrupt bootloader is the failure mode that
            // bricks a board, and a checksum error is the only warning available.
            return Result<ImageBlob>::failure(
                QStringLiteral("%1 第 %2 行校验和错误（期望 0x%3，实际 0x%4）")
                    .arg(QFileInfo(path).fileName()).arg(lineNumber)
                    .arg(expected, 2, 16, QLatin1Char('0'))
                    .arg(quint8(record.at(record.size() - 1)), 2, 16, QLatin1Char('0')));
        }

        switch (type) {
        case kHexData:
            appendSegment(&blob, base + address, record.mid(4, count));
            break;
        case kHexEof:
            sawEof = true;
            break;
        case kHexExtendedSegment:
            if (count >= 2)
                base = quint32((quint8(record.at(4)) << 8) | quint8(record.at(5))) << 4;
            break;
        case kHexExtendedLinear:
            if (count >= 2)
                base = quint32((quint8(record.at(4)) << 8) | quint8(record.at(5))) << 16;
            break;
        case kHexStartSegment:
        case kHexStartLinear:
            break; // entry point: not needed for programming
        default:
            break;
        }
    }
    file.close();

    if (blob.isEmpty()) {
        return Result<ImageBlob>::failure(
            QStringLiteral("%1 里没有数据记录").arg(QFileInfo(path).fileName()));
    }
    if (!sawEof) {
        // Not fatal, but a truncated file is worth flagging: the last records may
        // simply be missing.
        qWarning("Intel HEX %s has no EOF record", qPrintable(path));
    }
    return Result<ImageBlob>::success(blob);
}

Result<ImageBlob> loadElfBlob(const QString &path)
{
    const auto elf = ElfFile::load(path);
    if (!elf) {
        return Result<ImageBlob>::failure(
            QStringLiteral("无法解析 %1：%2").arg(QFileInfo(path).fileName(), elf.error));
    }

    ImageBlob blob;
    blob.origin = QFileInfo(path).fileName();

    for (const ElfSection &section : elf.value.sections()) {
        if ((section.flags & kShfAlloc) == 0)
            continue;
        if (section.type == kShtNobits)
            continue; // .bss: address only, no bytes
        if (section.address == 0 || section.size == 0)
            continue;
        const QByteArray data = elf.value.sectionData(section.name);
        if (data.isEmpty())
            continue;
        appendSegment(&blob, section.address, data.left(int(section.size)));
    }

    if (blob.isEmpty()) {
        return Result<ImageBlob>::failure(
            QStringLiteral("%1 里没有可加载的段（是否用了 Debug 构建且未生成 .axf？）")
                .arg(QFileInfo(path).fileName()));
    }
    return Result<ImageBlob>::success(blob);
}

Result<ImageBlob> loadImageBlob(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("hex") || suffix == QLatin1String("ihx")) {
        return parseIntelHex(path);
    }
    if (suffix == QLatin1String("axf") || suffix == QLatin1String("elf")
        || suffix == QLatin1String("out")) {
        return loadElfBlob(path);
    }
    // Unknown extension: try HEX first (it is text and fails fast), then ELF.
    const auto asHex = parseIntelHex(path);
    if (asHex)
        return asHex;
    return loadElfBlob(path);
}

Result<QByteArray> mergeBlobs(const QVector<ImageBlob> &blobs, quint32 *startInOut,
                              quint32 *endInOut, QStringList *notes)
{
    QVector<ImageBlob> nonEmpty;
    for (const ImageBlob &blob : blobs) {
        if (!blob.isEmpty())
            nonEmpty.append(blob);
    }
    if (nonEmpty.isEmpty())
        return Result<QByteArray>::failure(QStringLiteral("没有可合并的内容"));

    const quint32 lowest = [&nonEmpty] {
        quint32 value = nonEmpty.first().lowestAddress();
        for (const ImageBlob &blob : nonEmpty)
            value = qMin(value, blob.lowestAddress());
        return value;
    }();
    const quint32 highest = [&nonEmpty] {
        quint32 value = 0;
        for (const ImageBlob &blob : nonEmpty)
            value = qMax(value, blob.highestEnd());
        return value;
    }();

    // The caller may widen the range (a user-edited end address), but shrinking it past
    // real content would silently drop data, so that is refused.
    const quint32 start = (startInOut && *startInOut != 0) ? *startInOut : lowest;
    const quint32 end = (endInOut && *endInOut != 0) ? *endInOut : highest;

    if (start > lowest) {
        return Result<QByteArray>::failure(
            QStringLiteral("整体起始地址 0x%1 高于内容最低地址 0x%2，会丢掉数据")
                .arg(start, 8, 16, QLatin1Char('0'))
                .arg(lowest, 8, 16, QLatin1Char('0')));
    }
    if (end < highest) {
        return Result<QByteArray>::failure(
            QStringLiteral("整体结束地址 0x%1 低于内容最高地址 0x%2，会丢掉数据")
                .arg(end, 8, 16, QLatin1Char('0'))
                .arg(highest, 8, 16, QLatin1Char('0')));
    }
    if (end <= start)
        return Result<QByteArray>::failure(QStringLiteral("结束地址必须大于起始地址"));

    // Gaps become 0xFF (erased): writing them is harmless and keeps the image one block,
    // which is what a single loadbin call needs.
    QByteArray merged(int(end - start), char(0xFF));
    // Track which bytes were actually written, to detect two images colliding.
    QVector<bool> written(merged.size(), false);

    for (const ImageBlob &blob : nonEmpty) {
        for (const ImageSegment &segment : blob.segments) {
            for (int i = 0; i < segment.bytes.size(); ++i) {
                const quint32 address = segment.address + quint32(i);
                const int offset = int(address - start);
                if (offset < 0 || offset >= merged.size())
                    continue;
                const char value = segment.bytes.at(i);
                if (written.at(offset)) {
                    if (merged.at(offset) != value) {
                        // Same address, different byte: the two images disagree. This is
                        // the bootloader/application overlap that must not be guessed at.
                        return Result<QByteArray>::failure(
                            QStringLiteral("两个镜像在地址 0x%1 处内容冲突（0x%2 vs 0x%3）")
                                .arg(address, 8, 16, QLatin1Char('0'))
                                .arg(quint8(merged.at(offset)), 2, 16, QLatin1Char('0'))
                                .arg(quint8(value), 2, 16, QLatin1Char('0')));
                    }
                    continue; // identical bytes: benign overlap
                }
                merged[offset] = value;
                written[offset] = true;
            }
        }
    }

    if (notes) {
        int gaps = 0;
        int gapBytes = 0;
        int run = 0;
        for (bool flag : written) {
            if (flag) {
                if (run > 0)
                    ++gaps;
                run = 0;
            } else {
                ++run;
                ++gapBytes;
            }
        }
        if (run > 0)
            ++gaps;
        if (gapBytes > 0) {
            notes->append(QStringLiteral("镜像内有 %1 处空档、共 %2 字节，已按 0xFF 填充")
                              .arg(gaps).arg(gapBytes));
        }
    }

    if (startInOut)
        *startInOut = start;
    if (endInOut)
        *endInOut = end;
    return Result<QByteArray>::success(merged);
}

} // namespace mrd
