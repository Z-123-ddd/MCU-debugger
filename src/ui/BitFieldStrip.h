#pragma once

#include "core/SvdTypes.h"

#include <QWidget>

namespace mrd {

/// 32-cell bit-field strip for one register.
///
/// * colours: read-write fields, read-only fields, reserved (hatched)
/// * shows the current value bit by bit below the field map
/// * hover shows the field description, click selects a field
class BitFieldStrip : public QWidget
{
    Q_OBJECT

public:
    explicit BitFieldStrip(QWidget *parent = nullptr);

    void setRegister(const SvdRegister *reg);
    const SvdRegister *reg() const { return m_register; }

    void setValue(quint32 value, bool valid);
    quint32 value() const { return m_value; }
    bool valueValid() const { return m_valueValid; }

    void setSelectedField(int fieldIndex);
    int selectedField() const { return m_selectedField; }

    /// Width of one bit cell actually used (adapts to the widget width).
    int bitWidth() const { return effectiveBitWidth(); }

    QSize sizeHint() const override;
    /// Small, so the surrounding dock can be resized freely; the strip scales
    /// itself down instead of forcing a ~900 px minimum width.
    QSize minimumSizeHint() const override;

signals:
    void fieldSelected(int fieldIndex);
    void fieldActivated(int fieldIndex); ///< double click

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int fieldAt(const QPoint &pos) const;
    /// Cell width derived from the current widget width (clamped for legibility).
    int effectiveBitWidth() const;
    QRect fieldRect(const SvdField &field) const;
    int bitAt(const QPoint &pos) const;

    const SvdRegister *m_register = nullptr;
    quint32 m_value = 0;
    bool m_valueValid = false;
    int m_selectedField = -1;
    int m_hoverField = -1;
    int m_bitWidth = 28;
};

} // namespace mrd
