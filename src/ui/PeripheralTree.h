#pragma once

#include "core/SvdTypes.h"

#include <QStringList>
#include <QWidget>

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

    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QMap<QString, QVector<const SvdPeripheral *>> m_groups;
    QStringList m_pinned;
    QString m_selected;
    bool m_updating = false;
};

} // namespace mrd
