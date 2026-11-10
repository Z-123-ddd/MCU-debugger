#include "ui/PeripheralTree.h"

#include "core/HexUtil.h"

#include <QFont>
#include <QHeaderView>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace mrd {

namespace {
constexpr int kColumnName = 0;
constexpr int kColumnBase = 1;
} // namespace

PeripheralTree::PeripheralTree(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("过滤外设（如 TIM / GPIO）"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);
    connect(m_filter, &QLineEdit::textChanged, this, &PeripheralTree::onFilterChanged);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("外设"), QStringLiteral("基地址")});
    m_tree->header()->setSectionResizeMode(kColumnName, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kColumnBase, QHeaderView::ResizeToContents);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    layout->addWidget(m_tree, 1);

    connect(m_tree, &QTreeWidget::itemClicked, this, &PeripheralTree::onItemClicked);
    connect(m_tree, &QTreeWidget::itemChanged, this, &PeripheralTree::onItemChanged);
}

void PeripheralTree::setDevice(const SvdDevice &device)
{
    m_groups.clear();
    for (const auto &peripheral : device.peripherals)
        m_groups[peripheral.groupLabel()].append(&peripheral);
    m_selected.clear();
    rebuild();
}

void PeripheralTree::clear()
{
    m_groups.clear();
    m_selected.clear();
    m_tree->clear();
}

void PeripheralTree::rebuild()
{
    m_updating = true;
    m_tree->clear();

    const QString filter = m_filter->text().trimmed();
    QFont bold = font();
    bold.setBold(true);

    for (auto it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) {
        QVector<const SvdPeripheral *> visible;
        for (const auto *peripheral : it.value()) {
            if (filter.isEmpty() || peripheral->name.contains(filter, Qt::CaseInsensitive))
                visible.append(peripheral);
        }
        if (visible.isEmpty())
            continue;

        auto *groupItem = new QTreeWidgetItem(m_tree);
        groupItem->setText(kColumnName, it.key());
        groupItem->setFont(kColumnName, bold);
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setExpanded(!filter.isEmpty() || it.value().size() <= 4);

        for (const auto *peripheral : visible) {
            auto *item = new QTreeWidgetItem(groupItem);
            item->setText(kColumnName, peripheral->name);
            item->setText(kColumnBase, hexAddr(peripheral->baseAddress));
            item->setData(kColumnName, Qt::UserRole, peripheral->name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(kColumnName, m_pinned.contains(peripheral->name, Qt::CaseInsensitive)
                                                 ? Qt::Checked
                                                 : Qt::Unchecked);
            QString tip = peripheral->description;
            if (!tip.isEmpty())
                tip += QLatin1Char('\n');
            tip += QStringLiteral("寄存器 %1 个").arg(peripheral->registers.size());
            item->setToolTip(kColumnName, tip);
        }
    }
    m_updating = false;
}

void PeripheralTree::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    rebuild();
}

void PeripheralTree::selectPeripheral(const QString &name)
{
    m_tree->setCurrentItem(nullptr);
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem *item = group->child(c);
            if (item->data(kColumnName, Qt::UserRole).toString().compare(name, Qt::CaseInsensitive) == 0) {
                m_tree->setCurrentItem(item);
                m_tree->scrollToItem(item);
                m_selected = name;
                return;
            }
        }
    }
}

void PeripheralTree::onItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item)
        return;
    const QString name = item->data(kColumnName, Qt::UserRole).toString();
    if (name.isEmpty())
        return;
    m_selected = name;
    emit peripheralSelected(name);
}

void PeripheralTree::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating || column != kColumnName || !item)
        return;
    const QString name = item->data(kColumnName, Qt::UserRole).toString();
    if (name.isEmpty())
        return;
    const bool checked = item->checkState(kColumnName) == Qt::Checked;
    const bool already = m_pinned.contains(name, Qt::CaseInsensitive);
    if (checked && !already)
        m_pinned << name;
    else if (!checked && already)
        m_pinned.removeAll(name);
    else
        return;
    emit pinnedChanged(m_pinned);
}

QStringList PeripheralTree::pinnedPeripherals() const
{
    return m_pinned;
}

void PeripheralTree::setPinnedPeripherals(const QStringList &names)
{
    m_pinned = names;
    rebuild();
}

} // namespace mrd
