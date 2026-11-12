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
    table.dwarfVersion = DwarfReader::dwarfVersion(elf);

    const QHash<QString, const ElfSymbol *> symbols = indexSymbols(elf.symbols());

    // Primary source: DWARF knows names, types and sizes.
    const auto dwarfResult = DwarfReader::readVariables(elf);
    if (dwarfResult) {
        for (const DwarfVariable &dv : dwarfResult.value) {
            WatchVariable watch;
            watch.name = dv.name;
            watch.typeLabel = dv.typeLabel();
            watch.kind = dv.kind;
            watch.elementSize = dv.elementSize;
            watch.elementCount = dv.elementCount > 0 ? dv.elementCount : 1;
            watch.totalSize = dv.totalSize;
            watch.address = dv.address;
            watch.linkageName = dv.linkageName;

            // The DWARF location expression is authoritative when it gave an
            // address; otherwise consult the linker symbol of the same name.
            if (dv.hasAddress && dv.address != 0) {
                watch.address = dv.address;
            } else {
                // The mangled name is what .symtab uses. Matching on the demangled
                // DWARF name silently fails for every C++ variable ("endpoint_id"
                // vs "_ZL11endpoint_id"), which is why a C++ image appeared to have
                // almost no variables at all.
                const ElfSymbol *sym = nullptr;
                if (!dv.linkageName.isEmpty())
                    sym = symbols.value(dv.linkageName, nullptr);
                if (!sym)
                    sym = symbols.value(dv.name, nullptr);
                if (!sym) {
                    // Namespaced C++ statics: drop the scope and try the tail.
                    const int scopeEnd = dv.name.lastIndexOf(QStringLiteral("::"));
                    if (scopeEnd >= 0)
                        sym = symbols.value(dv.name.mid(scopeEnd + 2), nullptr);
                }
                if (sym) {
                    watch.address = sym->value;
                    watch.addressFromSymbol = true;
                    table.usedSymbolFallback = true;
                    if (watch.totalSize <= 0 && sym->size > 0)
                        watch.totalSize = static_cast<int>(sym->size);
                    // A symbol's size covers the whole array; the element count then
                    // follows from the element width.
                    if (watch.elementSize > 0 && watch.elementCount <= 1 && sym->size > 0) {
                        const int deduced = static_cast<int>(sym->size) / watch.elementSize;
                        if (deduced > 1)
                            watch.elementCount = deduced;
                    }
                }
            }

            if (!watch.usable())
                continue;
            // Reject addresses that cannot be RAM/Flash: a location expression the
            // reader mis-parsed would otherwise show up as a "variable" at 0x0.
            if (watch.address < 0x1000)
                continue;
            if (watch.totalSize <= 0)
                watch.totalSize = watch.elementSize * watch.elementCount;
            table.variables.append(watch);
        }
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
