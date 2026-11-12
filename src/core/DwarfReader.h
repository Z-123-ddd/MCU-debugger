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
    Pointer,  ///< pointer: sampled as an unsigned word, not followed
    Enum,     ///< stored as an integer, drawn as a number
    Composite, ///< struct/union/class: has children, not directly drawable
};

QString scalarKindName(ScalarKind kind);
int scalarKindSizeGuess(ScalarKind kind);

/// What kind of thing a node in the variable tree is.
///
/// The tree keeps the source shape instead of flattening it: a struct is a node
/// with member children, an array is a node with element children. That is both
/// what the compiler recorded and what a user recognises from their own code.
enum class NodeKind
{
    Scalar,
    Array,
    Struct,
    Union,
    Class,
    Pointer,
    Enum,
};

QString nodeKindName(NodeKind kind);

/// One node of the variable tree: a variable, an array element, or a member.
///
/// `name` is only the local name ("g_points", "[0]", "x"); the dotted path is
/// rebuilt by walking the parents, so the same tree can be shown hierarchically in
/// the UI and listed flat in the log without storing the name twice.
struct VariableNode
{
    QString name;
    QString typeName;       ///< "float", "uint16_t", "Point"
    NodeKind kind = NodeKind::Scalar;
    ScalarKind scalar = ScalarKind::Unknown;

    quint32 address = 0;    ///< absolute when hasAddress
    bool hasAddress = false;
    int byteSize = 0;       ///< total bytes this node spans
    int elementSize = 0;    ///< scalar width, or the array's element width
    int elementCount = 1;   ///< array extent (1 for non-arrays)

    QString linkageName;    ///< mangled name, roots only (pairs with .symtab)
    bool addressFromSymbol = false;
    /// Set when the layout could not be read (a forward declaration, or the
    /// expansion cap was hit) so the UI can say so instead of showing an empty node.
    bool incomplete = false;

    QVector<VariableNode> children;

    bool isLeaf() const { return children.isEmpty(); }
    bool isArray() const { return kind == NodeKind::Array; }
    bool isAggregate() const
    {
        return kind == NodeKind::Array || kind == NodeKind::Struct
            || kind == NodeKind::Union || kind == NodeKind::Class;
    }
    /// True when this node's bytes can be decoded into a number (a scalar, or one
    /// element of a scalar array). Aggregates need children to be read.
    bool samplable() const
    {
        return hasAddress && elementSize > 0
            && (scalar == ScalarKind::Signed || scalar == ScalarKind::Unsigned
                || scalar == ScalarKind::Float || scalar == ScalarKind::Boolean
                || scalar == ScalarKind::Pointer || scalar == ScalarKind::Enum);
    }

    /// "Point[2]", "uint16_t[4]", "float" -- the type as C would spell it.
    QString typeLabel() const
    {
        if (kind == NodeKind::Array && elementCount > 0) {
            const QString base = typeName.isEmpty() ? QStringLiteral("?") : typeName;
            return QStringLiteral("%1[%2]").arg(base).arg(elementCount);
        }
        return typeName;
    }

    /// Number of leaf values contained here (arrays multiply out).
    int leafCount() const;
    /// Sum of byteSize over the whole subtree's leaves.
    int totalNodeCount() const;
};

/// Extract every statically allocated variable from an ELF image's DWARF, as a
/// forest (one root per global/static).
///
/// This is what makes a J-Scope-style variable browser possible without asking the
/// user to type addresses: the compiler already recorded each variable's name,
/// type, size and location, including the nesting of structs and arrays. Only
/// globals and statics are found -- locals live on the stack and have no fixed
/// address, so every tool of this kind (J-Scope included) is limited to the same
/// set.
///
/// Addresses come from `DW_OP_addr` when the compiler emits one; `SymbolFile`
/// fills the rest in from `.symtab`, because a linker symbol's `st_value` is the
/// load address and is more reliable than parsing location expressions.
class DwarfReader
{
public:
    static Result<QVector<VariableNode>> readVariables(const ElfFile &elf);

    /// DWARF version found in `.debug_info` (0 when absent). Diagnostics only.
    static int dwarfVersion(const ElfFile &elf);
};

} // namespace mrd
