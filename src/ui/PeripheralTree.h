#pragma once

#include "core/DeviceTreeScanner.h"
#include "core/SvdTypes.h"

#include <QStringList>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace mrd {

/// Left dock: peripherals grouped by `groupName` (CubeMX style), with a checkbox
/// to include a peripheral in the polling set.
class PeripheralTree : public QWidget
{
    Q_OBJECT

public:
    explicit PeripheralTree(QWidget *parent = nullptr);

    void setDevice(const SvdDevice &device);
    void clear();

    /// Park the tree until a target is connected: like the package view, the
    /// peripheral list is only meaningful once it reflects the real device.
    void showWaiting(const QString &message);
    bool isWaiting() const { return m_waiting; }

    QString selectedPeripheral() const { return m_selected; }
    void selectPeripheral(const QString &name);

    /// Apply the device tree read back from the target. Peripherals the part does
    /// not implement are hidden (or greyed) instead of being listed as if present.
    void setStatuses(const QVector<PeripheralStatus> &statuses);
    bool hasStatuses() const { return !m_statuses.isEmpty(); }

    /// Update the "live" column of one peripheral row (e.g. a timer's counter).
    ///
    /// Cheap enough to call every poll: it only touches the existing item, no
    /// rebuild. An empty  text clears the cell.
    void setLiveValue(const QString &peripheralName, const QString &text);
    /// Tooltip for the live cell (e.g. the register name and hex value).
    void setLiveToolTip(const QString &peripheralName, const QString &text);
    /// Clear every live cell (on disconnect / device change).
    void clearLiveValues();

    /// Peripheral names the user pinned (checkbox checked) - polled periodically.
    QStringList pinnedPeripherals() const;
    void setPinnedPeripherals(const QStringList &names);

signals:
    void peripheralSelected(const QString &name);
    void pinnedChanged(const QStringList &names);

private slots:
    void onFilterChanged(const QString &text);
    void onItemClicked(QTreeWidgetItem *item, int column);
    void onItemChanged(QTreeWidgetItem *item, int column);

private:
    void rebuild();
    enum class Filter { All, OnChip, Clocked };

    QHash<QString, PeripheralStatus> m_statuses;
    bool m_waiting = false;
    QString m_waitingMessage = QStringLiteral("连接目标后自动读取设备树");
    struct { int onChip = 0; int clocked = 0; int total = 0; } m_counts;
    // Defaults to "only what the target confirmed"; setStatuses() switches it
    // back to All when the read back produced no usable answers.
    Filter m_filterMode = Filter::OnChip;

    QLineEdit *m_filter = nullptr;
    QComboBox *m_stateFilter = nullptr;
    QTreeWidget *m_tree = nullptr;
    /// Row lookup so live values can be updated without rebuilding the tree.
    QHash<QString, QTreeWidgetItem *> m_rows;
    QMap<QString, QVector<const SvdPeripheral *>> m_groups;
    QStringList m_pinned;
    QString m_selected;
    bool m_updating = false;
};

} // namespace mrd
