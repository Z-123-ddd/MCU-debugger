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

    QString selectedPeripheral() const { return m_selected; }
    void selectPeripheral(const QString &name);

    /// Apply the device tree read back from the target. Peripherals the part does
    /// not implement are hidden (or greyed) instead of being listed as if present.
    void setStatuses(const QVector<PeripheralStatus> &statuses);
    bool hasStatuses() const { return !m_statuses.isEmpty(); }

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
    struct { int onChip = 0; int clocked = 0; int total = 0; } m_counts;
    Filter m_filterMode = Filter::All;

    QLineEdit *m_filter = nullptr;
    QComboBox *m_stateFilter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QMap<QString, QVector<const SvdPeripheral *>> m_groups;
    QStringList m_pinned;
    QString m_selected;
    bool m_updating = false;
};

} // namespace mrd
