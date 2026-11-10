#pragma once

#include "core/SvdTypes.h"

#include <QWidget>

namespace mrd {

/// 32-cell bit-field strip for one register.
///
/// * colours: read-write fields, read-only fields, reserved (hatched)
/// * shows the current value bit by bit below the field map
/// * hover shows the field description, click selects a field
///
/// All 32 bits are always drawn side by side; the cell width shrinks with the
/// dock (labels and the "0/1" digits are dropped below 14/18 px per cell instead
/// of losing bits). Nothing is ever clipped, at any panel width, and the height
/// does not depend on the width. `paintedBitsRect()` exposes that invariant so
/// the self-test can catch a regression.
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
    int bitWidth() const { return cellWidth(); }

    /// Total height the strip needs for its bands (independent of the width).
    static int preferredHeight();

    QSize sizeHint() const override;
    /// Deliberately small: the dock must stay draggable, so the strip scales its
    /// cells down instead of demanding a ~900 px minimum width.
    QSize minimumSizeHint() const override;

    /// Union of every painted bit cell / field / value box.
    ///
    /// Must stay inside `rect()`; a regression here means the register view is
    /// silently cut off (the bug this widget guards against).
    QRect paintedBitsRect() const;

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
    /// Cell width that fits all 32 bits into the current widget width.
    int cellWidth() const;
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
