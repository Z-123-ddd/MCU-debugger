#include "ui/PlotWidget.h"

#include "ui/Theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace mrd {

namespace {

/// Stable, well-separated curve colours. Chosen to stay distinguishable on the
/// dark theme and in greyscale print.
const QColor kCurveColors[] = {
    QColor(0x4f, 0xc3, 0xf7), QColor(0xff, 0xb7, 0x4d), QColor(0x81, 0xc7, 0x84),
    QColor(0xe5, 0x73, 0x73), QColor(0xba, 0x68, 0xc8), QColor(0x4d, 0xd0, 0xe1),
    QColor(0xff, 0xf1, 0x76), QColor(0xa1, 0x88, 0x7f),
};
constexpr int kCurveColorCount = int(sizeof(kCurveColors) / sizeof(kCurveColors[0]));

QColor curveColor(int index)
{
    return kCurveColors[index % kCurveColorCount];
}

/// Round a raw span up to a "nice" axis step (1/2/5 x 10^n).
double niceStep(double rawStep)
{
    if (rawStep <= 0.0 || !std::isfinite(rawStep))
        return 1.0;
    const double exponent = std::floor(std::log10(rawStep));
    const double power = std::pow(10.0, exponent);
    const double fraction = rawStep / power;
    double nice = 10.0;
    if (fraction <= 1.0)
        nice = 1.0;
    else if (fraction <= 2.0)
        nice = 2.0;
    else if (fraction <= 5.0)
        nice = 5.0;
    return nice * power;
}

QString formatValue(double value)
{
    const double magnitude = std::fabs(value);
    if (magnitude != 0.0 && (magnitude >= 100000.0 || magnitude < 0.001))
        return QString::number(value, 'g', 4);
    if (magnitude >= 100.0)
        return QString::number(value, 'f', 0);
    if (magnitude >= 1.0)
        return QString::number(value, 'f', 2);
    return QString::number(value, 'f', 4);
}

} // namespace

PlotWidget::PlotWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(280, 180);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void PlotWidget::setHistory(const SampleHistory *history)
{
    m_history = history;
    m_visible.clear();
    if (m_history)
        m_visible.fill(true, m_history->traceCount());
    update();
}

void PlotWidget::refresh()
{
    update();
}

void PlotWidget::setTimeWindow(double seconds)
{
    m_timeWindow = qMax(0.0, seconds);
    update();
}

void PlotWidget::setAutoScale(bool enabled)
{
    m_autoScale = enabled;
    update();
}

void PlotWidget::setFixedRange(double low, double high)
{
    if (high <= low)
        high = low + 1.0;
    m_fixedLow = low;
    m_fixedHigh = high;
    m_autoScale = false;
    update();
}

void PlotWidget::setTraceVisible(int trace, bool visible)
{
    if (trace < 0)
        return;
    while (m_visible.size() <= trace)
        m_visible.append(true); // newly introduced traces start visible
    m_visible[trace] = visible;
    update();
}

bool PlotWidget::traceVisible(int trace) const
{
    if (trace < 0 || trace >= m_visible.size())
        return true;
    return m_visible.at(trace);
}

void PlotWidget::setShowGrid(bool on)
{
    m_showGrid = on;
    update();
}

void PlotWidget::setShowLegend(bool on)
{
    m_showLegend = on;
    update();
}

QSize PlotWidget::sizeHint() const
{
    return QSize(640, 320);
}

QRect PlotWidget::plotArea() const
{
    // Left margin holds the value axis labels; bottom holds the time axis.
    const int left = m_showLegend ? 56 : 48;
    const int right = 12;
    const int top = 10;
    const int bottom = 24;
    return rect().adjusted(left, top, -right, -bottom);
}

PlotWidget::Range PlotWidget::computeValueRange() const
{
    Range range;
    if (!m_autoScale) {
        range.low = m_fixedLow;
        range.high = m_fixedHigh;
        return range;
    }
    if (!m_history || m_history->isEmpty()) {
        range.low = 0.0;
        range.high = 1.0;
        return range;
    }
    double low = 0.0;
    double high = 0.0;
    bool first = true;
    for (int t = 0; t < m_history->traceCount(); ++t) {
        if (!traceVisible(t))
            continue;
        for (int i = 0; i < m_history->count(); ++i) {
            const double value = m_history->valueAt(t, i);
            if (!std::isfinite(value))
                continue;
            if (first) {
                low = high = value;
                first = false;
            } else {
                low = qMin(low, value);
                high = qMax(high, value);
            }
        }
    }
    if (first) {
        range.low = 0.0;
        range.high = 1.0;
        return range;
    }
    if (qFuzzyCompare(low + 1.0, high + 1.0)) {
        // A flat trace still needs a visible band around it.
        const double pad = qMax(0.5, std::fabs(low) * 0.1);
        range.low = low - pad;
        range.high = high + pad;
    } else {
        const double pad = (high - low) * 0.08;
        range.low = low - pad;
        range.high = high + pad;
    }
    return range;
}

void PlotWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void PlotWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect area = plotArea();
    if (area.width() <= 2 || area.height() <= 2)
        return;

    // Background and frame.
    painter.fillRect(rect(), Theme::current().surface);
    painter.setPen(QPen(Theme::current().border, 1));
    painter.drawRect(area);

    const Range range = computeValueRange();
    const double valueSpan = qMax(1e-12, range.high - range.low);

    // --- value axis ---------------------------------------------------------
    const double step = niceStep(valueSpan / 5.0);
    const int decimals = step >= 1.0 ? 0 : (step >= 0.01 ? 2 : 4);
    QFont axisFont = font();
    axisFont.setPointSizeF(qMax(7.0, font().pointSizeF() - 1.5));
    painter.setFont(axisFont);
    const QFontMetrics metrics(axisFont);

    if (m_showGrid) {
        painter.setPen(QPen(Theme::current().border.lighter(140), 1, Qt::DotLine));
        const double startTick = std::ceil(range.low / step) * step;
        for (double value = startTick; value <= range.high + 1e-9; value += step) {
            const double ratio = (value - range.low) / valueSpan;
            const int y = area.bottom() - static_cast<int>(ratio * area.height());
            if (y < area.top() || y > area.bottom())
                continue;
            painter.drawLine(area.left(), y, area.right(), y);
            painter.setPen(QPen(Theme::current().text, 1));
            const QString label = QString::number(value, 'f', decimals);
            painter.drawText(QRect(0, y - metrics.height() / 2, area.left() - 6,
                                   metrics.height()),
                             Qt::AlignRight | Qt::AlignVCenter, label);
            painter.setPen(QPen(Theme::current().border.lighter(140), 1, Qt::DotLine));
        }
    }

    // --- time axis ----------------------------------------------------------
    const qint64 last = m_history ? m_history->lastTime() : 0;
    const qint64 first = m_history ? m_history->firstTime() : 0;
    qint64 windowMs = 0;
    if (m_history && !m_history->isEmpty()) {
        windowMs = m_timeWindow > 0.0
            ? static_cast<qint64>(m_timeWindow * 1000.0)
            : qMax<qint64>(1, last - first);
    }
    const qint64 startMs = qMax<qint64>(first, last - windowMs);

    if (m_showGrid && windowMs > 0) {
        painter.setPen(QPen(Theme::current().border.lighter(140), 1, Qt::DotLine));
        const int divisions = 5;
        for (int i = 0; i <= divisions; ++i) {
            const int x = area.left() + area.width() * i / divisions;
            painter.drawLine(x, area.top(), x, area.bottom());
            painter.setPen(QPen(Theme::current().text.lighter(120), 1));
            const double seconds = (windowMs / 1000.0) * (1.0 - double(i) / divisions);
            painter.drawText(QRect(x - 30, area.bottom() + 2, 60, metrics.height() + 2),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QStringLiteral("-%1s").arg(seconds, 0, 'f', seconds < 10 ? 1 : 0));
            painter.setPen(QPen(Theme::current().border.lighter(140), 1, Qt::DotLine));
        }
    }

    if (!m_history || m_history->isEmpty()) {
        painter.setPen(QPen(Theme::current().text.lighter(150), 1));
        painter.drawText(area, Qt::AlignCenter,
                         QStringLiteral("尚无采样数据\n选择变量后点「开始」"));
        return;
    }

    // --- curves -------------------------------------------------------------
    const int count = m_history->count();
    painter.setClipRect(area);
    // Curve index for colouring follows the history's trace order.
    int drawnTrace = 0;
    for (int t = 0; t < m_history->traceCount(); ++t) {
        if (!traceVisible(t))
            continue;
        QPainterPath path;
        bool started = false;
        int lastX = INT_MIN;
        double lastY = 0.0;
        for (int i = 0; i < count; ++i) {
            const qint64 time = m_history->timeAt(i);
            if (windowMs > 0 && time < startMs)
                continue;
            const double value = m_history->valueAt(t, i);
            if (!std::isfinite(value)) {
                started = false;
                continue;
            }
            const double timeRatio = windowMs > 0
                ? double(time - startMs) / double(windowMs)
                : (count > 1 ? double(i) / double(count - 1) : 1.0);
            const double valueRatio = (value - range.low) / valueSpan;
            const int x = area.left() + int(timeRatio * (area.width() - 1));
            const double y = area.bottom() - valueRatio * (area.height() - 1);
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else if (x != lastX) {
                path.lineTo(x, y);
            } else if (std::fabs(y - lastY) > 0.5) {
                // Several samples landed in the same pixel column. When they differ
                // by more than half a pixel, drop a vertical segment so a spike is
                // not swallowed by the downsampling.
                path.lineTo(x, y);
            }
            lastX = x;
            lastY = y;
        }
        painter.setPen(QPen(curveColor(drawnTrace), 1.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        ++drawnTrace;
    }
    painter.setClipping(false);

    // --- legend + latest values --------------------------------------------
    if (m_showLegend) {
        int y = area.top() + 4;
        int colourIndex = 0;
        for (int t = 0; t < m_history->traceCount(); ++t) {
            if (!traceVisible(t))
                continue;
            const QString name = m_history->traceNames().value(t);
            const double latest = count > 0 ? m_history->valueAt(t, count - 1) : 0.0;
            const QString text = QStringLiteral("%1  %2").arg(name, formatValue(latest));
            const int textWidth = metrics.horizontalAdvance(text) + 22;
            const int boxX = area.right() - textWidth - 6;
            if (boxX < area.left() + 4)
                break; // no room; the variable list still shows the names
            QRect box(boxX, y, textWidth, metrics.height() + 2);
            painter.fillRect(box, QColor(0, 0, 0, 90));
            painter.setPen(QPen(curveColor(colourIndex), 2));
            painter.drawLine(box.left() + 4, box.center().y(), box.left() + 14, box.center().y());
            painter.setPen(QPen(Theme::current().text, 1));
            painter.drawText(box.adjusted(18, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
            y += box.height() + 1;
            ++colourIndex;
            if (y > area.bottom() - metrics.height())
                break;
        }
    }
}

} // namespace mrd
