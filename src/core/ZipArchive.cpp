#include "core/ZipArchive.h"

#include <QFile>

#include <zlib.h>

namespace mrd {
namespace {

constexpr quint32 kEocdSignature = 0x06054b50;      // "PK\5\6"
constexpr quint32 kCentralSignature = 0x02014b50;   // "PK\1\2"
constexpr quint32 kLocalSignature = 0x04034b50;     // "PK\3\4"
constexpr quint32 kZip64LocatorSignature = 0x07064b50;

quint16 readU16(const QByteArray &bytes, int offset)
{
    return quint16(quint8(bytes.at(offset)) | (quint8(bytes.at(offset + 1)) << 8));
}

quint32 readU32(const QByteArray &bytes, int offset)
{
    return quint32(quint8(bytes.at(offset)))
        | (quint32(quint8(bytes.at(offset + 1))) << 8)
        | (quint32(quint8(bytes.at(offset + 2))) << 16)
        | (quint32(quint8(bytes.at(offset + 3))) << 24);
}

} // namespace

Result<QByteArray> inflateRaw(const QByteArray &input, int expectedSize)
{
    if (input.isEmpty())
        return Result<QByteArray>::success(QByteArray());

    z_stream stream{};
    // Negative window bits: the member payload is a raw deflate stream, without the
    // zlib header that a standalone .gz would carry.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return Result<QByteArray>::failure(QStringLiteral("zlib 初始化失败"));

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = uInt(input.size());

    // Start from the recorded size, but still grow: a deliberately wrong header should
    // not truncate the output silently.
    QByteArray out;
    out.resize(qMax(64, expectedSize > 0 ? expectedSize : input.size() * 4));

    int result = Z_OK;
    while (true) {
        if (stream.total_out >= uInt(out.size())) {
            out.resize(out.size() * 2);
            stream.next_out = reinterpret_cast<Bytef *>(out.data() + stream.total_out);
            stream.avail_out = uInt(out.size() - int(stream.total_out));
        } else {
            stream.next_out = reinterpret_cast<Bytef *>(out.data() + stream.total_out);
            stream.avail_out = uInt(out.size() - int(stream.total_out));
        }

        result = inflate(&stream, Z_NO_FLUSH);
        if (result == Z_STREAM_END)
            break;
        if (result != Z_OK) {
            inflateEnd(&stream);
            return Result<QByteArray>::failure(
                QStringLiteral("解压失败（zlib %1）").arg(result));
        }
        if (stream.avail_in == 0 && stream.avail_out > 0) {
            // Input exhausted without a stream end: truncated member.
            inflateEnd(&stream);
            return Result<QByteArray>::failure(QStringLiteral("压缩数据被截断"));
        }
    }

    out.resize(int(stream.total_out));
    inflateEnd(&stream);
    return Result<QByteArray>::success(out);
}

Result<ZipArchive> ZipArchive::open(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<ZipArchive>::failure(
            QStringLiteral("无法打开 %1：%2").arg(path, file.errorString()));
    }
    ZipArchive archive;
    archive.m_path = path;
    archive.m_bytes = file.readAll();
    file.close();

    if (!archive.load(archive.m_bytes)) {
        return Result<ZipArchive>::failure(
            QStringLiteral("%1 不是有效的 ZIP/FMU 包").arg(path));
    }
    return Result<ZipArchive>::success(archive);
}

bool ZipArchive::load(const QByteArray &bytes)
{
    m_entries.clear();
    m_index.clear();

    // --- end of central directory ----------------------------------------
    // Scanned backwards because the EOCD is followed by a comment of unknown length.
    int eocd = -1;
    const int lowest = qMax(0, bytes.size() - (0xFFFF + 22));
    for (int i = bytes.size() - 22; i >= lowest; --i) {
        if (readU32(bytes, i) == kEocdSignature) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return false;

    // An archive with a ZIP64 locator cannot be handled: refuse instead of mis-reading.
    if (eocd >= 20 && readU32(bytes, eocd - 20) == kZip64LocatorSignature)
        return false;

    const int entryCount = readU16(bytes, eocd + 10);
    const quint32 centralOffset = readU32(bytes, eocd + 16);
    if (centralOffset + 4 > quint32(bytes.size()))
        return false;

    // --- central directory ------------------------------------------------
    int cursor = int(centralOffset);
    for (int i = 0; i < entryCount; ++i) {
        if (cursor + 46 > bytes.size() || readU32(bytes, cursor) != kCentralSignature)
            return false;

        Entry entry;
        entry.method = readU16(bytes, cursor + 10);
        entry.compressedBytes = readU32(bytes, cursor + 20);
        entry.uncompressedBytes = readU32(bytes, cursor + 24);
        const int nameLength = readU16(bytes, cursor + 28);
        const int extraLength = readU16(bytes, cursor + 30);
        const int commentLength = readU16(bytes, cursor + 32);
        entry.localHeaderOffset = readU32(bytes, cursor + 42);

        if (cursor + 46 + nameLength > bytes.size())
            return false;
        entry.path = QString::fromUtf8(bytes.mid(cursor + 46, nameLength));
        entry.path.replace(QLatin1Char('\\'), QLatin1Char('/'));

        m_index.insert(entry.path, m_entries.size());
        m_entries.append(entry);

        cursor += 46 + nameLength + extraLength + commentLength;
    }

    m_valid = !m_entries.isEmpty();
    return m_valid;
}

QStringList ZipArchive::paths() const
{
    QStringList list;
    list.reserve(m_entries.size());
    for (const Entry &entry : m_entries)
        list << entry.path;
    return list;
}

bool ZipArchive::contains(const QString &entryPath) const
{
    return m_index.contains(entryPath);
}

QStringList ZipArchive::entriesUnder(const QString &prefix) const
{
    QStringList list;
    for (const Entry &entry : m_entries) {
        if (entry.path.startsWith(prefix, Qt::CaseInsensitive))
            list << entry.path;
    }
    return list;
}

QByteArray ZipArchive::read(const QString &entryPath) const
{
    const auto it = m_index.constFind(entryPath);
    if (it == m_index.constEnd())
        return QByteArray();

    const Entry &entry = m_entries.at(it.value());
    const int local = int(entry.localHeaderOffset);
    if (local + 30 > m_bytes.size() || readU32(m_bytes, local) != kLocalSignature)
        return QByteArray();

    // The local header repeats the name/extra lengths, and *those* are the ones that
    // locate the payload -- the central directory's copy is allowed to differ.
    const int nameLength = readU16(m_bytes, local + 26);
    const int extraLength = readU16(m_bytes, local + 28);
    const int dataStart = local + 30 + nameLength + extraLength;
    const int dataLength = int(entry.compressedBytes);
    if (dataStart < 0 || dataLength < 0 || dataStart + dataLength > m_bytes.size())
        return QByteArray();

    const QByteArray payload = m_bytes.mid(dataStart, dataLength);
    if (entry.method == 0)
        return payload; // stored
    if (entry.method == 8) {
        const auto inflated = inflateRaw(payload, int(entry.uncompressedBytes));
        return inflated.ok ? inflated.value : QByteArray();
    }
    return QByteArray(); // unsupported method
}

QByteArray ZipArchive::readFirstMatching(const QString &prefix, const QString &suffix,
                                         QString *matchedPath) const
{
    for (const Entry &entry : m_entries) {
        if (!entry.path.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        if (!entry.path.endsWith(suffix, Qt::CaseInsensitive))
            continue;
        // Prefer the 64-bit Windows binary when both are present, mirroring what every
        // FMI tool does on a 64-bit host.
        if (entry.path.contains(QLatin1String("/win64"), Qt::CaseInsensitive)
            || entry.path.contains(QLatin1String("x86_64"), Qt::CaseInsensitive)) {
            const QByteArray data = read(entry.path);
            if (!data.isEmpty()) {
                if (matchedPath)
                    *matchedPath = entry.path;
                return data;
            }
        }
    }
    // Nothing 64-bit: fall back to whatever matched.
    for (const Entry &entry : m_entries) {
        if (!entry.path.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        if (!entry.path.endsWith(suffix, Qt::CaseInsensitive))
            continue;
        const QByteArray data = read(entry.path);
        if (!data.isEmpty()) {
            if (matchedPath)
                *matchedPath = entry.path;
            return data;
        }
    }
    return QByteArray();
}

} // namespace mrd
