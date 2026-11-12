#include "ui/ScopePage.h"

#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeController.h"
#include "ui/PlotWidget.h"

#include <QApplication>
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
    auto *listGroup = new QGroupBox(QStringLiteral("可采样变量（全局 / 静态）"), this);
    auto *listLayout = new QVBoxLayout(listGroup);
    listLayout->setContentsMargins(6, 6, 6, 6);
    listLayout->setSpacing(4);

    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);
    m_filterEdit = new QLineEdit(listGroup);
    m_filterEdit->setPlaceholderText(QStringLiteral("按名字 / 类型 / 地址过滤，例如 g_adc 或 float"));
    filterRow->addWidget(m_filterEdit, 1);
    m_selectAll = new QCheckBox(QStringLiteral("全选"), listGroup);
    filterRow->addWidget(m_selectAll);
    listLayout->addLayout(filterRow);

    m_variableTree = new QTreeWidget(listGroup);
    m_variableTree->setColumnCount(4);
    m_variableTree->setHeaderLabels({QStringLiteral("变量"), QStringLiteral("类型"),
                                     QStringLiteral("地址"), QStringLiteral("大小")});
    m_variableTree->setRootIsDecorated(false);
    m_variableTree->setUniformRowHeights(true);
    m_variableTree->setAlternatingRowColors(true);
    m_variableTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_variableTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    listLayout->addWidget(m_variableTree, 1);

    root->addWidget(listGroup, 2);

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

void ScopePage::rebuildVariableList()
{
    m_variableTree->clear();
    const QString needle = m_filterEdit->text().trimmed();

    // The tree keeps the source shape: a struct expands into its members, an array
    // into its elements. Filtering prunes the branches instead of flattening them,
    // so the hierarchy the user is reading survives a search.
    const QVector<VariableNode> roots = m_symbols.filteredRoots(needle);
    const QSet<QString> selected = selectedLeafNames();

    // Building the whole tree can be thousands of items; suspend the checkbox
    // signals while doing it, or every item would trigger a re-evaluation.
    const QSignalBlocker blocker(m_variableTree);

    struct Builder
    {
        ScopePage *page = nullptr;
        QTreeWidget *tree = nullptr;
        const QSet<QString> *selected = nullptr;
        const QString *needle = nullptr;

        void build(const VariableNode &node, QTreeWidgetItem *parent, const QString &prefix)
        {
            // Same spelling as the flat path so a selection maps back to a
            // WatchVariable by name.
            QString path;
            if (prefix.isEmpty())
                path = node.name;
            else if (node.name.startsWith(QLatin1Char('[')))
                path = prefix + node.name;
            else
                path = prefix + QLatin1Char('.') + node.name;

            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            // Members and array elements read better without repeating the parent,
            // so only the local name is shown; the tooltip carries the full path.
            item->setText(0, node.name);
            item->setText(1, node.typeLabel());
            item->setText(2, node.hasAddress && node.address != 0
                                 ? hexAddr(node.address)
                                 : QString());
            item->setText(3, node.byteSize > 0 ? QStringLiteral("%1 B").arg(node.byteSize)
                                               : QString());

            QStringList tip;
            tip << path;
            tip << QStringLiteral("类型: %1（%2）").arg(node.typeLabel(), nodeKindName(node.kind));
            if (node.kind == NodeKind::Array)
                tip << QStringLiteral("元素数: %1，每元素 %2 字节")
                           .arg(node.elementCount)
                           .arg(node.elementSize);
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
            item->setToolTip(0, tip.join(QLatin1Char('\n')));

            item->setData(0, Qt::UserRole, path);
            item->setData(0, Qt::UserRole + 1, node.samplable());
            item->setData(0, Qt::UserRole + 2, node.isAggregate());

            if (node.isAggregate()) {
                // Ticking an aggregate ticks everything under it: Qt does the
                // propagation when the item is auto-tristate.
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable
                               | Qt::ItemIsAutoTristate);
            } else {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            }
            item->setCheckState(0, selected->contains(path) ? Qt::Checked : Qt::Unchecked);

            // Array elements and members start collapsed: an ODrive build has
            // thousands of nodes and expanding everything would freeze the view.
            item->setExpanded(false);

            for (const VariableNode &child : node.children)
                build(child, item, path);
        }
    } builder{this, m_variableTree, &selected, &needle};

    for (const VariableNode &root : roots)
        builder.build(root, nullptr, QString());

    // Recompute the aggregate states bottom-up. Qt's auto-tristate keeps them in
    // sync while the user clicks, but during construction the children are created
    // after the parent's own state was set -- so a restored selection would show
    // fully-unticked parents above ticked leaves.
    struct Sync
    {
        static void run(QTreeWidgetItem *item)
        {
            if (!item || item->childCount() == 0)
                return;
            for (int i = 0; i < item->childCount(); ++i)
                run(item->child(i));
            int checked = 0;
            int partial = 0;
            for (int i = 0; i < item->childCount(); ++i) {
                const Qt::CheckState state = item->child(i)->checkState(0);
                if (state == Qt::Checked)
                    ++checked;
                else if (state == Qt::PartiallyChecked)
                    ++partial;
            }
            // Only aggregates carry a checkbox of their own; a leaf's state was set
            // above and must not be touched here.
            if ((item->flags() & Qt::ItemIsAutoTristate) == 0)
                return;
            if (checked == item->childCount())
                item->setCheckState(0, Qt::Checked);
            else if (checked == 0 && partial == 0)
                item->setCheckState(0, Qt::Unchecked);
            else
                item->setCheckState(0, Qt::PartiallyChecked);
        }
    };
    for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i)
        Sync::run(m_variableTree->topLevelItem(i));

    // A filter that matched something inside a collapsed root should reveal it.
    if (!needle.isEmpty()) {
        for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i)
            m_variableTree->topLevelItem(i)->setExpanded(true);
    }
    Q_UNUSED(blocker);
}

void ScopePage::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    rebuildVariableList();
}

QSet<QString> ScopePage::selectedLeafNames() const
{
    // Every visible leaf that is ticked. Aggregates are auto-tristate, so a parent
    // tick has already propagated down to its leaves.
    QSet<QString> names;
    struct Walker
    {
        static void walk(const QTreeWidgetItem *item, QSet<QString> *out)
        {
            if (!item)
                return;
            const bool leaf = item->childCount() == 0;
            if (leaf && item->checkState(0) == Qt::Checked)
                out->insert(item->data(0, Qt::UserRole).toString());
            for (int i = 0; i < item->childCount(); ++i)
                walk(item->child(i), out);
        }
    };
    for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i)
        Walker::walk(m_variableTree->topLevelItem(i), &names);
    return names;
}

void ScopePage::onVariableToggled(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(item);
    Q_UNUSED(column);
    // Auto-tristate fires this once per changed child, so a "select all" on a big
    // struct would otherwise rebuild the frame layout hundreds of times. Coalesce.
    if (!m_selectionTimer.isActive())
        m_selectionTimer.start();
}

QVector<WatchVariable> ScopePage::selectedVariables() const
{
    const QSet<QString> names = selectedLeafNames();
    QVector<WatchVariable> chosen;
    for (const WatchVariable &variable : m_symbols.variables) {
        if (names.contains(variable.name))
            chosen.append(variable);
    }
    // Frame order must match the generated firmware snippet, so keep it stable and
    // sorted by address rather than by click order.
    std::sort(chosen.begin(), chosen.end(),
              [](const WatchVariable &a, const WatchVariable &b) { return a.address < b.address; });
    return chosen;
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
