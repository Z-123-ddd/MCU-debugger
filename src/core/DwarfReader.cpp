#include "core/DwarfReader.h"

#include "core/Logger.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cstring>

namespace mrd {

QString scalarKindName(ScalarKind kind)
{
    switch (kind) {
    case ScalarKind::Signed:
        return QStringLiteral("有符号");
    case ScalarKind::Unsigned:
        return QStringLiteral("无符号");
    case ScalarKind::Float:
        return QStringLiteral("浮点");
    case ScalarKind::Boolean:
        return QStringLiteral("布尔");
    case ScalarKind::Pointer:
        return QStringLiteral("指针");
    case ScalarKind::Enum:
        return QStringLiteral("枚举");
    case ScalarKind::Composite:
        return QStringLiteral("结构体");
    case ScalarKind::Unknown:
        break;
    }
    return QStringLiteral("未知");
}

QString nodeKindName(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Scalar:
        return QStringLiteral("标量");
    case NodeKind::Array:
        return QStringLiteral("数组");
    case NodeKind::Struct:
        return QStringLiteral("结构体");
    case NodeKind::Union:
        return QStringLiteral("联合体");
    case NodeKind::Class:
        return QStringLiteral("类");
    case NodeKind::Pointer:
        return QStringLiteral("指针");
    case NodeKind::Enum:
        return QStringLiteral("枚举");
    }
    return QStringLiteral("未知");
}

int VariableNode::leafCount() const
{
    if (children.isEmpty()) {
        // A stack-relative local is displayable before it is addressable.
        const bool decodable = samplable()
            || (stackRelative && elementSize > 0 && scalar != ScalarKind::Unknown
                && scalar != ScalarKind::Composite);
        return decodable ? qMax(1, elementCount) : 0;
    }
    int total = 0;
    for (const VariableNode &child : children)
        total += child.leafCount();
    return total;
}

int VariableNode::totalNodeCount() const
{
    int total = 1;
    for (const VariableNode &child : children)
        total += child.totalNodeCount();
    return total;
}

int scalarKindSizeGuess(ScalarKind kind)
{
    switch (kind) {
    case ScalarKind::Signed:
    case ScalarKind::Unsigned:
    case ScalarKind::Float:
    case ScalarKind::Pointer:
        return 4;
    case ScalarKind::Boolean:
        return 1;
    default:
        return 0;
    }
}

namespace {

// --- DWARF constants (only what is used below) -------------------------------
constexpr quint64 TAG_ARRAY_TYPE = 0x01;
constexpr quint64 TAG_ENUMERATION_TYPE = 0x04;
constexpr quint64 TAG_MEMBER = 0x0d;
constexpr quint64 TAG_POINTER_TYPE = 0x0f;
constexpr quint64 TAG_COMPILE_UNIT = 0x11;
constexpr quint64 TAG_STRUCTURE_TYPE = 0x13;
constexpr quint64 TAG_TYPEDEF = 0x16;
constexpr quint64 TAG_CLASS_TYPE = 0x02;
constexpr quint64 TAG_NAMESPACE = 0x39;
constexpr quint64 TAG_UNION_TYPE = 0x17;
constexpr quint64 TAG_SUBRANGE_TYPE = 0x21;
constexpr quint64 TAG_BASE_TYPE = 0x24;
constexpr quint64 TAG_CONST_TYPE = 0x26;
constexpr quint64 TAG_VARIABLE = 0x34;
constexpr quint64 TAG_SUBPROGRAM = 0x2e;
constexpr quint64 TAG_FORMAL_PARAMETER = 0x05;
constexpr quint64 TAG_LEXICAL_BLOCK = 0x0b;
constexpr quint64 TAG_VOLATILE_TYPE = 0x35;
constexpr quint64 TAG_RESTRICT_TYPE = 0x37;
constexpr quint64 TAG_ATOMIC_TYPE = 0x47;

constexpr quint64 AT_NAME = 0x03;
constexpr quint64 AT_LOCATION = 0x02;
constexpr quint64 AT_BYTE_SIZE = 0x0b;
constexpr quint64 AT_LOW_PC = 0x11;
constexpr quint64 AT_HIGH_PC = 0x12;
constexpr quint64 AT_FRAME_BASE = 0x40;
constexpr quint64 AT_DECL_FILE = 0x3a;
constexpr quint64 AT_STMT_LIST = 0x10;
constexpr quint64 AT_COUNT = 0x37;
constexpr quint64 AT_DATA_MEMBER_LOCATION = 0x38;
constexpr quint64 AT_DECLARATION = 0x3c;
constexpr quint64 AT_ENCODING = 0x3e;
constexpr quint64 AT_UPPER_BOUND = 0x2f;
constexpr quint64 AT_LOWER_BOUND = 0x22;
constexpr quint64 AT_TYPE = 0x49;
constexpr quint64 AT_BIT_SIZE = 0x0d;
constexpr quint64 AT_LINKAGE_NAME = 0x6e;
constexpr quint64 AT_MIPS_LINKAGE_NAME = 0x2007;
constexpr quint64 AT_STR_OFFSETS_BASE = 0x72;

constexpr quint64 FORM_ADDR = 0x01;
constexpr quint64 FORM_BLOCK2 = 0x03;
constexpr quint64 FORM_BLOCK4 = 0x04;
constexpr quint64 FORM_DATA2 = 0x05;
constexpr quint64 FORM_DATA4 = 0x06;
constexpr quint64 FORM_DATA8 = 0x07;
constexpr quint64 FORM_STRING = 0x08;
constexpr quint64 FORM_BLOCK = 0x09;
constexpr quint64 FORM_BLOCK1 = 0x0a;
constexpr quint64 FORM_DATA1 = 0x0b;
constexpr quint64 FORM_FLAG = 0x0c;
constexpr quint64 FORM_SDATA = 0x0d;
constexpr quint64 FORM_STRP = 0x0e;
constexpr quint64 FORM_UDATA = 0x0f;
constexpr quint64 FORM_REF_ADDR = 0x10;
constexpr quint64 FORM_REF1 = 0x11;
constexpr quint64 FORM_REF2 = 0x12;
constexpr quint64 FORM_REF4 = 0x13;
constexpr quint64 FORM_REF8 = 0x14;
constexpr quint64 FORM_REF_UDATA = 0x15;
constexpr quint64 FORM_INDIRECT = 0x16;
constexpr quint64 FORM_SEC_OFFSET = 0x17;
constexpr quint64 FORM_EXPRLOC = 0x18;
constexpr quint64 FORM_FLAG_PRESENT = 0x19;
constexpr quint64 FORM_STRX = 0x1a;
constexpr quint64 FORM_ADDRX = 0x1b;
constexpr quint64 FORM_DATA16 = 0x1e;
constexpr quint64 FORM_LINE_STRP = 0x1f;
constexpr quint64 FORM_IMPLICIT_CONST = 0x21;
constexpr quint64 FORM_LOCLISTX = 0x22;
constexpr quint64 FORM_RNGLISTX = 0x23;
constexpr quint64 FORM_STRX1 = 0x25;
constexpr quint64 FORM_STRX2 = 0x26;
constexpr quint64 FORM_STRX3 = 0x27;
constexpr quint64 FORM_STRX4 = 0x28;
constexpr quint64 FORM_ADDRX1 = 0x29;
constexpr quint64 FORM_ADDRX2 = 0x2a;
constexpr quint64 FORM_ADDRX3 = 0x2b;
constexpr quint64 FORM_ADDRX4 = 0x2c;

constexpr int ATE_ADDRESS = 0x01;
constexpr int ATE_BOOLEAN = 0x02;
constexpr int ATE_COMPLEX_FLOAT = 0x03;
constexpr int ATE_FLOAT = 0x04;
constexpr int ATE_SIGNED = 0x05;
constexpr int ATE_SIGNED_CHAR = 0x06;
constexpr int ATE_UNSIGNED = 0x07;
constexpr int ATE_UNSIGNED_CHAR = 0x08;

constexpr quint8 OP_ADDR = 0x03;
constexpr quint8 OP_REG0 = 0x50;
constexpr quint8 OP_BREG0 = 0x70;
constexpr quint8 OP_FBREG = 0x91;
constexpr quint8 OP_CALL_FRAME_CFA = 0x9c;

// DWARF call-frame instructions (only the CFA-relevant ones are decoded).
constexpr quint8 CFA_NOP = 0x00;
constexpr quint8 CFA_ADVANCE_LOC = 0x40;
constexpr quint8 CFA_DEF_CFA = 0x0c;
constexpr quint8 CFA_DEF_CFA_REGISTER = 0x0d;
constexpr quint8 CFA_DEF_CFA_OFFSET = 0x0e;
constexpr quint8 CFA_REMEMBER_STATE = 0x0a;
constexpr quint8 CFA_RESTORE_STATE = 0x0b;

/// Bounds-checked cursor over a DWARF section.
struct Cursor
{
    const char *base = nullptr;
    int size = 0;
    int pos = 0;
    bool le = true;

    bool ok() const { return pos <= size; }
    int remaining() const { return size - pos; }

    const char *at(int offset) const { return base + offset; }

    quint64 readU(int bytes)
    {
        if (bytes <= 0 || pos + bytes > size) {
            pos = size + 1;
            return 0;
        }
        const uchar *b = reinterpret_cast<const uchar *>(base + pos);
        quint64 value = 0;
        if (le) {
            for (int i = bytes - 1; i >= 0; --i)
                value = (value << 8) | b[i];
        } else {
            for (int i = 0; i < bytes; ++i)
                value = (value << 8) | b[i];
        }
        pos += bytes;
        return value;
    }

    quint64 readUleb()
    {
        quint64 result = 0;
        int shift = 0;
        while (pos < size) {
            const quint8 byte = static_cast<quint8>(base[pos++]);
            if (shift < 64)
                result |= static_cast<quint64>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0)
                return result;
        }
        pos = size + 1;
        return result;
    }

    qint64 readSleb()
    {
        qint64 result = 0;
        int shift = 0;
        quint8 byte = 0;
        while (pos < size) {
            byte = static_cast<quint8>(base[pos++]);
            if (shift < 64)
                result |= static_cast<qint64>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0)
                break;
        }
        if (shift < 64 && (byte & 0x40))
            result |= -(Q_INT64_C(1) << shift);
        return result;
    }

    QString readCString()
    {
        const int start = pos;
        while (pos < size && base[pos] != '\0')
            ++pos;
        const QString text = QString::fromUtf8(base + start, pos - start);
        if (pos < size)
            ++pos; // consume the terminator
        return text;
    }

    QByteArray readBytes(int count)
    {
        if (count <= 0 || pos + count > size) {
            pos = size + 1;
            return {};
        }
        const QByteArray out(base + pos, count);
        pos += count;
        return out;
    }

    void skip(int count)
    {
        if (count <= 0 || pos + count > size) {
            pos = size + 1;
            return;
        }
        pos += count;
    }
};

struct AttrSpec
{
    quint64 name = 0;
    quint64 form = 0;
    qint64 implicitConst = 0;
};

struct Abbrev
{
    quint64 tag = 0;
    bool children = false;
    QVector<AttrSpec> attrs;
};

struct FormValue
{
    quint64 u = 0;
    qint64 s = 0;
    QString text;
    QByteArray bytes;
    quint64 ref = 0;   ///< resolved DIE offset (absolute, already CU-adjusted)
    bool isString = false;
    bool isBlock = false;
};

/// Everything the type resolver needs from one DIE.
struct Die
{
    quint64 offset = 0;
    quint64 parent = 0; ///< enclosing DIE (0 at CU level)
    quint64 tag = 0;
    QHash<quint64, FormValue> attrs;

    bool has(quint64 name) const { return attrs.contains(name); }
    FormValue value(quint64 name) const { return attrs.value(name); }
    quint64 u(quint64 name, quint64 fallback = 0) const
    {
        const auto it = attrs.constFind(name);
        return it == attrs.constEnd() ? fallback : it->u;
    }
    qint64 s(quint64 name, qint64 fallback = 0) const
    {
        const auto it = attrs.constFind(name);
        return it == attrs.constEnd() ? fallback : it->s;
    }
    QString text(quint64 name) const
    {
        const auto it = attrs.constFind(name);
        return it == attrs.constEnd() ? QString() : it->text;
    }
};

/// One compilation unit's view of the section offsets it references.
struct UnitContext
{
    quint64 start = 0;         ///< DIE offset where this CU's DIEs begin
    quint64 abbrevOffset = 0;
    int addressSize = 4;
    int version = 4;
    quint64 strOffsetsBase = 0;
    /// `.debug_line` file-name table for this unit, index 1 = first entry
    /// (DWARF uses 1-based indices in DW_AT_decl_file).
    QStringList fileNames;
};

class Parser
{
public:
    Parser(const ElfFile &elf, const QByteArray &info, const QByteArray &abbrev,
           const QByteArray &str, const QByteArray &strOffsets,
           const QByteArray &debugLoc, const QByteArray &debugFrame,
           const QByteArray &debugLine)
        : m_elf(elf), m_info(info), m_abbrev(abbrev), m_str(str),
          m_strOffsets(strOffsets), m_debugLoc(debugLoc), m_debugFrame(debugFrame),
          m_debugLine(debugLine)
    {
        m_le = elf.littleEndian();
    }

    Result<DwarfContents> run();

private:
    bool parseAbbrevTable(quint64 offset, QHash<quint64, Abbrev> *out);
    bool readUnit(Cursor &info, UnitContext *unit, QVector<Die> *dies, quint64 *nextOffset);
    FormValue readForm(quint64 form, Cursor &c, const UnitContext &unit, qint64 implicitConst);
    QString resolveString(quint64 form, const FormValue &value, const UnitContext &unit);

    struct TypeInfo
    {
        ScalarKind kind = ScalarKind::Unknown;
        QString display;
        int elementSize = 0;
        int elementCount = 1;
        int totalSize = 0;
        quint64 elementTypeRef = 0;
    };
    TypeInfo resolveType(quint64 typeOffset, int depth);
    const Die *dieAt(quint64 offset) const;

    void emitVariable(const Die &variable, const UnitContext &unit, QVector<VariableNode> *out);

    /// Collect the locals of one DW_TAG_subprogram. Only frame-relative and
    /// register-homed variables are meaningful before the core is halted at that
    /// exact function, which is why they are kept per function.
    void collectFunction(const Die &subprogram, const UnitContext &unit,
                         QVector<FunctionScope> *out);

    /// Canonical-frame-address rule for a PC, from `.debug_frame`.
    ///
    /// GCC emits `DW_AT_frame_base = DW_OP_call_frame_cfa` for locals, which means
    /// "the CFA of this PC" -- the address just above the frame. It is `SP + N`
    /// (or `frame pointer + N`) and only `.debug_frame` knows N. Guessing `SP`
    /// would silently read the wrong memory, so this is resolved properly.
    struct Cfa
    {
        int reg = -1;   ///< register the CFA is relative to (-1 = unknown)
        qint32 offset = 0;
        bool valid() const { return reg >= 0; }
    };
public:
    Cfa cfaForPc(quint32 pc);

    /// Locate the FDE that covers \a pc: its entry offset (in .debug_frame) and the
    /// CIE offset it references. Returns false when nothing covers the PC.
    bool findFde(quint32 pc, quint64 *fdeEntry, quint64 *cieEntry);

    /// File-name table at  offset in `.debug_line` (DWARF 2-4 prologue format).
    ///
    /// Only the prologue is decoded: the reader needs "which source file is index N"
    /// (for DW_AT_decl_file), not line numbers. Version 5 changed the entry format to
    /// a self-describing one; it is left unsupported and yields an empty table so
    /// grouping simply falls back to a flat list rather than guessing.
    QStringList fileTableAt(quint64 offset);

private:
    static qint64 readSleb(const char **cursor, const char *end);

    /// Add one local's subtree with `stackRelative` set (address resolved later).
    void emitLocal(const Die &variable, qint32 frameOffset, int registerNumber,
                   const UnitContext &unit, FunctionScope *scope);

    /// Build one subtree: a struct becomes a node with member children, an array a
    /// node with element children, a scalar a leaf. Returns false when the type is
    /// unusable (an unresolved forward declaration, an unknown encoding).
    ///
    /// This replaces the old "flatten everything into `a.b.c` strings" walk. Keeping
    /// the shape means the UI can show `g_points` as an array of two `Point`s with
    /// their members, which is what the source says.
    bool buildNode(const QString &name, quint64 typeOffset, quint32 address, bool hasAddress,
                   int depth, VariableNode *out);

    /// Node count still allowed for the variable being built. A single huge object
    /// (ODrive's 14 KB `controllers`) must not produce a hundred thousand rows.
    int m_nodeBudget = 0;
    static constexpr int kNodeBudgetPerRoot = 4000;
    /// Array elements expanded into child nodes. Beyond this the node still shows
    /// its extent, it just does not materialise every element.
    static constexpr int kMaxArrayChildren = 64;

    /// Address from a `DW_AT_location` location list in `.debug_loc`.
    ///
    /// A compiler may put a variable's address in a location list instead of an
    /// inline `DW_OP_addr`. For a statically allocated variable the list holds one
    /// range whose expression is exactly `DW_OP_addr`, so the answer is simply the
    /// first such entry. Entries describing registers or frame offsets belong to
    /// locals and are skipped -- those have no fixed address by nature.
    quint32 addressFromLocationList(quint64 listOffset, const UnitContext &unit);

    /// "ns::Class::name" for a C++ variable, so two same-named statics in
    /// different namespaces stay distinguishable in the list.
    QString qualifiedName(quint64 dieOffset, const QString &leaf) const;

    /// Element count of an array DIE (1 when the extent cannot be determined).
    int arrayExtent(quint64 arrayTypeOffset) const;

    /// C++ types are often referenced by a forward declaration DIE in one CU while
    /// the definition lives in another. `m_typeDefinitions` maps a type name to the
    /// DIE that actually carries the layout.
    QHash<QString, const Die *> m_typeDefinitions;

    /// Struct/class/union DIE that owns the layout for  typeOffset, following
    /// typedefs and forward declarations.
    const Die *layoutOf(quint64 typeOffset) const;

    /// Per-object budget for struct/array expansion, so one huge object cannot
    /// produce a hundred thousand rows. It is reset for every top-level variable:
    /// a file-wide budget gets consumed by the first few big objects, which made

    const ElfFile &m_elf;
    QByteArray m_info;
    QByteArray m_abbrev;
    QByteArray m_str;
    QByteArray m_strOffsets;
    QByteArray m_debugLoc;
    QByteArray m_debugFrame;
    QByteArray m_debugLine;
    /// stmt_list offset -> file table, so units sharing a line table parse it once.
    QHash<quint64, QStringList> m_fileTables;
    QHash<quint32, Cfa> m_cfaCache;
    bool m_le = true;
    QHash<quint64, QHash<quint64, Abbrev>> m_abbrevCache; ///< by abbrev-section offset
    QHash<quint64, Die> m_dies;                            ///< by absolute offset
    QSet<quint64> m_resolving;                             ///< cycle guard for types
    /// Child DIEs by parent offset. DWARF puts an array's extent on a child
    /// subrange and a struct's layout on child members, so the tree has to be
    /// reconstructed while walking -- it cannot be found by scanning.
    QHash<quint64, QVector<Die>> m_children;
    int m_dwarfVersion = 0;

    // Diagnostics: a CU that fails is currently skipped silently, which makes a
    // nearly-empty result look like "the file has no variables".
    int m_unitsSeen = 0;
    int m_unitsFailed = 0;
    int m_variablesSeen = 0;
    int m_variablesNoAddress = 0;
    int m_variablesNoType = 0;
    int m_compositesSkipped = 0;
    QStringList m_failures;
};

bool Parser::parseAbbrevTable(quint64 offset, QHash<quint64, Abbrev> *out)
{
    const auto cached = m_abbrevCache.constFind(offset);
    if (cached != m_abbrevCache.constEnd()) {
        *out = cached.value();
        return true;
    }
    Cursor c{m_abbrev.constData(), static_cast<int>(m_abbrev.size()),
             static_cast<int>(offset), m_le};
    QHash<quint64, Abbrev> table;
    while (c.ok() && c.remaining() > 0) {
        const quint64 code = c.readUleb();
        if (!c.ok())
            return false;
        if (code == 0)
            break; // end of this table
        Abbrev abbrev;
        abbrev.tag = c.readUleb();
        if (!c.ok() || c.remaining() < 1)
            return false;
        abbrev.children = (c.at(c.pos)[0] != 0);
        c.skip(1);
        while (c.ok()) {
            const quint64 name = c.readUleb();
            const quint64 form = c.readUleb();
            if (!c.ok())
                return false;
            if (name == 0 && form == 0)
                break;
            AttrSpec spec{name, form, 0};
            if (form == FORM_IMPLICIT_CONST)
                spec.implicitConst = c.readSleb();
            abbrev.attrs.append(spec);
        }
        if (!c.ok())
            return false;
        table.insert(code, abbrev);
    }
    m_abbrevCache.insert(offset, table);
    *out = table;
    return true;
}

FormValue Parser::readForm(quint64 form, Cursor &c, const UnitContext &unit, qint64 implicitConst)
{
    FormValue v;
    switch (form) {
    case FORM_ADDR:
        v.u = c.readU(unit.addressSize);
        return v;
    case FORM_DATA1:
    case FORM_FLAG:
        v.u = c.readU(1);
        v.s = static_cast<qint8>(v.u);
        return v;
    case FORM_DATA2:
        v.u = c.readU(2);
        v.s = static_cast<qint16>(v.u);
        return v;
    case FORM_DATA4:
    case FORM_SEC_OFFSET:
        v.u = c.readU(4);
        v.s = static_cast<qint32>(v.u);
        return v;
    case FORM_DATA8:
        v.u = c.readU(8);
        v.s = static_cast<qint64>(v.u);
        return v;
    case FORM_DATA16:
        v.bytes = c.readBytes(16);
        v.isBlock = true;
        return v;
    case FORM_SDATA:
        v.s = c.readSleb();
        v.u = static_cast<quint64>(v.s);
        return v;
    case FORM_UDATA:
        v.u = c.readUleb();
        v.s = static_cast<qint64>(v.u);
        return v;
    case FORM_STRING:
        v.text = c.readCString();
        v.isString = true;
        return v;
    case FORM_STRP:
    case FORM_LINE_STRP: {
        const quint64 off = c.readU(4);
        if (off < static_cast<quint64>(m_str.size()))
            v.text = QString::fromUtf8(m_str.constData() + off,
                                       qstrnlen(m_str.constData() + off,
                                                m_str.size() - static_cast<int>(off)));
        v.isString = true;
        return v;
    }
    case FORM_STRX:
    case FORM_STRX1:
    case FORM_STRX2:
    case FORM_STRX3:
    case FORM_STRX4: {
        quint64 index = 0;
        if (form == FORM_STRX)
            index = c.readUleb();
        else
            index = c.readU(static_cast<int>(form - FORM_STRX1 + 1));
        // .debug_str_offsets holds 32-bit offsets; index is relative to the CU base.
        const quint64 at = unit.strOffsetsBase + index * 4;
        if (at + 4 <= static_cast<quint64>(m_strOffsets.size())) {
            Cursor sc{m_strOffsets.constData(), static_cast<int>(m_strOffsets.size()),
                      static_cast<int>(at), m_le};
            const quint64 off = sc.readU(4);
            if (off < static_cast<quint64>(m_str.size()))
                v.text = QString::fromUtf8(m_str.constData() + off,
                                           qstrnlen(m_str.constData() + off,
                                                    m_str.size() - static_cast<int>(off)));
        }
        v.isString = true;
        return v;
    }
    case FORM_REF1:
        v.ref = unit.start + c.readU(1);
        v.u = v.ref;
        return v;
    case FORM_REF2:
        v.ref = unit.start + c.readU(2);
        v.u = v.ref;
        return v;
    case FORM_REF4:
        v.ref = unit.start + c.readU(4);
        v.u = v.ref;
        return v;
    case FORM_REF8:
        v.ref = unit.start + c.readU(8);
        v.u = v.ref;
        return v;
    case FORM_REF_UDATA:
        v.ref = unit.start + c.readUleb();
        v.u = v.ref;
        return v;
    case FORM_REF_ADDR:
        v.ref = c.readU(4);
        v.u = v.ref;
        return v;
    case FORM_BLOCK1: {
        const quint64 len = c.readU(1);
        v.bytes = c.readBytes(static_cast<int>(len));
        v.isBlock = true;
        return v;
    }
    case FORM_BLOCK2: {
        const quint64 len = c.readU(2);
        v.bytes = c.readBytes(static_cast<int>(len));
        v.isBlock = true;
        return v;
    }
    case FORM_BLOCK4: {
        const quint64 len = c.readU(4);
        v.bytes = c.readBytes(static_cast<int>(len));
        v.isBlock = true;
        return v;
    }
    case FORM_BLOCK:
    case FORM_EXPRLOC: {
        const quint64 len = c.readUleb();
        v.bytes = c.readBytes(static_cast<int>(len));
        v.isBlock = true;
        return v;
    }
    case FORM_FLAG_PRESENT:
        v.u = 1;
        v.s = 1;
        return v;
    case FORM_IMPLICIT_CONST:
        v.s = implicitConst;
        v.u = static_cast<quint64>(implicitConst);
        return v;
    case FORM_LOCLISTX:
    case FORM_RNGLISTX:
        v.u = c.readUleb();
        return v;
    case FORM_ADDRX:
        v.u = c.readUleb();
        return v;
    case FORM_ADDRX1:
    case FORM_ADDRX2:
    case FORM_ADDRX3:
    case FORM_ADDRX4:
        v.u = c.readU(static_cast<int>(form - FORM_ADDRX1 + 1));
        return v;
    case FORM_INDIRECT: {
        const quint64 real = c.readUleb();
        return readForm(real, c, unit, implicitConst);
    }
    default:
        // Unknown form: the DIE cannot be measured, so stop trusting this unit.
        c.pos = c.size + 1;
        return v;
    }
}

bool Parser::readUnit(Cursor &info, UnitContext *unit, QVector<Die> *dies, quint64 *nextOffset)
{
    const int unitStart = info.pos;
    const quint64 length = info.readU(4);
    if (!info.ok() || length == 0 || length == 0xffffffffu)
        return false;
    const int unitEnd = unitStart + 4 + static_cast<int>(length);
    if (unitEnd > info.size || unitEnd <= info.pos)
        return false;

    unit->version = static_cast<int>(info.readU(2));
    m_dwarfVersion = unit->version;
    if (!info.ok())
        return false;

    if (unit->version >= 5) {
        const quint64 unitType = info.readU(1);
        unit->addressSize = static_cast<int>(info.readU(1));
        unit->abbrevOffset = info.readU(4);
        // DW_UT_skeleton(4) / split_compile(5) / split_type(6) carry an extra dwo id;
        // type units carry a signature and type offset. None appear in MCU builds,
        // but skipping them keeps the cursor honest if they ever do.
        if (unitType == 4 || unitType == 5 || unitType == 6)
            info.skip(8);
        else if (unitType == 2 || unitType == 6)
            info.skip(8 + 4);
    } else {
        unit->abbrevOffset = info.readU(4);
        unit->addressSize = static_cast<int>(info.readU(1));
    }
    if (!info.ok() || unit->addressSize <= 0 || unit->addressSize > 8)
        return false;
    if (unit->version < 2 || unit->version > 5)
        return false;

    // DW_FORM_ref1/2/4/8/udata are offsets *relative to the start of this
    // compilation unit*, i.e. to the unit_length field -- not to the first DIE.
    // Using the DIE position here silently pointed every type reference at the
    // wrong DIE, so all types came back unknown.
    unit->start = static_cast<quint64>(unitStart);

    QHash<quint64, Abbrev> table;
    if (!parseAbbrevTable(unit->abbrevOffset, &table))
        return false;

    // Walk the DIE tree, recording each DIE's attributes. A null entry closes the
    // current sibling chain, which is what makes the children stack work.
    QVector<quint64> ancestors;
    while (info.pos < unitEnd && info.ok()) {
        const quint64 dieOffset = static_cast<quint64>(info.pos);
        const quint64 code = info.readUleb();
        if (!info.ok())
            break;
        if (code == 0) {
            if (ancestors.isEmpty())
                break; // top-level null: end of this unit
            ancestors.removeLast();
            continue;
        }
        const Abbrev abbrev = table.value(code);
        if (abbrev.tag == 0) {
            // An abbrev code we have no definition for: the rest of the unit is
            // unreadable, so stop here rather than reporting garbage.
            break;
        }
        Die die;
        die.offset = dieOffset;
        die.tag = abbrev.tag;
        for (const AttrSpec &spec : abbrev.attrs) {
            const FormValue value = readForm(spec.form, info, *unit, spec.implicitConst);
            if (!info.ok())
                break;
            die.attrs.insert(spec.name, value);
            // DWARF5 records the string-offset table base on the CU DIE itself; the
            // names of every later DIE depend on it.
            if (spec.name == AT_STR_OFFSETS_BASE)
                unit->strOffsetsBase = value.u;
            // The line table holds the file names that DW_AT_decl_file indexes into.
            // Offset 0 is a *valid* line table (it is the first one in the section),
            // so the presence of the attribute is what matters, not a non-zero value
            // -- testing `!= 0` silently dropped the first compilation unit's files.
            if (spec.name == AT_STMT_LIST)
                unit->fileNames = fileTableAt(value.u);
        }
        if (!info.ok())
            break;
        if (!ancestors.isEmpty()) {
            die.parent = ancestors.last();
            m_children[ancestors.last()].append(die);
        }
        if (abbrev.children)
            ancestors.append(die.offset);
        dies->append(die);
        m_dies.insert(die.offset, die);
        if (die.tag == TAG_VARIABLE)
            ++m_variablesSeen;
    }

    *nextOffset = static_cast<quint64>(unitEnd);
    info.pos = unitEnd;
    return info.ok();
}

quint32 Parser::addressFromLocationList(quint64 listOffset, const UnitContext &unit)
{
    if (m_debugLoc.isEmpty() || listOffset >= static_cast<quint64>(m_debugLoc.size()))
        return 0;

    Cursor c{m_debugLoc.constData(), static_cast<int>(m_debugLoc.size()),
             static_cast<int>(listOffset), m_le};
    const quint64 maxAddress = unit.addressSize >= 4 ? 0xFFFFFFFFull
                                                     : ((Q_UINT64_C(1) << (unit.addressSize * 8)) - 1);
    quint64 base = 0;
    // A list is a sequence of [begin, end, uint16 len, expr]; (0,0) ends it and
    // (maxAddress, base) sets the base other entries are relative to.
    for (int guard = 0; guard < 4096; ++guard) {
        const quint64 begin = c.readU(unit.addressSize);
        const quint64 end = c.readU(unit.addressSize);
        if (!c.ok())
            return 0;
        if (begin == 0 && end == 0)
            return 0; // end of list, nothing static here
        if (begin == maxAddress) {
            base = end;
            continue;
        }
        const quint64 length = c.readU(2);
        if (!c.ok())
            return 0;
        const QByteArray expr = c.readBytes(static_cast<int>(length));
        if (!c.ok())
            return 0;
        if (expr.size() >= 1 + unit.addressSize
            && static_cast<quint8>(expr.at(0)) == OP_ADDR) {
            const uchar *b = reinterpret_cast<const uchar *>(expr.constData() + 1);
            quint64 value = 0;
            if (m_le) {
                for (int i = unit.addressSize - 1; i >= 0; --i)
                    value = (value << 8) | b[i];
            } else {
                for (int i = 0; i < unit.addressSize; ++i)
                    value = (value << 8) | b[i];
            }
            // Some producers store the address relative to a base entry.
            if (base != 0 && value < base)
                value += base;
            return static_cast<quint32>(value);
        }
        // Not an address (register or frame offset): a local, so keep looking for a
        // static entry -- there is at most one per variable in practice.
    }
    return 0;
}

QString Parser::qualifiedName(quint64 dieOffset, const QString &leaf) const
{
    QStringList scope;
    quint64 cursor = dieOffset;
    for (int depth = 0; depth < 16; ++depth) {
        const Die *die = dieAt(cursor);
        if (!die || die->parent == 0)
            break;
        const Die *parent = dieAt(die->parent);
        if (!parent)
            break;
        // Only C++ scopes that name the variable; other DIEs are skipped so the
        // path stays readable.
        if (parent->tag == TAG_NAMESPACE || parent->tag == TAG_CLASS_TYPE
            || parent->tag == TAG_STRUCTURE_TYPE || parent->tag == TAG_UNION_TYPE) {
            const QString name = parent->text(AT_NAME);
            if (!name.isEmpty())
                scope.prepend(name);
        }
        cursor = die->parent;
    }
    if (scope.isEmpty())
        return leaf;
    return scope.join(QStringLiteral("::")) + QStringLiteral("::") + leaf;
}

const Die *Parser::dieAt(quint64 offset) const
{
    const auto it = m_dies.constFind(offset);
    return it == m_dies.constEnd() ? nullptr : &it.value();
}

int Parser::arrayExtent(quint64 arrayTypeOffset) const
{
    const Die *die = dieAt(arrayTypeOffset);
    if (!die || die->tag != TAG_ARRAY_TYPE)
        return 1;
    if (die->has(AT_COUNT))
        return qMax(1, static_cast<int>(die->u(AT_COUNT, 1)));
    if (die->has(AT_UPPER_BOUND))
        return qMax(1, static_cast<int>(die->s(AT_UPPER_BOUND, 0)) + 1);
    // The usual place is a child DW_TAG_subrange_type.
    for (const Die &child : m_children.value(arrayTypeOffset)) {
        if (child.tag != TAG_SUBRANGE_TYPE)
            continue;
        if (child.has(AT_COUNT))
            return qMax(1, static_cast<int>(child.u(AT_COUNT, 1)));
        if (child.has(AT_UPPER_BOUND))
            return qMax(1, static_cast<int>(child.s(AT_UPPER_BOUND, 0)) + 1);
    }
    return 1;
}

Parser::TypeInfo Parser::resolveType(quint64 typeOffset, int depth)
{
    TypeInfo info;
    if (typeOffset == 0 || depth > 12)
        return info;
    if (m_resolving.contains(typeOffset))
        return info; // typedef/struct cycle
    const Die *die = dieAt(typeOffset);
    if (!die)
        return info;

    m_resolving.insert(typeOffset);
    switch (die->tag) {
    case TAG_BASE_TYPE: {
        const int encoding = static_cast<int>(die->u(AT_ENCODING, 0));
        info.elementSize = static_cast<int>(die->u(AT_BYTE_SIZE, 0));
        info.display = die->text(AT_NAME);
        switch (encoding) {
        case ATE_FLOAT:
        case ATE_COMPLEX_FLOAT:
            info.kind = ScalarKind::Float;
            break;
        case ATE_BOOLEAN:
            info.kind = ScalarKind::Boolean;
            break;
        case ATE_SIGNED:
        case ATE_SIGNED_CHAR:
            info.kind = ScalarKind::Signed;
            break;
        case ATE_UNSIGNED:
        case ATE_UNSIGNED_CHAR:
            info.kind = ScalarKind::Unsigned;
            break;
        case ATE_ADDRESS:
            info.kind = ScalarKind::Pointer;
            break;
        default:
            // No encoding: fall back to size so the value can still be sampled.
            info.kind = ScalarKind::Unsigned;
            break;
        }
        break;
    }
    case TAG_TYPEDEF:
    case TAG_CONST_TYPE:
    case TAG_VOLATILE_TYPE:
    case TAG_RESTRICT_TYPE:
    case TAG_ATOMIC_TYPE: {
        const TypeInfo base = resolveType(die->u(AT_TYPE, 0), depth + 1);
        info = base;
        // A typedef's own name is what the user wrote, so prefer it for display
        // while keeping the underlying encoding.
        const QString name = die->text(AT_NAME);
        if (die->tag == TAG_TYPEDEF && !name.isEmpty())
            info.display = name;
        break;
    }
    case TAG_POINTER_TYPE:
        info.kind = ScalarKind::Pointer;
        info.elementSize = static_cast<int>(die->u(AT_BYTE_SIZE, 0));
        if (info.elementSize <= 0)
            info.elementSize = 4;
        info.display = die->text(AT_NAME);
        if (info.display.isEmpty())
            info.display = QStringLiteral("void *");
        break;
    case TAG_ENUMERATION_TYPE:
        // Enums are stored as ints; the underlying size decides the width.
        info.elementSize = static_cast<int>(die->u(AT_BYTE_SIZE, 4));
        if (info.elementSize <= 0)
            info.elementSize = 4;
        // Stored as an integer; the kind is kept distinct only so the label can say
        // "enum" and the decoder still treats it as a signed number.
        info.kind = ScalarKind::Enum;
        info.display = die->text(AT_NAME);
        if (info.display.isEmpty())
            info.display = QStringLiteral("enum");
        break;
    case TAG_ARRAY_TYPE: {
        const quint64 elementType = die->u(AT_TYPE, 0);
        TypeInfo element = resolveType(elementType, depth + 1);
        const int count = arrayExtent(typeOffset);
        info = element;
        info.elementCount = count * element.elementCount;
        info.elementTypeRef = elementType;
        const int declared = static_cast<int>(die->u(AT_BYTE_SIZE, 0));
        info.totalSize = declared > 0 ? declared : element.elementSize * info.elementCount;
        if (element.kind == ScalarKind::Unknown)
            info.kind = ScalarKind::Unknown;
        break;
    }
    case TAG_STRUCTURE_TYPE:
    case TAG_UNION_TYPE:
    case TAG_CLASS_TYPE: {
        // A C++ forward declaration ("class Controller;" as DW_TAG_class_type with
        // DW_AT_declaration) has no size and no children. Following it to the real
        // definition is what makes `controllers` -- and every other C++ object --
        // resolvable at all.
        const Die *definition = die;
        if (die->u(AT_DECLARATION, 0) != 0 || die->u(AT_BYTE_SIZE, 0) == 0) {
            if (const Die *found = m_typeDefinitions.value(die->text(AT_NAME)); found)
                definition = found;
        }
        info.kind = ScalarKind::Composite;
        info.display = definition->text(AT_NAME);
        if (info.display.isEmpty()) {
            info.display = definition->tag == TAG_UNION_TYPE ? QStringLiteral("union")
                                                             : QStringLiteral("struct");
        }
        info.totalSize = static_cast<int>(definition->u(AT_BYTE_SIZE, 0));
        info.elementSize = info.totalSize;
        info.elementTypeRef = definition->offset;
        break;
    }
    default:
        info.kind = ScalarKind::Unknown;
        break;
    }
    m_resolving.remove(typeOffset);
    if (info.totalSize == 0 && info.elementSize > 0)
        info.totalSize = info.elementSize * info.elementCount;
    return info;
}

bool Parser::buildNode(const QString &name, quint64 typeOffset, quint32 address,
                       bool hasAddress, int depth, VariableNode *out)
{
    if (!out || typeOffset == 0)
        return false;
    if (depth > 12 || m_nodeBudget <= 0)
        return false;

    // Walk through typedefs and qualifiers, remembering the typedef name because
    // that is what the user wrote ("uint16_t" rather than "unsigned short").
    quint64 current = typeOffset;
    QString alias;
    const Die *die = nullptr;
    for (int i = 0; i < 10; ++i) {
        die = dieAt(current);
        if (!die)
            return false;
        if (die->tag == TAG_TYPEDEF) {
            if (alias.isEmpty())
                alias = die->text(AT_NAME);
            current = die->u(AT_TYPE, 0);
            if (current == 0)
                return false;
            continue;
        }
        if (die->tag == TAG_CONST_TYPE || die->tag == TAG_VOLATILE_TYPE
            || die->tag == TAG_RESTRICT_TYPE || die->tag == TAG_ATOMIC_TYPE) {
            current = die->u(AT_TYPE, 0);
            if (current == 0)
                return false;
            continue;
        }
        break;
    }
    if (!die)
        return false;

    --m_nodeBudget;
    out->name = name;
    out->address = address;
    out->hasAddress = hasAddress;

    switch (die->tag) {
    case TAG_ARRAY_TYPE: {
        const quint64 elementType = die->u(AT_TYPE, 0);
        const TypeInfo element = resolveType(elementType, depth + 1);
        out->kind = NodeKind::Array;
        out->elementCount = arrayExtent(current);
        out->elementSize = element.elementSize > 0 ? element.elementSize : 1;
        out->typeName = element.display;
        out->byteSize = static_cast<int>(die->u(AT_BYTE_SIZE, 0));
        if (out->byteSize <= 0)
            out->byteSize = out->elementSize * out->elementCount;
        out->scalar = element.kind;

        // Only aggregates get element children: `Point[2]` is worth expanding,
        // `uint8_t[39406]` is not (the extent is already in the type label, and a
        // scalar array is drawn as one curve per element without extra nodes).
        if (element.kind == ScalarKind::Composite) {
            const int limit = qMin(out->elementCount, kMaxArrayChildren);
            if (out->elementCount > limit)
                out->incomplete = true;
            for (int index = 0; index < limit; ++index) {
                VariableNode child;
                if (!buildNode(QStringLiteral("[%1]").arg(index), elementType,
                               static_cast<quint32>(address + index * out->elementSize),
                               hasAddress, depth + 1, &child)) {
                    break;
                }
                out->children.append(child);
            }
        }
        return true;
    }
    case TAG_STRUCTURE_TYPE:
    case TAG_UNION_TYPE:
    case TAG_CLASS_TYPE: {
        out->kind = die->tag == TAG_UNION_TYPE
            ? NodeKind::Union
            : (die->tag == TAG_CLASS_TYPE ? NodeKind::Class : NodeKind::Struct);

        // A forward declaration has no layout: follow the name to the definition in
        // whichever unit carries it. ODrive's `controllers` is typed by exactly such
        // a declaration.
        const Die *definition = die;
        const bool declared =
            die->u(AT_DECLARATION, 0) != 0 || die->u(AT_BYTE_SIZE, 0) == 0;
        if (declared) {
            if (const Die *found = m_typeDefinitions.value(die->text(AT_NAME)); found)
                definition = found;
        }
        out->typeName = definition->text(AT_NAME);
        if (!alias.isEmpty())
            out->typeName = alias;
        if (out->typeName.isEmpty())
            out->typeName = nodeKindName(out->kind);
        out->byteSize = static_cast<int>(definition->u(AT_BYTE_SIZE, 0));
        out->elementSize = out->byteSize;
        out->scalar = ScalarKind::Composite;

        // No members available (declaration-only type, or a definition the compiler
        // never emitted). Say so instead of pretending the object is empty.
        if (out->byteSize <= 0 || !m_children.contains(definition->offset)) {
            out->incomplete = true;
            return true;
        }

        for (const Die &member : m_children.value(definition->offset)) {
            if (member.tag != TAG_MEMBER)
                continue;
            const QString memberName = member.text(AT_NAME);
            if (memberName.isEmpty())
                continue;
            // A bitfield member has no byte address of its own; keep it visible but
            // unreadable rather than silently dropping the field.
            const bool bitfield = member.has(AT_BIT_SIZE);
            const qint64 memberOffset = member.s(AT_DATA_MEMBER_LOCATION, 0);
            VariableNode child;
            if (!buildNode(memberName, member.u(AT_TYPE, 0),
                           static_cast<quint32>(address + memberOffset), hasAddress,
                           depth + 1, &child)) {
                continue;
            }
            if (bitfield)
                child.incomplete = true;
            out->children.append(child);
        }
        if (out->children.isEmpty())
            out->incomplete = true;
        return true;
    }
    case TAG_POINTER_TYPE: {
        out->kind = NodeKind::Pointer;
        out->scalar = ScalarKind::Pointer;
        out->byteSize = static_cast<int>(die->u(AT_BYTE_SIZE, 4));
        if (out->byteSize <= 0)
            out->byteSize = 4;
        out->elementSize = out->byteSize;
        out->typeName = alias.isEmpty() ? QStringLiteral("void *") : alias;
        return true;
    }
    case TAG_ENUMERATION_TYPE: {
        out->kind = NodeKind::Enum;
        out->scalar = ScalarKind::Enum;
        out->byteSize = static_cast<int>(die->u(AT_BYTE_SIZE, 4));
        if (out->byteSize <= 0)
            out->byteSize = 4;
        out->elementSize = out->byteSize;
        out->typeName = alias.isEmpty() ? die->text(AT_NAME) : alias;
        if (out->typeName.isEmpty())
            out->typeName = QStringLiteral("enum");
        return true;
    }
    case TAG_BASE_TYPE: {
        const int encoding = static_cast<int>(die->u(AT_ENCODING, 0));
        out->kind = NodeKind::Scalar;
        out->byteSize = static_cast<int>(die->u(AT_BYTE_SIZE, 0));
        out->elementSize = out->byteSize;
        out->typeName = alias.isEmpty() ? die->text(AT_NAME) : alias;
        switch (encoding) {
        case ATE_FLOAT:
        case ATE_COMPLEX_FLOAT:
            out->scalar = ScalarKind::Float;
            break;
        case ATE_BOOLEAN:
            out->scalar = ScalarKind::Boolean;
            break;
        case ATE_SIGNED:
        case ATE_SIGNED_CHAR:
            out->scalar = ScalarKind::Signed;
            break;
        case ATE_ADDRESS:
            out->scalar = ScalarKind::Pointer;
            break;
        default:
            out->scalar = ScalarKind::Unsigned;
            break;
        }
        if (out->byteSize <= 0)
            return false;
        return true;
    }
    default:
        // An unsupported type DIE (subroutine, set, ...): keep the node so the tree
        // still shows the member, but mark it unreadable.
        out->kind = NodeKind::Scalar;
        out->scalar = ScalarKind::Unknown;
        out->typeName = alias.isEmpty() ? QStringLiteral("?") : alias;
        out->incomplete = true;
        return true;
    }
}

void Parser::emitVariable(const Die &variable, const UnitContext &unit,
                          QVector<VariableNode> *out)
{
    const QString name = variable.text(AT_NAME);
    if (name.isEmpty())
        return;
    if (variable.u(AT_DECLARATION, 0) != 0)
        return; // extern declaration, no storage here

    const quint64 typeOffset = variable.u(AT_TYPE, 0);
    const TypeInfo info = resolveType(typeOffset, 0);
    if (info.kind == ScalarKind::Unknown && info.elementSize <= 0) {
        ++m_variablesNoType;
        return;
    }

    // A fixed address is only present for statically allocated variables; locals
    // carry a frame-relative expression or a register location instead.
    quint32 address = 0;
    bool hasAddress = false;
    if (variable.has(AT_LOCATION)) {
        const FormValue loc = variable.value(AT_LOCATION);
        const QByteArray expr = loc.bytes;
        if (expr.size() >= 1 + unit.addressSize
            && static_cast<quint8>(expr.at(0)) == OP_ADDR) {
            const uchar *b = reinterpret_cast<const uchar *>(expr.constData() + 1);
            quint32 value = 0;
            if (m_le) {
                for (int i = unit.addressSize - 1; i >= 0; --i)
                    value = (value << 8) | b[i];
            } else {
                for (int i = 0; i < unit.addressSize; ++i)
                    value = (value << 8) | b[i];
            }
            address = value;
            hasAddress = (value != 0);
        } else if (!loc.isBlock && loc.u != 0) {
            // DW_FORM_sec_offset: a location list in .debug_loc. On the ODrive
            // firmware most statics land here rather than in an inline DW_OP_addr.
            const quint32 listed = addressFromLocationList(loc.u, unit);
            if (listed != 0) {
                address = listed;
                hasAddress = true;
            }
        }
        // DW_OP_addrx would need .debug_addr; when present the address is left
        // empty and SymbolFile recovers it from the linker symbol.
    }

    // Build the whole subtree. An aggregate needs a base address for its members to
    // mean anything, but it is still worth listing when the address is unknown --
    // the user can see the variable exists and the tree stays stable between builds.
    if (info.kind == ScalarKind::Composite && !hasAddress) {
        ++m_compositesSkipped;
    }

    m_nodeBudget = kNodeBudgetPerRoot;
    VariableNode node;
    if (!buildNode(qualifiedName(variable.offset, name), typeOffset, address, hasAddress, 0,
                   &node)) {
        ++m_variablesNoType;
        return;
    }
    // Which source file this belongs to: DW_AT_decl_file is a 1-based index into the
    // unit's line-table file list.
    const quint64 declFile = variable.u(AT_DECL_FILE, 0);
    if (declFile > 0 && declFile <= static_cast<quint64>(unit.fileNames.size()))
        node.sourceFile = unit.fileNames.at(static_cast<int>(declFile) - 1);
    node.linkageName = variable.text(AT_LINKAGE_NAME);
    if (node.linkageName.isEmpty())
        node.linkageName = variable.text(AT_MIPS_LINKAGE_NAME);
    if (!hasAddress)
        ++m_variablesNoAddress;
    out->append(node);
}

namespace {

/// Decode the simple location expressions used for locals.
///
/// `DW_OP_fbreg N` is "frame base + N" and `DW_OP_regN` is "the value lives in
/// register N". Anything more elaborate (location lists, DW_OP_piece, ...) is
/// reported as unusable rather than guessed at -- showing a wrong number would be
/// worse than showing nothing.
struct LocalLocation
{
    bool usable = false;
    bool inRegister = false;
    int registerNumber = -1;
    qint32 frameOffset = 0;
};

LocalLocation decodeLocalLocation(const QByteArray &expr, bool littleEndian)
{
    LocalLocation out;
    if (expr.isEmpty())
        return out;
    const uchar *b = reinterpret_cast<const uchar *>(expr.constData());
    const quint8 op = b[0];
    if (op == OP_FBREG && expr.size() >= 2) {
        // Signed LEB128.
        qint64 value = 0;
        int shift = 0;
        quint8 byte = 0;
        for (int i = 1; i < expr.size(); ++i) {
            byte = static_cast<quint8>(expr.at(i));
            if (shift < 64)
                value |= static_cast<qint64>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0)
                break;
        }
        if (shift < 64 && (byte & 0x40))
            value |= -(Q_INT64_C(1) << shift);
        out.usable = true;
        out.frameOffset = static_cast<qint32>(value);
        return out;
    }
    if (op >= OP_REG0 && op <= OP_REG0 + 31 && expr.size() == 1) {
        out.usable = true;
        out.inRegister = true;
        out.registerNumber = op - OP_REG0;
        return out;
    }
    if (op >= OP_BREG0 && op <= OP_BREG0 + 31) {
        // Register plus a signed offset: usable when the register is the frame base.
        qint64 value = 0;
        int shift = 0;
        quint8 byte = 0;
        for (int i = 1; i < expr.size(); ++i) {
            byte = static_cast<quint8>(expr.at(i));
            if (shift < 64)
                value |= static_cast<qint64>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0)
                break;
        }
        if (shift < 64 && (byte & 0x40))
            value |= -(Q_INT64_C(1) << shift);
        out.usable = true;
        out.inRegister = true;
        out.registerNumber = op - OP_BREG0;
        out.frameOffset = static_cast<qint32>(value);
        return out;
    }
    Q_UNUSED(littleEndian);
    return out;
}

} // namespace

void Parser::emitLocal(const Die &variable, qint32 frameOffset, int registerNumber,
                       const UnitContext &unit, FunctionScope *scope)
{
    Q_UNUSED(unit);
    const QString name = variable.text(AT_NAME);
    if (name.isEmpty() || !scope)
        return;
    const quint64 typeOffset = variable.u(AT_TYPE, 0);
    if (typeOffset == 0)
        return;

    m_nodeBudget = 256; // a local struct does not need the global budget
    VariableNode node;
    if (!buildNode(name, typeOffset, 0, false, 0, &node))
        return;
    node.stackRelative = true;
    node.stackOffset = frameOffset;
    if (registerNumber >= 0)
        node.address = static_cast<quint32>(registerNumber); // reused as reg index
    scope->locals.append(node);
}

qint64 Parser::readSleb(const char **cursor, const char *end)
{
    qint64 result = 0;
    int shift = 0;
    quint8 byte = 0;
    const char *p = *cursor;
    while (p < end) {
        byte = static_cast<quint8>(*p++);
        if (shift < 64)
            result |= static_cast<qint64>(byte & 0x7f) << shift;
        shift += 7;
        if ((byte & 0x80) == 0)
            break;
    }
    if (shift < 64 && (byte & 0x40))
        result |= -(Q_INT64_C(1) << shift);
    *cursor = p;
    return result;
}

QStringList Parser::fileTableAt(quint64 offset)
{
    if (m_debugLine.isEmpty() || offset >= static_cast<quint64>(m_debugLine.size()))
        return {};
    const auto cached = m_fileTables.constFind(offset);
    if (cached != m_fileTables.constEnd())
        return cached.value();

    QStringList names;
    Cursor c{m_debugLine.constData(), static_cast<int>(m_debugLine.size()),
             static_cast<int>(offset), m_le};
    const quint64 unitLength = c.readU(4);
    if (!c.ok() || unitLength == 0 || unitLength == 0xffffffffu)
        return {};
    const int version = static_cast<int>(c.readU(2));
    const quint64 headerLength = c.readU(4);
    if (!c.ok())
        return {};
    // Version 5 rewrote the prologue (directory/file entry formats). Bail out rather
    // than mis-parse: the caller then shows no grouping.
    if (version < 1 || version > 4) {
        m_fileTables.insert(offset, names);
        return names;
    }

    c.readU(1);                       // minimum_instruction_length
    if (version >= 4)
        c.readU(1);                   // maximum_operations_per_instruction
    c.readU(1);                       // default_is_stmt
    c.readU(1);                       // line_base (signed, unused here)
    c.readU(1);                       // line_range
    const quint64 opcodeBase = c.readU(1);
    if (!c.ok() || opcodeBase == 0 || opcodeBase > 32)
        return {};
    for (quint64 i = 1; i < opcodeBase; ++i)
        c.readU(1);                   // standard_opcode_lengths

    // include_directories: NUL-terminated strings, terminated by an empty one.
    QStringList directories;
    while (c.ok() && c.remaining() > 0) {
        const QString dir = c.readCString();
        if (dir.isEmpty())
            break;
        directories << dir;
    }

    // file_names: { name, dir_index (uleb), mtime (uleb), size (uleb) } until an
    // empty name. The index is 1-based, so index N maps to names[N-1].
    while (c.ok() && c.remaining() > 0) {
        const QString name = c.readCString();
        if (name.isEmpty())
            break;
        const quint64 dirIndex = c.readUleb();
        c.readUleb();                 // mtime
        c.readUleb();                 // length
        // Keep just the base name: the directory is usually a long absolute build
        // path, and the variable panel is narrow.
        QString display = name;
        const int slash = qMax(display.lastIndexOf(QLatin1Char('/')),
                               display.lastIndexOf(QChar(92)));
        if (slash >= 0)
            display = display.mid(slash + 1);
        Q_UNUSED(dirIndex);
        Q_UNUSED(directories);
        names << display;
    }

    Q_UNUSED(headerLength);
    m_fileTables.insert(offset, names);
    return names;
}

bool Parser::findFde(quint32 pc, quint64 *fdeEntry, quint64 *cieEntry)
{
    if (m_debugFrame.isEmpty() || !fdeEntry || !cieEntry)
        return false;
    const char *base = m_debugFrame.constData();
    const char *end = base + m_debugFrame.size();
    const char *p = base;

    auto readU = [](const char **cursor, int bytes, bool le) -> quint64 {
        const uchar *b = reinterpret_cast<const uchar *>(*cursor);
        quint64 value = 0;
        if (le) {
            for (int i = bytes - 1; i >= 0; --i)
                value = (value << 8) | b[i];
        } else {
            for (int i = 0; i < bytes; ++i)
                value = (value << 8) | b[i];
        }
        *cursor += bytes;
        return value;
    };

    while (p + 4 <= end) {
        const char *entryStart = p;
        quint64 length = readU(&p, 4, m_le);
        if (length == 0xffffffffu)
            length = readU(&p, 8, m_le);
        const char *entryEnd = entryStart + 4 + length;
        if (length == 0 || entryEnd > end) {
            if (length == 0)
                continue;
            break;
        }
        const char *q = p;
        const quint64 ciePointer = readU(&q, 4, m_le);
        if (ciePointer == 0xffffffffu) {
            p = entryEnd;
            continue; // a CIE
        }
        const quint64 initialLocation = readU(&q, 4, m_le);
        const quint64 addressRange = readU(&q, 4, m_le);
        if (pc >= initialLocation && pc < initialLocation + addressRange) {
            *fdeEntry = static_cast<quint64>(entryStart - base);
            *cieEntry = ciePointer;
            return true;
        }
        p = entryEnd;
    }
    return false;
}

Parser::Cfa Parser::cfaForPc(quint32 pc)
{
    Cfa result;
    quint64 fdeEntry = 0;
    quint64 cieEntry = 0;
    if (!findFde(pc, &fdeEntry, &cieEntry))
        return result;

    const char *base = m_debugFrame.constData();
    const char *end = base + m_debugFrame.size();
    auto readU = [](const char **cursor, int bytes, bool le) -> quint64 {
        const uchar *b = reinterpret_cast<const uchar *>(*cursor);
        quint64 value = 0;
        if (le) {
            for (int i = bytes - 1; i >= 0; --i)
                value = (value << 8) | b[i];
        } else {
            for (int i = 0; i < bytes; ++i)
                value = (value << 8) | b[i];
        }
        *cursor += bytes;
        return value;
    };

    // CIE baseline first, then this FDE's own instructions.
    const char *cieStart = base + cieEntry;
    if (cieStart < base || cieStart >= end)
        return result;
    const char *cq = cieStart;
    const quint64 cieLength = readU(&cq, 4, m_le);
    const char *cieEnd = cieStart + 4 + cieLength;
    if (cieEnd > end)
        cieEnd = end;
    readU(&cq, 4, m_le);            // CIE id
    ++cq;                           // version
    while (cq < cieEnd && *cq != '\0')
        ++cq;                       // augmentation
    ++cq;
    if (cq < cieEnd) {
        ++cq;                        // code alignment
        readSleb(&cq, cieEnd);       // data alignment
        ++cq;                        // return-address column
    }

    const char *fdeStart = base + fdeEntry;
    const char *fq = fdeStart;
    const quint64 fdeLength = readU(&fq, 4, m_le);
    const char *fdeEnd = fdeStart + 4 + fdeLength;
    if (fdeEnd > end)
        fdeEnd = end;
    readU(&fq, 4, m_le);            // cie pointer
    const quint64 initialLocation = readU(&fq, 4, m_le);
    readU(&fq, 4, m_le);            // address range

    auto runProgram = [&](const char *from, const char *to, quint64 *loc) {
        const char *r = from;
        while (r < to) {
            const quint8 op = static_cast<quint8>(*r++);
            const quint8 high = op & 0xc0;
            if (op == CFA_NOP)
                continue;
            if (high == CFA_ADVANCE_LOC) {
                *loc += (op & 0x3f);
                if (*loc > pc)
                    return; // the rest applies to code past our PC
                continue;
            }
            switch (op) {
            case CFA_DEF_CFA: {
                const quint64 reg = static_cast<quint8>(*r++);
                const qint64 offset = readSleb(&r, to);
                result.reg = static_cast<int>(reg);
                result.offset = static_cast<qint32>(offset);
                break;
            }
            case CFA_DEF_CFA_REGISTER: {
                const quint64 reg = static_cast<quint8>(*r++);
                result.reg = static_cast<int>(reg);
                break;
            }
            case CFA_DEF_CFA_OFFSET: {
                const qint64 offset = readSleb(&r, to);
                result.offset = static_cast<qint32>(offset);
                break;
            }
            default:
                // Skip the ULEB operand of instructions we do not interpret (they
                // concern saved registers, not the CFA).
                while (r < to && (static_cast<quint8>(*r) & 0x80))
                    ++r;
                if (r < to)
                    ++r;
                break;
            }
        }
    };

    quint64 loc = initialLocation;
    result = Cfa();
    runProgram(cq, cieEnd, &loc);
    loc = initialLocation;
    runProgram(fq, fdeEnd, &loc);
    return result;
}

void Parser::collectFunction(const Die &subprogram, const UnitContext &unit,
                            QVector<FunctionScope> *out)
{
    const quint64 lowPc = subprogram.u(AT_LOW_PC, 0);
    if (lowPc == 0)
        return; // declaration, or an inlined-only copy with no code
    quint64 highPc = subprogram.u(AT_HIGH_PC, 0);
    if (highPc != 0 && highPc <= lowPc)
        highPc = lowPc + highPc; // DW_AT_high_pc as a length instead of an address
    if (highPc == 0)
        highPc = lowPc + 1; // no range info: still usable for an exact PC match

    // Frame base: with DW_OP_regN the locals are at "regN + offset", which needs no
    // stack unwinding -- the common case for ARM at -O0 (r13/SP).
    int frameRegister = -1;
    qint32 frameOffset = 0;
    quint32 cfaFde = 0;
    if (subprogram.has(AT_FRAME_BASE)) {
        const QByteArray expr = subprogram.value(AT_FRAME_BASE).bytes;
        if (expr.size() >= 1) {
            const quint8 op = static_cast<quint8>(expr.at(0));
            if (op >= OP_REG0 && op <= OP_REG0 + 31) {
                // armclang spells it out: the frame base *is* register N (usually
                // r13 = SP), so no stack unwinding is needed.
                frameRegister = op - OP_REG0;
            } else if (op == OP_CALL_FRAME_CFA) {
                // GCC: "the CFA of this PC". The rule changes inside the function
                // (each prologue push does `DW_CFA_def_cfa_offset`), so the offset
                // must be evaluated at the halt PC, not at the entry. Record which
                // FDE applies and let the runtime evaluate it.
                quint64 fdeEntry = 0;
                quint64 cieEntry = 0;
                if (findFde(static_cast<quint32>(lowPc), &fdeEntry, &cieEntry))
                    cfaFde = static_cast<quint32>(fdeEntry);
            }
        }
    }

    FunctionScope scope;
    scope.name = qualifiedName(subprogram.offset, subprogram.text(AT_NAME));
    if (scope.name.isEmpty())
        scope.name = QStringLiteral("(匿名函数)");
    scope.lowPc = static_cast<quint32>(lowPc);
    scope.highPc = static_cast<quint32>(highPc);
    scope.frameBaseRegister = frameRegister;
    scope.frameBaseOffset = frameOffset;
    scope.cfaFde = cfaFde;

    for (const Die &child : m_children.value(subprogram.offset)) {
        // Variables can also sit inside a lexical block ({ ... }).
        // Parameters are DW_TAG_formal_parameter, not DW_TAG_variable -- a function
        // whose arguments sit on the stack (the -O0 case) would otherwise show only
        // its temporaries.
        if (child.tag == TAG_VARIABLE || child.tag == TAG_FORMAL_PARAMETER) {
            const QByteArray expr = child.value(AT_LOCATION).bytes;
            const LocalLocation location = decodeLocalLocation(expr, m_le);
            if (location.usable
                && (location.inRegister || frameRegister >= 0 || cfaFde != 0)) {
                emitLocal(child, location.frameOffset,
                          location.inRegister ? location.registerNumber : -1, unit, &scope);
            }
        } else if (child.tag == TAG_LEXICAL_BLOCK) {
            for (const Die &inner : m_children.value(child.offset)) {
                if (inner.tag != TAG_VARIABLE && inner.tag != TAG_FORMAL_PARAMETER)
                    continue;
                const LocalLocation location =
                    decodeLocalLocation(inner.value(AT_LOCATION).bytes, m_le);
                if (location.usable
                && (location.inRegister || frameRegister >= 0 || cfaFde != 0)) {
                    emitLocal(inner, location.frameOffset,
                              location.inRegister ? location.registerNumber : -1, unit,
                              &scope);
                }
            }
        }
    }

    if (!scope.locals.isEmpty())
        out->append(scope);
}

Result<DwarfContents> Parser::run()
{
    if (m_info.isEmpty())
        return Result<DwarfContents>::failure(
            QStringLiteral("没有 .debug_info 节（编译时未开调试信息）"));

    Cursor info{m_info.constData(), static_cast<int>(m_info.size()), 0, m_le};
    QVector<VariableNode> variables;
    // Every unit is kept until the end: type resolution needs all of them.
    QVector<QPair<UnitContext, QVector<Die>>> units;

    while (info.ok() && info.pos + 4 <= info.size) {
        UnitContext unit;
        QVector<Die> dies;
        quint64 next = 0;
        ++m_unitsSeen;
        if (!readUnit(info, &unit, &dies, &next)) {
            ++m_unitsFailed;
            if (m_failures.size() < 8) {
                m_failures << QStringLiteral("offset 0x%1, 读到 0x%2, %3 个 DIE")
                                  .arg(info.pos > 0 ? info.pos : 0, 0, 16)
                                  .arg(next, 0, 16)
                                  .arg(dies.size());
            }
            if (!info.ok())
                break;
            // A corrupt unit: skip to where its length said it ends.
            if (next > static_cast<quint64>(info.pos))
                info.pos = static_cast<int>(next);
            continue;
        }
        units.append(qMakePair(unit, dies));
        // readUnit already advanced the cursor to this unit's end, so comparing
        // `next` against info.pos stopped after the FIRST compilation unit -- on a
        // 107-CU C++ image that silently discarded 106 of them, and the caller saw
        // "the file has 3 variables".
        if (next == 0 || next > static_cast<quint64>(m_info.size()))
            break;
        info.pos = static_cast<int>(next);
    }

    // Pass 1b: index the type *definitions* by name.
    //
    // C++ references types through forward declarations as often as through
    // definitions -- `controllers` is typed `Controller[1]` where the Controller DIE
    // in that unit is a bare declaration. Resolving a variable must therefore be
    // able to look the layout up across the whole file, which is only possible once
    // every unit has been read.
    for (const auto &unit : units) {
        for (const Die &die : unit.second) {
            if (die.tag != TAG_STRUCTURE_TYPE && die.tag != TAG_UNION_TYPE
                && die.tag != TAG_CLASS_TYPE) {
                continue;
            }
            if (die.u(AT_DECLARATION, 0) != 0 || die.u(AT_BYTE_SIZE, 0) == 0)
                continue; // a declaration, not a layout
            const QString name = die.text(AT_NAME);
            if (name.isEmpty())
                continue;
            // Keep the first real definition: later duplicates describe the same
            // type and would only make the result order-dependent.
            if (!m_typeDefinitions.contains(name))
                m_typeDefinitions.insert(name, &die);
        }
    }

    // Pass 2: emit variables now that every type is reachable.
    QVector<FunctionScope> functions;
    for (const auto &unit : units) {
        for (const Die &die : unit.second) {
            if (die.tag == TAG_VARIABLE)
                emitVariable(die, unit.first, &variables);
            else if (die.tag == TAG_SUBPROGRAM)
                collectFunction(die, unit.first, &functions);
        }
    }

    if (!info.ok() && variables.isEmpty())
        return Result<DwarfContents>::failure(
            QStringLiteral("DWARF 解析失败（数据被截断或格式不支持）"));

    Logger::instance()->debug(
        QStringLiteral("DWARF: 版本 %1，%2 个 CU（失败 %3），%4 个 DIE，"
                       "%5 个变量 DIE，%6 个可采样（无地址 %7，无类型 %8，结构体缺地址 %9）")
            .arg(m_dwarfVersion)
            .arg(m_unitsSeen)
            .arg(m_unitsFailed)
            .arg(m_dies.size())
            .arg(m_variablesSeen)
            .arg(variables.size())
            .arg(m_variablesNoAddress)
            .arg(m_variablesNoType)
            .arg(m_compositesSkipped));
    {
        int nodes = 0;
        for (const VariableNode &root : variables)
            nodes += root.totalNodeCount();
        Logger::instance()->debug(
            QStringLiteral("变量树: %1 个根，%2 个节点").arg(variables.size()).arg(nodes));
        int localCount = 0;
        for (const FunctionScope &scope : functions)
            localCount += scope.localCount();
        Logger::instance()->debug(
            QStringLiteral("局部变量: %1 个函数含 locals，共 %2 个叶子")
                .arg(functions.size())
                .arg(localCount));
    }
    for (const QString &failure : m_failures)
        Logger::instance()->debug(QStringLiteral("DWARF CU 失败: %1").arg(failure));
    DwarfContents contents;
    contents.globals = variables;
    contents.functions = functions;
    return Result<DwarfContents>::success(contents);
}

} // namespace

bool DwarfReader::cfaAt(const ElfFile &elf, quint32 pc, int *reg, qint32 *offset)
{
    const QByteArray frame = elf.sectionData(QStringLiteral(".debug_frame"));
    if (frame.isEmpty() || !reg || !offset)
        return false;
    // Reuse the parser's evaluator: it needs .debug_info only for its own state, so
    // a frame-only instance is enough here.
    const QByteArray info = elf.sectionData(QStringLiteral(".debug_info"));
    const QByteArray abbrev = elf.sectionData(QStringLiteral(".debug_abbrev"));
    Parser parser(elf, info, abbrev, QByteArray(), QByteArray(), QByteArray(), frame,
                  QByteArray());
    const auto cfa = parser.cfaForPc(pc);
    if (!cfa.valid())
        return false;
    *reg = cfa.reg;
    *offset = cfa.offset;
    return true;
}

int FunctionScope::localCount() const
{
    int total = 0;
    for (const VariableNode &local : locals)
        total += local.leafCount();
    return total;
}

int DwarfReader::dwarfVersion(const ElfFile &elf)
{
    const QByteArray info = elf.sectionData(QStringLiteral(".debug_info"));
    if (info.size() < 6)
        return 0;
    ElfFile probe = elf; // reuse the byte-order helpers
    const char *p = info.constData();
    // version sits right after the 4-byte unit_length
    return static_cast<int>(probe.readU16(p + 4));
}

Result<QVector<VariableNode>> DwarfReader::readVariables(const ElfFile &elf)
{
    const auto contents = read(elf);
    if (!contents)
        return Result<QVector<VariableNode>>::failure(contents.error);
    return Result<QVector<VariableNode>>::success(contents.value.globals);
}

Result<DwarfContents> DwarfReader::read(const ElfFile &elf)
{
    const QByteArray info = elf.sectionData(QStringLiteral(".debug_info"));
    const QByteArray abbrev = elf.sectionData(QStringLiteral(".debug_abbrev"));
    const QByteArray str = elf.sectionData(QStringLiteral(".debug_str"));
    const QByteArray strOffsets = elf.sectionData(QStringLiteral(".debug_str_offsets"));

    if (info.isEmpty())
        return Result<DwarfContents>::failure(
            QStringLiteral("ELF 中没有 .debug_info（编译时未加 -g / 未开调试信息）"));
    if (abbrev.isEmpty())
        return Result<DwarfContents>::failure(
            QStringLiteral("ELF 中没有 .debug_abbrev，无法解析 DWARF"));

    const QByteArray debugLoc = elf.sectionData(QStringLiteral(".debug_loc"));
    const QByteArray debugFrame = elf.sectionData(QStringLiteral(".debug_frame"));
    const QByteArray debugLine = elf.sectionData(QStringLiteral(".debug_line"));
    Parser parser(elf, info, abbrev, str, strOffsets, debugLoc, debugFrame, debugLine);
    return parser.run();
}

} // namespace mrd
