#pragma once

#include "core/ElfFile.h"
#include "core/Result.h"

#include <QString>
#include <QVector>

namespace mrd {

/// How a variable's bytes should be interpreted when sampling it.
enum class ScalarKind
{
    Unknown,
    Signed,   ///< int8/16/32/64
    Unsigned, ///< uint8/16/32/64
    Float,    ///< float/double (IEEE-754, little-endian on ARM)
    Boolean,
    Pointer,
    Composite, ///< struct/union: not directly drawable, expanded into members
};

QString scalarKindName(ScalarKind kind);
int scalarKindSizeGuess(ScalarKind kind);

/// One sampleable scalar: either a plain variable or a struct member flattened out
/// of its parent (`g_point.x`).
struct DwarfVariable
{
    QString name;        ///< as written in source ("g_point", "g_point.x")
    QString typeDisplay; ///< "float", "uint16_t[4]", "Point"
    ScalarKind kind = ScalarKind::Unknown;
    quint32 address = 0; ///< valid only when hasAddress
    bool hasAddress = false;
    int elementSize = 0; ///< bytes per element (0 when unknown)
    int elementCount = 1;///< >1 for arrays
    int totalSize = 0;
    QString member;      ///< struct member path ("x" in "g_point.x"), empty for plain
    /// Mangled symbol name (`DW_AT_linkage_name`). This is what the linker's
    /// `.symtab` calls the variable, so it is the only reliable way to pair a
    /// C++ DWARF entry with its load address -- the DWARF name is demangled
    /// ("endpoint_id") while the symbol is "_ZL11endpoint_id".
    QString linkageName;

    bool drawable() const
    {
        return hasAddress && elementSize > 0 && kind != ScalarKind::Composite
            && kind != ScalarKind::Unknown;
    }
    /// Human label for the type column, with array extent folded in.
    QString typeLabel() const
    {
        if (elementCount > 1)
            return QStringLiteral("%1[%2]").arg(typeDisplay).arg(elementCount);
        return typeDisplay;
    }
};

/// Extract every statically allocated variable from an ELF image's DWARF.
///
/// This is what makes a J-Scope-style variable browser possible without asking the
/// user to type addresses: the compiler already recorded each variable's name,
/// type, size and location. Only globals and statics are found -- locals live on
/// the stack and have no fixed address, so every tool of this kind (J-Scope
/// included) is limited to the same set.
///
/// Addresses come from `DW_OP_addr` when the compiler emits one; `SymbolFile`
/// fills the rest in from `.symtab`, because a linker symbol's `st_value` is the
/// load address and is more reliable than parsing location expressions.
class DwarfReader
{
public:
    static Result<QVector<DwarfVariable>> readVariables(const ElfFile &elf);

    /// DWARF version found in `.debug_info` (0 when absent). Diagnostics only.
    static int dwarfVersion(const ElfFile &elf);
};

} // namespace mrd
