#pragma once

#include "core/SampleHistory.h"

#include <QWidget>

namespace mrd {

/// Multi-trace waveform, drawn with QPainter like the rest of the app.
///
/// It is a pure consumer: `paintEvent` reads from a SampleHistory the collector
/// fills, and never asks the probe for anything. That is what keeps rendering off
/// the acquisition path -- a slow repaint (huge window, colour change) cannot cost
/// a sample, and a fast one cannot speed sampling up.
class PlotWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PlotWidget(QWidget *parent = nullptr);

    /// Not owned; must outlive the widget.
    void setHistory(const SampleHistory *history);
    void refresh();

    /// Visible time window in seconds; 0 means "everything buffered".
    void setTimeWindow(double seconds);
    double timeWindow() const { return m_timeWindow; }

    void setAutoScale(bool enabled);
    bool autoScale() const { return m_autoScale; }
    void setFixedRange(double low, double high);

    /// Hide/show one trace without dropping its samples.
    void setTraceVisible(int trace, bool visible);
    bool traceVisible(int trace) const;

    void setShowGrid(bool on);
    void setShowLegend(bool on);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Range
    {
        double low = 0.0;
        double high = 1.0;
    };
    Range computeValueRange() const;
    QRect plotArea() const;

    const SampleHistory *m_history = nullptr;
    double m_timeWindow = 10.0;
    bool m_autoScale = true;
    bool m_showGrid = true;
    bool m_showLegend = true;
    double m_fixedLow = 0.0;
    double m_fixedHigh = 1.0;
    QVector<bool> m_visible;
};

} // namespace mrd
