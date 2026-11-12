#include "app/Cli.h"

#include "core/DwarfReader.h"
#include "core/ElfFile.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/SymbolFile.h"

#include <QHash>
#include <QStringList>

#include <cstdio>

namespace mrd::cli {

namespace {

/// Same shape as the other CLI entry points: plain stdout, "[FAIL]" on error.
void out(const QString &line)
{
    std::printf("%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

} // namespace

int runSymbols(const Options &options)
{
    // Reading a symbol file needs no hardware, so this doubles as the fastest way
    // to check a build before a board is plugged in.
    Logger::instance()->setMinimumLevel(options.verbose ? Logger::Debug : Logger::Warn);

    out(QStringLiteral("=== 符号文件: %1 ===").arg(options.symbolPath));

    const auto loaded = SymbolFile::load(options.symbolPath);
    if (!loaded) {
        out(QStringLiteral("[FAIL] %1").arg(loaded.error));
        return 1;
    }

    const SymbolTable &table = loaded.value;
    out(QStringLiteral("    DWARF   : v%1").arg(table.dwarfVersion));
    out(QStringLiteral("    变量数  : %1").arg(table.variables.size()));
    if (table.usedSymbolFallback)
        out(QStringLiteral("    备注    : 部分地址取自 .symtab（DWARF 未直接给出）"));

    // Type histogram: the quickest way to notice "everything fell back to Unknown",
    // which is what a broken type resolution looks like from the outside.
    QHash<QString, int> perKind;
    int fromSymbol = 0;
    for (const WatchVariable &variable : table.variables) {
        perKind[scalarKindName(variable.kind)] += 1;
        if (variable.addressFromSymbol)
            ++fromSymbol;
    }
    QStringList kinds;
    for (auto it = perKind.constBegin(); it != perKind.constEnd(); ++it)
        kinds << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
    kinds.sort();
    out(QStringLiteral("    类型分布: %1").arg(kinds.join(QStringLiteral("  "))));
    out(QStringLiteral("    地址来源: DWARF %1 / 符号表 %2")
            .arg(table.variables.size() - fromSymbol)
            .arg(fromSymbol));

    out(QString());
    out(QStringLiteral("    变量树（%1 个根，%2 个节点）")
            .arg(table.roots.size())
            .arg([&table] {
                int nodes = 0;
                for (const VariableNode &root : table.roots)
                    nodes += root.totalNodeCount();
                return nodes;
            }()));

    // Print the tree the way the UI shows it: nesting by indentation, arrays with
    // their extent in the type label and their elements as children.
    const bool showAll = options.verbose;
    int printedRoots = 0;
    int hiddenRoots = 0;

    struct Printer
    {
        static void print(const VariableNode &node, int depth, bool showAll,
                          int *budget, int *hidden)
        {
            if (*budget <= 0) {
                ++*hidden;
                return;
            }
            --*budget;
            QStringList bits;
            bits << QStringLiteral("%1%2").arg(QString(depth * 2, QLatin1Char(' ')), node.name);
            bits << node.typeLabel();
            if (node.hasAddress && node.address != 0)
                bits << hexAddr(node.address);
            if (node.byteSize > 0)
                bits << QStringLiteral("%1 B").arg(node.byteSize);
            if (node.incomplete)
                bits << QStringLiteral("[无布局]");
            const QString line = bits.join(QStringLiteral("  "));
            out(QStringLiteral("    %1").arg(line));
            Q_UNUSED(showAll);
            for (const VariableNode &child : node.children)
                print(child, depth + 1, showAll, budget, hidden);
        }
    };

    // Non-verbose output stops after a screenful so the listing stays readable.
    int budget = showAll ? 1 << 30 : 60;
    for (const VariableNode &root : table.roots) {
        if (budget <= 0) {
            ++hiddenRoots;
            continue;
        }
        ++printedRoots;
        Printer::print(root, 0, showAll, &budget, &hiddenRoots);
    }
    if (hiddenRoots > 0) {
        out(QStringLiteral("    ... 其余 %1 个节点省略（--verbose 显示全部）").arg(hiddenRoots));
    }

    // The flattened view is what sampling and the firmware snippet use; showing one
    // entry proves the two views agree.
    out(QString());
    out(QStringLiteral("    可采样叶子（扁平，%1 条）:").arg(table.variables.size()));
    for (int i = 0; i < table.variables.size() && i < (showAll ? 12 : 6); ++i) {
        const WatchVariable &variable = table.variables.at(i);
        out(QStringLiteral("      %1  %2  %3  %4 B")
                .arg(variable.name, -34)
                .arg(variable.typeLabel.left(18), -18)
                .arg(variable.addressLabel(), -10)
                .arg(variable.span()));
    }
    if (table.variables.size() > (showAll ? 12 : 6))
        out(QStringLiteral("      ... 其余 %1 条省略")
                .arg(table.variables.size() - (showAll ? 12 : 6)));

    return 0;
}

} // namespace mrd::cli
