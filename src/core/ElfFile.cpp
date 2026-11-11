#include "core/ElfFile.h"

#include <QFile>

namespace mrd {

namespace {

// ELF identification.
constexpr int kIdentSize = 16;
constexpr int kElfClass32 = 1;
constexpr int kElfClass64 = 2;
constexpr int kDataLsb = 1;
constexpr int kDataMsb = 2;

// Section types.
constexpr quint32 kShtSymtab = 2;
constexpr quint32 kShtStrtab = 3;
constexpr quint32 kShtDynsym = 11;

// Symbol binding / type.
constexpr int kStbLocal = 0;
constexpr int kSttObject = 1;
constexpr int kSttFunc = 2;
constexpr int kSttSection = 3;
constexpr int kSttFile = 4;
constexpr quint16 kShnUndef = 0;
constexpr quint16 kShnAbs = 0xfff1;

/// Section-name prefixes that make a symbol uninteresting in a variable browser.
bool isCompilerGenerated(const QString &name)
{
    return name.isEmpty() || name.startsWith(QLatin1String("$"))
        || name.startsWith(QLatin1String(".L")) || name.startsWith(QLatin1String("__"))
        || name.startsWith(QLatin1String("_Z"));
}

} // namespace

bool ElfSymbol::isDataObject() const
{
    if (name.isEmpty() || sectionIndex == kShnUndef)
        return false;
    // STT_OBJECT is the precise answer; some toolchains emit STT_NOTYPE for
    // assembly-defined data, so accept that when it is not a section/file entry.
    if (type() == kSttObject)
        return true;
    return type() == 0 && sectionIndex != kShnAbs && !isCompilerGenerated(name);
}

Result<ElfFile> ElfFile::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Result<ElfFile>::failure(
            QStringLiteral("无法打开 %1: %2").arg(path, file.errorString()));
    return fromBytes(file.readAll(), path);
}

Result<ElfFile> ElfFile::fromBytes(const QByteArray &bytes, const QString &origin)
{
    ElfFile elf;
    elf.m_data = bytes;
    elf.m_origin = origin;
    if (!elf.parse())
        return Result<ElfFile>::failure(elf.m_error);
    return Result<ElfFile>::success(std::move(elf));
}

quint16 ElfFile::readU16(const char *p) const
{
    const uchar *b = reinterpret_cast<const uchar *>(p);
    if (m_littleEndian)
        return static_cast<quint16>(b[0] | (b[1] << 8));
    return static_cast<quint16>((b[0] << 8) | b[1]);
}

quint32 ElfFile::readU32(const char *p) const
{
    const uchar *b = reinterpret_cast<const uchar *>(p);
    if (m_littleEndian) {
        return static_cast<quint32>(b[0]) | (static_cast<quint32>(b[1]) << 8)
            | (static_cast<quint32>(b[2]) << 16) | (static_cast<quint32>(b[3]) << 24);
    }
    return static_cast<quint32>(b[3]) | (static_cast<quint32>(b[2]) << 8)
        | (static_cast<quint32>(b[1]) << 16) | (static_cast<quint32>(b[0]) << 24);
}

quint64 ElfFile::readU64(const char *p) const
{
    const uchar *b = reinterpret_cast<const uchar *>(p);
    quint64 value = 0;
    if (m_littleEndian) {
        for (int i = 7; i >= 0; --i)
            value = (value << 8) | b[i];
    } else {
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | b[i];
    }
    return value;
}

bool ElfFile::parse()
{
    const char *base = m_data.constData();
    const int size = m_data.size();
    if (size < kIdentSize + 4) {
        m_error = QStringLiteral("文件太小，不是 ELF");
        return false;
    }
    if (base[0] != 0x7f || base[1] != 'E' || base[2] != 'L' || base[3] != 'F') {
        m_error = QStringLiteral("不是 ELF 文件（缺少 \\x7fELF 魔数）");
        return false;
    }
    const int elfClass = static_cast<uchar>(base[4]);
    const int dataEncoding = static_cast<uchar>(base[5]);
    if (dataEncoding != kDataLsb && dataEncoding != kDataMsb) {
        m_error = QStringLiteral("ELF 字节序字段无效 (%1)").arg(dataEncoding);
        return false;
    }
    m_littleEndian = (dataEncoding == kDataLsb);
    m_64bit = (elfClass == kElfClass64);
    if (elfClass != kElfClass32 && elfClass != kElfClass64) {
        m_error = QStringLiteral("ELF class %1 不支持").arg(elfClass);
        return false;
    }

    // Both ELF32 and ELF64 share the first 0x14 bytes; the section-table pointer
    // moves from 0x20 to 0x28 in the 64-bit header.
    const int shoffOffset = m_64bit ? 0x28 : 0x20;
    const int shentsizeOffset = m_64bit ? 0x3a : 0x2e;
    const int shnumOffset = m_64bit ? 0x3c : 0x30;
    const int shstrndxOffset = m_64bit ? 0x3e : 0x32;
    if (size < shentsizeOffset + 2) {
        m_error = QStringLiteral("ELF 头被截断");
        return false;
    }
    m_machine = readU16(base + 0x12);
    m_entry = readU32(base + 0x18);

    const quint64 shoff = m_64bit ? readU64(base + shoffOffset)
                                  : readU32(base + shoffOffset);
    const quint16 shentsize = readU16(base + shentsizeOffset);
    const quint16 shnum = readU16(base + shnumOffset);
    const quint16 shstrndx = readU16(base + shstrndxOffset);

    if (shoff == 0 || shnum == 0 || shentsize == 0) {
        m_error = QStringLiteral("ELF 没有节表");
        return false;
    }
    const quint64 need = shoff + static_cast<quint64>(shentsize) * shnum;
    if (need > static_cast<quint64>(size)) {
        m_error = QStringLiteral("节表超出文件末尾");
        return false;
    }

    struct RawSection
    {
        quint32 nameOff, type, flags, addr, off, sz, link, info, align, entsize;
    };
    QVector<RawSection> raw;
    raw.reserve(shnum);
    for (int i = 0; i < shnum; ++i) {
        const char *p = base + shoff + static_cast<quint64>(i) * shentsize;
        RawSection r{};
        if (m_64bit) {
            r.nameOff = readU32(p + 0x00);
            r.type = readU32(p + 0x04);
            r.flags = readU32(p + 0x08);
            r.addr = static_cast<quint32>(readU64(p + 0x10));
            r.off = static_cast<quint32>(readU64(p + 0x18));
            r.sz = static_cast<quint32>(readU64(p + 0x20));
            r.link = readU32(p + 0x28);
            r.align = readU32(p + 0x30);
            r.entsize = readU32(p + 0x38);
        } else {
            r.nameOff = readU32(p + 0x00);
            r.type = readU32(p + 0x04);
            r.flags = readU32(p + 0x08);
            r.addr = readU32(p + 0x0c);
            r.off = readU32(p + 0x10);
            r.sz = readU32(p + 0x14);
            r.link = readU32(p + 0x18);
            r.info = readU32(p + 0x1c);
            r.align = readU32(p + 0x20);
            r.entsize = readU32(p + 0x24);
        }
        raw.append(r);
    }

    // Section names come from the section named by e_shstrndx.
    const char *shstr = nullptr;
    int shstrSize = 0;
    if (shstrndx < raw.size()) {
        const RawSection &str = raw.at(shstrndx);
        if (str.off + str.sz <= static_cast<quint64>(size)) {
            shstr = base + str.off;
            shstrSize = static_cast<int>(str.sz);
        }
    }
    auto nameAt = [&](quint32 off) -> QString {
        if (!shstr || off >= static_cast<quint32>(shstrSize))
            return QString();
        const int maxLen = shstrSize - static_cast<int>(off);
        return QString::fromLatin1(shstr + off, qstrnlen(shstr + off, maxLen));
    };

    m_sections.reserve(raw.size());
    for (const RawSection &r : raw) {
        ElfSection s;
        s.name = nameAt(r.nameOff);
        s.type = r.type;
        s.flags = r.flags;
        s.address = r.addr;
        s.offset = r.off;
        s.size = r.sz;
        s.link = r.link;
        s.entsize = r.entsize;
        m_sections.append(s);
    }

    // Symbols: prefer .symtab, fall back to .dynsym.
    const ElfSection *symtab = nullptr;
    for (const ElfSection &s : m_sections) {
        if (s.type == kShtSymtab) {
            symtab = &s;
            break;
        }
    }
    if (!symtab) {
        for (const ElfSection &s : m_sections) {
            if (s.type == kShtDynsym) {
                symtab = &s;
                break;
            }
        }
    }
    if (symtab && symtab->link < static_cast<quint32>(m_sections.size())) {
        const ElfSection &strtab = m_sections.at(static_cast<int>(symtab->link));
        const quint64 symEnd = static_cast<quint64>(symtab->offset) + symtab->size;
        const quint64 strEnd = static_cast<quint64>(strtab.offset) + strtab.size;
        if (symEnd <= static_cast<quint64>(size) && strEnd <= static_cast<quint64>(size)) {
            const int entsize = m_64bit ? 24 : 16;
            const int count = entsize > 0 ? static_cast<int>(symtab->size) / entsize : 0;
            const char *symBase = base + symtab->offset;
            const char *strBase = base + strtab.offset;
            for (int i = 0; i < count; ++i) {
                const char *p = symBase + i * entsize;
                ElfSymbol sym;
                sym.value = readU32(p + (m_64bit ? 8 : 4));
                sym.size = readU32(p + (m_64bit ? 16 : 8));
                const quint32 nameOff = readU32(p);
                if (m_64bit) {
                    sym.info = static_cast<quint8>(p[4]);
                    sym.sectionIndex = readU16(p + 6);
                } else {
                    sym.info = static_cast<quint8>(p[12]);
                    sym.sectionIndex = readU16(p + 14);
                }
                if (nameOff < strtab.size) {
                    const int maxLen = static_cast<int>(strtab.size - nameOff);
                    sym.name = QString::fromLatin1(strBase + nameOff,
                                                   qstrnlen(strBase + nameOff, maxLen));
                }
                if (!sym.name.isEmpty())
                    m_symbols.append(sym);
            }
        }
    }

    m_valid = true;
    return true;
}

const ElfSection *ElfFile::section(const QString &name) const
{
    for (const ElfSection &s : m_sections) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}

QByteArray ElfFile::sectionData(const QString &name) const
{
    const ElfSection *s = section(name);
    if (!s)
        return {};
    const quint64 end = static_cast<quint64>(s->offset) + s->size;
    if (end > static_cast<quint64>(m_data.size()))
        return {};
    return m_data.mid(static_cast<int>(s->offset), static_cast<int>(s->size));
}

} // namespace mrd
