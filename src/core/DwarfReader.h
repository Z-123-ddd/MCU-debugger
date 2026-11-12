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
    /// A function, shown so its locals can be listed under it.
    Function,
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
    /// This node is a `static` class member: it has its own address, not an offset
    /// inside the enclosing instance.
    bool staticMember = false;
    /// A file-scope variable the compiler recorded with no usable location (armclang
    /// writes `DW_OP_addr: 0` for link-time symbols). It is still a real declaration,
    /// so it is listed -- but it cannot be read, and saying so beats hiding it.
    bool addressUnresolved = false;
    /// Locals live at "frame pointer + offset", and the frame pointer is only known
    /// while the core is halted. The offset is kept signed and resolved at display
    /// time instead of baking in an address that would be stale immediately.
    qint32 stackOffset = 0;
    bool stackRelative = false;
    /// `.debug_loc` offset when the location is a list rather than one expression.
    ///
    /// This is the common case for locals (9285 in the ODrive image vs 1226 inline
    /// `DW_OP_fbreg`), and a list has to be evaluated at the halt PC: different
    /// ranges put the same variable in a register or on the stack.
    quint32 locListOffset = 0;
    int byteSize = 0;       ///< total bytes this node spans
    int elementSize = 0;    ///< scalar width, or the array's element width
    int elementCount = 1;   ///< array extent (1 for non-arrays)

    /// Source file this variable was declared in, from DWARF's line table
    /// (`DW_AT_decl_file` resolved through the CU's file-name table).
    ///
    /// Present because a flat list of 4000 names is unusable on a real project:
    /// grouping by file is how every debugger's watch window stays navigable.
    /// Empty when the compiler emitted no line table (or DWARF v5, whose table
    /// format this reader does not decode).
    QString sourceFile;
    /// Directory the file came from, as recorded in the line table. Used to tell the
    /// user's own code from chip-pack/compiler headers (`CMSIS`, `libcxx`,
    /// `ARMCLANG`, `ThirdParty`), so vendor tables do not clutter the tree.
    QString sourceDir;
    /// True when the source lives in a chip-pack / compiler / SDK directory.
    bool systemFile = false;
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
    ///
    /// Locals count too even though they have no address yet: their value becomes
    /// readable once the core is halted and the frame base is known.
    int leafCount() const;
    /// Sum of byteSize over the whole subtree's leaves.
    int totalNodeCount() const;
};

/// One member of a class/struct type, as declared in the source.
///
/// A member has no absolute address: it only exists inside an instance
/// (`encoders[0].index_found_`). The offset is what makes it locatable once an
/// instance is known, and it is what lets the UI explain where the data lives.
struct TypeMember
{
    QString name;
    QString typeLabel;
    int byteOffset = 0;
    int byteSize = 0;
    /// A `static` member (DW_AT_declaration / no data_member_location).
    ///
    /// It has its own address rather than an offset inside an instance, so it is a
    /// real static variable: `Encoder::ports_to_sample` lives at 0x200002F0 and is
    /// perfectly readable, while the instance it is "declared in" does not own it.
    bool isStatic = false;
    /// Resolved address of a static member (from .symtab); 0 when unknown.
    quint32 address = 0;
    bool hasAddress = false;
};

/// A class/struct/union *definition* and its members.
///
/// Listed per source file so a user opening `encoder.cpp` can see that `Encoder`
/// has `index_found_`, instead of hunting for whichever file happens to declare an
/// instance. Members of a type are reference data: they exist only through an
/// object, so they are shown with an offset rather than an address.
struct TypeDecl
{
    QString name;
    QString kind;        ///< "class" / "struct" / "union"
    QString sourceFile;  ///< where the type is defined
    int byteSize = 0;
    QVector<TypeMember> members;
};

/// One function that has local variables, as DWARF describes it.
///
/// Locals cannot be sampled like globals: they only exist while the core is halted
/// inside the function, and their address is `frameBase + stackOffset`. That is why
/// they are kept per function together with the PC range that identifies it.
struct FunctionScope
{
    QString name;          ///< qualified: "Axis::run_control"
    /// Source file the function was defined in (`DW_AT_decl_file`), so the variable
    /// tree can place its locals under the right file.
    QString sourceFile;
    quint32 lowPc = 0;
    quint32 highPc = 0;    ///< exclusive end
    int frameBaseRegister = -1; ///< -1 when DWARF did not give a usable register
    /// Added to the frame-base register to get the CFA. armclang gives 0 (the base
    /// is the register itself); GCC's DW_OP_call_frame_cfa needs the CFA offset from
    /// .debug_frame.
    qint32 frameBaseOffset = 0;
    /// Offset of the `.debug_frame` FDE covering this function, 0 when none.
    ///
    /// Needed when the frame base is `DW_OP_call_frame_cfa`: the CFA rule changes
    /// *within* a function (the prologue pushes registers), so it has to be
    /// evaluated at the PC the core is actually halted at, not at the entry.
    quint32 cfaFde = 0;
    QVector<VariableNode> locals;

    bool contains(quint32 pc) const { return pc >= lowPc && pc < highPc; }
    int localCount() const;
};

/// Everything one DWARF pass produces.
struct DwarfContents
{
    QVector<VariableNode> globals;
    QVector<FunctionScope> functions;
    /// Type definitions with their members, for the per-file type index.
    QVector<TypeDecl> types;
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
    /// Globals and per-function locals from a single pass (parsing a 4 MB image
    /// twice would double a ~1 s cost for no benefit).
    static Result<DwarfContents> read(const ElfFile &elf);

    /// Convenience wrapper when only the globals are needed.
    static Result<QVector<VariableNode>> readVariables(const ElfFile &elf);

    /// CFA rule at \a pc, read from `.debug_frame`: `CFA = registers[reg] + offset`.
    /// Returns false when the section is missing (armclang's DW_OP_reg13 style does
    /// not need it) or the PC is not covered.
    /// Where a local lives when the core is halted at some PC.
    struct LocalLoc
    {
        bool usable = false;
        bool inRegister = false; ///< value is in registers[reg]
        bool absolute = false;   ///< the expression was a plain DW_OP_addr
        int reg = -1;
        qint32 offset = 0;
        quint32 address = 0;     ///< when absolute
    };

    /// Resolve the `.debug_loc` entry covering the given PC.
    ///
    /// A location list is a sequence of [begin, end, expr] ranges. Only the entry
    /// containing the halted PC is valid, and different ranges legitimately put the
    /// same variable in a register or on the stack, so this cannot be pre-computed.
    static LocalLoc localAt(const ElfFile &elf, quint32 pc, quint32 listOffset);

    static bool cfaAt(const ElfFile &elf, quint32 pc, int *reg, qint32 *offset);

    /// True when a path belongs to the chip pack / compiler / SDK rather than the
    /// project. Public because the UI needs the same rule when filtering files.
    static bool isSystemPath(const QString &path);

    /// DWARF version found in `.debug_info` (0 when absent). Diagnostics only.
    static int dwarfVersion(const ElfFile &elf);
};

} // namespace mrd
