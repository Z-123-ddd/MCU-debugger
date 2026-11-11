#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace mrd {

/// One entry of an ELF file's section table, with the file offset resolved so the
/// bytes can be sliced straight out of the image.
struct ElfSection
{
    QString name;
    quint32 type = 0;
    quint32 flags = 0;
    quint32 address = 0; ///< sh_addr: where the linker placed it (0 for debug info)
    quint32 offset = 0;  ///< sh_offset: where the bytes sit inside the file
    quint32 size = 0;
    quint32 link = 0;    ///< sh_link: e.g. a symbol table's string-table index
    quint32 entsize = 0;
};

/// One entry of `.symtab`.
struct ElfSymbol
{
    QString name;
    quint32 value = 0; ///< st_value: the variable's load address
    quint32 size = 0;  ///< st_size
    quint8 info = 0;   ///< st_info: bind << 4 | type
    quint16 sectionIndex = 0;

    int bind() const { return info >> 4; }
    int type() const { return info & 0x0f; }

    /// `.bss`/`.data`/`.rodata` style data object (not a function, not undefined).
    bool isDataObject() const;
};

/// Read-only view of an ELF32 image (a Keil `.axf`, a GCC `.elf`).
///
/// Only what a symbol browser needs: the section table, the symbol table and the
/// raw bytes of any section (DWARF lives in named sections, so this is all the
/// DWARF reader needs to get at it). Both byte orders are handled because the
/// tool is not tied to one ARM toolchain.
class ElfFile
{
public:
    static Result<ElfFile> load(const QString &path);
    static Result<ElfFile> fromBytes(const QByteArray &bytes, const QString &origin);

    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }
    QString origin() const { return m_origin; }
    bool littleEndian() const { return m_littleEndian; }
    bool is64Bit() const { return m_64bit; }
    quint16 machine() const { return m_machine; }
    quint32 entryPoint() const { return m_entry; }

    QVector<ElfSection> sections() const { return m_sections; }
    const ElfSection *section(const QString &name) const;
    bool hasSection(const QString &name) const { return section(name) != nullptr; }

    /// Raw bytes of a section (empty when the section is absent or truncated).
    QByteArray sectionData(const QString &name) const;

    /// Symbols from `.symtab` (falls back to `.dynsym`). Unnamed and undefined
    /// entries are skipped.
    QVector<ElfSymbol> symbols() const { return m_symbols; }

    /// Byte-order-aware primitive readers, exposed so the DWARF reader reuses the
    /// same rules instead of duplicating them.
    quint16 readU16(const char *p) const;
    quint32 readU32(const char *p) const;
    quint64 readU64(const char *p) const;

private:
    bool parse();

    QByteArray m_data;
    QString m_origin;
    QString m_error;
    bool m_valid = false;
    bool m_littleEndian = true;
    bool m_64bit = false;
    quint16 m_machine = 0;
    quint32 m_entry = 0;
    QVector<ElfSection> m_sections;
    QVector<ElfSymbol> m_symbols;
};

} // namespace mrd
