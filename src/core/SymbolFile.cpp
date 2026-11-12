#include "core/SymbolFile.h"

#include "core/HexUtil.h"
#include "core/Logger.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mrd {

QString WatchVariable::addressLabel() const
{
    if (elementCount <= 1)
        return hexAddr(address);
    return QStringLiteral("%1..%2")
        .arg(hexAddr(address), hexAddr(address + static_cast<quint32>(span() - elementSize)));
}

const WatchVariable *SymbolTable::find(const QString &name) const
{
    for (const WatchVariable &v : variables) {
        if (v.name == name)
            return &v;
    }
    // Case-insensitive second pass: Keil and GCC agree on case, but a user typing
    // a name into a filter should not have to match it exactly.
    for (const WatchVariable &v : variables) {
        if (v.name.compare(name, Qt::CaseInsensitive) == 0)
            return &v;
    }
    return nullptr;
}

QVector<WatchVariable> SymbolTable::filtered(const QString &needle) const
{
    if (needle.trimmed().isEmpty())
        return variables;
    QVector<WatchVariable> out;
    for (const WatchVariable &v : variables) {
        if (v.name.contains(needle, Qt::CaseInsensitive)
            || v.typeLabel.contains(needle, Qt::CaseInsensitive)
            || hexAddr(v.address).contains(needle, Qt::CaseInsensitive)) {
            out.append(v);
        }
    }
    return out;
}

void SymbolTable::collectLeaves(const VariableNode &node, const QString &prefix,
                                QVector<WatchVariable> *out)
{
    if (!out)
        return;
    // Dotted path from the root, so a leaf is addressable by name even though the
    // tree itself only stores local names. Array elements use the C spelling
    // (`g_points[1].x`), which is what the user sees in the tree.
    // Array elements are spelled `g_points[0]`, not `g_points.[0]`, so the path
    // matches what the user would write in C.
    QString path;
    if (prefix.isEmpty())
        path = node.name;
    else if (node.name.startsWith(QLatin1Char('[')))
        path = prefix + node.name;
    else
        path = prefix + QLatin1Char('.') + node.name;

    if (node.children.isEmpty()) {
        if (!node.samplable())
            return;
        WatchVariable watch;
        watch.name = path;
        watch.typeLabel = node.typeLabel();
        watch.kind = node.scalar;
        watch.address = node.address;
        watch.elementSize = node.elementSize;
        // A scalar array is one entity with an extent: the plot fans it out into one
        // curve per element, so it must not become N separate leaves.
        watch.elementCount = node.kind == NodeKind::Array ? qMax(1, node.elementCount) : 1;
        watch.totalSize = node.byteSize > 0 ? node.byteSize
                                            : watch.elementSize * watch.elementCount;
        watch.addressFromSymbol = node.addressFromSymbol;
        out->append(watch);
        return;
    }

    for (const VariableNode &child : node.children)
        collectLeaves(child, path, out);
}

namespace {

/// Does any node in this subtree match the needle?
bool subtreeMatches(const VariableNode &node, const QString &needle)
{
    if (node.name.contains(needle, Qt::CaseInsensitive)
        || node.typeLabel().contains(needle, Qt::CaseInsensitive)
        || node.typeName.contains(needle, Qt::CaseInsensitive)) {
        return true;
    }
    for (const VariableNode &child : node.children) {
        if (subtreeMatches(child, needle))
            return true;
    }
    return false;
}

/// Keep only the branches leading to a match. A match on a parent keeps its whole
/// subtree (otherwise filtering for `config` would show the field but hide its
/// contents, which defeats the purpose).
VariableNode pruneTree(const VariableNode &node, const QString &needle)
{
    if (!subtreeMatches(node, needle))
        return VariableNode(); // caller drops empty names
    VariableNode copy = node;
    copy.children.clear();
    for (const VariableNode &child : node.children) {
        VariableNode kept = pruneTree(child, needle);
        if (!kept.name.isEmpty())
            copy.children.append(kept);
    }
    copy.incomplete = copy.incomplete || (!node.children.isEmpty() && copy.children.isEmpty());
    return copy;
}

} // namespace

QVector<VariableNode> SymbolTable::filteredRoots(const QString &needle) const
{
    if (needle.trimmed().isEmpty())
        return roots;
    QVector<VariableNode> out;
    for (const VariableNode &root : roots) {
        VariableNode kept = pruneTree(root, needle);
        if (!kept.name.isEmpty())
            out.append(kept);
    }
    return out;
}

QVector<TypeDecl> SymbolTable::typesInFile(const QString &file) const
{
    QVector<TypeDecl> out;
    for (const TypeDecl &type : types) {
        const QString key = type.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                      : type.sourceFile;
        if (key == file)
            out.append(type);
    }
    std::sort(out.begin(), out.end(), [](const TypeDecl &a, const TypeDecl &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QVector<FileGroup> SymbolTable::groupedByFile() const
{
    QMap<QString, FileGroup> byFile;
    for (const VariableNode &root : roots) {
        const QString key = root.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                      : root.sourceFile;
        FileGroup &group = byFile[key];
        group.file = key;
        group.variables.append(root);
        group.nodeCount += root.totalNodeCount();
    }

    // Keep "(未知文件)" last so a real grouping is never buried, and sort the rest.
    QVector<FileGroup> out;
    out.reserve(byFile.size());
    FileGroup unknown;
    for (auto it = byFile.constBegin(); it != byFile.constEnd(); ++it) {
        if (it.key() == QStringLiteral("(未知文件)"))
            unknown = it.value();
        else
            out.append(it.value());
    }
    std::sort(out.begin(), out.end(), [](const FileGroup &a, const FileGroup &b) {
        return a.file.compare(b.file, Qt::CaseInsensitive) < 0;
    });
    if (unknown.nodeCount > 0)
        out.append(unknown);
    return out;
}

bool SymbolTable::frameBaseAt(quint32 pc, int *reg, qint32 *offset) const
{
    const FunctionScope *scope = functionContaining(pc);
    if (!scope || !reg || !offset)
        return false;
    if (scope->frameBaseRegister >= 0) {
        // armclang style: the frame base is the register itself (usually r13).
        *reg = scope->frameBaseRegister;
        *offset = scope->frameBaseOffset;
        return true;
    }
    // GCC style: DW_OP_call_frame_cfa, so evaluate the rule for *this* PC.
    return DwarfReader::cfaAt(source, pc, reg, offset);
}

const FunctionScope *SymbolTable::functionContaining(quint32 pc) const
{
    if (pc == 0)
        return nullptr;
    for (const FunctionScope &scope : functions) {
        if (scope.contains(pc))
            return &scope;
    }
    // No range match: fall back to the nearest function that starts below the PC,
    // which is what happens when a function's range was left out of the DWARF.
    const FunctionScope *nearest = nullptr;
    for (const FunctionScope &scope : functions) {
        if (scope.lowPc <= pc && (!nearest || scope.lowPc > nearest->lowPc))
            nearest = &scope;
    }
    return nearest;
}

QStringList SymbolFile::fileFilters()
{
    return {QStringLiteral("Keil 符号文件 (*.axf *.elf *.out)"),
            QStringLiteral("ELF 文件 (*.axf *.elf *.out *.exe)"),
            QStringLiteral("所有文件 (*)")};
}

QVector<double> SymbolFile::decode(const WatchVariable &variable, const QByteArray &bytes,
                                   int count)
{
    QVector<double> out;
    const int width = variable.elementSize;
    if (width <= 0)
        return out;
    const int available = bytes.size() / width;
    const int wanted = qMin(count <= 0 ? 1 : count, available);
    out.reserve(wanted);
    for (int i = 0; i < wanted; ++i) {
        const char *p = bytes.constData() + i * width;
        const uchar *b = reinterpret_cast<const uchar *>(p);
        double value = 0.0;
        switch (variable.kind) {
        case ScalarKind::Float:
            if (width == 4) {
                quint32 raw = static_cast<quint32>(b[0]) | (static_cast<quint32>(b[1]) << 8)
                    | (static_cast<quint32>(b[2]) << 16) | (static_cast<quint32>(b[3]) << 24);
                float f = 0.0f;
                std::memcpy(&f, &raw, sizeof(f));
                value = static_cast<double>(f);
            } else if (width == 8) {
                quint64 raw = 0;
                for (int k = 7; k >= 0; --k)
                    raw = (raw << 8) | b[k];
                double d = 0.0;
                std::memcpy(&d, &raw, sizeof(d));
                value = d;
            }
            break;
        case ScalarKind::Signed: {
            qint64 raw = 0;
            for (int k = width - 1; k >= 0; --k)
                raw = (raw << 8) | b[k];
            // Sign-extend from the element width.
            const int bits = width * 8;
            if (bits < 64 && (raw & (Q_INT64_C(1) << (bits - 1))))
                raw |= ~((Q_INT64_C(1) << bits) - 1);
            value = static_cast<double>(raw);
            break;
        }
        case ScalarKind::Unsigned:
        case ScalarKind::Boolean:
        case ScalarKind::Pointer: {
            quint64 raw = 0;
            for (int k = width - 1; k >= 0; --k)
                raw = (raw << 8) | b[k];
            value = static_cast<double>(raw);
            break;
        }
        default:
            break;
        }
        out.append(value);
    }
    return out;
}

namespace {

/// Index .symtab by name so DWARF variables without a location can be resolved.
QHash<QString, const ElfSymbol *> indexSymbols(const QVector<ElfSymbol> &symbols)
{
    // Two passes so a later definition does not shadow the one we want: prefer
    // section-relative data objects over anything odd.
    QHash<QString, const ElfSymbol *> index;
    for (const ElfSymbol &sym : symbols) {
        if (!sym.isDataObject() || sym.value == 0)
            continue;
        const auto it = index.constFind(sym.name);
        if (it == index.constEnd() || it.value()->size == 0)
            index.insert(sym.name, &sym);
    }
    return index;
}

} // namespace

Result<SymbolTable> SymbolFile::load(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return Result<SymbolTable>::failure(QStringLiteral("文件不存在: %1").arg(path));

    const auto elfResult = ElfFile::load(path);
    if (!elfResult)
        return Result<SymbolTable>::failure(elfResult.error);
    const ElfFile &elf = elfResult.value;

    SymbolTable table;
    table.sourcePath = path;
    table.source = elfResult.value;
    table.dwarfVersion = DwarfReader::dwarfVersion(elf);

    const QHash<QString, const ElfSymbol *> symbols = indexSymbols(elf.symbols());

    // Primary source: DWARF knows names, types, sizes and the nesting.
    // One pass yields both views: parsing a 4 MB image twice costs ~1 s.
    auto dwarfResult = DwarfReader::read(elf);
    if (dwarfResult) {
        // Resolve each root's base address, then shift its whole subtree by the
        // same delta. Children were built as `parentBase + memberOffset`, so when
        // the parent base was unknown they hold plain offsets and adding the real
        // base makes them absolute -- which is exactly what the shift does.
        for (VariableNode &root : dwarfResult.value.globals) {
            quint32 oldBase = root.address;
            if (!root.hasAddress || root.address == 0) {
                // The mangled name is what .symtab uses. Matching the demangled
                // DWARF name fails for every C++ variable ("endpoint_id" vs
                // "_ZL11endpoint_id"), which once made a C++ image look like it had
                // almost no variables at all.
                const QString leafName = root.name.section(QStringLiteral("::"), -1);
                const ElfSymbol *sym = nullptr;
                if (!root.linkageName.isEmpty())
                    sym = symbols.value(root.linkageName, nullptr);
                if (!sym)
                    sym = symbols.value(root.name, nullptr);
                if (!sym)
                    sym = symbols.value(leafName, nullptr);
                if (!sym)
                    continue; // no address anywhere: a local or an extern
                root.address = sym->value;
                root.hasAddress = true;
                root.addressFromSymbol = true;
                table.usedSymbolFallback = true;
                if (root.byteSize <= 0 && sym->size > 0)
                    root.byteSize = static_cast<int>(sym->size);
                // A symbol's size covers the whole array; the element count follows
                // from the element width when DWARF did not state it.
                if (root.kind == NodeKind::Array && root.elementSize > 0
                    && root.elementCount <= 1 && sym->size > 0) {
                    const int deduced = static_cast<int>(sym->size) / root.elementSize;
                    if (deduced > 1)
                        root.elementCount = deduced;
                }
            }
            if (root.address < 0x1000)
                continue; // a mis-parsed location expression: not a real address

            // Propagate the resolved base down to every child.
            const qint64 delta = static_cast<qint64>(root.address)
                - static_cast<qint64>(oldBase);
            if (delta != 0) {
                struct Shift
                {
                    static void apply(VariableNode &node, qint64 by)
                    {
                        if (node.hasAddress || node.address != 0) {
                            node.address = static_cast<quint32>(
                                static_cast<qint64>(node.address) + by);
                            node.hasAddress = true;
                        }
                        for (VariableNode &child : node.children)
                            apply(child, by);
                    }
                };
                for (VariableNode &child : root.children)
                    Shift::apply(child, delta);
            }
            if (root.byteSize <= 0) {
                // A declaration-only aggregate still occupies the symbol's bytes.
                int span = root.elementSize * qMax(1, root.elementCount);
                if (span > 0)
                    root.byteSize = span;
            }
            table.roots.append(root);
        }

        // Flatten for sampling / frame layout / the firmware snippet.
        for (const VariableNode &root : table.roots)
            SymbolTable::collectLeaves(root, QString(), &table.variables);

        // Locals travel with their function: their address is frame-relative and
        // only meaningful while the core sits inside that function.
        table.functions = dwarfResult.value.functions;
        table.types = dwarfResult.value.types;
    }

    // DWARF absent or unusable: fall back to the symbol table alone. Types become
    // guesses (4-byte unsigned), which the UI lets the user override.
    if (table.variables.isEmpty()) {
        for (auto it = symbols.constBegin(); it != symbols.constEnd(); ++it) {
            if (it.value()->size == 0 || it.value()->size > 4096)
                continue;
            WatchVariable watch;
            watch.name = it.key();
            watch.elementSize = it.value()->size >= 8 ? 8 : (it.value()->size >= 4 ? 4 : 1);
            watch.elementCount = static_cast<int>(it.value()->size) / watch.elementSize;
            if (watch.elementCount <= 0)
                watch.elementCount = 1;
            watch.totalSize = static_cast<int>(it.value()->size);
            watch.address = it.value()->value;
            watch.kind = ScalarKind::Unsigned;
            watch.typeLabel = QStringLiteral("未知（按 %1 字节无符号）").arg(watch.elementSize);
            watch.addressFromSymbol = true;
            if (!watch.usable() || watch.address < 0x1000)
                continue;

            VariableNode node;
            node.name = watch.name;
            node.typeName = watch.typeLabel;
            node.scalar = watch.kind;
            node.kind = watch.elementCount > 1 ? NodeKind::Array : NodeKind::Scalar;
            node.address = watch.address;
            node.hasAddress = true;
            node.byteSize = watch.totalSize;
            node.elementSize = watch.elementSize;
            node.elementCount = watch.elementCount;
            node.addressFromSymbol = true;
            node.incomplete = true; // no type information at all
            table.roots.append(node);
            table.variables.append(watch);
        }
        table.usedSymbolFallback = true;
    }

    if (table.variables.isEmpty())
        return Result<SymbolTable>::failure(
            QStringLiteral("%1 里没有可用变量：需要带调试信息的 ELF（Keil 里勾选 Debug Information，"
                           "GCC 里加 -g），且至少有一个全局或静态变量")
                .arg(info.fileName()));

    std::sort(table.variables.begin(), table.variables.end(),
              [](const WatchVariable &a, const WatchVariable &b) {
                  if (a.address != b.address)
                      return a.address < b.address;
                  return a.name < b.name;
              });

    // Every compilation unit repeats the DIEs of the types it references, so the
    // same global can be described several times. Same name at the same address is
    // the same object; keep one. (Two variables that merely share an address keep
    // both rows -- the linker can merge globals, but then their names differ.)
    {
        QVector<WatchVariable> unique;
        unique.reserve(table.variables.size());
        QSet<QString> seen;
        for (const WatchVariable &variable : table.variables) {
            const QString key = variable.name + QLatin1Char('@')
                + QString::number(variable.address);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            unique.append(variable);
        }
        if (unique.size() != table.variables.size()) {
            Logger::instance()->debug(QStringLiteral("符号文件: 去重 %1 条（同一变量被多个 CU 重复描述）")
                                          .arg(table.variables.size() - unique.size()));
        }
        table.variables = unique;
    }

    // The tree needs the same treatment: a global described by three units would
    // otherwise appear three times in the variable list, which is what the UI shows.
    // Deduplicating the leaves alone is not enough once the tree is the primary view.
    {
        QVector<VariableNode> unique;
        unique.reserve(table.roots.size());
        QSet<QString> seen;
        for (const VariableNode &root : table.roots) {
            const QString key = root.name + QLatin1Char('@')
                + QString::number(root.address);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            unique.append(root);
        }
        if (unique.size() != table.roots.size()) {
            Logger::instance()->debug(
                QStringLiteral("符号文件: 树上去重 %1 个根（同一变量被多个 CU 重复描述）")
                    .arg(table.roots.size() - unique.size()));
        }
        table.roots = unique;
    }

    Logger::instance()->debug(QStringLiteral("符号文件: %1，DWARF v%2，%3 个变量%4")
                                  .arg(info.fileName())
                                  .arg(table.dwarfVersion)
                                  .arg(table.variables.size())
                                  .arg(table.usedSymbolFallback
                                           ? QStringLiteral("（部分地址取自符号表）")
                                           : QString()));
    return Result<SymbolTable>::success(table);
}

} // namespace mrd
