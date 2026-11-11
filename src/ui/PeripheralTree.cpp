#include "ui/PeripheralTree.h"

#include "core/HexUtil.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QFont>
#include <QHeaderView>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace mrd {

namespace {
constexpr int kColumnName = 0;
constexpr int kColumnBase = 1;
constexpr int kColumnState = 2;
constexpr int kColumnLive = 3;
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

    // Filters let the user hide peripherals this part does not implement.
    m_stateFilter = new QComboBox(this);
    m_stateFilter->addItem(QStringLiteral("显示: 全部"), int(Filter::All));
    m_stateFilter->addItem(QStringLiteral("显示: 仅本芯片存在"), int(Filter::OnChip));
    m_stateFilter->addItem(QStringLiteral("显示: 仅时钟已开启"), int(Filter::Clocked));
    m_stateFilter->setToolTip(QStringLiteral("依据 J-Link 读回的时钟门控与总线访问结果过滤"));
    layout->addWidget(m_stateFilter);
    connect(m_stateFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_filterMode = Filter(m_stateFilter->itemData(index).toInt());
        rebuild();
    });

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({QStringLiteral("外设"), QStringLiteral("基地址"),
                             QStringLiteral("时钟"), QStringLiteral("实时")});
    m_tree->header()->setSectionResizeMode(kColumnLive, QHeaderView::ResizeToContents);
    m_tree->setToolTip(QStringLiteral("“实时”列显示外设的活数据，例如定时器 CNT 计数值；"
                                      "它随轮询刷新，可用来判断外设是否真的在跑"));
    m_tree->header()->setSectionResizeMode(kColumnName, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kColumnBase, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kColumnState, QHeaderView::ResizeToContents);
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
    m_waiting = false; // an SVD is loaded again; the caller decides when to show it
    rebuild();
}

void PeripheralTree::showWaiting(const QString &message)
{
    m_waiting = true;
    m_waitingMessage = message;
    m_selected.clear();
    m_statuses.clear(); // stale device-tree data must not outlive the connection
    m_rows.clear();     // live values are stale too
    rebuild();
}

void PeripheralTree::clear()
{
    m_groups.clear();
    m_selected.clear();
    m_statuses.clear();
    m_rows.clear();
    m_waiting = false;
    m_tree->clear();
}

void PeripheralTree::setLiveValue(const QString &peripheralName, const QString &text)
{
    const auto it = m_rows.constFind(peripheralName);
    if (it == m_rows.constEnd() || !it.value())
        return;
    QTreeWidgetItem *item = it.value();
    if (item->text(kColumnLive) == text)
        return; // avoid needless repaints on every poll
    item->setText(kColumnLive, text);
    if (!text.isEmpty())
        item->setForeground(kColumnLive, Theme::current().accent);
}

void PeripheralTree::setLiveToolTip(const QString &peripheralName, const QString &text)
{
    const auto it = m_rows.constFind(peripheralName);
    if (it == m_rows.constEnd() || !it.value())
        return;
    it.value()->setToolTip(kColumnLive, text);
}

void PeripheralTree::clearLiveValues()
{
    for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it) {
        if (it.value())
            it.value()->setText(kColumnLive, QString());
    }
}

void PeripheralTree::setStatuses(const QVector<PeripheralStatus> &statuses)
{
    m_statuses.clear();
    m_counts = {};
    for (const auto &status : statuses) {
        m_statuses.insert(status.name, status);
        ++m_counts.total;
        if (status.onChip())
            ++m_counts.onChip;
        if (status.clockKnown && status.clockEnabled)
            ++m_counts.clocked;
    }

    // The SVD describes the die, not this part: switch to "only what the target
    // confirmed" as soon as the read back produced usable answers. If the probe
    // proved nothing (RCC unreadable, target unresponsive) stay on "show all" so
    // the tree does not silently go blank.
    if (m_counts.onChip > 0) {
        m_filterMode = Filter::OnChip;
        if (m_stateFilter) {
            QSignalBlocker blocker(m_stateFilter);
            const int index = m_stateFilter->findData(int(Filter::OnChip));
            if (index >= 0)
                m_stateFilter->setCurrentIndex(index);
        }
    } else {
        m_filterMode = Filter::All;
        if (m_stateFilter) {
            QSignalBlocker blocker(m_stateFilter);
            const int index = m_stateFilter->findData(int(Filter::All));
            if (index >= 0)
                m_stateFilter->setCurrentIndex(index);
        }
    }
    rebuild();
}

void PeripheralTree::rebuild()
{
    m_updating = true;
    m_tree->clear();
    m_rows.clear();

    // Before a target is connected the list is parked, mirroring the package view:
    // showing the whole SVD family would imply this part really has all of it.
    if (m_waiting) {
        auto *hint = new QTreeWidgetItem(m_tree);
        hint->setText(kColumnName, m_waitingMessage);
        hint->setFlags(Qt::NoItemFlags);
        hint->setForeground(kColumnName, Theme::current().textDim);
        m_updating = false;
        return;
    }

    const QString filter = m_filter->text().trimmed();
    QFont bold = font();
    bold.setBold(true);

    const bool haveStatus = !m_statuses.isEmpty();
    const Palette palette = Theme::current();

    for (auto it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) {
        QVector<const SvdPeripheral *> visible;
        for (const auto *peripheral : it.value()) {
            if (!filter.isEmpty() && !peripheral->name.contains(filter, Qt::CaseInsensitive))
                continue;

            const auto statusIt = m_statuses.constFind(peripheral->name);
            const bool known = haveStatus && statusIt != m_statuses.constEnd();

            // Apply the state filter (only meaningful once the tree was read back).
            if (known) {
                const PeripheralStatus &status = statusIt.value();

                // Never list a peripheral this package brings no pin out for: a
                // 48-pin part must not advertise GPIOD..GPIOG just because the die
                // decodes their registers.
                if (status.unbonded())
                    continue;

                // Static vendor judgement: the SVD covers the die/family, so it
                // lists blocks this model lacks (SPI5 on a 48-pin N32H473CE).
                // In "show all" they still appear, marked and greyed, so the user
                // can see what was dropped and why.
                // Default view: hide what the vendor says this model lacks, and
                // also anything the live probe proved absent (bus rejected with
                // the clock running) -- both mean "not usable on this part".
                if (m_filterMode == Filter::OnChip
                    && (!status.exists || status.reachability == PeripheralStatus::Missing))
                    continue;

                if (m_filterMode == Filter::OnChip && !status.onChip())
                    continue;
                if (m_filterMode == Filter::Clocked && !(status.clockKnown && status.clockEnabled))
                    continue;
            } else if (m_filterMode != Filter::All) {
                // No device-tree data yet: do not hide anything.
            }
            visible.append(peripheral);
        }
        if (visible.isEmpty())
            continue;

        auto *groupItem = new QTreeWidgetItem(m_tree);
        groupItem->setText(kColumnName, it.key());
        if (haveStatus) {
            int onChip = 0;
            for (const auto *peripheral : visible) {
                const auto statusIt = m_statuses.constFind(peripheral->name);
                if (statusIt != m_statuses.constEnd() && statusIt->onChip())
                    ++onChip;
            }
            if (onChip != visible.size())
                groupItem->setText(kColumnName, QStringLiteral("%1  (%2/%3)")
                                                     .arg(it.key())
                                                     .arg(onChip)
                                                     .arg(visible.size()));
        }
        groupItem->setFont(kColumnName, bold);
        groupItem->setFlags(Qt::ItemIsEnabled);
        groupItem->setExpanded(!filter.isEmpty() || it.value().size() <= 4);

        for (const auto *peripheral : visible) {
            auto *item = new QTreeWidgetItem(groupItem);
            m_rows.insert(peripheral->name, item);
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

            const auto statusIt = m_statuses.constFind(peripheral->name);
            if (haveStatus && statusIt != m_statuses.constEnd()) {
                const PeripheralStatus &status = statusIt.value();

                // Clock column: 开 / 关 / — (no gate bit) / ? (register unreadable).
                QString clockText = QStringLiteral("—");
                if (status.clockKnown) {
                    clockText = status.clockEnabled ? QStringLiteral("开") : QStringLiteral("关");
                    tip += QStringLiteral("\n时钟门控: %1 = %2")
                               .arg(status.clockSource,
                                    status.clockEnabled ? QStringLiteral("已使能")
                                                        : QStringLiteral("未使能"));
                } else {
                    tip += QStringLiteral("\n时钟门控: SVD 未提供");
                }
                item->setText(kColumnState, clockText);

                if (!status.exists) {
                    item->setText(kColumnName,
                                  QStringLiteral("%1  (本型号不具备)").arg(peripheral->name));
                    item->setForeground(kColumnName, palette.textDim);
                    tip += QStringLiteral("\n静态判定: %1")
                               .arg(status.absenceReason.isEmpty()
                                        ? QStringLiteral("厂商能力清单显示本型号无此实例")
                                        : status.absenceReason);
                }

                switch (status.reachability) {
                case PeripheralStatus::Readable:
                    tip += QStringLiteral("\n访问: 正常 (%1 = %2)")
                               .arg(status.probeRegister, hex32(status.probeValue));
                    break;
                case PeripheralStatus::Missing:
                    item->setText(kColumnName, QStringLiteral("%1  (本芯片未实现)").arg(peripheral->name));
                    item->setForeground(kColumnName, palette.textDim);
                    tip += QStringLiteral("\n访问: 总线拒绝，且时钟已开 -> 该型号不具备此条目的外设");
                    break;
                case PeripheralStatus::Unreachable:
                    item->setText(kColumnName, QStringLiteral("%1  (无法访问)").arg(peripheral->name));
                    item->setForeground(kColumnName, palette.textDim);
                    tip += QStringLiteral("\n访问: 总线拒绝%s")
                               .arg(status.clockKnown && !status.clockEnabled
                                        ? QStringLiteral("（时钟未开，不代表不存在）")
                                        : QString());
                    break;
                }
                if (!status.clockKnown || !status.clockEnabled)
                    item->setForeground(kColumnState, palette.textDim);
            }

            item->setToolTip(kColumnName, tip);
            item->setToolTip(kColumnState, tip);
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
