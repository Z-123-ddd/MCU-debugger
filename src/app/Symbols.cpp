#include "app/Cli.h"

#include "core/DwarfReader.h"
#include "core/ElfFile.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/SymbolFile.h"
#include "ui/ScopePage.h"

#include <QFile>
#include <QFileInfo>

#include <functional>
#include <QHash>
#include <QTreeWidget>
#include <QElapsedTimer>
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
    if (table.addressesCorrected > 0) {
        out(QStringLiteral("    地址校正: %1 个变量的 DWARF 地址与符号表不一致，已按符号表修正")
                .arg(table.addressesCorrected));
    }

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

    const QVector<FileGroup> files = table.groupedByFile();
    out(QStringLiteral("    按文件分组: %1 个文件").arg(files.size()));
    const int fileLimit = options.verbose ? files.size() : 12;
    for (int i = 0; i < files.size() && i < fileLimit; ++i) {
        out(QStringLiteral("      %1  %2 个变量")
                .arg(files.at(i).file, -32)
                .arg(files.at(i).variables.size()));
    }
    if (files.size() > fileLimit)
        out(QStringLiteral("      ... 其余 %1 个文件").arg(files.size() - fileLimit));

    out(QStringLiteral("    类型定义: %1 个（含成员的 class/struct/union）")
            .arg(table.types.size()));

    // Functions with locals: the reason `match_indx` and friends were invisible is
    // that they are function-scope data, not globals.
    int localTotal = 0;
    for (const FunctionScope &scope : table.functions)
        localTotal += scope.localCount();
    out(QStringLiteral("    函数局部变量: %1 个函数 / %2 个局部变量")
            .arg(table.functions.size())
            .arg(localTotal));
    if (options.verbose) {
        for (int i = 0; i < table.functions.size(); ++i) {
            const FunctionScope &scope = table.functions.at(i);
            QStringList names;
            for (const VariableNode &local : scope.locals) {
                names << QStringLiteral("%1:%2").arg(local.name, local.typeLabel());
                if (names.size() >= 6) {
                    names << QStringLiteral("…");
                    break;
                }
            }
            out(QStringLiteral("      %1  [%2]  %3")
                    .arg(scope.name.left(38), -38)
                    .arg(scope.sourceFile.left(18), -18)
                    .arg(names.join(QStringLiteral(", "))));
        }
    }

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

int runBenchScope(const Options &options)
{
    // Times the parts that used to make the window freeze: parsing the DWARF,
    // building the variable tree, and materialising every widget row. The last one
    // is what lazy loading removed, so the numbers show whether it helped.
    Logger::instance()->setMinimumLevel(Logger::Warn);

    out(QStringLiteral("=== 变量面板耗时: %1 ===").arg(options.symbolPath));

    QElapsedTimer timer;
    timer.start();
    const auto loaded = SymbolFile::load(options.symbolPath);
    const qint64 parseMs = timer.elapsed();
    if (!loaded) {
        out(QStringLiteral("[FAIL] %1").arg(loaded.error));
        return 1;
    }
    const SymbolTable &table = loaded.value;
    int nodes = 0;
    for (const VariableNode &root : table.roots)
        nodes += root.totalNodeCount();
    out(QStringLiteral("    解析 DWARF      : %1 ms（%2 个根 / %3 个节点 / %4 个可采样叶子）")
            .arg(parseMs)
            .arg(table.roots.size())
            .arg(nodes)
            .arg(table.variables.size()));

    // Mirrors ScopePage's internal role (Qt::UserRole + 7): marks a row that decodes
    // to one number.
    constexpr int kNumericRoleProbe = Qt::UserRole + 7;

    ScopePage page;
    timer.restart();
    QString error;
    if (!page.importSymbolFile(options.symbolPath, &error)) {
        out(QStringLiteral("[FAIL] 导入面板: %1").arg(error));
        return 1;
    }
    const qint64 importMs = timer.elapsed();
    // The selector lives in its own container (so a dock can adopt it), which means
    // it is not a child of the page: findChild() would return nullptr and report an
    // empty panel even when the model is full.
    QTreeWidget *tree = page.variableTree();
    out(QStringLiteral("    导入面板(懒加载): %1 ms（实际创建 %2 个顶层行）")
            .arg(importMs)
            .arg(tree ? tree->topLevelItemCount() : -1));
    if (tree) {
        QStringList names;
        for (int i = 0; i < tree->topLevelItemCount() && i < 5; ++i)
            names << QStringLiteral("%1(%2)")
                         .arg(tree->topLevelItem(i)->text(0))
                         .arg(tree->topLevelItem(i)->childCount());
        out(QStringLiteral("    顶层样例: %1").arg(names.join(QStringLiteral(" | "))));
        // Expand the first file row and report what appeared.
        if (tree->topLevelItemCount() > 0) {
            QTreeWidgetItem *first = tree->topLevelItem(0);
            first->setExpanded(true);
            out(QStringLiteral("    展开 \"%1\" 后: %2 行")
                    .arg(first->text(0))
                    .arg(first->childCount()));
        }
    }

    // Expand everything, which is what the old eager build did up front.
    if (tree) {
        timer.restart();
        struct Expander
        {
            static void run(QTreeWidgetItem *item)
            {
                item->setExpanded(true);
                for (int i = 0; i < item->childCount(); ++i)
                    run(item->child(i));
            }
        };
        int rows = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            Expander::run(tree->topLevelItem(i));
            rows += 0;
        }
        const qint64 expandMs = timer.elapsed();
        out(QStringLiteral("    全展开(旧行为)  : %1 ms").arg(expandMs));
    }

    // Does a local like `match_indx` (the reported case) actually appear under its
    // file now? This is the assertion the user cares about, so verify it end to end.
    if (tree) {
        const QString needle = QStringLiteral("match_indx");
        bool found = false;
        QString where;
        for (int i = 0; i < tree->topLevelItemCount() && !found; ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            if (fileRow->text(0) != QLatin1String("encoder.cpp"))
                continue;
            fileRow->setExpanded(true);
            for (int j = 0; j < fileRow->childCount() && !found; ++j) {
                QTreeWidgetItem *funcRow = fileRow->child(j);
                funcRow->setExpanded(true);
                for (int k = 0; k < funcRow->childCount(); ++k) {
                    if (funcRow->child(k)->text(0) == needle) {
                        found = true;
                        where = QStringLiteral("%1 / %2")
                                    .arg(fileRow->text(0), funcRow->text(0));
                        break;
                    }
                }
            }
        }
        out(QStringLiteral("    局部变量 match_indx: %1")
                .arg(found ? QStringLiteral("在 %1 下").arg(where)
                           : QStringLiteral("未找到")));

        // Class members: `index_found_` is an Encoder member, so it belongs under the
        // file that DEFINES Encoder, as well as under any instance of it.
        bool memberFound = false;
        QString memberWhere;
        for (int i = 0; i < tree->topLevelItemCount() && !memberFound; ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *typeRow = fileRow->child(j);
                if (typeRow->text(0) != QLatin1String("Encoder"))
                    continue;
                for (int k = 0; k < typeRow->childCount(); ++k) {
                    if (typeRow->child(k)->text(0) == QLatin1String("index_found_")) {
                        memberFound = true;
                        memberWhere = QStringLiteral("%1 → Encoder → index_found_ (%2)")
                                          .arg(fileRow->text(0),
                                               typeRow->child(k)->text(2));
                        break;
                    }
                }
            }
        }
        out(QStringLiteral("    类成员 index_found_: %1")
                .arg(memberFound ? memberWhere : QStringLiteral("未找到")));

        // A `static` class member is a real variable with its own address; it must
        // not be shown at the instance base.
        QString staticWhere;
        for (int i = 0; i < tree->topLevelItemCount() && staticWhere.isEmpty(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *typeRow = fileRow->child(j);
                if (typeRow->text(0) != QLatin1String("Encoder"))
                    continue;
                for (int k = 0; k < typeRow->childCount(); ++k) {
                    QTreeWidgetItem *m = typeRow->child(k);
                    if (m->text(0) != QLatin1String("ports_to_sample"))
                        continue;
                    staticWhere = QStringLiteral("%1 → Encoder → ports_to_sample @ %2 (%3)")
                                      .arg(fileRow->text(0), m->text(2), m->text(1));
                    break;
                }
            }
        }
        out(QStringLiteral("    静态成员 ports_to_sample: %1")
                .arg(staticWhere.isEmpty() ? QStringLiteral("未找到") : staticWhere));

        // Vendor files must be hidden unless the user asks for them.
        bool vendorVisible = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const QString row = tree->topLevelItem(i)->text(0);
            if (row.startsWith(QLatin1String("arm_")) || row.contains(QLatin1String("cmsis"))
                || row.contains(QLatin1String("libcxx"))) {
                vendorVisible = true;
            }
        }
        out(QStringLiteral("    芯片包文件默认隐藏: %1")
                .arg(vendorVisible ? QStringLiteral("否（有泄漏）") : QStringLiteral("是")));
    }

    // Filtering must actually reduce the tree. It used to stop at the variable
    // level, so searching still listed every function/type of every file.
    if (tree) {
        const QString needle = QStringLiteral("match_indx");
        page.filterForBench(needle);
        const int rowsAfter = tree->topLevelItemCount();
        int funcRows = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            funcRows += fileRow->childCount();
        }
        out(QStringLiteral("    搜索 \"%1\": %2 个文件行 / %3 个子行")
                .arg(needle)
                .arg(rowsAfter)
                .arg(funcRows));
        page.filterForBench(QString());
    }

    // Search behaviour on the real image: a member-only match must not dump the
    // whole class, and members must be selectable when an instance is known.
    if (tree) {
        for (const QString &needle : {QStringLiteral("abs"), QStringLiteral("index_found")}) {
            page.filterForBench(needle);
            int fileRows = tree->topLevelItemCount();
            int memberRows = 0;
            int selectable = 0;
            for (int i = 0; i < fileRows; ++i) {
                QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                fileRow->setExpanded(true);
                page.expandForTest(fileRow);
                for (int j = 0; j < fileRow->childCount(); ++j) {
                    QTreeWidgetItem *child = fileRow->child(j);
                    if (!child->text(1).startsWith(QStringLiteral("函数")))
                        continue;
                    child->setExpanded(true);
                    for (int k = 0; k < child->childCount(); ++k) {
                        ++memberRows;
                        if (child->child(k)->flags() & Qt::ItemIsUserCheckable)
                            ++selectable;
                    }
                }
            }
            out(QStringLiteral("    搜索 \"%1\": %2 个文件行，函数下 %3 项，可勾选 %4 项")
                    .arg(needle)
                    .arg(fileRows)
                    .arg(memberRows)
                    .arg(selectable));
        }
        // Is every surviving file row actually justified? A row that contains no
        // match anywhere is the "nothing was filtered" symptom.
        page.filterForBench(QStringLiteral("abs"));
        QStringList unjustified;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            const QString fileName = fileRow->text(0);
            const bool nameMatched = fileName.contains(QStringLiteral("abs"),
                                                       Qt::CaseInsensitive);
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            int matchedRows = 0;
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *row = fileRow->child(j);
                if (row->text(0).contains(QStringLiteral("abs"), Qt::CaseInsensitive)
                    || row->text(1).contains(QStringLiteral("abs"), Qt::CaseInsensitive)) {
                    ++matchedRows;
                    continue;
                }
                // Recurse fully: `encoders` matches only three levels down
                // (`encoders[0].sec_enc_pos_abs_`), so a shallow check would call a
                // legitimate row spurious.
                std::function<void(QTreeWidgetItem *)> deep = [&](QTreeWidgetItem *node) {
                    node->setExpanded(true);
                    page.expandForTest(node);
                    for (int k = 0; k < node->childCount(); ++k) {
                        QTreeWidgetItem *child = node->child(k);
                        if (child->text(0).contains(QStringLiteral("abs"), Qt::CaseInsensitive)
                            || child->text(1).contains(QStringLiteral("abs"),
                                                       Qt::CaseInsensitive)) {
                            ++matchedRows;
                        }
                        deep(child);
                    }
                };
                deep(row);
            }
            if (!nameMatched && matchedRows == 0)
                unjustified << fileName;
        }
        out(QStringLiteral("    搜索 \"abs\": 无匹配内容的文件行 %1 个%2")
                .arg(unjustified.size())
                .arg(unjustified.isEmpty()
                         ? QString()
                         : QStringLiteral("（%1）").arg(unjustified.join(QStringLiteral(", ")))));

        // How many members does Encoder show when searching? Before the fix,
        // a member-only match still listed all 63.
        page.filterForBench(QStringLiteral("abs"));
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            if (fileRow->text(0) != QLatin1String("encoder.hpp"))
                continue;
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *typeRow = fileRow->child(j);
                if (typeRow->text(0) != QLatin1String("Encoder"))
                    continue;
                QStringList shown;
                for (int k = 0; k < typeRow->childCount(); ++k)
                    shown << typeRow->child(k)->text(0);
                out(QStringLiteral("    搜索 \"abs\" 时 Encoder 显示 %1 个成员: %2")
                        .arg(shown.size())
                        .arg(shown.join(QStringLiteral(", ")).left(90)));
                break;
            }
            break;
        }

        // How much of the type index is actually usable? Members of a type with no
        // global instance have no address anywhere, so they cannot be sampled -- say
        // how many that is instead of leaving it a mystery.
        page.filterForBench(QString());
        int totalMembers = 0;
        int tickableMembers = 0;
        QStringList nonNumericTickable;
        std::function<void(QTreeWidgetItem *, bool)> walk =
            [&](QTreeWidgetItem *node, bool insideType) {
                for (int k = 0; k < node->childCount(); ++k) {
                    QTreeWidgetItem *child = node->child(k);
                    const bool isTypeRow = child->text(1).contains(QStringLiteral("个成员"));
                    if (insideType && !isTypeRow) {
                        ++totalMembers;
                        if (child->flags() & Qt::ItemIsUserCheckable) {
                            ++tickableMembers;
                            // The invariant the user asked for: a tickable row must be
                            // a single decimal number. The flag is stored on the row by
                            // the builder, so this checks the real type rather than
                            // guessing from the label text.
                            if (!child->data(0, kNumericRoleProbe).toBool())
                                nonNumericTickable << child->text(0);
                        }
                    }
                    walk(child, insideType || isTypeRow);
                }
            };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            walk(fileRow, false);
        }
        out(QStringLiteral("    类型成员: 共 %1 个，可勾选 %2 个（其余没有全局实例）")
                .arg(totalMembers)
                .arg(tickableMembers));
        out(QStringLiteral("    可勾选中疑似非数值的行: %1 个%2")
                .arg(nonNumericTickable.size())
                .arg(nonNumericTickable.isEmpty()
                         ? QString()
                         : QStringLiteral("（%1）")
                               .arg(nonNumericTickable.mid(0, 5).join(QStringLiteral(", ")))));

        // v
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            if (fileRow->text(0) != QLatin1String("encoder.hpp"))
                continue;
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *typeRow = fileRow->child(j);
                if (typeRow->text(0) != QLatin1String("Encoder"))
                    continue;
                QStringList found;
                std::function<void(QTreeWidgetItem *, int)> deep =
                    [&](QTreeWidgetItem *node, int depth) {
                        for (int k = 0; k < node->childCount(); ++k) {
                            QTreeWidgetItem *child = node->child(k);
                            if (child->text(0) == QLatin1String("config_")
                                || child->text(0) == QLatin1String("vel_gain")
                                || child->text(0) == QLatin1String("control_mode")) {
                                found << QStringLiteral("%1%2[%3|%4|%5]")
                                             .arg(QString(depth * 2, QLatin1Char(' ')),
                                                  child->text(0), child->text(1),
                                                  child->text(2), child->text(3));
                            }
                            if (depth < 5)
                                deep(child, depth + 1);
                        }
                    };
                deep(typeRow, 0);
                out(QStringLiteral("    Encoder 深层成员样例: %1")
                        .arg(found.mid(0, 6).join(QStringLiteral("  "))));
                break;
            }
            break;
        }

        page.filterForBench(QStringLiteral("index_found"));
        // The type row under encoder.hpp: members must be tickable via the instance.
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
            if (fileRow->text(0) != QLatin1String("encoder.hpp"))
                continue;
            fileRow->setExpanded(true);
            page.expandForTest(fileRow);
            for (int j = 0; j < fileRow->childCount(); ++j) {
                QTreeWidgetItem *typeRow = fileRow->child(j);
                for (int k = 0; k < typeRow->childCount(); ++k) {
                    QTreeWidgetItem *m = typeRow->child(k);
                    if (m->text(0) == QLatin1String("index_found_")) {
                        // Ticking a member from the type row must become a real sample
                        // point; before, the checkbox did nothing at all.
                        m->setCheckState(0, Qt::Checked);
                        page.settleSelectionForTest();
                        out(QStringLiteral("    勾选 index_found_ → 导出变量 %1 个")
                                .arg(page.testSelectedCount()));
                        m->setCheckState(0, Qt::Unchecked);
                        page.settleSelectionForTest();
                    }
                    out(QStringLiteral("    %1 → %2 → %3 | 地址 %4 | 可勾选 %5")
                            .arg(fileRow->text(0), typeRow->text(0), m->text(0), m->text(2))
                            .arg((m->flags() & Qt::ItemIsUserCheckable) ? QStringLiteral("是")
                                                                       : QStringLiteral("否")));
                }
            }
            break;
        }
        page.filterForBench(QString());
    }

    // Filtering: it used to rebuild the whole tree per keystroke.
    timer.restart();
    page.filterForBench(QStringLiteral("config"));
    out(QStringLiteral("    过滤重建        : %1 ms").arg(timer.elapsed()));
    return 0;
}

} // namespace mrd::cli
