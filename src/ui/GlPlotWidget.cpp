#include "ui/GlPlotWidget.h"

#include "core/Fft.h"
#include "core/Logger.h"
#include "ui/Theme.h"

#include <QElapsedTimer>
#include <QImage>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mrd {

namespace {

constexpr int kSpectrumPoints = 1024;
constexpr int kRobustMinSamples = 40;
constexpr double kRobustClipFraction = 0.005;

/// Oscilloscope palette: pure, fully saturated primaries.
///
/// Soft pastel colours read as "chart library"; a scope trace is identifiable at a
/// glance by hue alone, so nothing is desaturated. Background is pure black and the
/// grid is a dark grey, which keeps the trace the brightest thing on screen.
namespace scope {

QColor background()
{
    return QColor(0, 0, 0);
}

QColor grid()
{
    return QColor(0x3C, 0x3C, 0x3C);
}

/// Axis labels and the plot frame.
QColor axis()
{
    return QColor(0xC0, 0xC0, 0xC0);
}

QColor border()
{
    return QColor(0x76, 0x76, 0x76);
}

/// Text drawn on top of the dark plot (hover read-out, peak markers).
QColor text()
{
    return QColor(0xFF, 0xFF, 0xFF);
}

/// Translucent panel behind the hover read-out.
QColor panel()
{
    return QColor(0x18, 0x18, 0x18, 235);
}

} // namespace scope

QColor paletteColor(int index)
{
    // Pure hues, in the order a scope would assign them.
    static const QColor kColors[] = {
        QColor(0xFF, 0x00, 0x00), // red
        QColor(0xFF, 0xA5, 0x00), // orange
        QColor(0x00, 0xFF, 0x00), // green
        QColor(0x00, 0xB0, 0xFF), // azure
        QColor(0xFF, 0xFF, 0x00), // yellow
        QColor(0xFF, 0x00, 0xFF), // magenta
        QColor(0x00, 0xFF, 0xFF), // cyan
        QColor(0xFF, 0xFF, 0xFF), // white
    };
    const int count = int(sizeof(kColors) / sizeof(kColors[0]));
    return kColors[((index % count) + count) % count];
}

} // namespace

GlPlotWidget::GlPlotWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::WheelFocus);
}

GlPlotWidget::~GlPlotWidget()
{
    // Only touch GL when this widget ever got a context. A widget that was constructed
    // but never shown (the self-test builds several) has none, and makeCurrent() on it
    // crashes inside the driver.
    if (m_glReady && context()) {
        makeCurrent();
        if (m_xyBuffer.isCreated())
            m_xyBuffer.destroy();
        doneCurrent();
    }
    delete m_program;
    m_program = nullptr;
}

void GlPlotWidget::setHistory(const SampleHistory *history)
{
    m_history = history;
    update();
}

void GlPlotWidget::setXAxisMode(XAxisMode mode)
{
    if (m_xAxisMode == mode)
        return;
    m_xAxisMode = mode;
    update();
}

void GlPlotWidget::setXAxisLabel(const QString &label)
{
    m_xAxisLabel = label;
    update();
}

void GlPlotWidget::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;
    update();
}

void GlPlotWidget::setLiveMode(bool on)
{
    if (m_liveMode == on)
        return;
    m_liveMode = on;
    // Live: bounded window so detail stays readable. Stopped: fit everything, but keep
    // zoom/pan/hover -- a finished capture is still something to inspect.
    m_timeWindow = on ? m_defaultWindowSec : 0.0;
    m_followLatest = true;
    m_xOffsetSec = 0.0;
    m_xZoom = 1.0;
    update();
}

void GlPlotWidget::setInteractive(bool on)
{
    m_interactive = on;
    setMouseTracking(on);
    update();
}

void GlPlotWidget::setLiveWindow(double seconds)
{
    m_defaultWindowSec = qMax(0.0, seconds);
    m_timeWindow = m_defaultWindowSec;
    update();
}

void GlPlotWidget::setTimeWindow(double seconds)
{
    m_timeWindow = qMax(0.0, seconds);
    update();
}

void GlPlotWidget::setYScale(double scale)
{
    m_yScale = qBound(0.01, scale, 1000.0);
    m_userZoomed = true;
    update();
}

void GlPlotWidget::followLatest()
{
    m_followLatest = true;
    m_xOffsetSec = 0.0;
    m_anchorSec = 0.0;
    update();
}

void GlPlotWidget::setSampleRateHint(double hz)
{
    m_sampleRateHint = qMax(0.0, hz);
}

double GlPlotWidget::effectiveSampleRate() const
{
    if (m_sampleRateHint > 0.0)
        return m_sampleRateHint;
    if (!m_history || m_history->count() < 2)
        return 0.0;
    const qint64 spanUs = m_history->lastTime() - m_history->firstTime();
    if (spanUs <= 0)
        return 0.0;
    return 1e6 * double(m_history->count() - 1) / double(spanUs);
}

void GlPlotWidget::setAutoScale(bool enabled)
{
    m_autoScale = enabled;
    update();
}

bool GlPlotWidget::traceVisible(int trace) const
{
    if (trace < 0 || trace >= m_visible.size())
        return true;
    return m_visible.at(trace);
}

void GlPlotWidget::resetView()
{
    m_yScale = 1.0;
    m_yOffset = 0.0;
    m_xZoom = 1.0;
    m_xOffsetSec = 0.0;
    m_userZoomed = false;
    m_followLatest = true;
    m_timeWindow = m_liveMode ? m_defaultWindowSec : 0.0;
    update();
}

QSize GlPlotWidget::sizeHint() const
{
    return QSize(640, 320);
}

QColor GlPlotWidget::traceColor(int index) const
{
    return paletteColor(index);
}

int GlPlotWidget::prepareForTest(double *xLow, double *xHigh, double *yLow, double *yHigh)
{
    if (m_viewMode == ViewMode::Spectrum) {
        double minDb = 0.0;
        double maxDb = 0.0;
        buildSpectrumSeries(&minDb, &maxDb);
        // Report the SAME range paintGL draws. Hard-coding 0..Nyquist here made the
        // test blind to zoom: the curve narrowed while the reported range did not.
        const double nyquist = effectiveSampleRate() / 2.0;
        const double span = qMax(1e-9, nyquist / qMax(1e-9, m_xZoom));
        const double from = qBound(0.0, m_xOffsetSec * span, qMax(0.0, nyquist - span));
        if (xLow)
            *xLow = from;
        if (xHigh)
            *xHigh = from + span;
        if (yLow)
            *yLow = qMax(minDb, maxDb - 100.0);
        if (yHigh)
            *yHigh = maxDb;
        return m_gpuPoints;
    }

    double from = 0.0;
    double to = 1.0;
    visibleWindow(&from, &to);
    buildTimeSeries(from, to);
    const Range range = computeValueRange(from, to);
    if (xLow)
        *xLow = from;
    if (xHigh)
        *xHigh = to;
    if (yLow)
        *yLow = range.low;
    if (yHigh)
        *yHigh = range.high;
    return m_gpuPoints;
}

int GlPlotWidget::clippedPointsForTest(double from, double to) const
{
    computeValueRange(from, to);
    return m_clippedPoints;
}

QImage GlPlotWidget::offscreenImageForTest(int width, int height)
{
    if (width < 8 || height < 8)
        return QImage();
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        m_glError = QStringLiteral("离屏渲染需要当前 GL 上下文");
        return QImage();
    }
    if (m_functionsContext != static_cast<void *>(ctx)) {
        initializeOpenGLFunctions();
        m_functionsContext = static_cast<void *>(ctx);
        m_glReady = false;
    }
    ensureGl();
    if (!m_glReady)
        return QImage();

    QOpenGLFramebufferObject fbo(width, height);
    if (!fbo.bind()) {
        m_glError = QStringLiteral("无法绑定离屏 framebuffer");
        return QImage();
    }

    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    const QColor bg = scope::background();
    glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    double from = 0.0;
    double to = 1.0;
    visibleWindow(&from, &to);
    buildTimeSeries(from, to);
    const Range yRange = computeValueRange(from, to);
    Range xRange;
    xRange.low = from;
    xRange.high = to;

    drawSeries(m_traceXy, m_traceRuns, xRange, yRange, false);
    glFinish();

    QImage image = fbo.toImage();
    fbo.release();

    // Compose the QPainter overlay (grid, axis labels) onto the same image, so this
    // returns what the widget actually shows -- otherwise the test would verify the
    // traces and silently skip the grid.
    if (!image.isNull()) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect area = plotArea();
        if (area.width() > 4 && area.height() > 4)
            drawOverlay(painter, area, xRange, yRange);
    }
    return image;
}

int GlPlotWidget::renderOffscreenForTest(int width, int height, int *nonBackgroundPixels)
{
    if (nonBackgroundPixels)
        *nonBackgroundPixels = 0;
    const QImage image = offscreenImageForTest(width, height);
    if (image.isNull())
        return 0;

    const QColor bg = scope::background();
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = image.pixel(x, y);
            if (qAbs(qRed(px) - bg.red()) + qAbs(qGreen(px) - bg.green())
                    + qAbs(qBlue(px) - bg.blue()) > 40)
                ++painted;
        }
    }
    if (nonBackgroundPixels)
        *nonBackgroundPixels = painted;
    return m_gpuPoints;
}

// ---------------------------------------------------------------------------
// GL setup
// ---------------------------------------------------------------------------

void GlPlotWidget::initializeGL()
{
    initializeOpenGLFunctions();
    ensureGl();
}

const char *GlPlotWidget::vertexShaderSource()
{
    // Same shape as webgl-plot's thin-line program: a mat2 scale plus an offset maps
    // data space straight to clip space, so the CPU never touches a coordinate.
    //
    // GLSL 1.20 because that is what a default desktop context gives on Windows.
    return R"(#version 120
attribute vec2 coordinates;
uniform mat2 uscale;
uniform vec2 uoffset;
void main(void) {
    gl_Position = vec4(uscale * coordinates + uoffset, 0.0, 1.0);
}
)";
}

const char *GlPlotWidget::fragmentShaderSource()
{
    // No `precision mediump float;` here: precision statements are GLSL *ES* syntax and
    // a desktop compiler rejects them. That single line made the program fail to link,
    // and the widget then reported "no data" while sampling was working fine.
    return R"(#version 120
uniform vec4 uColor;
void main(void) {
    gl_FragColor = uColor;
}
)";
}

void GlPlotWidget::ensureGl()
{
    if (m_glReady)
        return;

    // Every GL object must be created against a live context. Creating them with none
    // (a widget that was never shown, or after doneCurrent()) dereferences null state
    // inside the driver.
    if (!QOpenGLContext::currentContext()) {
        m_glError = QStringLiteral("没有当前 GL 上下文");
        m_glReady = false;
        return;
    }

    delete m_program;
    // No widget parent: the program is tied to whichever context is current, which is
    // not necessarily the widget's own (the self-test renders offscreen).
    m_program = new QOpenGLShaderProgram();

    // Same shape as webgl-plot's thin-line program: a mat2 scale plus an offset maps
    // data space straight to clip space, so the CPU never touches a coordinate.
    //
    // Written as GLSL 1.20 because that is what a default desktop context gives you on
    // Windows. In particular there is NO `precision mediump float;` here: precision
    // statements are GLSL *ES* syntax and a desktop compiler rejects them, which made
    // the program fail to link and the widget fall back to "no data".
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource())
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                               fragmentShaderSource())
        || !m_program->link()) {
        // Keep the reason: without it a link failure is indistinguishable from
        // "no samples yet", which is exactly how this bug hid.
        m_glError = m_program->log().trimmed();
        if (m_glError.isEmpty())
            m_glError = QStringLiteral("着色器链接失败（驱动未给出原因）");
        Logger::instance()->warn(QStringLiteral("波形着色器失败：%1").arg(m_glError));
        m_glReady = false;
        return;
    }

    m_attrCoord = m_program->attributeLocation("coordinates");
    m_uniformScale = m_program->uniformLocation("uscale");
    m_uniformOffset = m_program->uniformLocation("uoffset");
    m_uniformColor = m_program->uniformLocation("uColor");
    if (m_attrCoord < 0 || m_uniformScale < 0 || m_uniformOffset < 0 || m_uniformColor < 0) {
        m_glError = QStringLiteral("着色器缺少必要的 attribute/uniform");
        Logger::instance()->warn(m_glError);
        m_glReady = false;
        return;
    }

    if (!m_xyBuffer.create()) {
        m_glError = QStringLiteral("无法创建顶点缓冲");
        m_glReady = false;
        return;
    }
    m_xyBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    m_glError.clear();
    m_glReady = true;
}

void GlPlotWidget::resizeGL(int w, int h)
{
    // Same hazard as paintGL: glViewport is a function pointer that only exists after
    // initializeOpenGLFunctions(). resizeGL can run before the widget is ever shown, and
    // calling through the null pointer kills the process. paintGL sets the viewport for
    // the plot area anyway, so there is nothing to do until GL is ready.
    if (!m_glReady)
        return;
    glViewport(0, 0, w, h);
}

// ---------------------------------------------------------------------------
// Data preparation
// ---------------------------------------------------------------------------

float GlPlotWidget::bridgeDistanceX(double spanX, int widthPx) const
{
    // Bridge anything narrower than a few pixels on screen.
    //
    // The span must be given explicitly: deriving it from visibleWindow() only works for
    // the time axis. The spectrum runs on Hz, so passing a seconds-based distance broke
    // the trace into single points.
    const int width = qMax(1, widthPx);
    return float(qMax(1e-12, spanX) * 3.0 / double(width));
}

int GlPlotWidget::buildRuns(const double *values, const float *xs, int count, float *out,
                            int *runStart, int *runLength, float maxGapX)
{
    if (!values || !xs || !out || count <= 0)
        return 0;

    int runs = 0;
    int runBegin = -1;
    int lastValid = -1;

    for (int i = 0; i < count; ++i) {
        const double value = values[i];
        if (!std::isfinite(value))
            continue; // a hole: bridged or broken below, never drawn as zero

        const bool gapTooBig = lastValid >= 0 && (xs[i] - xs[lastValid]) > maxGapX;
        if (runBegin >= 0 && gapTooBig) {
            // A real dropout: close the current run and start a new one so the line
            // does not jump across missing time.
            if (runStart && runLength) {
                runStart[runs] = runBegin;
                runLength[runs] = lastValid - runBegin + 1;
            }
            // A run can never cover fewer than one point, so `count` is the hard upper
            // bound; clamp rather than trusting the caller's allocation.
            if (++runs >= count)
                return runs;
        }
        if (runBegin < 0)
            runBegin = i;

        // Interleaved (x, y), which is what the shader's vec2 attribute expects.
        out[i * 2] = xs[i];
        out[i * 2 + 1] = float(value);
        lastValid = i;
    }
    if (runBegin >= 0 && lastValid >= runBegin) {
        if (runStart && runLength) {
            runStart[runs] = runBegin;
            runLength[runs] = lastValid - runBegin + 1;
        }
        ++runs;
    }
    return runs;
}

void GlPlotWidget::buildTimeSeries(double fromSec, double toSec)
{
    m_traceXy.clear();
    m_traceRuns.clear();
    m_gpuPoints = 0;
    if (!m_history || m_history->isEmpty())
        return;

    const int count = m_history->count();
    const int traces = m_history->traceCount();
    m_traceXy.resize(traces);
    m_traceRuns.resize(traces);

    // Visible range in axis units. One extra sample on each side keeps the line
    // connected to the edges instead of stopping short of them.
    int first = 0;
    int last = count - 1;
    if (m_xAxisMode == XAxisMode::Time) {
        first = count;
        for (int i = 0; i < count; ++i) {
            if (double(m_history->timeAt(i)) / 1e6 >= fromSec) {
                first = i;
                break;
            }
        }
        last = first;
        for (int i = first; i < count; ++i) {
            last = i;
            if (double(m_history->timeAt(i)) / 1e6 > toSec)
                break;
        }
        first = qMax(0, first - 1);
        last = qMin(count - 1, last + 1);
    } else {
        first = qMax(0, int(std::floor(fromSec)) - 1);
        last = qMin(count - 1, int(std::ceil(toSec)) + 1);
    }
    const int visible = last - first + 1;
    if (visible < 2)
        return;

    m_xScratch.resize(visible);
    for (int i = 0; i < visible; ++i) {
        const int index = first + i;
        m_xScratch[i] = m_xAxisMode == XAxisMode::Index
            ? float(index)
            : float(double(m_history->timeAt(index)) / 1e6);
    }

    QVector<double> column(visible);
    for (int t = 0; t < traces; ++t) {
        if (!traceVisible(t)) {
            m_traceXy[t].clear();
            m_traceRuns[t].clear();
            continue;
        }
        for (int i = 0; i < visible; ++i)
            column[i] = m_history->valueAt(t, first + i);

        m_traceXy[t].resize(visible * 2);
        // Worst case is one run per point (sparse data, every step wider than the
        // bridge distance), so size for that -- `visible/2+2` overflowed the heap.
        QVector<int> starts(visible + 1);
        QVector<int> lengths(visible + 1);
        const int runs = buildRuns(column.constData(), m_xScratch.constData(), visible,
                                   m_traceXy[t].data(), starts.data(), lengths.data(),
                                   bridgeDistanceX(toSec - fromSec, widthPx()));
        m_traceRuns[t].clear();
        m_traceRuns[t].reserve(runs);
        for (int r = 0; r < runs; ++r)
            m_traceRuns[t].append(qMakePair(starts[r], lengths[r]));
        m_gpuPoints += visible;
    }
}

void GlPlotWidget::buildSpectrumSeries(double *minDb, double *maxDb)
{
    *minDb = std::numeric_limits<double>::max();
    *maxDb = -std::numeric_limits<double>::max();

    const int count = m_history ? m_history->count() : 0;
    const int points = qMin(count, kSpectrumPoints);
    const double rate = effectiveSampleRate();

    m_traceXy.clear();
    m_traceRuns.clear();
    m_gpuPoints = 0;
    if (points < 16 || rate <= 0.0)
        return;

    // Analyzed window: the newest kSpectrumPoints samples. Averaging the whole buffer
    // would hide whatever the user just changed on the target.
    const int traces = m_history->traceCount();
    m_traceXy.resize(traces);
    m_traceRuns.resize(traces);

    const int width = qMax(1, plotArea().width());
    const double nyquist = rate / 2.0;
    // X zoom anchors at 0 Hz: zooming in reveals detail near DC, which is where motor
    // current ripple and encoder error live. Panning is a fraction of the visible span.
    const double span = qMax(1e-9, nyquist / qMax(1e-9, m_xZoom));
    const double from = qBound(0.0, m_xOffsetSec * span, qMax(0.0, nyquist - span));
    const double to = from + span;
    const float bridge = bridgeDistanceX(span, width);

    QVector<double> samples(points);
    QVector<double> frequencies;
    for (int t = 0; t < traces; ++t) {
        if (!traceVisible(t)) {
            m_traceXy[t].clear();
            m_traceRuns[t].clear();
            continue;
        }
        for (int i = 0; i < points; ++i)
            samples[i] = m_history->valueAt(t, count - points + i);
        const QVector<double> db = Fft::magnitudeSpectrum(samples);
        if (db.isEmpty()) {
            m_traceXy[t].clear();
            m_traceRuns[t].clear();
            continue;
        }

        const int bins = db.size();
        frequencies.resize(bins);
        for (int i = 0; i < bins; ++i)
            frequencies[i] = Fft::binFrequency(rate, bins * 2, i);

        // Only the visible band is handed to the GPU: same reason as the time view,
        // uploading 512 bins when 50 are on screen is wasted work.
        int firstBin = 0;
        while (firstBin < bins && frequencies.at(firstBin) < from)
            ++firstBin;
        int lastBin = bins - 1;
        while (lastBin > firstBin && frequencies.at(lastBin) > to)
            --lastBin;
        firstBin = qMax(0, firstBin - 1);
        lastBin = qMin(bins - 1, lastBin + 1);
        const int visible = lastBin - firstBin + 1;
        if (visible < 2) {
            m_traceXy[t].clear();
            m_traceRuns[t].clear();
            continue;
        }

        m_xScratch.resize(visible);
        QVector<double> visibleDb(visible);
        for (int i = 0; i < visible; ++i) {
            m_xScratch[i] = float(frequencies.at(firstBin + i));
            visibleDb[i] = db.at(firstBin + i);
            *minDb = qMin(*minDb, visibleDb.at(i));
            *maxDb = qMax(*maxDb, visibleDb.at(i));
        }

        m_traceXy[t].resize(visible * 2);
        QVector<int> starts(visible + 1);
        QVector<int> lengths(visible + 1);
        const int runs = buildRuns(visibleDb.constData(), m_xScratch.constData(), visible,
                                   m_traceXy[t].data(), starts.data(), lengths.data(),
                                   bridge);
        m_traceRuns[t].clear();
        for (int r = 0; r < runs; ++r)
            m_traceRuns[t].append(qMakePair(starts[r], lengths[r]));
        m_gpuPoints += visible;
    }
}

// ---------------------------------------------------------------------------
// Window / range maths
// ---------------------------------------------------------------------------

void GlPlotWidget::visibleWindow(double *fromSec, double *toSec) const
{
    if (!m_history || m_history->isEmpty()) {
        if (fromSec)
            *fromSec = 0.0;
        if (toSec)
            *toSec = m_defaultWindowSec;
        return;
    }

    const double total = m_xAxisMode == XAxisMode::Index
        ? qMax(1.0, double(m_history->count()))
        : qMax(0.0, double(m_history->lastTime()) / 1e6);

    // Window width: fixed by the user's zoom, never adjusted to fit the data. Changing
    // it automatically would fight the wheel, so the span only ever changes when the
    // user zooms.
    double base = 0.0;
    if (m_timeWindow > 0.0)
        base = m_timeWindow;
    else if (m_liveMode)
        base = m_defaultWindowSec;
    else
        base = qMax(1e-9, total);
    const double span = qMax(1e-9, base / qMax(1e-9, m_xZoom));

    // Until the data fills the window the axis starts at the left edge, so a fresh
    // capture grows rightwards from x=0 instead of appearing as a sliver at the right.
    // Once it is full the window follows the newest sample -- unless the user dragged,
    // in which case the anchor captured at drag start is held so the capture no longer
    // scrolls underneath them.
    double to = 0.0;
    if (m_followLatest && total <= span) {
        to = span; // still filling: right edge parked at one window width
    } else if (m_followLatest) {
        to = total;
    } else {
        to = m_anchorSec - m_xOffsetSec;
    }

    if (fromSec)
        *fromSec = to - span;
    if (toSec)
        *toSec = to;
}

GlPlotWidget::Range GlPlotWidget::computeValueRange(double from, double to) const
{
    Range range;
    if (!m_history || m_history->isEmpty()) {
        range.low = 0.0;
        range.high = 1.0;
        return range;
    }

    QVector<double> values;
    for (int t = 0; t < m_history->traceCount(); ++t) {
        if (!traceVisible(t))
            continue;
        for (int i = 0; i < m_history->count(); ++i) {
            const double x = m_xAxisMode == XAxisMode::Index
                ? double(i)
                : double(m_history->timeAt(i)) / 1e6;
            if (x < from || x > to)
                continue;
            const double value = m_history->valueAt(t, i);
            if (!std::isfinite(value))
                continue;
            values.append(value);
        }
    }
    if (values.isEmpty()) {
        range.low = 0.0;
        range.high = 1.0;
        return range;
    }

    double low = 0.0;
    double high = 0.0;
    m_clippedPoints = 0;
    if (values.size() >= kRobustMinSamples && m_autoScale) {
        std::sort(values.begin(), values.end());
        const int cut = qMax(1, int(values.size() * kRobustClipFraction));
        const int loIndex = qBound(0, cut, int(values.size()) - 1);
        const int hiIndex = qBound(0, int(values.size()) - 1 - cut, int(values.size()) - 1);
        low = values.at(loIndex);
        high = values.at(hiIndex);
        m_clippedPoints = loIndex + (int(values.size()) - 1 - hiIndex);
        if (!(high > low)) {
            low = values.first();
            high = values.last();
        }
    } else {
        low = values.first();
        high = values.last();
        for (double v : values) {
            low = qMin(low, v);
            high = qMax(high, v);
        }
    }

    if (qFuzzyCompare(low + 1.0, high + 1.0)) {
        const double pad = qMax(0.5, std::fabs(low) * 0.1);
        range.low = low - pad;
        range.high = high + pad;
    } else {
        const double pad = (high - low) * 0.08;
        range.low = low - pad;
        range.high = high + pad;
    }

    const double centre = (range.low + range.high) / 2.0 + m_yOffset;
    const double half = (range.high - range.low) / 2.0 / qMax(1e-9, m_yScale);
    range.low = centre - half;
    range.high = centre + half;
    return range;
}

int GlPlotWidget::widthPx() const
{
    return qMax(1, plotArea().width());
}

QRect GlPlotWidget::plotArea() const
{
    const int left = 62;
    const int right = 14;
    const int top = 10;
    const int bottom = 28;
    return rect().adjusted(left, top, -right, -bottom);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void GlPlotWidget::paintGL()
{
    const QRect area = plotArea();

    // Bail out BEFORE touching any GL entry point. `glClear`/`glClearColor` are function
    // pointers bound by initializeOpenGLFunctions(); calling them without a prepared
    // program jumps through a null pointer and takes the process down. A widget that was
    // resized but never shown can be asked to paint with no usable context.
    ensureGl();
    if (!m_glReady) {
        QPainter painter(this);
        painter.setPen(QPen(scope::axis(), 1));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("图形上下文不可用\n%1").arg(m_glError));
        return;
    }

    const QColor bg = scope::background();
    glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_history || m_history->isEmpty() || area.width() < 20
        || area.height() < 20) {
        QPainter painter(this);
        painter.setPen(QPen(scope::axis(), 1));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("尚无数据\n勾选变量后读取快照，或开始实时采样"));
        return;
    }

    Range xRange;
    Range yRange;
    if (m_viewMode == ViewMode::Spectrum) {
        double minDb = 0.0;
        double maxDb = 0.0;
        buildSpectrumSeries(&minDb, &maxDb);
        const double nyquist = effectiveSampleRate() / 2.0;
        xRange.low = 0.0;
        xRange.high = qMax(1.0, nyquist);
        yRange.low = qMax(minDb, maxDb - 100.0);
        yRange.high = maxDb > yRange.low ? maxDb : yRange.low + 1.0;
    } else {
        double from = 0.0;
        double to = 1.0;
        visibleWindow(&from, &to);
        xRange.low = from;
        xRange.high = to;
        buildTimeSeries(from, to);
        yRange = computeValueRange(from, to);
    }

    // --- GPU: viewport in device pixels, then one LINE_STRIP per run ----------
    const double dpr = devicePixelRatioF();
    const int vx = int(area.left() * dpr);
    const int vy = int((height() - area.bottom()) * dpr);
    const int vw = int(area.width() * dpr);
    const int vh = int(area.height() * dpr);
    glViewport(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, vw, vh);

    drawSeries(m_traceXy, m_traceRuns, xRange, yRange, false);

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, int(width() * dpr), int(height() * dpr));

    // --- CPU: only the text, which is a few dozen glyphs ---------------------
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawOverlay(painter, area, xRange, yRange);
    if (m_viewMode == ViewMode::TimeDomain)
        drawCrosshairOverlay(painter, area, xRange, yRange);
}

void GlPlotWidget::drawSeries(const QVector<QVector<float>> &traceXy,
                              const QVector<QVector<QPair<int, int>>> &runs,
                              const Range &xRange, const Range &yRange, bool smoothColors)
{
    Q_UNUSED(smoothColors);
    if (!m_program || !m_xyBuffer.isCreated())
        return;

    m_program->bind();
    m_attrCoord = m_program->attributeLocation("coordinates");
    m_uniformScale = m_program->uniformLocation("uscale");
    m_uniformOffset = m_program->uniformLocation("uoffset");
    m_uniformColor = m_program->uniformLocation("uColor");
    m_xyBuffer.bind();

    const float spanX = float(qMax(1e-12, xRange.high - xRange.low));
    const float spanY = float(qMax(1e-12, yRange.high - yRange.low));
    // Data space → clip space: scale by 2/span, shift the midpoint to the origin.
    // Identical to what webgl-plot's uscale/uoffset do.
    const float sx = 2.0f / spanX;
    const float sy = 2.0f / spanY;
    const float ox = -float(xRange.low + xRange.high) / spanX;
    const float oy = -float(yRange.low + yRange.high) / spanY;

    // A mat2 uniform must be uploaded as a MATRIX. setUniformValueArray() dispatches to
    // glUniform4fv (it has no matrix overload): the value type mismatch leaves the
    // matrix unset, every vertex collapses onto one point, and the curves vanish while
    // the QPainter overlay (axes, numbers) still draws -- exactly the reported symptom.
    // Column-major: column0 = (sx, 0), column1 = (0, sy).
    const GLfloat scale[4] = {sx, 0.0f, 0.0f, sy};
    glUniformMatrix2fv(m_uniformScale, 1, GL_FALSE, scale);
    m_program->setUniformValue(m_uniformOffset, QVector2D(ox, oy));

    m_program->enableAttributeArray(m_attrCoord);
    m_program->setAttributeBuffer(m_attrCoord, GL_FLOAT, 0, 2, 2 * sizeof(float));

    // One device pixel, no smoothing: a J-Scope capture measures 72% single-pixel runs
    // with an AA ratio of 0.12, i.e. hard edges. Asking GL for 1.4 px produced 2 px
    // runs plus partial pixels, which is what made this look like a chart library
    // rather than a scope.
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
    // 2 px: a 1 px line on a high-DPI panel is a hairline that reads as noise. Two
    // pixels is still crisp (no smoothing) but clearly a curve.
    glLineWidth(2.0f);
    for (int t = 0; t < traceXy.size(); ++t) {
        if (traceXy.at(t).isEmpty() || runs.at(t).isEmpty())
            continue;
        const QColor color = traceColor(t);
        m_program->setUniformValue(m_uniformColor, QVector4D(color.redF(), color.greenF(),
                                                             color.blueF(), 1.0f));
        const QVector<float> &xy = traceXy.at(t);
        m_xyBuffer.allocate(xy.constData(), int(xy.size() * sizeof(float)));
        for (const auto &run : runs.at(t)) {
            if (run.second < 2)
                continue;
            // Each run is its own strip: a NaN gap must not be joined across.
            glDrawArrays(GL_LINE_STRIP, run.first, run.second);
        }
    }

    m_program->disableAttributeArray(m_attrCoord);
    m_xyBuffer.release();
    m_program->release();
}

void GlPlotWidget::drawOverlay(QPainter &painter, const QRect &area, const Range &xRange,
                               const Range &yRange)
{
    const QFontMetrics metrics(font());
    const double spanX = qMax(1e-12, xRange.high - xRange.low);
    const double spanY = qMax(1e-12, yRange.high - yRange.low);

    // J-Scope's grid is a fixed pixel lattice, not a data-derived step: one division is
    // 100 px and the *labels* carry the units (s/div, units/div). That is what keeps the
    // squares square and the picture stable while data streams in.
    const int step = kPixelsPerDivision;

    // --- lattice -----------------------------------------------------------
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(scope::grid(), 1, Qt::SolidLine));

    // Horizontal lines, anchored at the BOTTOM so the baseline stays put as the range
    // changes instead of the whole grid sliding by a few pixels each frame.
    int offsetY = area.height() % step;
    for (int y = area.bottom() - offsetY; y >= area.top(); y -= step)
        painter.drawLine(area.left(), y, area.right(), y);
    for (int y = area.bottom() - offsetY + step; y <= area.bottom(); y += step) {
        if (y > area.top())
            painter.drawLine(area.left(), y, area.right(), y);
    }

    int offsetX = area.width() % step;
    for (int x = area.left() + offsetX; x <= area.right(); x += step)
        painter.drawLine(x, area.top(), x, area.bottom());

    // --- emphasised zero axes ----------------------------------------------
    // J-Scope draws the axis through zero (Y) and the baseline (X) brighter than the
    // rest of the lattice. It is the reference the eye uses to judge amplitude.
    painter.setPen(QPen(scope::axis(), 1, Qt::SolidLine));
    if (yRange.low < 0.0 && yRange.high > 0.0) {
        const int zeroY = area.bottom()
            - int((0.0 - yRange.low) / spanY * area.height());
        painter.drawLine(area.left(), zeroY, area.right(), zeroY);
    }
    painter.drawLine(area.left(), area.bottom(), area.right(), area.bottom());
    painter.drawLine(area.left(), area.top(), area.left(), area.bottom());

    // --- per-division labels ------------------------------------------------
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(scope::axis(), 1));

    // Y: one label per 100 px, showing the value that division line represents.
    for (int y = area.bottom() - offsetY; y >= area.top(); y -= step) {
        const double value = yRange.low + double(area.bottom() - y) / area.height() * spanY;
        painter.drawText(QRect(0, y - metrics.height() / 2, area.left() - 6,
                               metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         m_viewMode == ViewMode::Spectrum
                             ? QStringLiteral("%1 dB").arg(value, 0, 'f', 0)
                             : formatValue(value));
    }
    // X: label each division with the time (or frequency) at that line.
    const double perDivX = spanX * step / qMax(1, area.width());
    for (int x = area.left() + offsetX; x <= area.right(); x += step) {
        const double value = xRange.low + double(x - area.left()) / area.width() * spanX;
        QString label;
        if (m_viewMode == ViewMode::Spectrum) {
            label = value >= 1000.0
                ? QStringLiteral("%1k").arg(value / 1000.0, 0, 'f', 1)
                : QStringLiteral("%1").arg(value, 0, 'f', 0);
        } else if (m_xAxisMode == XAxisMode::Time) {
            // Scope convention: the newest sample is 0, older ones negative.
            label = formatDuration(value - xRange.high);
        } else {
            label = QString::number(qint64(value));
        }
        painter.drawText(QRect(x - 40, area.bottom() + 2, 80, metrics.height() + 2),
                         Qt::AlignHCenter | Qt::AlignTop, label);
    }

    // Divisions-per-window is useful when the numbers are terse; J-Scope shows the
    // resolution in the toolbar, so it belongs next to the axis here.
    if (m_viewMode == ViewMode::TimeDomain && m_xAxisMode == XAxisMode::Time) {
        painter.setPen(QPen(scope::axis(), 1));
        painter.drawText(QRect(area.right() - 150, area.top() - 8, 150, metrics.height() + 2),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1/div").arg(formatDuration(perDivX)));
    }

    // Peak markers for the spectrum stay: they answer "at what frequency", which the
    // axis alone does not. The time-domain view deliberately has no in-plot legend --
    // the variable list already carries the names, and a legend strip on a black scope
    // reads as chrome.
    if (m_viewMode == ViewMode::Spectrum) {
        int y = area.top() + 4;
        for (int t = 0; t < (m_history ? m_history->traceCount() : 0); ++t) {
            if (!traceVisible(t))
                continue;
            const QString name = m_history->traceNames().value(t);
            if (name.isEmpty())
                continue;
            painter.fillRect(QRect(area.left() + 6, y + 3, 12, 3), traceColor(t));
            painter.setPen(QPen(scope::text(), 1));
            painter.drawText(QRect(area.left() + 22, y - 2, area.width() - 30,
                                   metrics.height() + 4),
                             Qt::AlignLeft | Qt::AlignVCenter, name);
            y += metrics.height() + 2;
            if (y > area.bottom() - metrics.height())
                break;
        }
    }
}

void GlPlotWidget::drawCrosshairOverlay(QPainter &painter, const QRect &area,
                                        const Range &xRange, const Range &yRange)
{
    if (!area.contains(m_hover) || !m_history || m_history->isEmpty())
        return;

    const QFontMetrics metrics(font());
    const double spanX = qMax(1e-12, xRange.high - xRange.low);
    const double ratio = double(m_hover.x() - area.left()) / double(area.width());

    int sampleIndex = -1;
    if (m_xAxisMode == XAxisMode::Index) {
        sampleIndex = int(std::lround(xRange.low + spanX * ratio));
        if (sampleIndex < 0 || sampleIndex >= m_history->count())
            return;
    } else {
        sampleIndex = sampleIndexAt(xRange.low + spanX * ratio);
        if (sampleIndex < 0)
            return;
    }

    painter.setPen(QPen(scope::axis(), 1, Qt::DashLine));
    painter.drawLine(m_hover.x(), area.top(), m_hover.x(), area.bottom());

    QStringList lines;
    lines << (m_xAxisMode == XAxisMode::Index
                  ? QStringLiteral("#%1").arg(sampleIndex)
                  : QStringLiteral("t = %1")
                        .arg(formatDuration(double(m_history->timeAt(sampleIndex)) / 1e6
                                            - xRange.high)));
    for (int t = 0; t < m_history->traceCount(); ++t) {
        if (!traceVisible(t))
            continue;
        const double value = m_history->valueAt(t, sampleIndex);
        if (!std::isfinite(value))
            continue;
        lines << QStringLiteral("%1 = %2")
                     .arg(m_history->traceNames().value(t), formatValue(value));
    }

    const int width = 200;
    const int height = int(lines.size()) * (metrics.height() + 2) + 8;
    int x = m_hover.x() + 12;
    if (x + width > area.right())
        x = m_hover.x() - width - 12;
    const int y = qBound(area.top() + 2, m_hover.y() - height / 2, area.bottom() - height - 2);
    const QRect box(x, y, width, height);
    painter.setPen(QPen(scope::border(), 1));
    QColor bg = scope::panel();
    painter.setBrush(bg);
    painter.drawRoundedRect(box, 3, 3);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(scope::text(), 1));
    int lineY = box.top() + 4;
    for (const QString &line : lines) {
        painter.drawText(QRect(box.left() + 6, lineY, box.width() - 12, metrics.height() + 2),
                         Qt::AlignLeft | Qt::AlignVCenter, line);
        lineY += metrics.height() + 2;
    }
}

int GlPlotWidget::sampleIndexAt(double timeSec) const
{
    if (!m_history || m_history->isEmpty())
        return -1;
    const qint64 targetUs = qint64(timeSec * 1e6);
    int best = -1;
    qint64 bestDelta = std::numeric_limits<qint64>::max();
    for (int i = 0; i < m_history->count(); ++i) {
        const qint64 delta = qAbs(m_history->timeAt(i) - targetUs);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void GlPlotWidget::wheelEvent(QWheelEvent *event)
{
    if (!m_interactive) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }
    // Plain wheel = vertical (amplitude); Ctrl+wheel = horizontal (time). Shift kept as
    // an alias for the previous binding.
    if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        m_xZoom = qBound(0.01, m_xZoom * std::pow(1.25, steps), 100.0);
        m_userZoomed = true;
    } else {
        m_yScale = qBound(0.01, m_yScale * std::pow(1.2, steps), 1000.0);
        m_userZoomed = true;
    }
    update();
    event->accept();
}

void GlPlotWidget::mousePressEvent(QMouseEvent *event)
{
    if (!m_interactive) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::RightButton) {
        resetView();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOrigin = event->pos();
        // Remember where the right edge sits *now*. Following continues until the
        // pointer actually moves, so a plain click (crosshair) does not freeze the view.
        if (m_followLatest) {
            double from = 0.0;
            double to = 0.0;
            visibleWindow(&from, &to);
            m_anchorSec = to;
        }
        m_dragStartXOffset = m_xOffsetSec;
        m_dragStartYOffset = m_yOffset;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void GlPlotWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    // The way back to live: panning is a deliberate act, so leaving it takes one too.
    if (event->button() == Qt::LeftButton && m_interactive) {
        followLatest();
        update();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void GlPlotWidget::mouseMoveEvent(QMouseEvent *event)
{
    m_hover = event->pos();
    if (m_dragging && m_interactive) {
        const QRect area = plotArea();
        const QPoint delta = event->pos() - m_dragOrigin;
        // A few pixels of slack so a shaky click is not mistaken for a pan.
        constexpr int kDragThresholdPx = 3;
        if (m_followLatest && qAbs(delta.x()) >= kDragThresholdPx) {
            // Real drag: hand the axis over to the user, anchored where it was when the
            // button went down. Without this the advancing newest sample cancels the
            // offset every frame and the trace keeps sliding under the cursor.
            m_followLatest = false;
            m_dragStartXOffset = 0.0;
            m_xOffsetSec = 0.0;
            emit viewFrozen();
        }
        if (area.width() > 0) {
            double from = 0.0;
            double to = 0.0;
            visibleWindow(&from, &to);
            const double span = qMax(1e-12, to - from);
            m_xOffsetSec =
                m_dragStartXOffset + double(delta.x()) / double(area.width()) * span;
        }
    }
    update();
    QOpenGLWidget::mouseMoveEvent(event);
}

void GlPlotWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        unsetCursor();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void GlPlotWidget::leaveEvent(QEvent *event)
{
    m_hover = QPoint(-1, -1);
    update();
    QOpenGLWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

double GlPlotWidget::friendlyStep(double span, double targetLines)
{
    if (span <= 0.0 || targetLines <= 0.0)
        return 1.0;
    const double target = span / targetLines;
    const double base = std::pow(10.0, std::floor(std::log10(target)));
    static const double kMantissas[] = {1.0, 2.0, 5.0};
    for (double mantissa : kMantissas) {
        const double step = mantissa * base;
        if (step >= target * 0.7)
            return step;
    }
    return 10.0 * base;
}

QString GlPlotWidget::formatValue(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("—");
    const double magnitude = std::fabs(value);
    if (value == 0.0)
        return QStringLiteral("0");
    if (magnitude >= 100000.0 || magnitude < 0.001)
        return QString::number(value, 'e', 2);
    if (magnitude >= 100.0)
        return QString::number(value, 'f', 0);
    if (magnitude >= 1.0)
        return QString::number(value, 'f', 2);
    return QString::number(value, 'f', 4);
}

QString GlPlotWidget::formatDuration(double seconds)
{
    const double magnitude = std::fabs(seconds);
    if (magnitude >= 1.0)
        return QStringLiteral("%1s").arg(seconds, 0, 'f', magnitude >= 10.0 ? 0 : 2);
    if (magnitude >= 1e-3)
        return QStringLiteral("%1ms").arg(seconds * 1e3, 0, 'f', magnitude >= 1e-2 ? 1 : 2);
    if (magnitude >= 1e-6)
        return QStringLiteral("%1µs").arg(seconds * 1e6, 0, 'f', magnitude >= 1e-5 ? 1 : 2);
    return QStringLiteral("%1ns").arg(seconds * 1e9, 0, 'f', 0);
}

} // namespace mrd
