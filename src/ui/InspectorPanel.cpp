#include "ui/InspectorPanel.h"

#include "core/HexUtil.h"
#include "ui/BitFieldStrip.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace mrd {

InspectorPanel::InspectorPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_title = new QLabel(QStringLiteral("未选择寄存器"), this);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    m_title->setFont(titleFont);
    // Long register labels ("FDCAN_NDAT1  (FDCAN_NDAT1)") must not dictate the
    // panel width, otherwise the dock can never be dragged narrow.
    m_title->setWordWrap(true);
    m_title->setMinimumWidth(0);
    layout->addWidget(m_title);

    m_addressLabel = new QLabel(this);
    m_addressLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    // Single-line labels like "地址 0x4002C000 大小 32 bit 复位 0x… 访问 读/写"
    // otherwise report a ~340 px minimum and a long value line ~680 px, which
    // pins the whole dock wide and keeps the bit strip from ever wrapping.
    m_addressLabel->setWordWrap(true);
    m_addressLabel->setMinimumWidth(0);
    layout->addWidget(m_addressLabel);

    m_strip = new BitFieldStrip(this);
    layout->addWidget(m_strip);
    connect(m_strip, &BitFieldStrip::fieldSelected, this, [this](int index) {
        selectField(index);
        emit fieldSelected(index);
    });

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("位"), QStringLiteral("字段"), QStringLiteral("值"),
                                        QStringLiteral("访问")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    // A table of four columns otherwise pins the whole dock to ~680 px, which
    // both blocks dragging the panel narrow and made the bit strip think it had
    // room for all 32 bits. Let the columns shrink and scroll instead.
    m_table->horizontalHeader()->setMinimumSectionSize(32);
    m_table->setMinimumWidth(180);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(m_table, 1);
    connect(m_table, &QTableWidget::cellClicked, this, &InspectorPanel::onTableRowChanged);

    auto *editorBox = new QGroupBox(QStringLiteral("字段编辑"), this);
    auto *editorLayout = new QFormLayout(editorBox);
    editorLayout->setContentsMargins(8, 8, 8, 8);

    m_enumBox = new QComboBox(editorBox);
    m_enumBox->setVisible(false);
    editorLayout->addRow(QStringLiteral("枚举"), m_enumBox);
    connect(m_enumBox, &QComboBox::currentIndexChanged, this, &InspectorPanel::onEnumChanged);

    m_fieldEdit = new QLineEdit(editorBox);
    m_fieldEdit->setPlaceholderText(QStringLiteral("十六进制，例如 0x3"));
    editorLayout->addRow(QStringLiteral("字段值"), m_fieldEdit);

    auto *fieldButtons = new QHBoxLayout();
    m_writeFieldButton = new QPushButton(QStringLiteral("写入字段"), editorBox);
    m_setButton = new QPushButton(QStringLiteral("置 1"), editorBox);
    m_clearButton = new QPushButton(QStringLiteral("清 0"), editorBox);
    fieldButtons->addWidget(m_writeFieldButton);
    fieldButtons->addWidget(m_setButton);
    fieldButtons->addWidget(m_clearButton);
    fieldButtons->addStretch();
    editorLayout->addRow(fieldButtons);
    connect(m_writeFieldButton, &QPushButton::clicked, this, &InspectorPanel::onWriteField);
    connect(m_setButton, &QPushButton::clicked, this, &InspectorPanel::onSetOne);
    connect(m_clearButton, &QPushButton::clicked, this, &InspectorPanel::onClearZero);

    m_registerEdit = new QLineEdit(editorBox);
    m_registerEdit->setPlaceholderText(QStringLiteral("整寄存器值，例如 0x00000001"));
    editorLayout->addRow(QStringLiteral("寄存器"), m_registerEdit);

    auto *regButtons = new QHBoxLayout();
    m_writeRegisterButton = new QPushButton(QStringLiteral("写寄存器"), editorBox);
    m_readButton = new QPushButton(QStringLiteral("重新读取"), editorBox);
    regButtons->addWidget(m_writeRegisterButton);
    regButtons->addWidget(m_readButton);
    regButtons->addStretch();
    editorLayout->addRow(regButtons);
    connect(m_writeRegisterButton, &QPushButton::clicked, this, &InspectorPanel::onWriteRegister);
    connect(m_readButton, &QPushButton::clicked, this, &InspectorPanel::onReadClicked);

    layout->addWidget(editorBox);

    m_valueLabel = new QLabel(this);
    m_valueLabel->setWordWrap(true);
    m_valueLabel->setMinimumWidth(0);
    layout->addWidget(m_valueLabel);

    setConnected(false);
}

void InspectorPanel::setConnected(bool connected)
{
    m_connected = connected;
    const bool enable = connected && m_register != nullptr;
    m_writeFieldButton->setEnabled(enable);
    m_setButton->setEnabled(enable);
    m_clearButton->setEnabled(enable);
    m_writeRegisterButton->setEnabled(enable);
    m_readButton->setEnabled(connected && m_register != nullptr);
    m_fieldEdit->setEnabled(enable);
    m_registerEdit->setEnabled(enable);
    m_enumBox->setEnabled(enable);
}

void InspectorPanel::clear()
{
    m_register = nullptr;
    m_selectedField = -1;
    m_value = 0;
    m_valid = false;
    m_title->setText(QStringLiteral("未选择寄存器"));
    m_addressLabel->clear();
    m_valueLabel->clear();
    m_strip->setRegister(nullptr);
    m_table->setRowCount(0);
    m_enumBox->setVisible(false);
    m_enumBox->clear();
    m_fieldEdit->clear();
    m_registerEdit->clear();
    setConnected(m_connected);
}

void InspectorPanel::setRegister(const SvdRegister *reg, quint32 registerAddress, quint32 value, bool valid)
{
    const bool sameRegister = (reg == m_register);
    m_register = reg;
    m_address = registerAddress;
    m_value = value;
    m_valid = valid;

    if (!reg) {
        clear();
        return;
    }

    m_title->setText(QStringLiteral("%1  (%2)").arg(reg->label(), reg->name));
    m_addressLabel->setText(QStringLiteral("地址 %1   大小 %2 bit   复位 %3   访问 %4")
                                .arg(hexAddr(registerAddress))
                                .arg(reg->size)
                                .arg(hex32(reg->resetValue))
                                .arg(svdAccessName(reg->access)));
    m_strip->setRegister(reg);
    m_strip->setValue(value, valid);
    m_registerEdit->setText(valid ? hex32(value) : QString());
    m_valueLabel->setText(valid ? QStringLiteral("当前值 %1 (%2)").arg(hex32(value)).arg(value)
                                : QStringLiteral("当前值：未读取（离线显示复位值 %1）").arg(hex32(reg->resetValue)));

    refreshTable();
    if (!sameRegister) {
        m_selectedField = reg->fields.isEmpty() ? -1 : 0;
    }
    refreshEditor();
    setConnected(m_connected);
}

void InspectorPanel::refreshTable()
{
    if (!m_register) {
        m_table->setRowCount(0);
        return;
    }

    m_updating = true;
    m_table->setRowCount(m_register->fields.size());
    for (int i = 0; i < m_register->fields.size(); ++i) {
        const SvdField &field = m_register->fields.at(i);
        auto *rangeItem = new QTableWidgetItem(field.bitRangeText());
        auto *nameItem = new QTableWidgetItem(field.name);
        nameItem->setToolTip(field.description);
        QString valueText = QStringLiteral("-");
        if (m_valid)
            valueText = QStringLiteral("%1").arg(field.extract(m_value));
        auto *valueItem = new QTableWidgetItem(valueText);
        auto *accessItem = new QTableWidgetItem(svdAccessName(field.access));
        m_table->setItem(i, 0, rangeItem);
        m_table->setItem(i, 1, nameItem);
        m_table->setItem(i, 2, valueItem);
        m_table->setItem(i, 3, accessItem);
        if (!svdAccessIsWritable(field.access)) {
            nameItem->setForeground(Theme::current().textDim);
            valueItem->setForeground(Theme::current().textDim);
        }
    }
    m_table->resizeRowsToContents();
    if (m_selectedField >= 0 && m_selectedField < m_table->rowCount())
        m_table->selectRow(m_selectedField);
    m_updating = false;
}

int InspectorPanel::selectedFieldIndex() const
{
    return m_selectedField;
}

quint32 InspectorPanel::currentFieldValue() const
{
    if (!m_register || m_selectedField < 0 || m_selectedField >= m_register->fields.size())
        return 0;
    const SvdField &field = m_register->fields.at(m_selectedField);
    if (m_enumBox->isVisible() && m_enumBox->currentData().isValid())
        return m_enumBox->currentData().toUInt();
    quint32 parsed = 0;
    if (parseHex(m_fieldEdit->text(), &parsed))
        return parsed;
    return m_valid ? field.extract(m_value) : 0;
}

void InspectorPanel::refreshEditor()
{
    m_updating = true;
    const bool hasField = m_register && m_selectedField >= 0 && m_selectedField < m_register->fields.size();
    if (hasField) {
        const SvdField &field = m_register->fields.at(m_selectedField);
        const bool writable = svdAccessIsWritable(field.access);
        m_strip->setSelectedField(m_selectedField);
        if (m_table->rowCount() > m_selectedField)
            m_table->selectRow(m_selectedField);

        const quint32 current = m_valid ? field.extract(m_value) : 0;
        m_fieldEdit->setText(QStringLiteral("0x%1").arg(current, field.bitWidth > 4 ? 1 : 1, 16));
        m_fieldEdit->setEnabled(m_connected && writable);
        m_writeFieldButton->setEnabled(m_connected && writable);
        m_setButton->setEnabled(m_connected && writable);
        m_clearButton->setEnabled(m_connected && writable);

        const bool hasEnums = !field.enums.isEmpty();
        m_enumBox->setVisible(hasEnums);
        if (hasEnums) {
            QSignalBlocker blocker(m_enumBox);
            m_enumBox->clear();
            for (const auto &e : field.enums) {
                QString label = QStringLiteral("%1 = 0x%2").arg(e.name).arg(e.value, 1, 16);
                if (!e.description.isEmpty() && e.description != e.name)
                    label += QStringLiteral("  (%1)").arg(e.description);
                m_enumBox->addItem(label, e.value);
            }
            const int idx = m_enumBox->findData(current);
            if (idx >= 0)
                m_enumBox->setCurrentIndex(idx);
            m_enumBox->setEnabled(m_connected && writable);
        }
    } else {
        m_strip->setSelectedField(-1);
        m_fieldEdit->clear();
        m_enumBox->setVisible(false);
        m_enumBox->clear();
    }
    m_updating = false;
}

void InspectorPanel::selectField(int index)
{
    if (!m_register || index < 0 || index >= m_register->fields.size())
        return;
    m_selectedField = index;
    m_strip->setSelectedField(index);
    refreshEditor();
    emit fieldSelected(index);
}

void InspectorPanel::onTableRowChanged(int row, int column)
{
    Q_UNUSED(column);
    if (m_updating || row < 0)
        return;
    selectField(row);
}

void InspectorPanel::onEnumChanged(int index)
{
    if (m_updating || index < 0)
        return;
    const quint32 value = m_enumBox->itemData(index).toUInt();
    m_fieldEdit->setText(QStringLiteral("0x%1").arg(value, 1, 16));
}

void InspectorPanel::onWriteField()
{
    if (!m_register || m_selectedField < 0 || m_selectedField >= m_register->fields.size())
        return;
    const SvdField field = m_register->fields.at(m_selectedField);
    quint32 value = 0;
    if (!parseHex(m_fieldEdit->text(), &value)) {
        m_fieldEdit->setPlaceholderText(QStringLiteral("无法解析：请输入十六进制"));
        return;
    }
    const quint32 max = field.bitWidth >= 32 ? 0xFFFFFFFFu : ((1u << field.bitWidth) - 1u);
    emit fieldWriteRequested(field, value & max);
}

void InspectorPanel::onSetOne()
{
    if (!m_register || m_selectedField < 0 || m_selectedField >= m_register->fields.size())
        return;
    const SvdField field = m_register->fields.at(m_selectedField);
    const quint32 max = field.bitWidth >= 32 ? 0xFFFFFFFFu : ((1u << field.bitWidth) - 1u);
    emit fieldWriteRequested(field, max);
}

void InspectorPanel::onClearZero()
{
    if (!m_register || m_selectedField < 0 || m_selectedField >= m_register->fields.size())
        return;
    emit fieldWriteRequested(m_register->fields.at(m_selectedField), 0);
}

void InspectorPanel::onWriteRegister()
{
    quint32 value = 0;
    if (!parseHex(m_registerEdit->text(), &value))
        return;
    emit registerWriteRequested(value);
}

void InspectorPanel::onReadClicked()
{
    emit registerReadRequested();
}

} // namespace mrd
