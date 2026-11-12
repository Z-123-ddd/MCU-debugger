#include "app/Cli.h"

#include "core/DwarfReader.h"
#include "core/ElfFile.h"
#include "core/Logger.h"
#include "core/SymbolFile.h"

#include <QHash>

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
    out(QStringLiteral("    变量                       类型                    类别      地址        大小"));
    out(QStringLiteral("    -------------------------  ----------------------  ----------  -----"));
    const int limit = options.verbose ? table.variables.size() : 40;
    for (int i = 0; i < table.variables.size() && i < limit; ++i) {
        const WatchVariable &variable = table.variables.at(i);
        out(QStringLiteral("    %1  %2  %3  %4  %5 B")
                .arg(variable.name, -25)
                .arg(variable.typeLabel.left(22), -22)
                .arg(scalarKindName(variable.kind), -7)
                .arg(variable.addressLabel(), -10)
                .arg(variable.span()));
    }
    if (table.variables.size() > limit)
        out(QStringLiteral("    ... 其余 %1 个省略（--verbose 显示全部）")
                .arg(table.variables.size() - limit));

    return 0;
}

} // namespace mrd::cli
