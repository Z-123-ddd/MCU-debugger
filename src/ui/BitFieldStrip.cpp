#include "ui/BitFieldStrip.h"

#include "core/HexUtil.h"
#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace mrd {

namespace {

constexpr int kTotalBits = 32;
constexpr int kLeftMargin = 4;
constexpr int kBandGap = 2;      ///< padding above/below every band
constexpr int kFieldHeight = 42; ///< field map band
constexpr int kRulerHeight = 14; ///< bit-number ruler under the field map
constexpr int kValueHeight = 22; ///< one box per bit

constexpr int kMaxCell = 30;      ///< stop growing the cells past this
constexpr int kRulerMinCell = 18; ///< bit-number ruler needs this much room
constexpr int kValueTextMinCell = 14;
constexpr int kLabelMinCell = 22;

/// Everything the strip draws: gap + field map + gap + ruler + gap + values + gap.
constexpr int kContentHeight =
    kBandGap + kFieldHeight + kBandGap + kRulerHeight + kBandGap + kValueHeight + kBandGap;

} // namespace

BitFieldStrip::BitFieldStrip(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    // Height is constant, width is free: the cells adapt instead of the widget.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kContentHeight);
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

int BitFieldStrip::preferredHeight()
{
    return kContentHeight;
}

int BitFieldStrip::cellWidth() const
{
    const int usable = qMax(0, width() - kLeftMargin * 2);
    // Integer division guarantees 32 * cell <= usable, so the highest bit can
    // never fall outside the widget; the floor of 2 px only matters for absurdly
    // narrow docks and keeps the arithmetic safe.
    return qBound(2, usable / kTotalBits, kMaxCell);
}

QSize BitFieldStrip::sizeHint() const
{
    return QSize(kLeftMargin * 2 + kTotalBits * m_bitWidth, kContentHeight);
}

QSize BitFieldStrip::minimumSizeHint() const
{
    // No width floor worth speaking of: the strip scales down rather than
    // locking the dock (which used to force ~900 px, later 328 px).
    return QSize(kLeftMargin * 2 + kTotalBits * 2, kContentHeight);
}

QRect BitFieldStrip::fieldRect(const SvdField &field) const
{
    const int msb = qMin(field.bitOffset + field.bitWidth - 1, kTotalBits - 1);
    const int lsb = qMax(field.bitOffset, 0);
    if (msb < lsb)
        return QRect();
    const int cell = cellWidth();
    const int left = kLeftMargin + (kTotalBits - 1 - msb) * cell;
    const int width = (msb - lsb + 1) * cell;
    return QRect(left, kBandGap, qMax(1, width - 1), kFieldHeight);
}

QRect BitFieldStrip::paintedBitsRect() const
{
    if (!m_register)
        return QRect();
    const int cell = cellWidth();
    const int width = kTotalBits * cell;
    return QRect(kLeftMargin, kBandGap, width, kContentHeight - kBandGap * 2);
}

int BitFieldStrip::fieldAt(const QPoint &pos) const
{
    if (!m_register)
        return -1;
    for (int i = 0; i < m_register->fields.size(); ++i) {
        const QRect r = fieldRect(m_register->fields.at(i));
        if (!r.isNull() && r.contains(pos))
            return i;
    }
    return -1;
}

int BitFieldStrip::bitAt(const QPoint &pos) const
{
    const int cell = cellWidth();
    const int x = pos.x() - kLeftMargin;
    if (x < 0)
        return -1;
    const int col = x / qMax(1, cell);
    if (col < 0 || col >= kTotalBits)
        return -1;
    return kTotalBits - 1 - col;
}

void BitFieldStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const Palette p = Theme::current();
    painter.fillRect(rect(), p.surface);

    if (!m_register) {
        painter.setPen(p.textDim);
        painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         QStringLiteral("选择寄存器以显示位域"));
        return;
    }

    const int cell = cellWidth();
    const int fieldTop = kBandGap;
    const int rulerTop = fieldTop + kFieldHeight + kBandGap;
    const int valueTop = rulerTop + kRulerHeight + kBandGap;

    QFont small = font();
    small.setPointSizeF(font().pointSizeF() - 2.0);
    painter.setFont(small);

    // Bit ruler (only where the cells are wide enough to read).
    if (cell >= kRulerMinCell) {
        painter.setPen(p.textDim);
        for (int bit = kTotalBits - 1; bit >= 0; bit -= 4) {
            const int left = kLeftMargin + (kTotalBits - 1 - bit) * cell;
            if (left + cell * 4 > width())
                continue; // the group label would run past the right edge
            painter.drawText(QRect(left, rulerTop, cell * 4, kRulerHeight), Qt::AlignLeft,
                             QString::number(bit));
        }
    }

    // Reserved background, then the real fields paint over it.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(p.fieldReserved, Qt::BDiagPattern));
    painter.drawRect(QRect(kLeftMargin, fieldTop, kTotalBits * cell, kFieldHeight));

    for (int i = 0; i < m_register->fields.size(); ++i) {
        const SvdField &field = m_register->fields.at(i);
        const QRect r = fieldRect(field);
        if (r.isNull())
            continue;

        const bool readOnly = !svdAccessIsWritable(field.access);
        QColor colour = readOnly ? p.fieldReadOnly : p.fieldReadWrite;
        if (i == m_selectedField)
            colour = p.fieldSelected;
        else if (i == m_hoverField)
            colour = colour.lighter(112);

        painter.setBrush(colour);
        painter.setPen(QPen(p.border, 1));
        painter.drawRect(r);

        if (cell >= kLabelMinCell && r.width() > 22 && !field.name.isEmpty() && !field.isReserved()) {
            painter.setPen(readOnly ? p.textDim : p.text);
            QFont f = small;
            f.setBold(i == m_selectedField);
            painter.setFont(f);
            const QString label =
                painter.fontMetrics().elidedText(field.name, Qt::ElideRight, r.width() - 4);
            painter.drawText(r.adjusted(2, 1, -2, -1), Qt::AlignCenter, label);
            painter.setFont(small);
        }
    }

    // Value row: one box per bit, always all 32.
    for (int bit = kTotalBits - 1; bit >= 0; --bit) {
        const int left = kLeftMargin + (kTotalBits - 1 - bit) * cell;
        const QRect r(left, valueTop, qMax(2, cell - 1), kValueHeight);
        const bool set = m_valueValid && ((m_value >> bit) & 1u);
        painter.setBrush(set ? p.accent : p.surfaceAlt);
        painter.setPen(QPen(p.border, 1));
        painter.drawRect(r);
        if (cell >= kValueTextMinCell) {
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
