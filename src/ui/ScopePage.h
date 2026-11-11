#pragma once

#include "core/SampleCollector.h"
#include "core/SymbolFile.h"

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
    QString symbolPath() const { return m_symbols.sourcePath; }

signals:
    void statusMessage(const QString &text);

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

private:
    void rebuildVariableList();
    QVector<WatchVariable> selectedVariables() const;
    void updateControls();
    void refreshState();
    QString firmwareSnippet() const;

    ProbeController *m_probe = nullptr;
    SymbolTable m_symbols;
    RttControlBlock m_rtt;
    bool m_attached = false;

    SampleCollector m_collector;

    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QTreeWidget *m_variableTree = nullptr;
    QCheckBox *m_selectAll = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_codeButton = nullptr;
    QComboBox *m_windowCombo = nullptr;
    QCheckBox *m_autoScale = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    PlotWidget *m_plot = nullptr;
};

} // namespace mrd
