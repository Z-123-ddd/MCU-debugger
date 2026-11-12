#pragma once

#include "core/SampleCollector.h"
#include "core/SymbolFile.h"

#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace mrd {

class PlotWidget;
class ProbeController;

/// Tab that turns a Keil `.axf` into a live waveform.
///
/// Flow: import the ELF -> every statically allocated variable appears with its
/// type and address -> tick the ones to plot -> the firmware sends them over RTT in
/// the generated frame -> the plot updates from the collector's ring buffer.
///
/// Nothing here polls for values on a timer. The collector issues its next read as
/// soon as the previous one returns, and this page only repaints when a batch has
/// already landed, so a heavy repaint costs no samples.
class ScopePage : public QWidget
{
    Q_OBJECT

public:
    explicit ScopePage(QWidget *parent = nullptr);

    void setProbe(ProbeController *probe);

    /// Called after a successful connect: locate the RTT control block using the
    /// ELF's `_SEGGER_RTT` symbol, or by scanning RAM. \a ramStart/\a ramSize come
    /// from the identification result.
    void attachTarget(quint32 ramStart, quint32 ramSize);
    void detachTarget();

    /// Load an ELF and populate the variable list. Returns false on failure.
    bool importSymbolFile(const QString &path, QString *error = nullptr);
    bool hasSymbols() const { return m_symbols.isValid(); }

    /// The variable selector (filter + tree), owned by this page but shown by the
    /// main window in a dock so it can float and does not squeeze the plot.
    QWidget *variableSelector() const { return m_selectorBox; }

    /// Test hook: the tree itself (it lives inside the selector dock, not the page).
    QTreeWidget *variableTree() const { return m_variableTree; }
    /// Test hook: materialise one row's children the way an expand click does.
    void expandForTest(QTreeWidgetItem *item) { populateChildren(item); }
    QString symbolPath() const { return m_symbols.sourcePath; }

    /// Test/benchmark hook: apply a filter without going through the debounce.
    void filterForBench(const QString &text);

    /// Locals of the function the core is halted in.
    ///
    /// Requires registers, so it only works while halted -- and the tool never
    /// halts on its own: pausing a running control loop is the user's decision.
    void refreshLocals();
    /// Show locals for a known SP/PC pair (used by refreshLocals and the tests).
    void applyLocals(quint32 sp, quint32 pc, const QVector<quint32> &registers);

signals:
    void statusMessage(const QString &text);
    /// A symbol file was loaded: the selector has content worth showing.
    void symbolsImported();
    /// Asking for locals: keeps the QAction/shortcut wiring free of direct calls.
    void localsRefreshRequested();

private slots:
    void onBrowse();
    void onFilterChanged(const QString &text);
    void onVariableToggled(QTreeWidgetItem *item, int column);
    void onStartStop();
    void onSampleSettingsChanged();
    void onCopyFirmwareCode();
    void onSamplesAppended(int count);
    void onStatistics(quint64 frames, quint64 droppedBytes, double fps);
    void onCollectorFailed(const QString &message);
    void onRegistersRead(const QVector<quint32> &registers, bool ok, const QString &error);
    /// Build a collapsed node's children the first time it is opened. Creating all
    /// 5692 nodes up front is what made the window freeze.
    void onItemExpanded(QTreeWidgetItem *item);
    void onFilterDebounced();

private:
    void rebuildVariableList();
    /// Populate  item's children from the model (idempotent).
    void populateChildren(QTreeWidgetItem *item);
    /// Create one item for  node, adding a placeholder child so Qt draws the
    /// expand arrow without us building the subtree yet.
    QTreeWidgetItem *makeItem(const VariableNode &node, const QString &path,
                              QTreeWidgetItem *parent);
    /// Top-level row for one source file; its variables are created on expansion.
    QTreeWidgetItem *makeFileItem(const FileGroup &group, int groupIndex);
    /// Set  state on every checkbox below  item.
    static void setSubtreeChecked(QTreeWidgetItem *item, Qt::CheckState state);
    /// Recompute tri-state from the children, walking up to the root.
    static void recomputeAncestors(QTreeWidgetItem *item);
    /// Reflect one model node's selection into its item (and its children's states).
    void applySelectionToItem(QTreeWidgetItem *item, const VariableNode &node,
                              const QString &path);
    QVector<WatchVariable> selectedVariables() const;
    /// Ticked leaf paths (m_selectedLeaves, filtered to what exists).
    QSet<QString> selectedLeafNames() const;
    void updateControls();
    void refreshState();
    QString firmwareSnippet() const;

    ProbeController *m_probe = nullptr;
    SymbolTable m_symbols;
    /// The forest currently shown (a filtered copy of m_symbols.roots).
    QVector<VariableNode> m_visibleRoots;
    /// path -> node, so a lazy expansion can find its subtree without a search.
    QHash<QString, const VariableNode *> m_nodeByPath;
    /// Files shown in the tree, in display order. Top-level rows are files and their
    /// children are the variables declared in them -- the layout every debugger's
    /// watch window uses, and the only way 4000 names stay navigable.
    QVector<FileGroup> m_groups;
    /// Functions with locals, per source file. Shown under each file so a local like
    /// `match_indx` is findable at all -- it is not a global and never appears in the
    /// "globals/statics" list.
    QMap<QString, QVector<VariableNode>> m_functionsByFile;
    /// Every file worth a row: the union of files that declare a variable, define a
    /// function with locals, or define a type. Using only the variable files hid
    /// headers entirely -- `Encoder` is defined in one, so its members were
    /// unreachable from the tree.
    QStringList m_allFiles;
    /// The FileGroup backing each top-level row, in row order (created rows must not
    /// dangle, so they are copied here rather than pointing into m_groups).
    QVector<FileGroup> m_fileRowGroups;
    /// Resolved addresses of the halted function's locals, keyed by tree path.
    /// Filled by applyLocals; empty while the core runs (locals have no address then).
    QHash<QString, quint32> m_localAddr;
    /// Name of the function those addresses belong to.
    QString m_resolvedFunction;

    RttControlBlock m_rtt;
    bool m_attached = false;

    SampleCollector m_collector;
    /// Coalesces the burst of itemChanged signals an auto-tristate parent produces.
    QTimer m_selectionTimer;
    /// Debounces the filter box: rebuilding on every keystroke of a 637-root tree
    /// stutters visibly.
    QTimer m_filterTimer;
    /// Guards against re-entering the checkbox handlers while we propagate.
    bool m_syncingChecks = false;
    /// Ticked leaf paths -- the source of truth for what gets sampled.
    ///
    /// Widget check states are only a view of this set. With a lazily built tree the
    /// leaves of a big struct do not exist yet, so ticking the struct has to be
    /// recorded here; otherwise the selection would silently lose everything that
    /// was not expanded (and the generated frame would not match what the user saw).
    QSet<QString> m_selectedLeaves;

    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QWidget *m_selectorBox = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QTreeWidget *m_variableTree = nullptr;
    QCheckBox *m_selectAll = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_codeButton = nullptr;
    QPushButton *m_localsButton = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QCheckBox *m_autoScale = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    PlotWidget *m_plot = nullptr;
};

} // namespace mrd
