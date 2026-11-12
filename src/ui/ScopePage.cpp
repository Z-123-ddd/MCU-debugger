#include "ui/ScopePage.h"

#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeController.h"
#include "ui/PlotWidget.h"

#include <QApplication>

#include <algorithm>
#include <functional>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace mrd {

ScopePage::ScopePage(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // --- source file row ----------------------------------------------------
    auto *fileRow = new QHBoxLayout();
    fileRow->setSpacing(6);
    fileRow->addWidget(new QLabel(QStringLiteral("符号文件:"), this));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(
        QStringLiteral("尚未导入 —— 选择 Keil 生成的 .axf（需勾选 Debug Information）"));
    fileRow->addWidget(m_pathEdit, 1);
    m_browseButton = new QPushButton(QStringLiteral("导入 .axf…"), this);
    fileRow->addWidget(m_browseButton);
    root->addLayout(fileRow);

    connect(m_browseButton, &QPushButton::clicked, this, &ScopePage::onBrowse);

    // --- variable list ------------------------------------------------------
    // Parentless on purpose: MainWindow puts this in a dock so it can be moved,
    // floated or hidden, and the plot gets the full width of the tab. Owning the
    // widget here keeps all the tree logic (lazy loading, tri-state, filtering) in
    // one place instead of splitting it across two classes.
    m_selectorBox = new QWidget(nullptr);
    auto *listGroup = new QGroupBox(QStringLiteral("可采样变量（全局 / 静态）"), m_selectorBox);
    auto *selectorLayout = new QVBoxLayout(m_selectorBox);
    selectorLayout->setContentsMargins(0, 0, 0, 0);
    selectorLayout->addWidget(listGroup);
    auto *listLayout = new QVBoxLayout(listGroup);
    listLayout->setContentsMargins(6, 6, 6, 6);
    listLayout->setSpacing(4);

    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);
    m_filterEdit = new QLineEdit(listGroup);
    m_filterEdit->setPlaceholderText(QStringLiteral("按名字 / 类型 / 地址过滤，例如 g_adc 或 float"));
    filterRow->addWidget(m_filterEdit, 1);
    m_showSystemCheck = new QCheckBox(QStringLiteral("含芯片包"), listGroup);
    m_showSystemCheck->setToolTip(
        QStringLiteral("勾选后同时显示芯片包 / 编译器头文件里的变量\n"
                       "（CMSIS 查找表、libc++ 内部量等）。默认隐藏，"
                       "否则它们会淹没项目自己的变量。"));
    m_showSystemCheck->setChecked(m_showSystemFiles);
    filterRow->addWidget(m_showSystemCheck);
    m_selectAll = new QCheckBox(QStringLiteral("全选"), listGroup);
    filterRow->addWidget(m_selectAll);
    connect(m_showSystemCheck, &QCheckBox::checkStateChanged, this,
            [this](Qt::CheckState state) {
                m_showSystemFiles = (state == Qt::Checked);
                rebuildVariableList();
            });
    listLayout->addLayout(filterRow);

    m_variableTree = new QTreeWidget(listGroup);
    m_variableTree->setColumnCount(4);
    m_variableTree->setHeaderLabels({QStringLiteral("变量"), QStringLiteral("类型"),
                                     QStringLiteral("地址"), QStringLiteral("大小")});
    // setRootIsDecorated(false) removes the expand arrow from TOP-LEVEL items --
    // and every struct/array is a top-level item, so the tree rendered as a flat
    // list with no way to open anything. Keep the decoration.
    m_variableTree->setRootIsDecorated(true);
    m_variableTree->setIndentation(14);
    m_variableTree->setExpandsOnDoubleClick(true);
    m_variableTree->setUniformRowHeights(true);
    m_variableTree->setAlternatingRowColors(true);
    m_variableTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_variableTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    listLayout->addWidget(m_variableTree, 1);

    // (the selector is shown by MainWindow in a dock, not here)


    connect(m_filterEdit, &QLineEdit::textChanged, this, &ScopePage::onFilterChanged);
    connect(m_selectAll, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
        const bool on = (state == Qt::Checked);
        for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
            m_variableTree->topLevelItem(i)->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(m_variableTree, &QTreeWidget::itemChanged, this, &ScopePage::onVariableToggled);

    // --- controls -----------------------------------------------------------
    auto *controlRow = new QHBoxLayout();
    controlRow->setSpacing(6);
    m_startButton = new QPushButton(QStringLiteral("开始"), this);
    controlRow->addWidget(m_startButton);
    controlRow->addWidget(new QLabel(QStringLiteral("时间窗:"), this));
    m_windowCombo = new QComboBox(this);
    for (const auto &item : {std::make_pair(QStringLiteral("2 秒"), 2.0),
                             std::make_pair(QStringLiteral("5 秒"), 5.0),
                             std::make_pair(QStringLiteral("10 秒"), 10.0),
                             std::make_pair(QStringLiteral("30 秒"), 30.0),
                             std::make_pair(QStringLiteral("全部"), 0.0)}) {
        m_windowCombo->addItem(item.first, item.second);
    }
    m_windowCombo->setCurrentIndex(2);
    controlRow->addWidget(m_windowCombo);
    m_autoScale = new QCheckBox(QStringLiteral("自动缩放"), this);
    m_autoScale->setChecked(true);
    controlRow->addWidget(m_autoScale);
    m_codeButton = new QPushButton(QStringLiteral("复制固件端代码"), this);
    controlRow->addWidget(m_codeButton);
    m_localsButton = new QPushButton(QStringLiteral("读取局部变量"), this);
    m_localsButton->setToolTip(
        QStringLiteral("局部变量只在停机时可读：先「暂停」再点这里。\n"
                       "地址 = 帧基址 + DWARF 偏移，运行时会失效。"));
    controlRow->addWidget(m_localsButton);
    controlRow->addStretch(1);
    m_statsLabel = new QLabel(this);
    controlRow->addWidget(m_statsLabel);
    root->addLayout(controlRow);

    connect(m_startButton, &QPushButton::clicked, this, &ScopePage::onStartStop);
    connect(m_windowCombo, &QComboBox::currentIndexChanged, this,
            &ScopePage::onSampleSettingsChanged);
    connect(m_autoScale, &QCheckBox::checkStateChanged, this,
            &ScopePage::onSampleSettingsChanged);
    connect(m_codeButton, &QPushButton::clicked, this, &ScopePage::onCopyFirmwareCode);
    connect(m_localsButton, &QPushButton::clicked, this, &ScopePage::refreshLocals);

    // --- plot ---------------------------------------------------------------
    m_plot = new PlotWidget(this);
    m_plot->setHistory(m_collector.history());
    root->addWidget(m_plot, 3);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    root->addWidget(m_stateLabel);

    connect(&m_collector, &SampleCollector::samplesAppended, this, &ScopePage::onSamplesAppended);
    connect(&m_collector, &SampleCollector::statistics, this, &ScopePage::onStatistics);
    connect(&m_collector, &SampleCollector::failed, this, &ScopePage::onCollectorFailed);
    connect(&m_collector, &SampleCollector::runningChanged, this, [this](bool) {
        updateControls();
    });

    // One re-evaluation after a burst of checkbox changes settles.
    m_selectionTimer.setSingleShot(true);
    m_selectionTimer.setInterval(0);
    connect(&m_selectionTimer, &QTimer::timeout, this, &ScopePage::onSampleSettingsChanged);

    // Typing in the filter must not rebuild the tree on every character.
    m_filterTimer.setSingleShot(true);
    m_filterTimer.setInterval(220);
    connect(&m_filterTimer, &QTimer::timeout, this, &ScopePage::onFilterDebounced);
    connect(m_variableTree, &QTreeWidget::itemExpanded, this, &ScopePage::onItemExpanded);
    connect(m_probe, &ProbeController::registersRead, this, &ScopePage::onRegistersRead);
    connect(this, &ScopePage::localsRefreshRequested, this, &ScopePage::refreshLocals);

    updateControls();
    refreshState();
}

void ScopePage::setProbe(ProbeController *probe)
{
    if (m_probe == probe)
        return;
    if (m_probe)
        m_probe->disconnect(this);
    m_probe = probe;
    m_collector.setProbe(probe);
    if (!m_probe)
        return;
    // RTT discovery runs on the probe worker thread; this is where its answer
    // lands. Registered here rather than in the constructor because the
    // controller does not exist yet at that point.
    connect(m_probe, &ProbeController::rttLocated, this,
            [this](const RttControlBlock &block, const QString &error) {
                if (block.valid()) {
                    m_rtt = block;
                    m_collector.setChannel(m_rtt, 0);
                    emit statusMessage(
                        QStringLiteral("RTT 已就绪：%1 个上行通道，通道 0 缓冲 %2 字节")
                            .arg(m_rtt.upBuffers.size())
                            .arg(m_rtt.upBuffers.first().size));
                } else {
                    m_rtt = RttControlBlock();
                    emit statusMessage(QStringLiteral("未找到 RTT 控制块：%1").arg(error));
                }
                refreshState();
            });
}

void ScopePage::attachTarget(quint32 ramStart, quint32 ramSize)
{
    m_attached = true;
    m_rtt = RttControlBlock();

    if (!m_probe) {
        refreshState();
        return;
    }

    // The probe lives on the worker thread, so discovery is dispatched there too;
    // the answer arrives via ProbeController::rttLocated.
    quint32 hint = 0;
    if (m_symbols.isValid()) {
        if (const WatchVariable *v = m_symbols.find(QStringLiteral("_SEGGER_RTT")))
            hint = v->address;
    }
    emit statusMessage(QStringLiteral("正在定位 RTT 控制块…"));
    m_probe->requestFindRtt(hint, ramStart, ramSize);
    refreshState();
}

void ScopePage::detachTarget()
{
    m_collector.stop();
    m_attached = false;
    m_rtt = RttControlBlock();
    refreshState();
}

bool ScopePage::importSymbolFile(const QString &path, QString *error)
{
    const auto loaded = SymbolFile::load(path);
    if (!loaded) {
        if (error)
            *error = loaded.error;
        return false;
    }
    m_collector.stop();
    m_symbols = loaded.value;
    m_pathEdit->setText(path);
    m_pathEdit->setToolTip(path);
    rebuildVariableList();
    refreshState();
    emit symbolsImported();
    emit statusMessage(QStringLiteral("已导入 %1：%2 个变量（DWARF v%3）")
                           .arg(QFileInfo(path).fileName())
                           .arg(m_symbols.variables.size())
                           .arg(m_symbols.dwarfVersion));
    return true;
}

void ScopePage::onBrowse()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Keil 生成的符号文件"), QString(),
        SymbolFile::fileFilters().join(QStringLiteral(";;")));
    if (path.isEmpty())
        return;
    QString error;
    if (!importSymbolFile(path, &error)) {
        m_stateLabel->setText(QStringLiteral("导入失败：%1").arg(error));
        emit statusMessage(QStringLiteral("导入失败：%1").arg(error));
    }
}

namespace {

/// Full path of a node inside the tree, spelled the way C would: `g_points[0].x`.
QString childPath(const QString &parent, const QString &name)
{
    if (parent.isEmpty())
        return name;
    if (name.startsWith(QLatin1Char('[')))
        return parent + name;
    return parent + QLatin1Char('.') + name;
}

/// Multi-line tooltip for a variable row: the full path plus everything DWARF said
/// about it. Shared, so a file row's children and an expanded member look identical.
QString tooltipFor(const VariableNode &node, const QString &fullPath)
{
    QStringList tip;
    tip << fullPath;
    if (!node.sourceFile.isEmpty())
        tip << QStringLiteral("文件: %1").arg(node.sourceFile);
    tip << QStringLiteral("类型: %1（%2）").arg(node.typeLabel(), nodeKindName(node.kind));
    if (node.kind == NodeKind::Array) {
        tip << QStringLiteral("元素数: %1，每元素 %2 字节")
                   .arg(node.elementCount)
                   .arg(node.elementSize);
    }
    if (node.hasAddress && node.address != 0)
        tip << QStringLiteral("地址: %1").arg(hexAddr(node.address));
    if (node.byteSize > 0)
        tip << QStringLiteral("大小: %1 字节").arg(node.byteSize);
    if (node.samplable())
        tip << QStringLiteral("采样: %1").arg(scalarKindName(node.scalar));
    if (node.addressFromSymbol)
        tip << QStringLiteral("（地址取自链接符号表）");
    if (node.incomplete)
        tip << QStringLiteral("（布局不完整：类型仅有声明，或展开已截断）");
    return tip.join(QChar(10));
}

/// Marker stored on a placeholder child so it can be recognised and removed.
constexpr int kPlaceholderRole = Qt::UserRole + 4;
/// Index into m_groups for a file row, -1 for a variable row.
constexpr int kFileGroupRole = Qt::UserRole + 5;
/// Raw VariableNode pointer for rows whose node is not in m_nodeByPath (function
/// rows under a file). Valid because the tree is rebuilt whenever the model changes.
constexpr int kNodePtrRole = Qt::UserRole + 6;

const VariableNode *nodeOf(const QTreeWidgetItem *item)
{
    if (!item)
        return nullptr;
    const QVariant v = item->data(0, kNodePtrRole);
    if (!v.isValid())
        return nullptr;
    return reinterpret_cast<const VariableNode *>(v.value<qulonglong>());
}

} // namespace

namespace {

/// Every samplable leaf path under \a node, whose own full path is \a ownPath.
void collectLeafPathsAt(const VariableNode &node, const QString &ownPath, QStringList *out)
{
    if (node.children.isEmpty()) {
        if (node.samplable())
            out->append(ownPath);
        return;
    }
    for (const VariableNode &child : node.children)
        collectLeafPathsAt(child, childPath(ownPath, child.name), out);
}

} // namespace

QTreeWidgetItem *ScopePage::makeItem(const VariableNode &node, const QString &path,
                                     QTreeWidgetItem *parent)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_variableTree);
    item->setText(0, node.name);
    item->setText(1, node.typeLabel());
    // A local's address is only known while the core is halted in its function, so
    // it comes from the resolved map rather than from the node.
    quint32 shownAddress = node.address;
    bool hasShownAddress = node.hasAddress && node.address != 0;
    if (!hasShownAddress && (node.stackRelative || node.locListOffset != 0)) {
        const auto it = m_localAddr.constFind(path);
        if (it != m_localAddr.constEnd()) {
            shownAddress = it.value();
            hasShownAddress = shownAddress != 0;
        }
    }
    item->setText(2, hasShownAddress ? hexAddr(shownAddress) : QString());
    item->setText(3, node.byteSize > 0 ? QStringLiteral("%1 B").arg(node.byteSize) : QString());
    item->setData(0, Qt::UserRole, path);
    item->setData(0, Qt::UserRole + 2, node.isAggregate());
    // Lazy-loading state: has this node's subtree been materialised yet?
    item->setData(0, Qt::UserRole + 3, false);
    item->setData(0, kFileGroupRole, -1);

    // Tooltips are built lazily too -- 6000 multi-line tooltips is megabytes of
    // QString and shows up in the import time for no benefit.
    // Locals and function rows are not sampleable: a local's address changes with
    // the stack, so ticking it would produce a curve of nonsense.
    const bool selectable = node.kind != NodeKind::Function && !node.stackRelative
        && node.locListOffset == 0;
    item->setFlags(item->flags()
                   | (selectable ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));

    if (node.isAggregate()) {
        // A placeholder child makes Qt draw the expand arrow without us building
        // the subtree. It carries no checkbox and is replaced on first expand.
        auto *placeholder = new QTreeWidgetItem(item);
        placeholder->setData(0, kPlaceholderRole, true);
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setText(0, QStringLiteral("…"));
    }
    return item;
}

QTreeWidgetItem *ScopePage::makeFileItem(const FileGroup &group, int groupIndex)
{
    auto *item = new QTreeWidgetItem(m_variableTree);
    item->setText(0, group.file);
    item->setText(1, QStringLiteral("%1 个变量").arg(group.variables.size()));
    item->setText(3, QStringLiteral("%1 个节点").arg(group.nodeCount));
    item->setData(0, Qt::UserRole, QString());
    item->setData(0, Qt::UserRole + 2, true); // aggregating: ticking it ticks all
    item->setData(0, Qt::UserRole + 3, false);
    item->setData(0, kFileGroupRole, groupIndex);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setToolTip(0, QStringLiteral("%1\n%2 个变量 / %3 个节点")
                            .arg(group.file)
                            .arg(group.variables.size())
                            .arg(group.nodeCount));

    // Same lazy trick as an aggregate: one placeholder keeps the arrow drawn.
    auto *placeholder = new QTreeWidgetItem(item);
    placeholder->setData(0, kPlaceholderRole, true);
    placeholder->setFlags(Qt::NoItemFlags);
    placeholder->setText(0, QStringLiteral("…"));
    return item;
}

void ScopePage::populateChildren(QTreeWidgetItem *item)
{
    if (!item || item->data(0, Qt::UserRole + 3).toBool())
        return; // already populated
    item->setData(0, Qt::UserRole + 3, true);

    // Drop the placeholder.
    while (item->childCount() > 0) {
        QTreeWidgetItem *child = item->child(0);
        if (!child->data(0, kPlaceholderRole).toBool())
            break;
        delete item->takeChild(0);
    }

    // A file row's children are the variables declared in that file; a variable
    // row's children come from the model tree.
    // An unset role must mean "not a file row": QVariant::toInt() on an invalid
    // variant returns 0, which silently treated every other aggregate (the locals
    // header) as file group #0 and filled it with that file's variables.
    const QVariant fileRole = item->data(0, kFileGroupRole);
    const int groupIndex = fileRole.isValid() ? fileRole.toInt() : -1;
    if (groupIndex >= 0) {
        // Rows are built from the union of files (variables + functions + types), so
        // the index refers to m_fileRowGroups, not to m_groups -- using m_groups here
        // showed one file's variables under another file's row.
        if (groupIndex >= m_fileRowGroups.size())
            return;
        const QSignalBlocker fileBlocker(m_variableTree);
        const FileGroup &group = m_fileRowGroups.at(groupIndex);
        for (const VariableNode &variable : group.variables) {
            const QString fullPath = childPath(QString(), variable.name);
            QTreeWidgetItem *childItem = makeItem(variable, fullPath, item);
            if (variable.children.isEmpty() && variable.samplable()) {
                childItem->setCheckState(0, m_selectedLeaves.contains(fullPath)
                                                ? Qt::Checked
                                                : Qt::Unchecked);
            }
            childItem->setToolTip(0, tooltipFor(variable, fullPath));
        }

        // Types defined in this file, with their members.
        //
        // Members have no address of their own (only an offset inside an instance),
        // so they are not tickable -- but they are the reason someone opens
        // `encoder.cpp` in the first place: `Encoder::index_found_` must be visible
        // here, not only under whichever file declares a global instance.
        const QVector<TypeDecl> types = m_symbols.typesInFile(group.file);
        for (const TypeDecl &type : types) {
            auto *typeItem = new QTreeWidgetItem(item);
            typeItem->setText(0, type.name);
            typeItem->setText(1, QStringLiteral("%1 · %2 个成员").arg(type.kind)
                                     .arg(type.members.size()));
            typeItem->setText(3, type.byteSize > 0
                                     ? QStringLiteral("%1 B").arg(type.byteSize)
                                     : QString());
            typeItem->setFlags(Qt::ItemIsEnabled);
            typeItem->setToolTip(
                0, QStringLiteral("%1 %2\n%3 个成员，共 %4 字节\n"
                                  "成员没有独立地址，只能通过实例访问（例如 encoders[0]）")
                       .arg(type.kind, type.name)
                       .arg(type.members.size())
                       .arg(type.byteSize));
            // Members are created up front: there are at most 512 and they are
            // plain rows, so lazily loading them would cost more than it saves.
            for (const TypeMember &member : type.members) {
                if (member.isStatic) {
                    // A static member is a real variable with its own address, so it
                    // is selectable like any global -- which is what "all static
                    // variables" means for something like Encoder::ports_to_sample.
                    auto *staticItem = new QTreeWidgetItem(typeItem);
                    staticItem->setText(0, member.name);
                    staticItem->setText(1, QStringLiteral("static %1").arg(member.typeLabel));
                    staticItem->setText(2, member.hasAddress ? hexAddr(member.address)
                                                             : QStringLiteral("?"));
                    staticItem->setText(3, member.byteSize > 0
                                             ? QStringLiteral("%1 B").arg(member.byteSize)
                                             : QString());
                    staticItem->setData(0, Qt::UserRole,
                                        QStringLiteral("%1::%2").arg(type.name, member.name));
                    staticItem->setData(0, Qt::UserRole + 2, false);
                    if (member.hasAddress) {
                        staticItem->setFlags(staticItem->flags() | Qt::ItemIsUserCheckable);
                        staticItem->setCheckState(
                            0, m_selectedLeaves.contains(
                                   QStringLiteral("%1::%2").arg(type.name, member.name))
                                   ? Qt::Checked
                                   : Qt::Unchecked);
                        // Register it so ticking it can be turned into a sample point.
                        m_staticMembers.insert(
                            QStringLiteral("%1::%2").arg(type.name, member.name),
                            qMakePair(member.address, member.byteSize));
                    } else {
                        staticItem->setFlags(Qt::ItemIsEnabled);
                    }
                    staticItem->setToolTip(
                        0, QStringLiteral("%1::%2  (static)\n类型: %3\n%4\n"
                                          "静态成员有自己的地址，可像全局变量一样采样")
                               .arg(type.name, member.name, member.typeLabel,
                                    member.hasAddress
                                        ? QStringLiteral("地址: %1").arg(hexAddr(member.address))
                                        : QStringLiteral("地址未知（链接期解析或已优化）")));
                    continue;
                }
                auto *memberItem = new QTreeWidgetItem(typeItem);
                memberItem->setText(0, member.name);
                memberItem->setText(1, member.typeLabel);
                memberItem->setText(2, QStringLiteral("+%1").arg(member.byteOffset));
                memberItem->setText(3, member.byteSize > 0
                                         ? QStringLiteral("%1 B").arg(member.byteSize)
                                         : QString());
                memberItem->setFlags(Qt::ItemIsEnabled);
                memberItem->setToolTip(
                    0, QStringLiteral("%1::%2\n类型: %3\n实例内偏移: +%4 字节\n"
                                      "（成员随实例存在，同一类型的不同实例各有自己的副本）")
                           .arg(type.name, member.name, member.typeLabel)
                           .arg(member.byteOffset));
            }
        }

        // Then the functions of this file, so their locals are reachable.
        QVector<VariableNode> &functions = m_functionsByFile[group.file];
        for (VariableNode &function : functions) {
            const QString funcPath = childPath(QString(), function.name);
            QTreeWidgetItem *funcItem = makeItem(function, funcPath, item);
            funcItem->setData(0, kNodePtrRole,
                              QVariant::fromValue<qulonglong>(
                                  static_cast<qulonglong>(reinterpret_cast<quintptr>(&function))));
            funcItem->setText(1, QStringLiteral("函数 · %1 个局部变量")
                                     .arg(function.children.size()));
            funcItem->setToolTip(
                0, QStringLiteral("%1()\n%2 个局部变量\n只在停机且停在该函数内时可读")
                       .arg(function.name)
                       .arg(function.children.size()));
        }
        recomputeAncestors(item->child(0));
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    // A function row is not in m_nodeByPath; it carries its own pointer.
    const VariableNode *node = nodeOf(item);
    if (!node)
        node = m_nodeByPath.value(path, nullptr);
    if (!node || node->children.isEmpty())
        return;

    const QSignalBlocker blocker(m_variableTree);
    for (const VariableNode &child : node->children) {
        const QString childFullPath = childPath(path, child.name);
        QTreeWidgetItem *childItem = makeItem(child, childFullPath, item);
        if (child.children.isEmpty() && child.samplable()) {
            childItem->setCheckState(0, m_selectedLeaves.contains(childFullPath)
                                            ? Qt::Checked
                                            : Qt::Unchecked);
        }
        childItem->setToolTip(0, tooltipFor(child, childFullPath));
    }
    // Newly created children can flip this node between checked/partial/unchecked.
    recomputeAncestors(item->child(0));
}

void ScopePage::rebuildVariableList()
{
    const QSignalBlocker blocker(m_variableTree);
    m_variableTree->clear();
    m_nodeByPath.clear();

    const QString needle = m_filterEdit->text().trimmed();
    m_visibleRoots = m_symbols.filteredRoots(needle);

    // Regroup for display. Grouping is done here rather than in the model because
    // the filter changes which variables are visible, but the model itself must stay
    // a plain forest (sampling and the frame layout work off it).
    {
        QMap<QString, FileGroup> byFile;
        std::function<void(const VariableNode &)> add = [&](const VariableNode &node) {
            const QString key = node.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                          : node.sourceFile;
            FileGroup &group = byFile[key];
            group.file = key;
            group.variables.append(node);
            group.nodeCount += node.totalNodeCount();
        };
        for (const VariableNode &root : m_visibleRoots)
            add(root);
        m_groups.clear();
        m_groups.reserve(byFile.size());
        FileGroup unknown;
        for (auto it = byFile.constBegin(); it != byFile.constEnd(); ++it) {
            if (it.key() == QStringLiteral("(未知文件)"))
                unknown = it.value();
            else
                m_groups.append(it.value());
        }
        if (unknown.nodeCount > 0)
            m_groups.append(unknown); // always last
    }

    // Index every visible node by path so a lazy expansion can find its subtree.
    struct Index
    {
        static void run(const VariableNode &node, const QString &prefix,
                        QHash<QString, const VariableNode *> *out)
        {
            const QString path = childPath(prefix, node.name);
            out->insert(path, &node);
            for (const VariableNode &child : node.children)
                run(child, path, out);
        }
    };
    for (const VariableNode &root : m_visibleRoots)
        Index::run(root, QString(), &m_nodeByPath);

    // Functions with locals, grouped by their source file. These are NOT in the model
    // forest (that is globals/statics only), which is exactly why a local such as
    // `match_indx` was unfindable: it is function-scope data.
    m_functionsByFile.clear();
    for (const FunctionScope &scope : m_symbols.functions) {
        if (scope.locals.isEmpty())
            continue;
        const QString file = scope.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                        : scope.sourceFile;
        VariableNode functionNode;
        functionNode.name = scope.name;
        functionNode.typeName = QStringLiteral("函数");
        functionNode.kind = NodeKind::Function;
        functionNode.children = scope.locals;
        functionNode.sourceFile = scope.sourceFile;
        m_functionsByFile[file].append(functionNode);
    }

    // The row list is a union: files with variables, files with functions, and files
    // that only define types (headers). Leaving headers out made `Encoder` -- and
    // therefore `index_found_` -- unreachable from the tree.
    {
        QSet<QString> files;
        for (const FileGroup &group : m_groups)
            files.insert(group.file);
        for (auto it = m_functionsByFile.constBegin(); it != m_functionsByFile.constEnd();
             ++it) {
            files.insert(it.key());
        }
        // Types always contribute their file: `encoder.hpp` defines `Encoder`, and a
        // user file must never disappear just because the header holds no variables.
        // Whether the file is vendor code is decided by the system filter below.
        for (const TypeDecl &type : m_symbols.types) {
            files.insert(type.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                   : type.sourceFile);
        }
        m_allFiles = QStringList(files.constBegin(), files.constEnd());
        // Order: files that declare variables first, then files with functions, then
        // headers that only define types. Alphabetical order alone pushed libc++
        // internals like `__bit_reference` above `adc.c`, which is backwards for a
        // user looking for their own code.
        QSet<QString> withVariables;
        for (const FileGroup &group : m_groups) {
            if (!group.variables.isEmpty())
                withVariables.insert(group.file);
        }
        QSet<QString> withFunctions;
        for (auto it = m_functionsByFile.constBegin(); it != m_functionsByFile.constEnd();
             ++it) {
            if (!it.value().isEmpty())
                withFunctions.insert(it.key());
        }
        std::sort(m_allFiles.begin(), m_allFiles.end(),
                  [&](const QString &a, const QString &b) {
                      const int rankA = withVariables.contains(a) ? 0
                          : (withFunctions.contains(a) ? 1 : 2);
                      const int rankB = withVariables.contains(b) ? 0
                          : (withFunctions.contains(b) ? 1 : 2);
                      if (rankA != rankB)
                          return rankA < rankB;
                      return a.compare(b, Qt::CaseInsensitive) < 0;
                  });
        // "(未知文件)" always last so a real grouping is never buried.
        const QString unknown = QStringLiteral("(未知文件)");
        if (m_allFiles.removeAll(unknown) > 0)
            m_allFiles.append(unknown);
    }

    // Top level is one row per source file; the variables in it are created when the
    // file is opened. A flat 637-row list of names was unusable; file -> variable is
    // how the user knows where to look.
    // Chip-pack / compiler headers are hidden by default: they contain vendor tables
    // (CMSIS `Weights_*`, libc++ internals) that are not project state.
    if (!m_showSystemFiles) {
        QStringList userFiles;
        for (const QString &file : m_allFiles) {
            if (m_symbols.isSystemFile(file))
                continue;
            userFiles << file;
        }
        m_allFiles = userFiles;
    }

    for (const QString &file : m_allFiles) {
        // Find the variables group for this file (may be absent for a header).
        FileGroup empty;
        empty.file = file;
        const FileGroup *group = &empty;
        for (const FileGroup &candidate : m_groups) {
            if (candidate.file == file) {
                group = &candidate;
                break;
            }
        }
        m_fileRowGroups.append(*group);
        makeFileItem(m_fileRowGroups.last(), m_fileRowGroups.size() - 1);
    }

    // A filter that matched inside a file should reveal the path to the match,
    // otherwise the user sees collapsed rows and cannot tell the search worked.
    if (!needle.isEmpty()) {
        for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_variableTree->topLevelItem(i);
            item->setExpanded(true);
            populateChildren(item);
            // Show the matched variables themselves (aggregates stay collapsed so a
            // big struct does not explode the view).
            for (int j = 0; j < item->childCount(); ++j) {
                QTreeWidgetItem *child = item->child(j);
                if (child->data(0, Qt::UserRole + 2).toBool())
                    continue;
                child->setHidden(false);
            }
        }
    }
    Q_UNUSED(blocker);
}

void ScopePage::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    // Rebuilding a 637-root tree per keystroke stutters; wait for a pause.
    m_filterTimer.start();
}

void ScopePage::filterForBench(const QString &text)
{
    m_filterEdit->setText(text);
    rebuildVariableList();
}

void ScopePage::onFilterDebounced()
{
    rebuildVariableList();
}

void ScopePage::onItemExpanded(QTreeWidgetItem *item)
{
    populateChildren(item);
}

QSet<QString> ScopePage::selectedLeafNames() const
{
    // The set is maintained by onVariableToggled from the model, so it also holds
    // leaves whose widgets have never been created. That is what makes "tick a
    // 500-member struct without expanding it" work.
    return m_selectedLeaves;
}

void ScopePage::applySelectionToItem(QTreeWidgetItem *item, const VariableNode &node,
                                    const QString &path)
{
    if (!item)
        return;
    if (node.children.isEmpty()) {
        if (node.samplable()) {
            item->setCheckState(0, m_selectedLeaves.contains(path) ? Qt::Checked
                                                                  : Qt::Unchecked);
        }
        return;
    }

    // Children were created in model order and skip the placeholder, so a simple
    // index walk keeps widget and model aligned.
    int modelIndex = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (child->data(0, kPlaceholderRole).toBool())
            continue;
        if (modelIndex >= node.children.size())
            break;
        applySelectionToItem(child, node.children.at(modelIndex),
                             child->data(0, Qt::UserRole).toString());
        ++modelIndex;
    }

    // An aggregate's own state is derived from the children it has materialised.
    int checked = 0;
    int unchecked = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        const QTreeWidgetItem *child = item->child(i);
        if (!(child->flags() & Qt::ItemIsUserCheckable))
            continue;
        if (child->checkState(0) == Qt::Checked)
            ++checked;
        else if (child->checkState(0) == Qt::Unchecked)
            ++unchecked;
    }
    const int total = checked + unchecked;
    Qt::CheckState state = Qt::PartiallyChecked;
    if (total == 0)
        state = Qt::Unchecked;
    else if (checked == total)
        state = Qt::Checked;
    else if (unchecked == total)
        state = Qt::Unchecked;
    item->setCheckState(0, state);
}

void ScopePage::setSubtreeChecked(QTreeWidgetItem *item, Qt::CheckState state)
{
    if (!item)
        return;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (child->data(0, kPlaceholderRole).toBool())
            continue;
        if (child->flags() & Qt::ItemIsUserCheckable)
            child->setCheckState(0, state);
        setSubtreeChecked(child, state);
    }
}

void ScopePage::recomputeAncestors(QTreeWidgetItem *item)
{
    // A parent is Checked when every materialised child is Checked, Unchecked when
    // all are Unchecked, otherwise PartiallyChecked. Children that have not been
    // created yet (lazy tree) do not participate.
    for (QTreeWidgetItem *parent = item ? item->parent() : nullptr; parent;
         parent = parent->parent()) {
        if ((parent->flags() & Qt::ItemIsUserCheckable) == 0)
            continue;
        int checked = 0;
        int unchecked = 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            const QTreeWidgetItem *child = parent->child(i);
            if (!(child->flags() & Qt::ItemIsUserCheckable))
                continue; // the lazy placeholder
            const Qt::CheckState state = child->checkState(0);
            if (state == Qt::Checked)
                ++checked;
            else if (state == Qt::Unchecked)
                ++unchecked;
        }
        const int total = checked + unchecked;
        Qt::CheckState next = Qt::PartiallyChecked;
        if (total == 0)
            next = Qt::Unchecked;
        else if (checked == total)
            next = Qt::Checked;
        else if (unchecked == total)
            next = Qt::Unchecked;
        if (parent->checkState(0) != next) {
            const QSignalBlocker blocker(parent->treeWidget());
            parent->setCheckState(0, next);
        }
    }
}

void ScopePage::onVariableToggled(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || m_syncingChecks)
        return;

    // Propagate in both directions ourselves. Qt's auto-tristate only manages
    // children that already exist, which does not hold for a lazily built tree.
    const QScopedValueRollback<bool> guard(m_syncingChecks, true);

    const Qt::CheckState state = item->checkState(0);
    const QString path = item->data(0, Qt::UserRole).toString();
    const bool aggregate = item->data(0, Qt::UserRole + 2).toBool();

    if (aggregate && state != Qt::PartiallyChecked) {
        setSubtreeChecked(item, state);
        // Record the choice against every leaf of this subtree, including the ones
        // whose widgets do not exist yet -- otherwise ticking a collapsed struct
        // would sample nothing.
        QStringList leaves;
        const QVariant fileRole = item->data(0, kFileGroupRole);
        const int groupIndex = fileRole.isValid() ? fileRole.toInt() : -1;
        if (groupIndex >= 0 && groupIndex < m_groups.size()) {
            // A file row: every variable declared in that file.
            for (const VariableNode &variable : m_groups.at(groupIndex).variables) {
                collectLeafPathsAt(variable, childPath(QString(), variable.name), &leaves);
            }
        } else if (const VariableNode *node = m_nodeByPath.value(path, nullptr)) {
            collectLeafPathsAt(*node, path, &leaves);
        }
        for (const QString &leaf : leaves) {
            if (state == Qt::Checked)
                m_selectedLeaves.insert(leaf);
            else
                m_selectedLeaves.remove(leaf);
        }
    } else if (!aggregate) {
        if (state == Qt::Checked)
            m_selectedLeaves.insert(path);
        else
            m_selectedLeaves.remove(path);
    }
    recomputeAncestors(item);

    // A single tick on a big struct produces one itemChanged per descendant; fold
    // them into one frame-layout recomputation.
    if (!m_selectionTimer.isActive())
        m_selectionTimer.start();
}

QVector<WatchVariable> ScopePage::selectedVariables() const
{
    QVector<WatchVariable> chosen;
    for (const WatchVariable &variable : m_symbols.variables) {
        if (m_selectedLeaves.contains(variable.name))
            chosen.append(variable);
    }
    // Frame order must match the generated firmware snippet, so keep it stable and
    // sorted by address rather than by click order.
    std::sort(chosen.begin(), chosen.end(),
              [](const WatchVariable &a, const WatchVariable &b) { return a.address < b.address; });
    return chosen;
}

void ScopePage::refreshLocals()
{
    if (!m_probe || !m_symbols.isValid()) {
        emit statusMessage(QStringLiteral("先导入 .axf 并连接目标"));
        return;
    }
    if (m_symbols.functions.isEmpty()) {
        emit statusMessage(
            QStringLiteral("该符号文件里没有局部变量信息（编译时可能需要 -g / Debug Information）"));
        return;
    }
    // Registers are only meaningful while the core is stopped, and stopping a
    // running control loop is the user's call -- never done implicitly here.
    emit statusMessage(QStringLiteral("正在读取寄存器…"));
    m_probe->requestReadRegisters();
}

void ScopePage::onRegistersRead(const QVector<quint32> &registers, bool ok, const QString &error)
{
    if (!ok) {
        emit statusMessage(QStringLiteral("读取寄存器失败：%1（多数后端只在停机时可读）")
                               .arg(error));
        return;
    }
    // Cortex-M order from the GDB `g` packet: r0-r12, sp, lr, pc, xpsr.
    if (registers.size() < 16) {
        emit statusMessage(QStringLiteral("寄存器数量异常（%1 个）").arg(registers.size()));
        return;
    }
    applyLocals(registers.at(13), registers.at(15), registers);
}

void ScopePage::applyLocals(quint32 sp, quint32 pc, const QVector<quint32> &registers)
{
    m_localAddr.clear();
    m_resolvedFunction.clear();
    if (!m_symbols.isValid() || pc == 0) {
        rebuildVariableList();
        return;
    }
    const FunctionScope *scope = m_symbols.functionContaining(pc);
    if (!scope) {
        emit statusMessage(
            QStringLiteral("PC %1 不在任何带局部变量的函数内").arg(hexAddr(pc)));
        rebuildVariableList();
        return;
    }

    // The frame base is "register + offset". armclang gives register 13 with offset
    // 0; GCC points at the CFA, whose offset changes with the prologue, so it is
    // evaluated for the *current* PC rather than at the function entry.
    int frameRegister = -1;
    qint32 frameOffset = 0;
    const bool haveFrameBase = m_symbols.frameBaseAt(pc, &frameRegister, &frameOffset);
    const quint32 frameBase =
        static_cast<quint32>(static_cast<qint32>((frameRegister >= 0
                                                  && frameRegister < registers.size())
                                                     ? registers.at(frameRegister)
                                                     : sp)
                             + (haveFrameBase ? frameOffset : 0));

    // Resolve every local of this function. A location list is evaluated at the halt
    // PC (different ranges put the same variable in a register or on the stack), so
    // this cannot be done at import time.
    const QString funcPath = childPath(QString(), scope->name);
    int resolved = 0;
    for (const VariableNode &local : scope->locals) {
        const QString localPath = childPath(funcPath, local.name);
        quint32 address = 0;
        if (local.locListOffset != 0) {
            const DwarfReader::LocalLoc loc =
                DwarfReader::localAt(m_symbols.source, pc, local.locListOffset);
            if (!loc.usable)
                continue;
            if (loc.absolute)
                address = loc.address;
            else if (loc.inRegister && loc.reg >= 0 && loc.reg < registers.size())
                address = registers.at(loc.reg) + static_cast<quint32>(loc.offset);
            else
                address = static_cast<quint32>(static_cast<qint32>(frameBase) + loc.offset);
        } else if (local.stackRelative) {
            address = static_cast<quint32>(static_cast<qint32>(frameBase) + local.stackOffset);
        } else {
            continue;
        }
        if (address == 0)
            continue;
        m_localAddr.insert(localPath, address);
        ++resolved;
    }

    m_resolvedFunction = scope->name;
    rebuildVariableList();

    // Open the file and function rows so the locals are actually visible.
    for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *fileRow = m_variableTree->topLevelItem(i);
        if (!scope->sourceFile.isEmpty() && fileRow->text(0) != scope->sourceFile)
            continue;
        fileRow->setExpanded(true);
        populateChildren(fileRow);
        for (int j = 0; j < fileRow->childCount(); ++j) {
            QTreeWidgetItem *child = fileRow->child(j);
            if (child->text(0) == scope->name) {
                child->setExpanded(true);
                break;
            }
        }
        if (!scope->sourceFile.isEmpty())
            break;
    }

    emit statusMessage(QStringLiteral("局部变量：%1 内 %2/%3 个地址已解析（SP=%4，PC=%5）")
                           .arg(scope->name)
                           .arg(resolved)
                           .arg(scope->locals.size())
                           .arg(hexAddr(sp), hexAddr(pc)));
}

void ScopePage::onSampleSettingsChanged()
{
    const QVector<WatchVariable> chosen = selectedVariables();
    const bool running = m_collector.isRunning();
    m_collector.setVariables(chosen);
    if (m_autoScale->isChecked())
        m_plot->setAutoScale(true);
    m_plot->setTimeWindow(m_windowCombo->currentData().toDouble());
    m_plot->setHistory(m_collector.history());
    if (running && !chosen.isEmpty()) {
        // Restart with the new layout so the frame size and the firmware snippet
        // stay in agreement.
        m_collector.start();
    }
    refreshState();
}

void ScopePage::onStartStop()
{
    if (m_collector.isRunning()) {
        m_collector.stop();
        refreshState();
        return;
    }
    if (!m_attached) {
        emit statusMessage(QStringLiteral("尚未连接目标"));
        return;
    }
    if (!m_rtt.valid()) {
        emit statusMessage(QStringLiteral("目标没有 RTT 控制块：固件需要先链接 SEGGER RTT"));
        return;
    }
    if (selectedVariables().isEmpty()) {
        emit statusMessage(QStringLiteral("请先勾选要绘制的变量"));
        return;
    }
    m_collector.clearHistory();
    m_plot->setHistory(m_collector.history());
    m_collector.start();
    refreshState();
}

void ScopePage::updateControls()
{
    const bool running = m_collector.isRunning();
    m_startButton->setText(running ? QStringLiteral("停止") : QStringLiteral("开始"));
    m_startButton->setEnabled(!selectedVariables().isEmpty() || running);
    m_codeButton->setEnabled(!selectedVariables().isEmpty());
    if (m_localsButton)
        m_localsButton->setEnabled(m_attached && m_symbols.isValid()
                                   && !m_symbols.functions.isEmpty());
}

void ScopePage::refreshState()
{
    updateControls();

    QStringList lines;
    if (!m_symbols.isValid()) {
        lines << QStringLiteral("① 导入 Keil 生成的 .axf（Debug Information 必须打开）");
    } else {
        lines << QStringLiteral("① 已导入 %1：%2 个变量")
                     .arg(QFileInfo(m_symbols.sourcePath).fileName())
                     .arg(m_symbols.variables.size());
    }
    if (!m_attached) {
        lines << QStringLiteral("② 连接目标后会自动定位 RTT 控制块");
    } else if (m_rtt.valid()) {
        lines << QStringLiteral("② RTT 控制块 @ %1，通道 0 缓冲 %2 字节")
                     .arg(hexAddr(m_rtt.address))
                     .arg(m_rtt.upBuffers.first().size);
    } else {
        lines << QStringLiteral("② 目标没有 RTT：固件需链接 SEGGER RTT 并至少写一次数据");
    }

    const QVector<WatchVariable> chosen = selectedVariables();
    if (chosen.isEmpty()) {
        lines << QStringLiteral("③ 勾选变量后点「开始」，再用「复制固件端代码」把发送代码贴进固件");
    } else {
        lines << QStringLiteral("③ 已选 %1 个变量，帧长 %2 字节（含 2 字节同步头）")
                     .arg(chosen.size())
                     .arg(m_collector.frameSize());
    }
    m_stateLabel->setText(lines.join(QLatin1Char('\n')));
}

void ScopePage::onSamplesAppended(int count)
{
    Q_UNUSED(count);
    // The samples are already in the collector's history; repaint only.
    m_plot->refresh();
}

void ScopePage::onStatistics(quint64 frames, quint64 droppedBytes, double fps)
{
    m_statsLabel->setText(QStringLiteral("%1 帧 · %2 帧/秒 · 丢弃 %3 字节")
                              .arg(frames)
                              .arg(fps, 0, 'f', 0)
                              .arg(droppedBytes));
}

void ScopePage::onCollectorFailed(const QString &message)
{
    m_stateLabel->setText(message);
    emit statusMessage(message);
    refreshState();
}

QString ScopePage::firmwareSnippet() const
{
    const QVector<WatchVariable> chosen = selectedVariables();
    if (chosen.isEmpty())
        return {};

    int payload = 0;
    for (const WatchVariable &v : chosen)
        payload += v.span();

    QString code;
    code += QStringLiteral("/* ===== 由 MCU Debugger 生成，请勿手工修改 =====\n"
                           " * 采样帧：2 字节同步头 0xA5 0x5A + %1 字节数据（共 %2 字节）\n"
                           " * 上位机按下面的顺序解析，改变勾选后需要重新生成并重新编译。\n"
                           " */\n")
                .arg(payload)
                .arg(payload + 2);
    code += QStringLiteral("#include <string.h>\n#include \"SEGGER_RTT.h\"\n\n");
    code += QStringLiteral("typedef struct {\n"
                           "    unsigned char sync[2];\n"
                           "    unsigned char data[%1];\n"
                           "} MrdScopeFrame;\n\n")
                .arg(payload);
    code += QStringLiteral("void MrdScope_Send(void)\n{\n"
                           "    MrdScopeFrame f;\n"
                           "    f.sync[0] = 0xA5;\n"
                           "    f.sync[1] = 0x5A;\n");
    int offset = 0;
    for (const WatchVariable &v : chosen) {
        code += QStringLiteral(
                    "    memcpy(f.data + %1, (const void *)%2u, %3);   /* %4 : %5 */\n")
                    .arg(offset)
                    .arg(hexAddr(v.address))
                    .arg(v.span())
                    .arg(v.name)
                    .arg(v.typeLabel);
        offset += v.span();
    }
    code += QStringLiteral("    SEGGER_RTT_Write(0, &f, sizeof(f));\n}\n\n");
    code += QStringLiteral("/* 在定时器中断或主循环里按需要调用，例如每 1ms 一次：\n"
                           " *     MrdScope_Send();\n"
                           " */\n");
    return code;
}

void ScopePage::onCopyFirmwareCode()
{
    const QString code = firmwareSnippet();
    if (code.isEmpty()) {
        emit statusMessage(QStringLiteral("请先勾选变量"));
        return;
    }
    QApplication::clipboard()->setText(code);
    emit statusMessage(QStringLiteral("固件端代码已复制到剪贴板（%1 字节的发送帧）")
                           .arg(m_collector.frameSize()));
    Logger::instance()->info(QStringLiteral("固件端采样代码已复制，帧长 %1 字节")
                                 .arg(m_collector.frameSize()));
}

} // namespace mrd
