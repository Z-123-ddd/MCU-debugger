#pragma once

#include "core/DwarfReader.h"
#include "core/Result.h"

#include <QString>
#include <QVector>

namespace mrd {

/// One samplable thing the user can tick in the variable list.
///
/// Either a plain global/static variable or one scalar member of a struct, already
/// flattened (`g_point.x`). Arrays stay single entries with `elementCount > 1`,
/// because the plot needs one curve per element and can fan them out itself.
struct WatchVariable
{
    QString name;
    QString typeLabel;
    ScalarKind kind = ScalarKind::Unknown;
    quint32 address = 0;
    int elementSize = 0;
    int elementCount = 1;
    int totalSize = 0;
    QString linkageName;             ///< mangled name, used to pair with .symtab
    bool addressFromSymbol = false; ///< address came from .symtab, not DWARF

    bool usable() const { return address != 0 && elementSize > 0 && elementCount > 0; }

    /// Load address of element \a index of the array.
    quint32 elementAddress(int index) const
    {
        return address + static_cast<quint32>(index * elementSize);
    }
    /// "0x20000010" for scalars, "0x20000010..0x2000001C" for arrays.
    QString addressLabel() const;
    /// Total bytes this variable occupies.
    int span() const { return elementSize * elementCount; }
};

/// Everything a symbol file tells us, resolved and merged.
struct SymbolTable
{
    QString sourcePath;
    QVector<WatchVariable> variables;
    int dwarfVersion = 0;
    bool usedSymbolFallback = false; ///< DWARF lacked addresses; .symtab supplied them

    bool isValid() const { return !variables.isEmpty(); }
    /// Look up by exact name (including the `parent.member` form).
    const WatchVariable *find(const QString &name) const;
    /// Variables whose name, type or address contains \a needle (case-insensitive).
    QVector<WatchVariable> filtered(const QString &needle) const;
};

/// Load a Keil `.axf` (or any ELF with DWARF) and list its variables.
///
/// Why an ELF and not a `.map`: the map only carries name/address/size, which is
/// enough to *find* a variable but not to *interpret* it -- a plot has to know
/// whether those four bytes are a float, an int32 or an enum. DWARF records that,
/// so the chart can decode values without asking the user each time.
///
/// Addresses are taken from `DW_OP_addr` when present and otherwise from the
/// linker's `.symtab`; a symbol's `st_value` is the load address and is more
/// reliable than a location expression the reader might not fully understand.
class SymbolFile
{
public:
    static Result<SymbolTable> load(const QString &path);

    /// Supported extensions, for the file dialog.
    static QStringList fileFilters();

    /// Decode \a count elements of \a variable out of \a bytes (probe data).
    /// Returns raw doubles so the plot does not care about the source type.
    static QVector<double> decode(const WatchVariable &variable, const QByteArray &bytes,
                                  int count = 1);
};

} // namespace mrd
