#pragma once

#include "core/DwarfReader.h"
#include "core/Result.h"

#include <QMap>
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

/// One source file and the variables declared in it.
///
/// Real projects scatter thousands of globals over hundreds of files; a flat list is
/// unusable. This is the grouping every debugger's watch window uses.
struct FileGroup
{
    QString file;                    ///< base name ("axis.cpp"), empty for unknown
    QVector<VariableNode> variables;
    int nodeCount = 0;
};

/// Everything a symbol file tells us, resolved and merged.
struct SymbolTable
{
    QString sourcePath;
    /// Source-shaped forest, addresses resolved by the loader.
    QVector<VariableNode> roots;
    /// Flattened scalar leaves with dotted names (one row per curve source).
    QVector<WatchVariable> variables;
    /// Functions with locals, so the UI can resolve "which locals exist at this PC".
    QVector<FunctionScope> functions;
    /// Class/struct definitions with their members, grouped per file by the UI.
    ///
    /// These have no address: a member only exists inside an instance. They are
    /// listed so a user opening `encoder.cpp` can see that `Encoder` has
    /// `index_found_`, instead of having to find whichever file declares an instance.
    QVector<TypeDecl> types;
    /// The image itself, kept so `.debug_frame` can be re-read at any PC (the CFA
    /// rule is PC-dependent). ElfFile holds implicitly-shared byte arrays, so this
    /// is a cheap handle rather than a 4 MB copy.
    ElfFile source;
    int dwarfVersion = 0;
    bool usedSymbolFallback = false; ///< DWARF lacked addresses; .symtab supplied them

    bool isValid() const { return !variables.isEmpty() || !roots.isEmpty(); }
    /// Look up a leaf by its dotted name.
    const WatchVariable *find(const QString &name) const;
    /// Flattened leaves whose name, type or address contains \a needle.
    QVector<WatchVariable> filtered(const QString &needle) const;
    /// Roots pruned to the branches containing a match, so filtering does not
    /// flatten or destroy the hierarchy.
    QVector<VariableNode> filteredRoots(const QString &needle) const;

    /// The function whose code range contains \a pc, or nullptr.
    /// Frame base for a halt at  pc: `frameBase = registers[reg] + offset`.
    ///
    /// armclang states it directly (usually register 13 = SP, offset 0); GCC points
    /// at the CFA, whose rule changes inside the function, so it is read from
    /// `.debug_frame` for this exact PC.
    /// Types defined in  file (empty name = the unknown-file bucket).
    QVector<TypeDecl> typesInFile(const QString &file) const;

    /// Variables grouped by the source file they were declared in (name order,
    /// address order inside each). Entries without file information land in a
    /// trailing "(未知文件)" group.
    QVector<FileGroup> groupedByFile() const;

    bool frameBaseAt(quint32 pc, int *reg, qint32 *offset) const;

    const FunctionScope *functionContaining(quint32 pc) const;

    /// Collect every samplable leaf under \a node into \a out.
    static void collectLeaves(const VariableNode &node, const QString &prefix,
                              QVector<WatchVariable> *out);
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
