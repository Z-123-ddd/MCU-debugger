#include "ui/BitFieldStrip.h"

#include "core/HexUtil.h"
#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace mrd {

namespace {
constexpr int kStripHeight = 46;
constexpr int kValueHeight = 22;
constexpr int kLeftMargin = 4;
} // namespace

BitFieldStrip::BitFieldStrip(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(kStripHeight + kValueHeight + 6);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BitFieldStrip::setRegister(const SvdRegister *reg)
{
    m_register = reg;
    m_selectedField = -1;
    m_hoverField = -1;
    if (reg)
        setValue(m_value, m_valueValid);
    updateGeometry();
    update();
}

void BitFieldStrip::setValue(quint32 value, bool valid)
{
    m_value = value;
    m_valueValid = valid;
    update();
}

void BitFieldStrip::setSelectedField(int fieldIndex)
{
    if (m_selectedField == fieldIndex)
        return;
    m_selectedField = fieldIndex;
    update();
}

int BitFieldStrip::effectiveBitWidth() const
{
    const int bits = 32;
    const int usable = qMax(0, width() - kLeftMargin * 2);
    // Below ~10 px per cell the labels are dropped; above 30 px it stops growing.
    return qBound(10, usable / bits, 30);
}

QSize BitFieldStrip::sizeHint() const
{
    const int bits = 32;
    return QSize(kLeftMargin * 2 + bits * m_bitWidth, kStripHeight + kValueHeight + 6);
}

QSize BitFieldStrip::minimumSizeHint() const
{
    // Only the height is fixed; the width is free so the dock can be dragged.
    return QSize(kLeftMargin * 2 + 32 * 10, kStripHeight + kValueHeight + 6);
}

QRect BitFieldStrip::fieldRect(const SvdField &field) const
{
    const int bits = 32;
    const int msb = qMin(field.bitOffset + field.bitWidth - 1, bits - 1);
    const int lsb = qMax(field.bitOffset, 0);
    const int cellWidth = effectiveBitWidth();
    const int left = kLeftMargin + (bits - 1 - msb) * cellWidth;
    const int width = (msb - lsb + 1) * cellWidth;
    return QRect(left, 2, width - 1, kStripHeight - 4);
}

int BitFieldStrip::fieldAt(const QPoint &pos) const
{
    if (!m_register)
        return -1;
    for (int i = 0; i < m_register->fields.size(); ++i) {
        if (fieldRect(m_register->fields.at(i)).contains(pos))
            return i;
    }
    return -1;
}

int BitFieldStrip::bitAt(const QPoint &pos) const
{
    const int x = pos.x() - kLeftMargin;
    if (x < 0)
        return -1;
    const int cell = x / qMax(1, effectiveBitWidth());
    if (cell < 0 || cell >= 32)
        return -1;
    return 31 - cell;
}

void BitFieldStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const Palette p = Theme::current();
    painter.fillRect(rect(), p.surface);

    const int bits = 32;
    const int stripTop = 2;
    const int stripHeight = kStripHeight - 4;

    if (!m_register) {
        painter.setPen(p.textDim);
        painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         QStringLiteral("选择寄存器以显示位域"));
        return;
    }

    // Bit ruler (only for bit widths that leave room).
    painter.setPen(p.textDim);
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() - 2.0);
    painter.setFont(small);
    const int cellWidth = effectiveBitWidth();
    if (cellWidth >= 18) {
        for (int bit = 0; bit < bits; bit += 4) {
            const int left = kLeftMargin + (bits - 1 - bit) * cellWidth;
            painter.drawText(QRect(left - 12, stripHeight + 4, cellWidth * 4, 14), Qt::AlignLeft,
                             QString::number(bit));
        }
    }

    // Field map: reserved bits first so real fields paint over them.
    auto reserved = QBrush(p.fieldReserved, Qt::BDiagPattern);
    painter.setPen(Qt::NoPen);
    painter.setBrush(reserved);
    painter.drawRect(QRect(kLeftMargin, stripTop, bits * cellWidth, stripHeight));

    for (int i = 0; i < m_register->fields.size(); ++i) {
        const SvdField &field = m_register->fields.at(i);
        const QRect r = fieldRect(field);
        const bool readOnly = !svdAccessIsWritable(field.access);
        QColor colour = readOnly ? p.fieldReadOnly : p.fieldReadWrite;
        if (i == m_selectedField)
            colour = p.fieldSelected;
        else if (i == m_hoverField)
            colour = colour.lighter(112);

        painter.setBrush(colour);
        painter.setPen(QPen(p.border, 1));
        painter.drawRect(r);

        if (r.width() > 22 && !field.name.isEmpty() && !field.isReserved()) {
            painter.setPen(readOnly ? p.textDim : p.text);
            QFont f = small;
            f.setBold(i == m_selectedField);
            painter.setFont(f);
            const QString label = painter.fontMetrics().elidedText(field.name, Qt::ElideRight, r.width() - 4);
            painter.drawText(r.adjusted(2, 1, -2, -1), Qt::AlignCenter, label);
            painter.setFont(small);
        }
    }

    // Value row: one box per bit.
    const int valueTop = kStripHeight + 18;
    painter.setFont(small);
    for (int bit = 31; bit >= 0; --bit) {
        const int left = kLeftMargin + (bits - 1 - bit) * cellWidth;
        const QRect r(left, valueTop, cellWidth - 1, kValueHeight);
        const bool set = m_valueValid && ((m_value >> bit) & 1u);
        painter.setBrush(set ? p.accent : p.surfaceAlt);
        painter.setPen(QPen(p.border, 1));
        painter.drawRect(r);
        if (cellWidth >= 14) {
            painter.setPen(set ? QColor(Qt::white) : p.textDim);
            painter.drawText(r, Qt::AlignCenter, set ? QStringLiteral("1") : QStringLiteral("0"));
        }
    }
}

void BitFieldStrip::mouseMoveEvent(QMouseEvent *event)
{
    const int field = fieldAt(event->pos());
    if (field != m_hoverField) {
        m_hoverField = field;
        update();
    }
    if (field >= 0 && m_register) {
        const SvdField &f = m_register->fields.at(field);
        QString text = QStringLiteral("<b>%1</b> [%2]<br/>%3<br/>访问: %4")
                           .arg(f.name, f.bitRangeText(),
                                f.description.isEmpty() ? QStringLiteral("-") : f.description.toHtmlEscaped(),
                                svdAccessName(f.access));
        if (!f.enums.isEmpty()) {
            text += QStringLiteral("<br/>取值: ");
            QStringList parts;
            for (const auto &e : f.enums)
                parts << QStringLiteral("%1=%2").arg(e.value).arg(e.name);
            text += parts.join(QStringLiteral(", ")).toHtmlEscaped();
        }
        if (m_valueValid)
            text += QStringLiteral("<br/>当前: %1 (%2)").arg(hexValue(f.extract(m_value), 1),
                                                             QString::number(f.extract(m_value)));
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

void BitFieldStrip::mousePressEvent(QMouseEvent *event)
{
    const int field = fieldAt(event->pos());
    if (field >= 0) {
        m_selectedField = field;
        update();
        emit fieldSelected(field);
    } else {
        const int bit = bitAt(event->pos());
        if (bit >= 0 && m_register) {
            for (int i = 0; i < m_register->fields.size(); ++i) {
                const SvdField &f = m_register->fields.at(i);
                if (bit >= f.bitOffset && bit < f.bitOffset + f.bitWidth) {
                    m_selectedField = i;
                    update();
                    emit fieldSelected(i);
                    break;
                }
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void BitFieldStrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int field = fieldAt(event->pos());
    if (field >= 0)
        emit fieldActivated(field);
    QWidget::mouseDoubleClickEvent(event);
}

void BitFieldStrip::leaveEvent(QEvent *event)
{
    if (m_hoverField != -1) {
        m_hoverField = -1;
        update();
    }
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

} // namespace mrd
