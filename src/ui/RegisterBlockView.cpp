#include "ui/RegisterBlockView.h"

#include "core/HexUtil.h"
#include "ui/Theme.h"

#include <QFontMetrics>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>

namespace mrd {

namespace {
constexpr qreal kBlockWidth = 168.0;
constexpr qreal kBlockHeight = 74.0;
constexpr qreal kGapX = 10.0;
constexpr qreal kGapY = 10.0;
constexpr qreal kMargin = 12.0;
} // namespace

QSizeF RegisterBlockItem::blockSize()
{
    return QSizeF(kBlockWidth, kBlockHeight);
}

RegisterBlockItem::RegisterBlockItem(const SvdRegister *reg, int index, QGraphicsItem *parent)
    : QGraphicsObject(parent)
    , m_register(reg)
    , m_index(index)
{
    setAcceptHoverEvents(true);
    setFlag(QGraphicsItem::ItemIsSelectable, false);
    setToolTip(reg ? QStringLiteral("%1\n%2\n偏移 %3  复位值 %4")
                         .arg(reg->name,
                              reg->description.isEmpty() ? QStringLiteral("-") : reg->description,
                              hexAddr(reg->addressOffset), hex32(reg->resetValue))
                   : QString());
}

QRectF RegisterBlockItem::boundingRect() const
{
    return QRectF(0, 0, kBlockWidth, kBlockHeight);
}

void RegisterBlockItem::setValue(quint32 value, bool valid)
{
    if (m_value == value && m_valid == valid)
        return;
    m_value = value;
    m_valid = valid;
    update();
}

void RegisterBlockItem::setSelectedState(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    update();
}

void RegisterBlockItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    if (!m_register)
        return;

    const Palette p = Theme::current();
    const QRectF r = boundingRect().adjusted(1, 1, -1, -1);

    QColor fill = m_valid ? p.blockNormal : p.blockOffline;
    if (m_valid && m_value != m_register->resetValue)
        fill = p.blockChanged;

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(fill);
    QPen border(m_selected ? p.blockSelected : (m_hovered ? p.accent : p.border));
    border.setWidthF(m_selected ? 2.0 : 1.0);
    painter->setPen(border);
    painter->drawRoundedRect(r, 6, 6);

    // Name + offset
    QFont title = painter->font();
    title.setBold(true);
    title.setPointSizeF(title.pointSizeF() + 0.5);
    painter->setFont(title);
    painter->setPen(p.text);
    const QString name = painter->fontMetrics().elidedText(m_register->label(), Qt::ElideRight, int(kBlockWidth) - 60);
    painter->drawText(QRectF(r.left() + 8, r.top() + 4, kBlockWidth - 66, 18), Qt::AlignLeft | Qt::AlignVCenter, name);

    QFont small = painter->font();
    small.setBold(false);
    small.setPointSizeF(small.pointSizeF() - 2.0);
    painter->setFont(small);
    painter->setPen(p.textDim);
    painter->drawText(QRectF(r.right() - 58, r.top() + 5, 52, 16), Qt::AlignRight | Qt::AlignVCenter,
                      hexValue(m_register->addressOffset, 3));

    // Value
    painter->setFont(small);
    painter->setPen(m_valid ? p.text : p.textDim);
    const QString valueText = m_valid ? hex32(m_value) : QStringLiteral("-- 未读取 --");
    painter->drawText(QRectF(r.left() + 8, r.top() + 24, kBlockWidth - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
                      valueText);

    // Bit map bar
    const qreal barTop = r.top() + 44;
    const qreal barHeight = 10;
    const qreal cell = (kBlockWidth - 16) / 32.0;
    painter->setPen(Qt::NoPen);
    painter->setBrush(p.fieldReserved);
    painter->drawRect(QRectF(r.left() + 8, barTop, cell * 32, barHeight));

    const quint32 shown = m_valid ? m_value : m_register->resetValue;
    for (const auto &field : m_register->fields) {
        if (field.bitWidth <= 0)
            continue;
        const int msb = qMin(field.bitOffset + field.bitWidth - 1, 31);
        const int lsb = qMax(field.bitOffset, 0);
        const qreal x = r.left() + 8 + cell * (31 - msb);
        const qreal w = cell * (msb - lsb + 1) - 0.5;
        if (w <= 0)
            continue;
        const bool writable = svdAccessIsWritable(field.access) && !field.isReserved();
        painter->setBrush(writable ? p.fieldReadWrite : p.fieldReadOnly);
        painter->drawRect(QRectF(x, barTop, w, barHeight));
    }

    // Field / access count summary
    painter->setPen(p.textDim);
    const QString info = QStringLiteral("%1 位域").arg(m_register->fields.size());
    painter->drawText(QRectF(r.left() + 8, r.top() + 56, kBlockWidth - 16, 14), Qt::AlignLeft | Qt::AlignVCenter, info);

    painter->setPen(p.textDim);
    painter->drawText(QRectF(r.left() + 8, r.top() + 56, kBlockWidth - 16, 14), Qt::AlignRight | Qt::AlignVCenter,
                      svdAccessIsWritable(m_register->access) ? QStringLiteral("RW") : QStringLiteral("RO"));
    Q_UNUSED(shown);
}

void RegisterBlockItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    emit clicked(m_index);
    QGraphicsObject::mousePressEvent(event);
}

void RegisterBlockItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
    emit activated(m_index);
    QGraphicsObject::mouseDoubleClickEvent(event);
}

void RegisterBlockItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event)
{
    m_hovered = true;
    update();
    QGraphicsObject::hoverEnterEvent(event);
}

void RegisterBlockItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event)
{
    m_hovered = false;
    update();
    QGraphicsObject::hoverLeaveEvent(event);
}

// ---------------------------------------------------------------------------
// RegisterBlockView
// ---------------------------------------------------------------------------

RegisterBlockView::RegisterBlockView(QWidget *parent)
    : QGraphicsView(parent)
{
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setBackgroundBrush(Theme::current().background);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void RegisterBlockView::setPeripheral(const SvdPeripheral *peripheral)
{
    m_peripheral = peripheral;
    m_selected = -1;
    rebuild();
}

void RegisterBlockView::setValues(const QHash<quint32, quint32> &values, bool connected)
{
    m_values = values;
    m_connected = connected;
    if (!m_peripheral)
        return;
    for (auto *item : m_items) {
        const int index = item->index();
        if (index < 0 || index >= m_peripheral->registers.size())
            continue;
        const SvdRegister &reg = m_peripheral->registers.at(index);
        const quint32 address = m_peripheral->baseAddress + reg.addressOffset;
        const auto it = values.constFind(address);
        item->setValue(it != values.constEnd() ? it.value() : reg.resetValue,
                       connected && it != values.constEnd());
    }
    viewport()->update();
}

void RegisterBlockView::setSelectedRegister(int index)
{
    if (m_selected == index)
        return;
    m_selected = index;
    for (auto *item : m_items)
        item->setSelectedState(item->index() == index);
    if (index >= 0 && index < m_items.size())
        ensureVisible(m_items.at(index), 40, 40);
}

void RegisterBlockView::rebuild()
{
    scene()->clear();
    m_items.clear();

    if (!m_peripheral) {
        scene()->setSceneRect(QRectF(0, 0, 400, 120));
        return;
    }

    for (int i = 0; i < m_peripheral->registers.size(); ++i) {
        auto *item = new RegisterBlockItem(&m_peripheral->registers.at(i), i);
        connect(item, &RegisterBlockItem::clicked, this, [this](int index) {
            setSelectedRegister(index);
            emit registerSelected(index);
        });
        connect(item, &RegisterBlockItem::activated, this, [this](int index) { emit registerActivated(index); });
        scene()->addItem(item);
        m_items << item;
    }

    setValues(m_values, m_connected);
    relayout();
}

void RegisterBlockView::relayout()
{
    if (m_items.isEmpty()) {
        scene()->setSceneRect(QRectF(0, 0, 400, 120));
        return;
    }

    const qreal available = qMax<qreal>(kBlockWidth + kMargin * 2, viewport()->width() / m_scale - kMargin);
    const int columns = qMax(1, int((available + kGapX) / (kBlockWidth + kGapX)));
    qreal y = kMargin;
    for (int i = 0; i < m_items.size(); i += columns) {
        for (int c = 0; c < columns && i + c < m_items.size(); ++c) {
            RegisterBlockItem *item = m_items.at(i + c);
            item->setPos(kMargin + c * (kBlockWidth + kGapX), y);
        }
        y += kBlockHeight + kGapY;
    }

    const qreal width = kMargin * 2 + columns * (kBlockWidth + kGapX);
    scene()->setSceneRect(QRectF(0, 0, width, y + kMargin));
}

void RegisterBlockView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    relayout();
}

void RegisterBlockView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0)
            zoomIn();
        else
            zoomOut();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void RegisterBlockView::zoomIn()
{
    m_scale = qMin(2.0, m_scale * 1.15);
    resetTransform();
    scale(m_scale, m_scale);
    relayout();
}

void RegisterBlockView::zoomOut()
{
    m_scale = qMax(0.5, m_scale / 1.15);
    resetTransform();
    scale(m_scale, m_scale);
    relayout();
}

void RegisterBlockView::resetZoom()
{
    m_scale = 1.0;
    resetTransform();
    relayout();
}

} // namespace mrd
