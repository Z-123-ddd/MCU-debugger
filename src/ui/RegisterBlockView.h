#pragma once

#include "core/SvdTypes.h"

#include <QHash>
#include <QGraphicsObject>
#include <QGraphicsView>
#include <QVector>

namespace mrd {

/// One register rendered as a block: name, offset, current value and a thin
/// bit-field bar. Clicking selects it; double-click zooms into the bit strip.
class RegisterBlockItem : public QGraphicsObject
{
    Q_OBJECT

public:
    RegisterBlockItem(const SvdRegister *reg, int index, QGraphicsItem *parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    void setValue(quint32 value, bool valid);
    void setSelectedState(bool selected);
    void setIndex(int index) { m_index = index; }
    int index() const { return m_index; }

    static QSizeF blockSize();

signals:
    void clicked(int index);
    void activated(int index);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    const SvdRegister *m_register = nullptr;
    int m_index = 0;
    quint32 m_value = 0;
    bool m_valid = false;
    bool m_selected = false;
    bool m_hovered = false;
};

/// CubeMX-like register layout: a scrollable grid of register blocks for one
/// peripheral, ordered by address offset, with address gaps collapsed.
class RegisterBlockView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit RegisterBlockView(QWidget *parent = nullptr);

    void setPeripheral(const SvdPeripheral *peripheral);
    void setValues(const QHash<quint32, quint32> &values, bool connected);
    void setSelectedRegister(int index);

    const SvdPeripheral *peripheral() const { return m_peripheral; }
    int selectedRegister() const { return m_selected; }

    void zoomIn();
    void zoomOut();
    void resetZoom();

signals:
    void registerSelected(int index);
    void registerActivated(int index);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void rebuild();
    void relayout();

    const SvdPeripheral *m_peripheral = nullptr;
    QVector<RegisterBlockItem *> m_items;
    QHash<quint32, quint32> m_values;
    bool m_connected = false;
    int m_selected = -1;
    double m_scale = 1.0;
};

} // namespace mrd
