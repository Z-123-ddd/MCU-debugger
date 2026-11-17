#pragma once

#include "core/HssSampler.h"
#include "core/SnapshotReader.h"
#include "core/SymbolFile.h"

#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QFileSystemWatcher;
class QTreeWidget;
class QTreeWidgetItem;

namespace mrd {

class GlPlotWidget;
class ProbeController;
class SampleHistory;
class ValuePoller;

/// Variable picker plus the two ways to read target memory.
///
/// Everything happens in-process: the ELF is parsed here to make thousands of
/// variables navigable, a snapshot reads memory on demand, and HSS background
/// sampling streams live curves. No external visualizer is launched and no project
/// file is written, so the tool has no second program to keep in sync.
class ScopePage : public QWidget
{
    Q_OBJECT

public:
    explicit ScopePage(QWidget *parent = nullptr);

    void setProbe(ProbeController *probe);

    /// Load an ELF and populate the variable list. Returns false on failure.
    bool importSymbolFile(const QString &path, QString *error = nullptr);
    bool hasSymbols() const { return m_symbols.isValid(); }
    QString symbolPath() const { return m_symbols.sourcePath; }

    /// The variable selector (filter + tree), owned by this page but shown by the main
    /// window in the left dock next to the peripheral tree.
    QWidget *variableSelector() const { return m_selectorBox; }

    /// Test hook: the tree itself (it lives inside the selector dock, not the page).
    QTreeWidget *variableTree() const { return m_variableTree; }
    /// Test hook: materialise one row's children the way an expand click does.
    void expandForTest(QTreeWidgetItem *item) { populateChildren(item); }
    /// Test hook: flush the coalescing timer so a tick is applied immediately.
    void settleSelectionForTest() { refreshState(); }
    /// Test hook: how many variables the current ticks would export.
    int testSelectedCount() const;
    /// Test hook: show a fabricated capture through the real table/plot path, so the
    /// self-test checks the widgets rather than a private helper.
    void showSnapshotForTest(const SnapshotReader::Snapshot &snapshot);
    /// Test hook: symbol file watching state, so the auto-reload can be checked.
    bool isWatchingForTest(const QString &path) const;
    /// Test hook: how many variables the current symbol table holds.
    int variableCountForTest() const { return m_symbols.variables.size(); }
    /// Test hook: the value poller, so the 值 column can be checked without a probe.
    ValuePoller *pollerForTest() const { return m_poller; }
    /// Test hook: find a row by its label, at any depth.
    QTreeWidgetItem *findRowForTest(const QString &label) const;
    /// Test hook: the two views, to assert what they ended up displaying.
    GlPlotWidget *plotForTest() const { return m_plot; }

    /// Apply a filter without going through the debounce (benchmarks/tests).
    void filterForBench(const QString &text);

signals:
    void statusMessage(const QString &text);
    /// A symbol file was loaded: the selector has content worth showing.
    void symbolsImported(const mrd::SymbolTable &symbols);

private slots:
    void onBrowse();
    void onFilterChanged(const QString &text);
    void onFilterDebounced();
    void onItemExpanded(QTreeWidgetItem *item);
    void onVariableToggled(QTreeWidgetItem *item, int column);
    /// Read the ticked variables once (arrays) or repeatedly (scalars) and show them.
    /// Selection settled (debounced): may need to rebuild the live session.
    void onSelectionSettled();
    /// Pause/resume the live capture. Paused keeps the drawn data and the zoom.
    void onTogglePause();
    /// Fresh numbers arrived: repaint just those cells.
    void onValuesUpdated(const QStringList &keys);
    /// The tree scrolled or expanded: tell the poller what is now on screen.
    void updateVisibleValueTargets();
    /// Drop collected samples and empty the chart.
    void onClearChart();

public:
    /// Write the current history as CSV (index, µs, ms, one column per variable).
    /// Separate from the slot so the self-test can verify the exact bytes.
    static bool writeHistoryCsv(const SampleHistory &history, const QString &path,
                                QString *error);
    void onCaptureSnapshot();
    void onSnapshotFinished(const mrd::SnapshotReader::Snapshot &snapshot);
    /// Start/stop HSS background sampling: live curves while the target runs.
    void onToggleLiveCapture();
    /// Timer tick: drain the probe buffer into the plot.
    void onLiveTick();
    /// The worker finished a drain (history already updated inside the sampler).
    void onLiveDrained(const mrd::HssSampler::Result &result);
    /// Time-domain / spectrum toggle for the live view.
    void onToggleFft();
    /// Write the visible history to a CSV next to the symbol file.
    void onExportCsv();
    /// The .axf changed on disk: debounce, then re-import it.
    void onSymbolFileChanged();
    /// Re-import and re-resolve every address the selection depends on.
    void reloadSymbols();

private:
    void rebuildVariableList();
    void populateChildren(QTreeWidgetItem *item);
    QTreeWidgetItem *makeItem(const VariableNode &node, const QString &path,
                              QTreeWidgetItem *parent);
    /// Top-level row for one source file; its variables are created on expansion.
    QTreeWidgetItem *makeFileItem(const FileGroup &group, int groupIndex);
    /// Create the rows for one type member, recursing into aggregates so that every
    /// selectable row is a single number.
    void buildMemberRows(QTreeWidgetItem *parent, const TypeMember &member, int depth,
                         quint32 baseAddress, bool hasAddress, const QString &instanceName);
    /// Restore one row's checkbox state from m_selectedLeaves.
    void applySelectionToItem(QTreeWidgetItem *item, const VariableNode &node,
                              const QString &path);
    /// Set \a state on every checkbox below \a item.
    static void setSubtreeChecked(QTreeWidgetItem *item, Qt::CheckState state);
    /// Recompute tri-state from the children, walking up to the root.
    static void recomputeAncestors(QTreeWidgetItem *item);

    QVector<WatchVariable> selectedVariables() const;
    /// Dotted names of every ticked leaf.
    QSet<QString> selectedLeafNames() const;
    void updateControls();
    void refreshState();
    /// Fill the plot from a capture (one curve per variable, index on X).
    void rebuildPlot(const SnapshotReader::Snapshot &snapshot);
    /// Which view the user is looking at, so a fresh capture refreshes both.
    void refreshViews();
    /// Live HSS status line (sample rate, per-channel min/max/average).
    void refreshLiveStatus();
    /// Register a row's current-value key so the poller can fill its 值 cell.
    void registerValueKey(QTreeWidgetItem *item, const QString &key,
                          const WatchVariable &variable);
    /// Create one read-only row per array element, plus a summary row when the array is
    /// longer than the display cap. Used both for tree nodes and for search results.
    void appendArrayElementRows(QTreeWidgetItem *item, const QString &path,
                                const QString &typeName, ScalarKind scalar, quint32 address,
                                int elementSize, int count);
    /// Paint one row's 值 cell from the poller's latest numbers.
    void applyValueToItem(QTreeWidgetItem *item);
    /// Rows under \a item that are currently visible in the viewport.
    void collectVisibleKeys(QTreeWidgetItem *item, int *budget, QVector<WatchVariable> *out);
    /// Every address the poller may refresh in the background.
    QVector<WatchVariable> allValueTargets() const;

    /// Fill \a project from the current selection. Fails when nothing is ticked or no
    /// symbol file is loaded.

    ProbeController *m_probe = nullptr;
    SymbolTable m_symbols;
    /// The forest currently shown (a filtered copy of m_symbols.roots).
    QVector<VariableNode> m_visibleRoots;
    /// path -> node, so a lazy expansion can find its subtree without a search.
    QHash<QString, const VariableNode *> m_nodeByPath;
    /// Files shown in the tree, in display order.
    QVector<FileGroup> m_groups;
    /// The FileGroup backing each top-level row, in row order.
    QVector<FileGroup> m_fileRowGroups;
    /// Functions with locals, per source file.
    QMap<QString, QVector<VariableNode>> m_functionsByFile;
    /// Every file worth a row: union of files with variables, functions or types.
    QStringList m_allFiles;
    /// Members that resolved to a real address through a known instance, keyed by the
    /// name used in the tree. Without this the checkboxes on those rows did nothing.
    QHash<QString, WatchVariable> m_memberVariables;
    /// Type name -> the (path, address) of a global instance of it.
    QHash<QString, QPair<QString, quint32>> m_typeInstances;
    /// Type name -> byte size of that known instance.
    QHash<QString, int> m_typeInstanceSizes;
    /// Ticked leaf paths -- the source of truth for what gets exported.
    QSet<QString> m_selectedLeaves;
    /// Active search term (needed when children are created lazily).
    QString m_filterNeedle;
    /// Show chip-pack / compiler-header files.
    bool m_showSystemFiles = false;
    /// Guards against re-entering the checkbox handlers while we propagate.
    bool m_syncingChecks = false;

    /// Coalesces the burst of itemChanged signals an auto-tristate parent produces.
    QTimer m_selectionTimer;
    /// Debounces the filter box.
    QTimer m_filterTimer;

    QWidget *m_selectorBox = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QTreeWidget *m_variableTree = nullptr;
    QCheckBox *m_selectAll = nullptr;
    QCheckBox *m_showSystemCheck = nullptr;

    /// Local capture: read memory on demand, no continuous sampling.
    SnapshotReader *m_reader = nullptr;
    /// Last finished capture, so switching tabs does not re-read the target.
    SnapshotReader::Snapshot m_snapshot;
    /// Backing store for the plot; owned here so its lifetime covers the widget.
    SampleHistory *m_history = nullptr;

    /// Fills the 值 column: visible rows at 200 ms, everything else slowly.
    ValuePoller *m_poller = nullptr;
    /// Value key -> how to decode it. Separate from m_memberVariables so read-only rows
    /// (array elements) never leak into the selection.
    QHash<QString, WatchVariable> m_valueVariables;
    /// Watches the imported .axf so a rebuild is picked up without a re-import.
    QFileSystemWatcher *m_symbolWatcher = nullptr;
    QTimer m_reloadTimer;
    /// Addresses of the ticked leaves before a reload, to report what moved.
    QHash<QString, quint32> m_addressesBeforeReload;
    /// Reload attempts after a parse failure (the linker may still be writing).
    int m_reloadRetries = 0;

    QPushButton *m_captureButton = nullptr;
    QPushButton *m_liveButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    /// True while a started capture is paused: sampling stopped, chart kept as-is.
    bool m_paused = false;
    /// Rate limit for the status-bar readout (the chart page shows no live text line).
    qint64 m_lastStatusMs = 0;
    int m_lastClippedSeen = 0;
    /// Guards against repeating the "capture stopped" line.
    int m_reportedStopSamples = -1;
    QPushButton *m_fftButton = nullptr;
    QPushButton *m_csvButton = nullptr;
    QSpinBox *m_periodSpin = nullptr;
    HssSampler *m_sampler = nullptr;
    /// Consecutive catch-up drains without a repaint, to bound the loop.
    int m_catchUpRounds = 0;
    QTimer m_liveTimer;
    QSpinBox *m_roundsSpin = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QPushButton *m_viewToggle = nullptr;
    GlPlotWidget *m_plot = nullptr;
};

} // namespace mrd
