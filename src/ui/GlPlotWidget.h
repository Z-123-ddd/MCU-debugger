#pragma once

#include "core/SampleHistory.h"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QVector>

class QOpenGLShaderProgram;

namespace mrd {

/// GPU-rendered waveform, ported from the technique used by webgl-plot.
///
/// The CPU never rasterises a curve. Each trace is a vertex buffer of (x, y) floats
/// uploaded once per frame, drawn with a single `LINE_STRIP` call, and the
/// data-space → clip-space mapping happens in the vertex shader exactly the way
/// webgl-plot does it:
///
///     attribute vec2 coordinates;
///     uniform mat2 uscale;  uniform vec2 uoffset;
///     gl_Position = vec4(uscale * coordinates + uoffset, 0.0, 1.0);
///
/// Only text (axis labels, crosshair read-out) is drawn with QPainter on top. Those
/// are a few dozen glyphs; the tens of thousands of samples are the part that used to
/// cost the CPU and now do not.
class GlPlotWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    enum class XAxisMode
    {
        Time,  ///< real seconds; newest sample at 0, older ones negative
        Index, ///< element / sample index
    };

    enum class ViewMode
    {
        TimeDomain,
        Spectrum, ///< FFT magnitude in dB
    };

    explicit GlPlotWidget(QWidget *parent = nullptr);
    ~GlPlotWidget() override;

    void setHistory(const SampleHistory *history);
    void refresh() { update(); }

    void setXAxisMode(XAxisMode mode);
    XAxisMode xAxisMode() const { return m_xAxisMode; }
    void setXAxisLabel(const QString &label);

    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_viewMode; }

    void setLiveMode(bool on);
    bool liveMode() const { return m_liveMode; }

    void setInteractive(bool on);
    bool interactive() const { return m_interactive; }

    void setLiveWindow(double seconds);
    void setTimeWindow(double seconds);
    double timeWindow() const { return m_timeWindow; }

    void setYScale(double scale);
    double yScale() const { return m_yScale; }

    bool isFollowingLatest() const { return m_followLatest; }
    void followLatest();

    void setSampleRateHint(double hz);
    double effectiveSampleRate() const;

    void setAutoScale(bool enabled);
    bool autoScale() const { return m_autoScale; }

    bool traceVisible(int trace) const;

    void resetView();

    int clippedPointCount() const { return m_clippedPoints; }


    /// Run the data pipeline without touching GL, so tests can check what *would* be
    /// drawn. QOpenGLWidget content cannot be captured with QWidget::render(), so pixel
    /// comparison is not available offscreen.
    int prepareForTest(double *xLow, double *xHigh, double *yLow, double *yHigh);
    /// Points the robust auto-range would ignore for the given window.
    int clippedPointsForTest(double from, double to) const;

    /// Render the curves into an offscreen framebuffer and count pixels that are not the
    /// background.
    ///
    /// Everything else about this widget can be checked without GL, but "is the line
    /// actually visible?" cannot: a degenerate uniform matrix draws nothing while every
    /// coordinate and range still looks correct. Requires a current GL context.
    int renderOffscreenForTest(int width, int height, int *nonBackgroundPixels);

    /// The same offscreen render, handed back as an image so a test can inspect actual
    /// pixel colours (the palette is the thing being checked, not the geometry).
    QImage offscreenImageForTest(int width, int height);

    QSize sizeHint() const override;

    /// Pixels per grid division.
    ///
    /// SEGGER's manual states it plainly: "One div is defined as 100px (the same size
    /// as the black background squares)". Measured from a real J-Scope capture the
    /// lattice is exactly 100 px in both directions. Deriving the step from the data
    /// (a 1-2-5 ladder) makes grid lines land at arbitrary pixels and shift on every
    /// repaint, which is what made this chart read as "not a scope".
    static constexpr int kPixelsPerDivision = 100;

    /// The exact shader sources used for rendering.
    ///
    /// Exposed so the self-test can compile them in an offscreen context: the failure
    /// mode here is silent (the widget just says "no data"), so it needs a test that
    /// fails loudly instead.
    static const char *vertexShaderSource();
    static const char *fragmentShaderSource();
    /// Non-empty when the GL program could not be built.
    QString glError() const { return m_glError; }

    // --- testable helpers, no GL involved -----------------------------------
    /// Flatten a trace of \a count samples into \a out as (x, y) pairs.
    ///
    /// Returns the number of drawable runs. A GL LINE_STRIP cannot be interrupted, so
    /// the data has to be split into runs -- but the split rule matters: breaking on
    /// *every* NaN makes one missed sample look like a torn trace. \a maxGapX is the
    /// largest X distance that is still bridged; shorter holes are connected (the line
    /// jumps them, the way an oscilloscope does) and longer ones start a new run.
    ///
    /// \a runStart / \a runLength must hold at least \a count / 2 + 1 entries.
    static int buildRuns(const double *values, const float *xs, int count, float *out,
                         int *runStart, int *runLength, float maxGapX);

    /// Largest X distance that buildRuns will still bridge: 3 pixels at this zoom.
    float bridgeDistanceX(double spanX, int widthPx) const;
    /// Pixel width of the plot area, for pixel-based decisions.
    int widthPx() const;

signals:
    /// A drag took the axis away from the live edge; the page uses this to tell the user
    /// (once) that double-clicking returns to following.
    void viewFrozen();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Range
    {
        double low = 0.0;
        double high = 1.0;
    };

    void ensureGl();
    void buildTimeSeries(double fromSec, double toSec);
    void buildSpectrumSeries(double *minDb, double *maxDb);
    void drawSeries(const QVector<QVector<float>> &traceXy,
                    const QVector<QVector<QPair<int, int>>> &runs,
                    const Range &xRange, const Range &yRange, bool smoothColors);
    Range computeValueRange(double from, double to) const;
    QRect plotArea() const;
    void visibleWindow(double *from, double *to) const;
    void drawOverlay(QPainter &painter, const QRect &area, const Range &xRange,
                     const Range &yRange);
    void drawCrosshairOverlay(QPainter &painter, const QRect &area, const Range &xRange,
                              const Range &yRange);
    static double friendlyStep(double span, double targetLines);
    static QString formatValue(double value);
    static QString formatDuration(double seconds);
    int sampleIndexAt(double timeSec) const;
    QColor traceColor(int index) const;



    const SampleHistory *m_history = nullptr;
    XAxisMode m_xAxisMode = XAxisMode::Index;
    ViewMode m_viewMode = ViewMode::TimeDomain;
    QString m_xAxisLabel;

    bool m_interactive = true;
    bool m_liveMode = false;
    bool m_autoScale = true;
    double m_timeWindow = 0.0;
    double m_defaultWindowSec = 2.0;
    double m_yScale = 1.0;
    double m_yOffset = 0.0;
    double m_xZoom = 1.0;
    double m_xOffsetSec = 0.0;
    bool m_followLatest = true;
    /// Axis value held at drag start; used instead of the newest sample while panned,
    /// so a running capture cannot slide the view out from under the cursor.
    double m_anchorSec = 0.0;
    bool m_userZoomed = false;
    /// Per-trace visibility; set through the tree, read at draw time.
    QVector<bool> m_visible;
    double m_sampleRateHint = 0.0;

    bool m_dragging = false;
    QPoint m_dragOrigin;
    double m_dragStartXOffset = 0.0;
    double m_dragStartYOffset = 0.0;
    QPoint m_hover{-1, -1};
    mutable int m_clippedPoints = 0;
    int m_gpuPoints = 0;


    // --- GL objects --------------------------------------------------------
    bool m_glReady = false;
    /// Context the GL function pointers were resolved against and m_program belongs to.
    void *m_functionsContext = nullptr;
    /// Why the GL program is unusable, shown in the widget instead of "no data".
    QString m_glError;
    QOpenGLShaderProgram *m_program = nullptr;
    QOpenGLBuffer m_xyBuffer{QOpenGLBuffer::VertexBuffer};
    int m_attrCoord = -1;
    int m_uniformScale = -1;
    int m_uniformOffset = -1;
    int m_uniformColor = -1;

    // Scratch reused every frame so painting allocates nothing.
    QVector<QVector<float>> m_traceXy;
    QVector<QVector<QPair<int, int>>> m_traceRuns;
    QVector<float> m_xScratch;
};

} // namespace mrd
