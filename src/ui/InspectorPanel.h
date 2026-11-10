#pragma once

#include "core/SvdTypes.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace mrd {

class BitFieldStrip;

/// Right-hand inspector: bit-field table, per-field editor and register-level
/// read / write actions.
class InspectorPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget *parent = nullptr);

    void setRegister(const SvdRegister *reg, quint32 registerAddress, quint32 value, bool valid);
    void setConnected(bool connected);
    void clear();

    const SvdRegister *reg() const { return m_register; }
    /// The bit-field strip (exposed so the self-test can verify it never clips).
    const BitFieldStrip *bitStrip() const { return m_strip; }
    BitFieldStrip *bitStrip() { return m_strip; }
    int selectedField() const { return m_selectedField; }
    void selectField(int index);

signals:
    /// Field value written back to the register (already shifted into place).
    void fieldWriteRequested(const SvdField &field, quint32 fieldValue);
    void registerWriteRequested(quint32 value);
    void registerReadRequested();
    void fieldSelected(int index);

private slots:
    void onTableRowChanged(int row, int column);
    void onWriteField();
    void onSetOne();
    void onClearZero();
    void onWriteRegister();
    void onEnumChanged(int index);
    void onReadClicked();

private:
    void refreshTable();
    void refreshEditor();
    quint32 currentFieldValue() const;
    int selectedFieldIndex() const;

    const SvdRegister *m_register = nullptr;
    quint32 m_address = 0;
    quint32 m_value = 0;
    bool m_valid = false;
    bool m_connected = false;
    int m_selectedField = -1;
    bool m_updating = false;

    BitFieldStrip *m_strip = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_addressLabel = nullptr;
    QLabel *m_valueLabel = nullptr;
    QComboBox *m_enumBox = nullptr;
    QLineEdit *m_fieldEdit = nullptr;
    QLineEdit *m_registerEdit = nullptr;
    QPushButton *m_writeFieldButton = nullptr;
    QPushButton *m_setButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_writeRegisterButton = nullptr;
    QPushButton *m_readButton = nullptr;
};

} // namespace mrd
