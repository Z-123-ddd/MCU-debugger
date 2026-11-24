#include "ui/ScopePage.h"

#include "core/AppSettings.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/JLinkDllProbe.h"
#include "core/ProbeController.h"
#include "ui/GlPlotWidget.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace mrd {

namespace {

/// armclang wraps an array in an anonymous struct, so a `short[256]` surfaces as the
/// member `__elems_` of `error_table`. The member name is an implementation detail;
/// the user asked for `error_table`, and that is what the table and legend must say.
QString displayNameFor(const QString &path)
{
    QString name = path;
    const QString suffix = QStringLiteral(".__elems_");
    if (name.endsWith(suffix))
        name.chop(suffix.size());
    return name;
}

/// Last component of a dotted path, for a column that would otherwise repeat a long
/// prefix on every one of hundreds of rows.
/// Short, human label for a path: the last two components when there are two.
///
/// `encoders[0].pos_estimate_.content_` becomes `pos_estimate_.content_` and
/// `g_points[1].x` stays `g_points[1].x`. One component alone is often ambiguous --
/// `content_` appears 34 times on a real image, `x` once per array element -- so a
/// single leaf name is not enough to tell rows apart. Uniqueness is still verified by
/// the caller, which falls back to the full path when two rows collide.
QString shortNameFor(const QString &path)
{
    const QString full = displayNameFor(path);
    const int lastDot = full.lastIndexOf(QLatin1Char('.'));
    if (lastDot < 0)
        return full;
    const int prevDot = full.lastIndexOf(QLatin1Char('.'), lastDot - 1);
    if (prevDot < 0)
        return full;
    QString label = full.mid(prevDot + 1);
    // `g_points.[0].x` style paths can leave a stray separator; tidy it.
    label.replace(QLatin1String(".["), QLatin1String("["));
    return label;
}

/// Decoded value as a human reads it: integers without a trailing `.0`, floats with
/// enough significant digits to see the trend but not the double's noise.
QString formatValue(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("—");
    // A whole number that fits 64 bits is an integer, whatever type it came from --
    // printing `100` instead of `100.000000` is what makes a table scannable.
    if (std::fabs(value) < 9.007199254740992e15 && std::floor(value) == value)
        return QString::number(static_cast<qint64>(value));
    return QString::number(value, 'g', 9);
}

/// The same value as a hex word.
///
/// Integer values show as the raw 32-bit word (what a register-level user expects);
/// anything else shows the IEEE-754 bit pattern of the float, which is how a `float`
/// variable's storage actually looks in memory.
QString formatValueHex(double value)
{
    if (!std::isfinite(value))
        return QString();
    if (std::fabs(value) < 4294967296.0 && std::floor(value) == value) {
        const quint32 bits = static_cast<quint32>(static_cast<qint64>(value));
        return QStringLiteral("0x%1").arg(bits, 8, 16, QLatin1Char('0')).toUpper()
            .replace(QLatin1String("0X"), QLatin1String("0x"));
    }
    const float asFloat = static_cast<float>(value);
    quint32 bits = 0;
    std::memcpy(&bits, &asFloat, sizeof(bits));
    return QStringLiteral("0x%1").arg(bits, 8, 16, QLatin1Char('0')).toUpper()
        .replace(QLatin1String("0X"), QLatin1String("0x"));
}

} // namespace

ScopePage::ScopePage(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // --- symbol file --------------------------------------------------------
    auto *fileRow = new QHBoxLayout();
    fileRow->setSpacing(6);
    fileRow->addWidget(new QLabel(QStringLiteral("符号文件:"), this));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(
        QStringLiteral("尚未导入 —— 选择 Keil 生成的 .axf（需勾选 Debug Information）"));
    fileRow->addWidget(m_pathEdit, 1);
    m_browseButton = new QPushButton(QStringLiteral("导入 .axf…"), this);
    fileRow->addWidget(m_browseButton);
    root->addLayout(fileRow);
    connect(m_browseButton, &QPushButton::clicked, this, &ScopePage::onBrowse);

    // --- variable selector --------------------------------------------------
    // This page owns the selector; MainWindow puts it in the left dock next to the
    // peripheral tree, so it is a tab there rather than a pane competing for width.
    m_selectorBox = new QWidget(nullptr);
    auto *listGroup = new QGroupBox(QStringLiteral("可采样变量（全局 / 静态）"), m_selectorBox);
    auto *selectorLayout = new QVBoxLayout(m_selectorBox);
    selectorLayout->setContentsMargins(0, 0, 0, 0);
    selectorLayout->addWidget(listGroup);

    auto *listLayout = new QVBoxLayout(listGroup);
    listLayout->setContentsMargins(6, 6, 6, 6);
    listLayout->setSpacing(4);

    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);
    m_filterEdit = new QLineEdit(listGroup);
    m_filterEdit->setPlaceholderText(
        QStringLiteral("按名字 / 类型 / 地址过滤，例如 pos_abs 或 float"));
    filterRow->addWidget(m_filterEdit, 1);
    m_showSystemCheck = new QCheckBox(QStringLiteral("含芯片包"), listGroup);
    m_showSystemCheck->setToolTip(
        QStringLiteral("勾选后同时显示芯片包 / 编译器头文件里的变量"
                       "（CMSIS 查找表、libc++ 内部量等）。默认隐藏，"
                       "否则它们会淹没项目自己的变量。"));
    m_showSystemCheck->setChecked(m_showSystemFiles);
    filterRow->addWidget(m_showSystemCheck);
    m_selectAll = new QCheckBox(QStringLiteral("全选"), listGroup);
    filterRow->addWidget(m_selectAll);
    listLayout->addLayout(filterRow);

    m_variableTree = new QTreeWidget(listGroup);
    m_variableTree->setColumnCount(4);
    m_variableTree->setHeaderLabels({QStringLiteral("变量"), QStringLiteral("类型"),
                                     QStringLiteral("地址"), QStringLiteral("大小")});
    m_variableTree->setRootIsDecorated(true);
    m_variableTree->setIndentation(14);
    m_variableTree->setExpandsOnDoubleClick(true);
    m_variableTree->setUniformRowHeights(true);
    m_variableTree->setAlternatingRowColors(true);
    m_variableTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_variableTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_variableTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    listLayout->addWidget(m_variableTree, 1);

    connect(m_filterEdit, &QLineEdit::textChanged, this, &ScopePage::onFilterChanged);
    connect(m_showSystemCheck, &QCheckBox::checkStateChanged, this,
            [this](Qt::CheckState state) {
                m_showSystemFiles = (state == Qt::Checked);
                rebuildVariableList();
            });
    connect(m_selectAll, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
        const bool on = (state == Qt::Checked);
        for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
            m_variableTree->topLevelItem(i)->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(m_variableTree, &QTreeWidget::itemChanged, this, &ScopePage::onVariableToggled);
    connect(m_variableTree, &QTreeWidget::itemExpanded, this, &ScopePage::onItemExpanded);

    m_filterTimer.setSingleShot(true);
    m_filterTimer.setInterval(220);
    connect(&m_filterTimer, &QTimer::timeout, this, &ScopePage::onFilterDebounced);

    // The selection drives the next snapshot -- AND, while sampling, the live session:
    // the HSS block list is fixed at start(), so a changed selection has to rebuild it.
    // A single tick on a struct fires one itemChanged per descendant, so the rebuild is
    // debounced instead of restarting per row.
    m_selectionTimer.setSingleShot(true);
    m_selectionTimer.setInterval(150);
    connect(&m_selectionTimer, &QTimer::timeout, this, &ScopePage::onSelectionSettled);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    root->addWidget(m_stateLabel);

    // --- live capture via the J-Link DLL's HSS engine -----------------------
    // The chart's only source: HSS samples on the probe's own timebase while the core
    // keeps running.
    auto *liveRow = new QHBoxLayout();
    liveRow->setSpacing(6);
    m_liveButton = new QPushButton(QStringLiteral("开始实时采样"), this);
    m_liveButton->setToolTip(
        QStringLiteral("用 J-Link DLL 的 HSS 后台采样：目标继续运行，曲线实时刷新。\n"
                       "最多 %1 个变量；数组只采第 0 个元素。").arg(HssSampler::maxVariables()));
    liveRow->addWidget(m_liveButton);

    m_pauseButton = new QPushButton(QStringLiteral("暂停"), this);
    m_pauseButton->setToolTip(
        QStringLiteral("暂停采样与重绘。已画出的曲线和当前缩放都保留，继续采样从原时间轴接着走。"));
    m_pauseButton->setEnabled(false);
    liveRow->addWidget(m_pauseButton);

    m_clearButton = new QPushButton(QStringLiteral("清空"), this);
    m_clearButton->setToolTip(QStringLiteral("丢弃已采集的数据，图表回到空白"));
    liveRow->addWidget(m_clearButton);

    liveRow->addWidget(new QLabel(QStringLiteral("周期(µs):"), this));
    m_periodSpin = new QSpinBox(this);
    m_periodSpin->setRange(10, 1000000);
    m_periodSpin->setValue(1000);
    m_periodSpin->setSingleStep(100);
    m_periodSpin->setToolTip(
        QStringLiteral("HSS 采样周期。越小越快，但 J-Link PLUS 这类非 Pro 型号\n"
                       "官方上限约 1 kHz，设太小反而会丢样本。"));
    liveRow->addWidget(m_periodSpin);

    // No live readout line here: the numeric detail duplicated what the crosshair shows
    // on hover, and a permanently updating line of numbers under the buttons was noise.
    // The sample count and clipping warning go to the status bar instead (see below).
    liveRow->addStretch(1);
    root->addLayout(liveRow);

    m_fftButton = new QPushButton(QStringLiteral("频谱"), this);
    m_fftButton->setCheckable(true);
    m_fftButton->setToolTip(
        QStringLiteral("在时域波形和 FFT 频谱之间切换。\n"
                       "频谱取最新 1024 点、加 Hanning 窗，纵轴为 dB。"));
    liveRow->addWidget(m_fftButton);
    m_csvButton = new QPushButton(QStringLiteral("导出 CSV…"), this);
    m_csvButton->setToolTip(QStringLiteral("把当前缓冲的采样数据写成 CSV"));
    liveRow->addWidget(m_csvButton);

    connect(m_liveButton, &QPushButton::clicked, this, &ScopePage::onToggleLiveCapture);
    connect(m_fftButton, &QPushButton::clicked, this, &ScopePage::onToggleFft);
    connect(m_csvButton, &QPushButton::clicked, this, &ScopePage::onExportCsv);
    connect(m_pauseButton, &QPushButton::clicked, this, &ScopePage::onTogglePause);
    connect(m_clearButton, &QPushButton::clicked, this, &ScopePage::onClearChart);

    // --- pick up a rebuilt .axf without asking ------------------------------
    // Keil rewrites the file on every build, and the addresses in the chart and the
    // selector silently go stale otherwise -- the chart keeps drawing whatever now
    // lives at the old address. Watching the directory as well as the file is
    // required: an editor or linker usually replaces the file rather than editing it,
    // which invalidates the file watch.
    m_symbolWatcher = new QFileSystemWatcher(this);
    connect(m_symbolWatcher, &QFileSystemWatcher::fileChanged, this,
            &ScopePage::onSymbolFileChanged);
    connect(m_symbolWatcher, &QFileSystemWatcher::directoryChanged, this,
            &ScopePage::onSymbolFileChanged);
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(400); // a build produces a burst of events
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScopePage::reloadSymbols);

    m_sampler = new HssSampler(this);
    connect(m_sampler, &HssSampler::failed, this, [this](const QString &error) {
        emit statusMessage(QStringLiteral("实时采样失败：%1").arg(error));
    });
    connect(m_sampler, &HssSampler::drained, this, &ScopePage::onLiveDrained);
    // Draining on a UI timer decouples sampling rate from repaint rate: the probe
    // fills its buffer at the configured period, and the window only redraws at ~30 Hz
    // no matter how fast samples arrive.
    // Repaint cadence only. Draining is chained (see onLiveDrained), so a slow frame
    // does not leave samples queued in the probe.
    m_liveTimer.setInterval(20);
    connect(&m_liveTimer, &QTimer::timeout, this, &ScopePage::onLiveTick);

    // --- the chart ----------------------------------------------------------
    // 10000 points at the probe's 1 kHz ceiling is ten seconds of history, which is
    // enough to catch a transient and still scroll cheaply.
    m_history = new SampleHistory(10000);
    m_plot = new GlPlotWidget(this);
    // Say it once, when it happens, instead of parking a hint on the chart forever.
    connect(m_plot, &GlPlotWidget::viewFrozen, this, [this]() {
        emit statusMessage(QStringLiteral("视图已停止跟随最新样本；双击图表恢复实时跟随"));
    });

    m_plot->setHistory(m_history);
    m_plot->setAutoScale(true);
    root->addWidget(m_plot, 1);

    updateControls();
    refreshState();
}

void ScopePage::setProbe(ProbeController *probe)
{
    m_probe = probe;
}

bool ScopePage::importSymbolFile(const QString &path, QString *error)
{
    const auto loaded = SymbolFile::load(path);
    if (!loaded) {
        if (error)
            *error = loaded.error;
        return false;
    }
    m_symbols = loaded.value;
    if (m_symbolWatcher) {
        // Remember the previous file so a switch does not keep watching it, and watch
        // the directory too: linkers replace the file, which breaks a file-only watch.
        const QStringList watched = m_symbolWatcher->files() + m_symbolWatcher->directories();
        if (!watched.isEmpty())
            m_symbolWatcher->removePaths(watched);
        m_symbolWatcher->addPath(path);
        m_symbolWatcher->addPath(QFileInfo(path).absolutePath());
    }
    m_pathEdit->setText(path);
    m_pathEdit->setToolTip(path);
    rebuildVariableList();
    refreshState();
    emit symbolsImported(m_symbols);
    emit statusMessage(QStringLiteral("已导入 %1：%2 个变量（DWARF v%3）")
                           .arg(QFileInfo(path).fileName())
                           .arg(m_symbols.variables.size())
                           .arg(m_symbols.dwarfVersion));
    return true;
}

void ScopePage::onBrowse()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Keil 生成的符号文件"), QString(),
        SymbolFile::fileFilters().join(QStringLiteral(";;")));
    if (path.isEmpty())
        return;
    QString error;
    if (!importSymbolFile(path, &error)) {
        if (m_stateLabel)
            m_stateLabel->setText(QStringLiteral("导入失败：%1").arg(error));
        emit statusMessage(QStringLiteral("导入失败：%1").arg(error));
    }
}

void ScopePage::onSymbolFileChanged()
{
    // A rebuild touches the file several times (write, rename, temp file). Wait for the
    // burst to finish instead of parsing a half-written image.
    if (m_reloadTimer.isActive())
        m_reloadTimer.stop();
    m_reloadTimer.start();
}

void ScopePage::reloadSymbols()
{
    const QString path = m_symbols.sourcePath;
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        emit statusMessage(QStringLiteral("符号文件已不存在，保留上一次的解析结果：%1")
                               .arg(QFileInfo(path).fileName()));
        return;
    }

    // Remember where the ticked variables live now, so the report can say what moved.
    m_addressesBeforeReload.clear();
    for (const WatchVariable &variable : selectedVariables())
        m_addressesBeforeReload.insert(variable.name, variable.address);

    const auto loaded = SymbolFile::load(path);
    if (!loaded) {
        // A parse failure here usually means the linker was still writing. One retry is
        // cheap and avoids dumping a scary error for a transient state.
        if (m_reloadRetries < 2) {
            ++m_reloadRetries;
            m_reloadTimer.start();
            return;
        }
        m_reloadRetries = 0;
        emit statusMessage(QStringLiteral("自动重新导入失败，继续使用上一次的结果：%1")
                               .arg(loaded.error));
        return;
    }
    m_reloadRetries = 0;

    const int previousCount = m_symbols.variables.size();
    m_symbols = loaded.value;
    rebuildVariableList();

    // Count what happened to the selection: the whole point of reloading is that the
    // addresses changed, and silently keeping the old ones would plot the wrong memory.
    int moved = 0;
    int lost = 0;
    for (auto it = m_addressesBeforeReload.constBegin();
         it != m_addressesBeforeReload.constEnd(); ++it) {
        const WatchVariable *now = m_symbols.find(it.key());
        if (!now) {
            ++lost;
            continue;
        }
        if (now->address != it.value())
            ++moved;
    }

    // A running session samples absolute addresses, so it must be rebuilt or it keeps
    // reading the pre-rebuild layout.
    if (m_sampler && m_probe && m_sampler->isRunning()) {
        const QVector<WatchVariable> chosen = selectedVariables();
        m_liveTimer.stop();
        m_sampler->stop();
        if (m_history) {
            QStringList names;
            for (const WatchVariable &variable : chosen) {
                if (names.size() >= HssSampler::maxVariables())
                    break;
                names << variable.name;
            }
            m_history->clear();
            m_history->setTraceNames(names);
        }
        if (!chosen.isEmpty()) {
            if (const Status st = m_sampler->start(chosen, m_sampler->periodUs()); st) {
                m_paused = false;
                m_catchUpRounds = 0;
                m_liveTimer.start();
            } else {
                emit statusMessage(QStringLiteral("地址已更新，但实时采样重启失败：%1")
                                       .arg(st.error));
            }
        }
        if (m_plot) {
            m_plot->followLatest();
            m_plot->refresh();
        }
    }

    QStringList report;
    report << QStringLiteral("已自动重新导入 %1（%2 个变量，%3%4）")
                  .arg(QFileInfo(path).fileName())
                  .arg(m_symbols.variables.size())
                  .arg(previousCount)
                  .arg(QStringLiteral(" → %1").arg(m_symbols.variables.size()));
    if (!m_addressesBeforeReload.isEmpty()) {
        report << QStringLiteral("勾选的 %1 个变量中 %2 个地址已变化")
                      .arg(m_addressesBeforeReload.size())
                      .arg(moved);
        if (lost > 0)
            report << QStringLiteral("%1 个在新文件里已不存在（可能被优化掉）").arg(lost);
    }
    emit statusMessage(report.join(QStringLiteral("，")));
    refreshState();
}

void ScopePage::updateControls()
{
    const QVector<WatchVariable> chosen = selectedVariables();
    if (m_pauseButton) {
        const bool active = m_sampler && (m_sampler->isRunning() || m_paused);
        m_pauseButton->setEnabled(active);
    }
    if (m_clearButton)
        m_clearButton->setEnabled(true);

}

void ScopePage::refreshState()
{
    updateControls();

    QStringList lines;
    if (!m_symbols.isValid()) {
        lines << QStringLiteral("① 导入 Keil 生成的 .axf（需打开 Debug Information）");
    } else {
        lines << QStringLiteral("① 已导入 %1：%2 个变量")
                     .arg(QFileInfo(m_symbols.sourcePath).fileName())
                     .arg(m_symbols.variables.size());
    }
    const QVector<WatchVariable> chosen = selectedVariables();
    if (chosen.isEmpty()) {
        lines << QStringLiteral("② 在左侧「变量选择」里勾选要观察的变量");
        lines << QStringLiteral("③ 点「开始实时采样」画实时曲线；选中任意一行，第 5 列显示它现在的值");
    } else {
        int bytes = 0;
        int arrays = 0;
        int scalars = 0;
        for (const WatchVariable &variable : chosen) {
            bytes += variable.span();
            if (variable.elementCount > 1)
                ++arrays;
            else
                ++scalars;
        }
        lines << QStringLiteral("② 已选 %1 个变量（%2 个数组 / %3 个标量，共 %4 字节读一次）")
                     .arg(chosen.size())
                     .arg(arrays)
                     .arg(scalars)
                     .arg(bytes);
        lines << QStringLiteral("③ 点「开始实时采样」画实时曲线；选中任意一行，第 5 列显示它现在的值");
    }
    m_stateLabel->setText(lines.join(QLatin1Char('\n')));
}

bool ScopePage::writeHistoryCsv(const SampleHistory &history, const QString &path,
                                 QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    // Excel reads a UTF-8 CSV as mojibake unless it sees the BOM first.
    out << QChar(0xFEFF);

    // One row per sample, one column per channel. Both time units are written because
    // the history is stored in microseconds while a spreadsheet is easier to read in
    // milliseconds -- and the bare index lets a formula recover the sample rate.
    out << "index,timestamp_us,timestamp_ms";
    const QStringList names = history.traceNames();
    auto csvField = [](const QString &text) {
        if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"'))
            && !text.contains(QLatin1Char('\n')))
            return text;
        QString quoted = text;
        quoted.replace(QLatin1Char('"'), QLatin1String("\"\""));
        return QLatin1Char('"') + quoted + QLatin1Char('"');
    };
    for (const QString &name : names)
        out << ',' << csvField(name);
    out << '\n';

    const int count = history.count();
    for (int i = 0; i < count; ++i) {
        const qint64 timeUs = history.timeAt(i);
        out << i << ',' << timeUs << ','
            << QString::number(double(timeUs) / 1000.0, 'f', 3);
        for (int t = 0; t < names.size(); ++t) {
            const double value = history.valueAt(t, i);
            out << ',';
            // A gap must stay a gap in the file too, otherwise the plot of the CSV
            // differs from what the user just saw on screen.
            if (std::isfinite(value))
                out << QString::number(value, 'g', 10);
        }
        out << '\n';
    }
    file.close();
    return true;
}

void ScopePage::onTogglePause()
{
    if (!m_sampler || !m_probe)
        return;

    // ---- resume -----------------------------------------------------------
    if (m_paused) {
        const QVector<WatchVariable> chosen = selectedVariables();
        if (chosen.isEmpty()) {
            emit statusMessage(QStringLiteral("没有勾选变量，无法继续采样"));
            return;
        }
        // The selection may have changed while paused. Keeping the old history would
        // draw the new samples under the previous variable names, so drop it instead.
        QStringList expected;
        for (const WatchVariable &variable : chosen) {
            if (expected.size() >= HssSampler::maxVariables())
                break;
            expected << variable.name;
        }
        if (m_history && m_history->traceNames() != expected) {
            m_history->clear();
            m_history->setTraceNames(expected);
            if (m_plot)
                m_plot->refresh();
        }

        // Continue the timeline where the chart ends: a fresh HSS session restarts its
        // timestamp at 0, and without this offset the resumed samples would land before
        // everything already drawn.
        const qint64 offsetUs = m_history && !m_history->isEmpty() ? m_history->lastTime() : 0;
        if (const Status st = m_sampler->start(chosen, m_sampler->periodUs(), offsetUs); !st) {
            emit statusMessage(QStringLiteral("继续采样失败：%1").arg(st.error));
            return;
        }
        m_paused = false;
        m_catchUpRounds = 0;
        m_liveTimer.start();
        if (m_plot)
            m_plot->followLatest(); // resume means "show me the newest again"
        if (m_pauseButton)
            m_pauseButton->setText(QStringLiteral("暂停"));
        emit statusMessage(QStringLiteral("已继续采样（接在原时间轴之后）"));
        refreshState();
        return;
    }

    // ---- pause ------------------------------------------------------------
    if (!m_sampler->isRunning())
        return;
    m_liveTimer.stop();
    m_sampler->stop(); // stops the probe too: nothing accumulates while paused
    m_catchUpRounds = 0;
    m_paused = true;
    if (m_pauseButton)
        m_pauseButton->setText(QStringLiteral("继续"));
    // Deliberately touches neither m_history nor the view: the drawn data and the zoom
    // stay exactly as they are.
    emit statusMessage(QStringLiteral("已暂停（已画出的数据与缩放保留）"));
    refreshState();
}

void ScopePage::onClearChart()
{
    if (m_history)
        m_history->clear();
    m_lastForwardedUs = 0;

    // A running capture has to restart so its timeline begins at zero again; otherwise
    // the first new sample would sit at the old (large) timestamp and look like a gap.
    // A paused one only needs the buffer dropped -- resuming stays the user's call.
    const bool wasPaused = m_paused;
    if (m_sampler && m_probe && m_sampler->isRunning() && !wasPaused) {
        m_liveTimer.stop();
        m_sampler->stop();
        const QVector<WatchVariable> chosen = selectedVariables();
        if (!chosen.isEmpty()) {
            if (const Status st = m_sampler->start(chosen, m_sampler->periodUs(), 0); st) {
                m_paused = false;
                if (m_pauseButton)
                    m_pauseButton->setText(QStringLiteral("暂停"));
                m_liveTimer.start();
            } else {
                emit statusMessage(QStringLiteral("清空后重启采样失败：%1").arg(st.error));
            }
        }
    }

    m_catchUpRounds = 0;
    if (m_plot) {
        m_plot->followLatest();
        m_plot->refresh();
    }
    emit statusMessage(wasPaused ? QStringLiteral("已清空图表与采集数据（仍处于暂停）")
                                 : QStringLiteral("已清空图表与采集数据"));
    refreshState();
}

void ScopePage::onSelectionSettled()
{
    refreshState();

    // Only an active live session needs rebuilding; otherwise this is just bookkeeping
    // for the next snapshot/start.
    if (!m_sampler || !m_sampler->isRunning() || !m_probe)
        return;

    const QVector<WatchVariable> chosen = selectedVariables();
    if (chosen.isEmpty()) {
        // Nothing selected: stop sampling rather than keep pulling a stale set.
        m_liveTimer.stop();
        m_sampler->stop();
        m_catchUpRounds = 0;
        if (m_liveButton)
            m_liveButton->setText(QStringLiteral("开始实时采样"));
        if (m_plot)
            m_plot->refresh();
        emit statusMessage(QStringLiteral("已取消全部变量，实时采样停止"));
        return;
    }

    const int period = m_sampler->periodUs();
    // Channels change, so the old curves are meaningless -- the buffer describes a
    // different set of series. Clearing is the honest thing; keeping it would draw the
    // previous channel's samples under the new name.
    if (m_history) {
        QStringList names;
        for (const WatchVariable &variable : chosen) {
            if (names.size() >= HssSampler::maxVariables())
                break;
            names << variable.name;
        }
        m_history->clear();
        m_history->setTraceNames(names);
        m_lastForwardedUs = 0; // new session: forward from the start again
    }

    m_sampler->stop();
    if (const Status st = m_sampler->start(chosen, period); !st) {
        emit statusMessage(QStringLiteral("切换采样变量失败：%1").arg(st.error));
        return;
    }
    m_catchUpRounds = 0;
    if (m_plot) {
        m_plot->setLiveMode(true);
        m_plot->followLatest();
        m_plot->refresh();
    }
    emit statusMessage(QStringLiteral("采样变量已切换为 %1 个（%2%3）")
                           .arg(qMin(chosen.size(), HssSampler::maxVariables()))
                           .arg(m_sampler->channels().size())
                           .arg(chosen.size() > HssSampler::maxVariables()
                                    ? QStringLiteral("，超出 8 个的部分已忽略")
                                    : QString()));
}

QStringList ScopePage::liveChannelNames() const
{
    if (m_sampler && !m_sampler->channels().isEmpty()) {
        QStringList names;
        names.reserve(m_sampler->channels().size());
        for (const HssSampler::Channel &channel : m_sampler->channels())
            names << channel.name;
        return names;
    }
    if (m_history && !m_history->traceNames().isEmpty())
        return m_history->traceNames();

    // Nothing running yet: offer whatever is ticked, since that is what a start would
    // begin sampling.
    QStringList names;
    for (const WatchVariable &variable : selectedVariables()) {
        if (names.size() >= HssSampler::maxVariables())
            break;
        names << variable.name;
    }
    return names;
}

void ScopePage::requestLiveCaptureStart()
{
    if (!m_sampler)
        return;
    if (m_sampler->isRunning())
        return;
    if (selectedVariables().isEmpty()) {
        emit statusMessage(QStringLiteral("仿真需要一个数据源：请先在「变量选择」里勾选变量"));
        return;
    }
    onToggleLiveCapture(); // starts, since it is not running
}

bool ScopePage::isLiveCapturing() const
{
    return m_sampler && m_sampler->isRunning();
}

bool ScopePage::releaseHss()
{
    if (!isLiveCapturing())
        return false;
    // onToggleLiveCapture() already does the whole orderly stop (timer, DLL session,
    // button texts, keep the drawn data readable). Calling it keeps one stop path
    // instead of a second one that would drift.
    onToggleLiveCapture();
    emit statusMessage(QStringLiteral("变量曲线已释放 HSS 会话（寄存器页接管实时采样）"));
    return true;
}

void ScopePage::onToggleLiveCapture()
{
    if (!m_sampler || !m_probe)
        return;

    if (m_sampler->isRunning()) {
        m_liveTimer.stop();
        m_sampler->stop();
        m_paused = false;
        if (m_pauseButton)
            m_pauseButton->setText(QStringLiteral("暂停"));
        if (m_liveButton)
            m_liveButton->setText(QStringLiteral("开始实时采样"));
        if (m_fftButton) {
            m_fftButton->setChecked(false);
            if (m_plot)
                m_plot->setViewMode(GlPlotWidget::ViewMode::TimeDomain);
        }
        // Keep exactly what the user was looking at. Resetting here was a bug: the view
        // snapped from the 2 s window to "fit everything", which squeezed the whole
        // buffer into one screen. A stopped capture must stay inspectable -- zoom and the
        // hover read-out keep working, they are not a live-only feature.
        if (m_plot) {
            m_plot->setLiveMode(false);
            m_plot->refresh();
        }
        emit liveCaptureStateChanged(false);
        refreshLiveStatus(); // reports the final sample count once
        return;
    }

    const QVector<WatchVariable> chosen = selectedVariables();
    if (chosen.isEmpty()) {
        emit statusMessage(QStringLiteral("请先在左侧勾选要实时观察的变量"));
        return;
    }

    // HSS runs inside the DLL session, which lives on the worker thread; the UI asks
    // the controller rather than reaching for the probe object (it is unreachable).
    if (!m_probe->supportsHss()) {
        emit statusMessage(
            QStringLiteral("实时采样需要 J-Link DLL 后端（当前后端不支持 HSS）"));
        return;
    }
    m_sampler->setProbe(m_probe);

    // A fresh session starts from an empty curve, otherwise the new run would be
    // appended to the previous one and the X axis would look continuous when it is not.
    if (m_history) {
        // Register the channels *before* any data arrives: setTraceNames() clears the
        // buffer by design (a different trace set invalidates old samples), so doing it
        // after the first drain silently threw that drain away.
        QStringList names;
        for (const WatchVariable &variable : chosen) {
            if (names.size() >= HssSampler::maxVariables())
                break;
            names << variable.name;
        }
        m_history->clear();
        m_history->setTraceNames(names);
    }

    const int period = m_periodSpin ? m_periodSpin->value() : 1000;
    if (const Status st = m_sampler->start(chosen, period); !st) {
        emit statusMessage(QStringLiteral("启动实时采样失败：%1").arg(st.error));
        return;
    }

    // Sampling only produces data while the core executes, so make that explicit
    // instead of leaving the user with an empty plot and no explanation.
    m_probe->requestRun();

    if (m_liveButton)
        m_liveButton->setText(QStringLiteral("停止实时采样"));
    if (m_plot) {
        // A live trace is a scope: wheel zoom, drag pan and a hover read-out are what
        // make several kHz of samples interpretable. The snapshot view keeps them off.
        m_plot->setInteractive(true);
        m_plot->setXAxisMode(GlPlotWidget::XAxisMode::Time);
        m_plot->setSampleRateHint(1e6 / double(qMax(1, period)));
        // Enter live mode with the built-in window; the user adjusts it with the wheel.
        m_plot->setLiveMode(true);
        m_plot->followLatest();
    }
    m_catchUpRounds = 0;
    m_paused = false;
    if (m_pauseButton)
        m_pauseButton->setText(QStringLiteral("暂停"));
    m_liveTimer.start();
    emit liveCaptureStateChanged(true);
    emit statusMessage(QStringLiteral("实时采样已开始：%1 个变量 @ %2 µs")
                           .arg(qMin(chosen.size(), HssSampler::maxVariables()))
                           .arg(period));
}

void ScopePage::onLiveTick()
{
    if (!m_sampler || !m_sampler->isRunning() || !m_history || !m_plot)
        return;
    // One drain may still be in flight on the worker thread; asking again would queue
    // overlapping reads of the same buffer.
    if (m_sampler->isDrainPending())
        return;
    m_sampler->setDrainTarget(m_history);
    m_sampler->requestDrain();
}

void ScopePage::onLiveDrained(const HssSampler::Result &result)
{
    if (!m_plot || !m_history)
        return;
    if (!result.error.isEmpty()) {
        emit statusMessage(QStringLiteral("实时采样读取失败：%1").arg(result.error));
        return;
    }

    // The probe still holds records: read again immediately. Waiting for the next tick
    // is what made the trace lag behind the target after a slow frame.
    if (result.morePending && m_sampler && m_sampler->isRunning()) {
        ++m_catchUpRounds;
        // Bounded: a runaway loop here would freeze the UI on a stalled probe.
        if (m_catchUpRounds < 64 && m_sampler->requestDrain())
            ; // repaint happens when the last round lands
        else
            m_catchUpRounds = 0;
    } else {
        m_catchUpRounds = 0;
    }

    if (result.samples <= 0)
        return;

    m_plot->setXAxisMode(GlPlotWidget::XAxisMode::Time);
    m_plot->setXAxisLabel(QStringLiteral("时间"));
    m_plot->refresh();
    refreshLiveStatus();

    // Hand the same numbers to any consumer (the simulation page) before repainting, so a
    // model step sees the sample the chart is about to draw.
    emitNewLiveSamples();
}

void ScopePage::onToggleFft()
{
    if (!m_plot || !m_fftButton)
        return;
    const bool spectrum = m_fftButton->isChecked();
    m_plot->setViewMode(spectrum ? GlPlotWidget::ViewMode::Spectrum
                                 : GlPlotWidget::ViewMode::TimeDomain);
    m_plot->refresh();
    emit statusMessage(spectrum
                           ? QStringLiteral("频谱视图：横轴频率 (Hz)，纵轴幅度 (dB)")
                           : QStringLiteral("时域视图"));
}

void ScopePage::onExportCsv()
{
    if (!m_history || m_history->isEmpty()) {
        emit statusMessage(QStringLiteral("还没有可导出的数据"));
        return;
    }
    const QString base = m_symbols.sourcePath.isEmpty()
        ? QString()
        : QFileInfo(m_symbols.sourcePath).absolutePath();
    const QString suggested =
        (base.isEmpty() ? QString() : base + QStringLiteral("/"))
        + QStringLiteral("mrd_samples.csv");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出采样数据"), suggested,
        QStringLiteral("CSV files (*.csv)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!writeHistoryCsv(*m_history, path, &error)) {
        emit statusMessage(error);
        return;
    }
    emit statusMessage(QStringLiteral("已导出 %1 个采样 / %2 个变量到 %3")
                           .arg(m_history->count())
                           .arg(m_history->traceNames().size())
                           .arg(path));
}

void ScopePage::emitNewLiveSamples()
{
    if (!m_history || m_history->isEmpty())
        return;

    const QStringList names = m_history->traceNames();
    if (names.isEmpty())
        return;

    // Forward everything newer than the last sample already sent. Walking by timestamp
    // rather than by index keeps this correct across the ring buffer wrapping.
    const int total = m_history->count();
    constexpr int kMaxPerDrain = 256; // a stalled UI must not produce an unbounded burst
    QVector<QPair<qint64, int>> fresh;
    fresh.reserve(total);
    for (int i = 0; i < total; ++i) {
        const qint64 timeUs = m_history->timeAt(i);
        if (timeUs > m_lastForwardedUs)
            fresh.append({timeUs, i});
    }
    if (fresh.isEmpty())
        return;

    const int start = fresh.size() > kMaxPerDrain ? fresh.size() - kMaxPerDrain : 0;
    for (int j = start; j < fresh.size(); ++j) {
        const qint64 timeUs = fresh.at(j).first;
        const int index = fresh.at(j).second;
        QHash<QString, double> values;
        values.reserve(names.size());
        for (int t = 0; t < names.size(); ++t) {
            const double value = m_history->valueAt(t, index);
            if (std::isfinite(value))
                values.insert(names.at(t), value);
        }
        if (!values.isEmpty())
            emit liveSample(timeUs, values);
    }
    m_lastForwardedUs = fresh.last().first;
}

void ScopePage::refreshLiveStatus()
{
    // No label to fill: the chart page shows no live text line by request. What is left
    // is the rate-limited status-bar report below.
    if (!m_sampler || !m_sampler->isRunning()) {
        // Report the final count once, not on every later call.
        if (m_sampler && m_sampler->totalSamples() > 0
            && m_sampler->totalSamples() != m_reportedStopSamples) {
            m_reportedStopSamples = m_sampler->totalSamples();
            emit statusMessage(QStringLiteral("实时采样已停止（共 %1 个样本）")
                                   .arg(m_reportedStopSamples));
        }
        return;
    }

    // Report to the status bar at most once a second: the numbers used to sit in a
    // dedicated line under the buttons, which the user asked to remove from the chart
    // page. They stay discoverable here without occupying chart space.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool clipChanged = m_plot && m_plot->clippedPointCount() != m_lastClippedSeen;
    if (nowMs - m_lastStatusMs >= 1000 || clipChanged) {
        m_lastStatusMs = nowMs;
        QStringList parts;
        for (const HssSampler::Channel &channel : m_sampler->channels()) {
            const QString hex = formatValueHex(channel.latest);
            parts << QStringLiteral("%1=%2%3")
                         .arg(shortNameFor(channel.name))
                         .arg(formatValue(channel.latest),
                              hex.isEmpty() ? QString()
                                            : QStringLiteral(" (%1)").arg(hex));
        }
        QString message = QStringLiteral("实时 %1 个样本 @ %2 µs")
                              .arg(m_sampler->totalSamples())
                              .arg(m_sampler->periodUs());
        if (!parts.isEmpty())
            message += QStringLiteral(" · ") + parts.join(QStringLiteral(" · "));
        if (m_plot && m_plot->clippedPointCount() > 0) {
            m_lastClippedSeen = m_plot->clippedPointCount();
            message += QStringLiteral(" · Y 轴已忽略 %1 个极值点")
                           .arg(m_plot->clippedPointCount());
        }
        emit statusMessage(message);
    }
}

namespace {

/// Full path of a node inside the tree, spelled the way C would: `g_points[0].x`.
QString childPath(const QString &parent, const QString &name)
{
    if (parent.isEmpty())
        return name;
    if (name.startsWith(QLatin1Char('[')))
        return parent + name;
    return parent + QLatin1Char('.') + name;
}

/// Does this text match the search term (case-insensitive)?
bool matchesNeedle(const QString &text, const QString &needle)
{
    return !needle.isEmpty() && text.contains(needle, Qt::CaseInsensitive);
}

/// Does \a node or any descendant match? Used to decide whether an aggregate (and
/// its function) should stay visible when a search is active.
bool matchesSubtree(const VariableNode &node, const QString &needle)
{
    if (matchesNeedle(node.name, needle) || matchesNeedle(node.typeLabel(), needle))
        return true;
    for (const VariableNode &child : node.children) {
        if (matchesSubtree(child, needle))
            return true;
    }
    return false;
}

/// Does the type itself, or one of its members, match?
bool typeMatches(const TypeDecl &type, const QString &needle)
{
    if (matchesNeedle(type.name, needle))
        return true;
    for (const TypeMember &member : type.members) {
        if (matchesNeedle(member.name, needle))
            return true;
    }
    return false;
}

/// Multi-line tooltip for a variable row: the full path plus everything DWARF said
/// about it. Shared, so a file row's children and an expanded member look identical.
QString tooltipFor(const VariableNode &node, const QString &fullPath)
{
    QStringList tip;
    tip << fullPath;
    if (!node.sourceFile.isEmpty())
        tip << QStringLiteral("文件: %1").arg(node.sourceFile);
    tip << QStringLiteral("类型: %1（%2）").arg(node.typeLabel(), nodeKindName(node.kind));
    if (node.kind == NodeKind::Array) {
        tip << QStringLiteral("元素数: %1，每元素 %2 字节")
                   .arg(node.elementCount)
                   .arg(node.elementSize);
    }
    if (node.hasAddress && node.address != 0)
        tip << QStringLiteral("地址: %1").arg(hexAddr(node.address));
    if (node.byteSize > 0)
        tip << QStringLiteral("大小: %1 字节").arg(node.byteSize);
    if (node.samplable())
        tip << QStringLiteral("采样: %1").arg(scalarKindName(node.scalar));
    if (node.isAggregate()) {
        // Point at the number instead of leaving a struct-looking row with no
        // explanation. `OutputPort<T>` stores the value in `content_`, and that is what
        // has to be ticked -- the aggregate itself is not a single value.
        bool hasContent = false;
        bool hasPair = false;
        for (const VariableNode &child : node.children) {
            if (child.name == QLatin1String("content_"))
                hasContent = true;
            if (child.name == QLatin1String("first") || child.name == QLatin1String("second"))
                hasPair = true;
        }
        if (hasContent)
            tip << QStringLiteral("数值在 content_ 里：展开后勾选它");
        else if (hasPair)
            tip << QStringLiteral("数值是 pair：展开后勾选 first / second");
        else
            tip << QStringLiteral("聚合类型：展开后勾选里面的标量成员");
    }
    if (node.addressFromSymbol)
        tip << QStringLiteral("（地址取自链接符号表）");
    if (node.incomplete)
        tip << QStringLiteral("（布局不完整：类型仅有声明，或展开已截断）");
    return tip.join(QChar(10));
}

/// Marker stored on a placeholder child so it can be recognised and removed.
constexpr int kPlaceholderRole = Qt::UserRole + 4;
/// Index into m_groups for a file row, -1 for a variable row.
constexpr int kFileGroupRole = Qt::UserRole + 5;
/// Raw VariableNode pointer for rows whose node is not in m_nodeByPath (function
/// rows under a file). Valid because the tree is rebuilt whenever the model changes.
constexpr int kNodePtrRole = Qt::UserRole + 6;
/// True when the row decodes to a single number (i.e. it may carry a checkbox).
/// Stored on the item so the invariant "checkable implies numeric" is testable
/// without re-deriving types from label text.
constexpr int kNumericRole = Qt::UserRole + 7;

const VariableNode *nodeOf(const QTreeWidgetItem *item)
{
    if (!item)
        return nullptr;
    const QVariant v = item->data(0, kNodePtrRole);
    if (!v.isValid())
        return nullptr;
    return reinterpret_cast<const VariableNode *>(v.value<qulonglong>());
}

} // namespace

namespace {

/// Every samplable leaf path under \a node, whose own full path is \a ownPath.
void collectLeafPathsAt(const VariableNode &node, const QString &ownPath, QStringList *out)
{
    if (node.children.isEmpty()) {
        if (node.samplable())
            out->append(ownPath);
        return;
    }
    for (const VariableNode &child : node.children)
        collectLeafPathsAt(child, childPath(ownPath, child.name), out);
}

} // namespace

QTreeWidgetItem *ScopePage::makeItem(const VariableNode &node, const QString &path,
                                     QTreeWidgetItem *parent)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_variableTree);
    item->setText(0, node.name);
    item->setText(1, node.typeLabel());
    // Locals can never be sampled (their address is frame-relative), so a
    // stack-relative node simply shows no address.
    const quint32 shownAddress = node.address;
    const bool hasShownAddress = node.hasAddress && node.address != 0;
    item->setText(2, hasShownAddress ? hexAddr(shownAddress) : QString());
    item->setText(3, node.byteSize > 0 ? QStringLiteral("%1 B").arg(node.byteSize) : QString());
    item->setData(0, Qt::UserRole, path);
    item->setData(0, Qt::UserRole + 2, node.isAggregate());
    // Lazy-loading state: has this node's subtree been materialised yet?
    item->setData(0, Qt::UserRole + 3, false);
    item->setData(0, kFileGroupRole, -1);

    // A checkbox is a promise that ticking the row produces a curve, and a curve is a
    // sequence of numbers. So only a row that resolves to exactly one numeric value may
    // be ticked:
    //   * a struct/union/class/array is not a number -- no checkbox (open it instead);
    //   * a pointer is a number (an address) and stays tickable;
    //   * a local has no stable address until halted, so it is not tickable either.
    // Everything below a struct is therefore reachable, while the struct itself is not
    // -- "all data structures end in something a decimal number can express".
    // Arrays are value-only (see populateChildren): a checkbox promises a curve, and an
    // array would need one curve per element -- 256 of them on screen and 8 into HSS.
    const bool selectable = node.samplable() && !node.stackRelative
        && node.locListOffset == 0 && !node.isArray();
    // Qt's DEFAULT flags already include ItemIsUserCheckable, so OR-ing NoItemFlags
    // would leave the checkbox in place -- the bit has to be cleared explicitly.
    Qt::ItemFlags flags = item->flags();
    if (selectable)
        flags |= Qt::ItemIsUserCheckable;
    else
        flags &= ~Qt::ItemIsUserCheckable;
    item->setFlags(flags);
    item->setData(0, kNumericRole, node.samplable());

    // A scalar array has nothing to expand any more: its elements are not plotted (a
    // checkbox would promise a curve the chart cannot fit) and there is no value column
    // to read them in. So it stays one row instead of an arrow onto dead rows.
    const bool expandable = node.isAggregate()
        && !(node.isArray() && node.children.isEmpty());
    if (expandable) {
        // A placeholder child makes Qt draw the expand arrow without us building
        // the subtree. It carries no checkbox and is replaced on first expand.
        auto *placeholder = new QTreeWidgetItem(item);
        placeholder->setData(0, kPlaceholderRole, true);
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setText(0, QStringLiteral("…"));
    }
    return item;
}

QTreeWidgetItem *ScopePage::makeFileItem(const FileGroup &group, int groupIndex)
{
    auto *item = new QTreeWidgetItem(m_variableTree);
    item->setText(0, group.file);
    item->setText(1, QStringLiteral("%1 个变量").arg(group.variables.size()));
    item->setText(3, QStringLiteral("%1 个节点").arg(group.nodeCount));
    item->setData(0, Qt::UserRole, QString());
    item->setData(0, Qt::UserRole + 2, true); // aggregating: ticking it ticks all
    item->setData(0, Qt::UserRole + 3, false);
    item->setData(0, kFileGroupRole, groupIndex);
    // No checkbox: a file is not a value. Use 全选 to tick everything visible.
    item->setFlags(Qt::ItemIsEnabled);
    item->setToolTip(0, QStringLiteral("%1\n%2 个变量 / %3 个节点")
                            .arg(group.file)
                            .arg(group.variables.size())
                            .arg(group.nodeCount));

    // Same lazy trick as an aggregate: one placeholder keeps the arrow drawn.
    auto *placeholder = new QTreeWidgetItem(item);
    placeholder->setData(0, kPlaceholderRole, true);
    placeholder->setFlags(Qt::NoItemFlags);
    placeholder->setText(0, QStringLiteral("…"));
    return item;
}

void ScopePage::buildMemberRows(QTreeWidgetItem *parent, const TypeMember &member, int depth,
                                quint32 baseAddress, bool hasAddress,
                                const QString &instanceName)
{
    if (!parent || depth > 8)
        return;

    // An aggregate member is not a number, so it gets no checkbox -- but its own
    // members are shown underneath, recursively, until something numeric appears.
    // That is what makes `Encoder::config_.vel_gain` reachable: without the recursion
    // `config_` was a dead row and every field inside it was invisible.
    const bool aggregate = member.kind == ScalarKind::Composite;
    const bool numeric = !aggregate && member.byteSize > 0
        && member.kind != ScalarKind::Unknown;

    auto *item = new QTreeWidgetItem(parent);
    item->setText(0, member.name);
    item->setText(1, member.isStatic ? QStringLiteral("static %1").arg(member.typeLabel)
                                     : member.typeLabel);
    item->setText(2, hasAddress ? hexAddr(baseAddress)
                                : QStringLiteral("+%1").arg(member.byteOffset));
    item->setText(3, member.byteSize > 0 ? QStringLiteral("%1 B").arg(member.byteSize)
                                         : QString());

    if (!member.isStatic && !instanceName.isEmpty()) {
        item->setData(0, Qt::UserRole, instanceName);
        item->setData(0, Qt::UserRole + 2, aggregate);
    }

    item->setData(0, kNumericRole, numeric);
    if (numeric && hasAddress && !member.isStatic) {
        // A real sample point: one address, one number.
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_selectedLeaves.contains(instanceName) ? Qt::Checked
                                                                      : Qt::Unchecked);
        if (!m_symbols.find(instanceName)) {
            WatchVariable variable;
            variable.name = instanceName;
            variable.typeLabel = member.typeLabel;
            variable.kind = member.kind;
            variable.address = baseAddress;
            variable.elementSize = member.byteSize;
            variable.elementCount = qMax(1, member.elementCount);
            variable.totalSize = variable.elementSize * variable.elementCount;
            m_memberVariables.insert(instanceName, variable);
        }
        item->setToolTip(0, QStringLiteral("%1\n类型: %2\n地址: %3\n"
                                           "可在曲线上按十进制数值显示")
                                .arg(instanceName, member.typeLabel, hexAddr(baseAddress)));
    } else if (numeric) {
        // Numeric but no instance: nothing to read, so no checkbox. Say why.
        item->setFlags(Qt::ItemIsEnabled);
        item->setToolTip(0, QStringLiteral("%1::%2\n类型: %3\n"
                                           "该类型在 RAM 里没有全局实例，没有可读地址")
                                .arg(QStringLiteral("?"), member.name, member.typeLabel));
        return;
    } else if (aggregate) {
        item->setFlags(Qt::ItemIsEnabled);
        item->setToolTip(0, QStringLiteral("%1\n类型: %2（聚合，展开看内部标量）")
                                .arg(instanceName.isEmpty() ? member.name : instanceName,
                                     member.typeLabel));
    } else {
        item->setFlags(Qt::ItemIsEnabled);
        return;
    }

    if (!aggregate)
        return;

    // Children obey the search filter too, at every depth: otherwise a deep match
    // would drag in every sibling field of the enclosing struct.
    // Recurse through the type index: find the member's own type definition and list
    // its members at the accumulated offset.
    const TypeDecl *decl = m_symbols.typeByName(member.typeLabel, member.byteSize);
    if (!decl) {
        const QString label = instanceName.isEmpty() ? member.name : instanceName;
        item->setToolTip(
            0, QStringLiteral("%1  %2（%3 字节）\n"
                              "同名类型在符号文件里有多个版本，无法确定这一份的成员，"
                              "因此不展开（展开错的字段比不展开更糟）")
                   .arg(label, member.typeLabel)
                   .arg(member.byteSize));
        return;
    }
    for (const TypeMember &child : decl->members) {
        if (child.isStatic)
            continue; // static members are not part of this instance's layout
        if (!m_filterNeedle.isEmpty() && !matchesNeedle(child.name, m_filterNeedle)
            && !matchesNeedle(child.typeLabel, m_filterNeedle)) {
            continue;
        }
        buildMemberRows(item, child, depth + 1,
                        baseAddress + static_cast<quint32>(child.byteOffset), hasAddress,
                        childPath(instanceName, child.name));
    }
}

void ScopePage::populateChildren(QTreeWidgetItem *item)
{
    if (!item || item->data(0, Qt::UserRole + 3).toBool())
        return; // already populated
    item->setData(0, Qt::UserRole + 3, true);

    // Drop the placeholder.
    while (item->childCount() > 0) {
        QTreeWidgetItem *child = item->child(0);
        if (!child->data(0, kPlaceholderRole).toBool())
            break;
        delete item->takeChild(0);
    }

    // A file row's children are the variables declared in that file; a variable
    // row's children come from the model tree.
    // An unset role must mean "not a file row": QVariant::toInt() on an invalid
    // variant returns 0, which silently treated every other aggregate (the locals
    // header) as file group #0 and filled it with that file's variables.
    const QVariant fileRole = item->data(0, kFileGroupRole);
    const int groupIndex = fileRole.isValid() ? fileRole.toInt() : -1;
    if (groupIndex >= 0) {
        // Rows are built from the union of files (variables + functions + types), so
        // the index refers to m_fileRowGroups, not to m_groups -- using m_groups here
        // showed one file's variables under another file's row.
        if (groupIndex >= m_fileRowGroups.size())
            return;
        const QSignalBlocker fileBlocker(m_variableTree);
        const FileGroup &group = m_fileRowGroups.at(groupIndex);
        for (const VariableNode &variable : group.variables) {
            const QString fullPath = childPath(QString(), variable.name);
            QTreeWidgetItem *childItem = makeItem(variable, fullPath, item);
            if (variable.children.isEmpty() && variable.samplable()) {
                childItem->setCheckState(0, m_selectedLeaves.contains(fullPath)
                                                ? Qt::Checked
                                                : Qt::Unchecked);
            }
            childItem->setToolTip(0, tooltipFor(variable, fullPath));
        }

        // Types defined in this file, with their members.
        //
        // Members have no address of their own (only an offset inside an instance),
        // so they are not tickable -- but they are the reason someone opens
        // `encoder.cpp` in the first place: `Encoder::index_found_` must be visible
        // here, not only under whichever file declares a global instance.
        QVector<TypeDecl> types = m_symbols.typesInFile(group.file);
        // A file can define several same-named aggregates. When an instance is known,
        // only the definition with its byte size is the real one -- the others are
        // unrelated types from another scope, and showing them put a 1040-byte
        // `Encoder` (whose `config_` resolve) next to the real 1836-byte one.
        for (int t = types.size() - 1; t >= 0; --t) {
            const int instanceSize = m_typeInstanceSizes.value(types.at(t).name, 0);
            if (instanceSize > 0 && types.at(t).byteSize != instanceSize)
                types.removeAt(t);
        }
        for (const TypeDecl &type : types) {
            // When searching, a type is shown because its name or one of its members
            // matched. A name match means "show me this type", so all members are
            // listed; a member-only match shows just those, because dumping all 63
            // members of Encoder is the clutter that made search look broken.
            const bool typeNameMatched = matchesNeedle(type.name, m_filterNeedle);
            auto *typeItem = new QTreeWidgetItem(item);
            typeItem->setText(0, type.name);
            typeItem->setText(1, QStringLiteral("%1 · %2 个成员").arg(type.kind)
                                     .arg(type.members.size()));
            typeItem->setText(3, type.byteSize > 0
                                     ? QStringLiteral("%1 B").arg(type.byteSize)
                                     : QString());
            typeItem->setFlags(Qt::ItemIsEnabled);
            typeItem->setToolTip(
                0, QStringLiteral("%1 %2\n%3 个成员，共 %4 字节\n"
                                  "成员没有独立地址，只能通过实例访问（例如 encoders[0]）")
                       .arg(type.kind, type.name)
                       .arg(type.members.size())
                       .arg(type.byteSize));
            // Members are created up front: there are at most 512 and they are
            // plain rows, so lazily loading them would cost more than it saves.
            for (const TypeMember &member : type.members) {
                // Hidden when searching, unless the type name itself matched.
                if (!m_filterNeedle.isEmpty() && !typeNameMatched
                    && !matchesNeedle(member.name, m_filterNeedle)
                    && !matchesNeedle(member.typeLabel, m_filterNeedle)) {
                    continue;
                }
                if (member.isStatic) {
                    // A static member is a real variable with its own address, so it
                    // is selectable like any global -- which is what "all static
                    // variables" means for something like Encoder::ports_to_sample.
                    auto *staticItem = new QTreeWidgetItem(typeItem);
                    staticItem->setText(0, member.name);
                    staticItem->setText(1, QStringLiteral("static %1").arg(member.typeLabel));
                    staticItem->setText(2, member.hasAddress ? hexAddr(member.address)
                                                             : QStringLiteral("?"));
                    staticItem->setText(3, member.byteSize > 0
                                             ? QStringLiteral("%1 B").arg(member.byteSize)
                                             : QString());
                    const QString staticKey =
                        QStringLiteral("%1::%2").arg(type.name, member.name);
                    staticItem->setData(0, Qt::UserRole, staticKey);
                    staticItem->setData(0, Qt::UserRole + 2, false);
                    // Same rule as everywhere else: only a single number may be
                    // ticked. A `static` aggregate (CMSIS `property_table`, a
                    // `singleton`) has an address but is not a value, so its members
                    // are what the user would sample.
                    const bool staticNumeric = member.kind != ScalarKind::Composite
                        && member.byteSize > 0 && member.kind != ScalarKind::Unknown;
                    staticItem->setData(0, kNumericRole, staticNumeric);
                    {
                        Qt::ItemFlags staticFlags = staticItem->flags();
                        if (member.hasAddress && staticNumeric)
                            staticFlags |= Qt::ItemIsUserCheckable;
                        else
                            staticFlags &= ~Qt::ItemIsUserCheckable;
                        staticItem->setFlags(staticFlags);
                    }
                    if (member.hasAddress && staticNumeric) {
                        staticItem->setCheckState(
                            0, m_selectedLeaves.contains(staticKey) ? Qt::Checked
                                                                    : Qt::Unchecked);
                        // A real sample point, so ticking it produces a curve.
                        WatchVariable variable;
                        variable.name = staticKey;
                        variable.typeLabel = member.typeLabel;
                        variable.kind = member.kind;
                        variable.address = member.address;
                        variable.elementSize = member.byteSize > 0 ? member.byteSize : 1;
                        variable.elementCount = qMax(1, member.elementCount);
                        variable.totalSize = variable.elementSize * variable.elementCount;
                        m_memberVariables.insert(staticKey, variable);
                    } else {
                        staticItem->setFlags(Qt::ItemIsEnabled);
                    }
                    staticItem->setToolTip(
                        0, QStringLiteral("%1::%2  (static)\n类型: %3\n%4\n"
                                          "静态成员有自己的地址，可像全局变量一样采样")
                               .arg(type.name, member.name, member.typeLabel,
                                    member.hasAddress
                                        ? QStringLiteral("地址: %1").arg(hexAddr(member.address))
                                        : QStringLiteral("地址未知（链接期解析或已优化）")));
                    continue;
                }
                // An instance member lives inside an object, so it is usable only when
                // a global instance of the type is known: `Encoder::index_found_` is
                // `encoders[0].index_found_` at 0x2000A11A.
                const auto instanceIt = m_typeInstances.constFind(type.name);
                const bool hasInstance = instanceIt != m_typeInstances.constEnd();
                const QString instancePath = hasInstance ? instanceIt.value().first : QString();
                const quint32 instanceAddress = hasInstance ? instanceIt.value().second : 0;
                buildMemberRows(typeItem, member, 0,
                                instanceAddress + static_cast<quint32>(member.byteOffset),
                                hasInstance,
                                hasInstance ? childPath(instancePath, member.name) : QString());
            }
        }

        // Then the functions of this file, so their locals are reachable.
        QVector<VariableNode> &functions = m_functionsByFile[group.file];
        for (VariableNode &function : functions) {
            const QString funcPath = childPath(QString(), function.name);
            QTreeWidgetItem *funcItem = makeItem(function, funcPath, item);
            funcItem->setData(0, kNodePtrRole,
                              QVariant::fromValue<qulonglong>(
                                  static_cast<qulonglong>(reinterpret_cast<quintptr>(&function))));
            funcItem->setText(1, QStringLiteral("函数 · %1 个局部变量")
                                     .arg(function.children.size()));
            funcItem->setToolTip(
                0, QStringLiteral("%1()\n%2 个局部变量\n只在停机且停在该函数内时可读")
                       .arg(function.name)
                       .arg(function.children.size()));
        }
        recomputeAncestors(item->child(0));
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    // A function row is not in m_nodeByPath; it carries its own pointer.
    const VariableNode *node = nodeOf(item);
    if (!node)
        node = m_nodeByPath.value(path, nullptr);
    // Search-result rows have no tree node. They used to expand into array elements;
    // with the value column gone there is nothing to show, so they simply do not expand.
    if (!node)
        return;

    if (node->children.isEmpty())
        return;

    const QSignalBlocker blocker(m_variableTree);
    // When searching, only the matching members are materialised. The node itself
    // survived because something below it matched, but showing all of its children
    // would bury the match -- the reported clutter.
    const bool selfMatched = !m_filterNeedle.isEmpty() && node->kind == NodeKind::Function
        && matchesNeedle(node->name, m_filterNeedle);
    for (const VariableNode &child : node->children) {
        if (!m_filterNeedle.isEmpty() && !selfMatched
            && !matchesSubtree(child, m_filterNeedle)) {
            continue;
        }
        const QString childFullPath = childPath(path, child.name);
        QTreeWidgetItem *childItem = makeItem(child, childFullPath, item);
        if (child.children.isEmpty() && child.samplable()) {
            childItem->setCheckState(0, m_selectedLeaves.contains(childFullPath)
                                            ? Qt::Checked
                                            : Qt::Unchecked);
        }
        childItem->setToolTip(0, tooltipFor(child, childFullPath));
    }
    // Newly created children can flip this node between checked/partial/unchecked.
    recomputeAncestors(item->child(0));
}

void ScopePage::rebuildVariableList()
{
    const QSignalBlocker blocker(m_variableTree);
    m_variableTree->clear();
    m_nodeByPath.clear();
    m_memberVariables.clear();

    const QString needle = m_filterEdit->text().trimmed();
    m_filterNeedle = needle;
    m_visibleRoots = m_symbols.filteredRoots(needle);

    // Regroup for display. Grouping is done here rather than in the model because
    // the filter changes which variables are visible, but the model itself must stay
    // a plain forest (sampling and the frame layout work off it).
    {
        QMap<QString, FileGroup> byFile;
        std::function<void(const VariableNode &)> add = [&](const VariableNode &node) {
            const QString key = node.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                          : node.sourceFile;
            FileGroup &group = byFile[key];
            group.file = key;
            group.variables.append(node);
            group.nodeCount += node.totalNodeCount();
        };
        for (const VariableNode &root : m_visibleRoots)
            add(root);
        m_groups.clear();
        m_groups.reserve(byFile.size());
        FileGroup unknown;
        for (auto it = byFile.constBegin(); it != byFile.constEnd(); ++it) {
            if (it.key() == QStringLiteral("(未知文件)"))
                unknown = it.value();
            else
                m_groups.append(it.value());
        }
        if (unknown.nodeCount > 0)
            m_groups.append(unknown); // always last
    }

    // Index every visible node by path so a lazy expansion can find its subtree.
    struct Index
    {
        static void run(const VariableNode &node, const QString &prefix,
                        QHash<QString, const VariableNode *> *out)
        {
            const QString path = childPath(prefix, node.name);
            out->insert(path, &node);
            for (const VariableNode &child : node.children)
                run(child, path, out);
        }
    };
    for (const VariableNode &root : m_visibleRoots)
        Index::run(root, QString(), &m_nodeByPath);

    // Which global instance can stand in for a type? Needed to make the members
    // listed under a type row usable: `Encoder::index_found_` only means something
    // as `encoders[0].index_found_`. The first instance found (lowest address, since
    // the forest is address-sorted) wins, and the choice is visible in the tooltip.
    m_typeInstances.clear();
    m_typeInstanceSizes.clear();
    {
        struct Scan
        {
            static void run(const VariableNode &node, const QString &prefix,
                            QHash<QString, QPair<QString, quint32>> *out,
                            QHash<QString, int> *sizes)
            {
                const QString path = childPath(prefix, node.name);
                const bool aggregate = node.kind == NodeKind::Struct
                    || node.kind == NodeKind::Class || node.kind == NodeKind::Union;
                if (aggregate && node.hasAddress && node.address != 0
                    && !node.typeName.isEmpty() && !out->contains(node.typeName)) {
                    out->insert(node.typeName, qMakePair(path, node.address));
                    // The instance's own size identifies which same-named definition
                    // describes it (`Encoder` exists in several scopes).
                    sizes->insert(node.typeName, node.byteSize);
                }
                for (const VariableNode &child : node.children)
                    run(child, path, out, sizes);
            }
        };
        for (const VariableNode &root : m_visibleRoots)
            Scan::run(root, QString(), &m_typeInstances, &m_typeInstanceSizes);
    }

    // Functions with locals, grouped by their source file. These are NOT in the model
    // forest (that is globals/statics only), which is exactly why a local such as
    // `match_indx` was unfindable: it is function-scope data.
    //
    // The filter applies here too. It used to stop at the variable level, so searching
    // for a name still listed every function and every type in every file -- the
    // search looked broken because nothing ever disappeared.
    m_functionsByFile.clear();
    for (const FunctionScope &scope : m_symbols.functions) {
        if (scope.locals.isEmpty())
            continue;
        const QString file = scope.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                        : scope.sourceFile;
        // A function matches when its own name does, or when one of its locals does
        // (so searching a local name leads to the function holding it).
        bool matches = needle.isEmpty() || matchesNeedle(scope.name, needle)
            || file.contains(needle, Qt::CaseInsensitive);
        if (!matches) {
            for (const VariableNode &local : scope.locals) {
                if (matchesSubtree(local, needle)) {
                    matches = true;
                    break;
                }
            }
        }
        if (!matches)
            continue;

        VariableNode functionNode;
        functionNode.name = scope.name;
        functionNode.typeName = QStringLiteral("函数");
        functionNode.kind = NodeKind::Function;
        functionNode.children = scope.locals;
        functionNode.sourceFile = scope.sourceFile;
        m_functionsByFile[file].append(functionNode);
    }

    // The row list is a union: files with variables, files with functions, and files
    // that only define types (headers). Leaving headers out made `Encoder` -- and
    // therefore `index_found_` -- unreachable from the tree.
    {
        QSet<QString> files;
        for (const FileGroup &group : m_groups)
            files.insert(group.file);
        for (auto it = m_functionsByFile.constBegin(); it != m_functionsByFile.constEnd();
             ++it) {
            files.insert(it.key());
        }
        // Types always contribute their file: `encoder.hpp` defines `Encoder`, and a
        // user file must never disappear just because the header holds no variables.
        // Whether the file is vendor code is decided by the system filter below.
        for (const TypeDecl &type : m_symbols.types) {
            if (!needle.isEmpty() && !typeMatches(type, needle))
                continue;
            files.insert(type.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                   : type.sourceFile);
        }

        // With a search active, keep only files that actually contain a match in
        // their own name, or hold something that survived the per-item filters.
        if (!needle.isEmpty()) {
            QSet<QString> keep;
            for (const FileGroup &group : m_groups) {
                if (!group.variables.isEmpty())
                    keep.insert(group.file);
            }
            for (auto it = m_functionsByFile.constBegin();
                 it != m_functionsByFile.constEnd(); ++it) {
                if (!it.value().isEmpty())
                    keep.insert(it.key());
            }
            for (const TypeDecl &type : m_symbols.types) {
                if (typeMatches(type, needle)) {
                    keep.insert(type.sourceFile.isEmpty() ? QStringLiteral("(未知文件)")
                                                          : type.sourceFile);
                }
            }
            // A filename that matches shows everything inside it -- that is what a
            // user typing a file name expects.
            for (const QString &file : files) {
                if (file.contains(needle, Qt::CaseInsensitive))
                    keep.insert(file);
            }
            files = keep;
        }
        m_allFiles = QStringList(files.constBegin(), files.constEnd());
        // Order: files that declare variables first, then files with functions, then
        // headers that only define types. Alphabetical order alone pushed libc++
        // internals like `__bit_reference` above `adc.c`, which is backwards for a
        // user looking for their own code.
        QSet<QString> withVariables;
        for (const FileGroup &group : m_groups) {
            if (!group.variables.isEmpty())
                withVariables.insert(group.file);
        }
        QSet<QString> withFunctions;
        for (auto it = m_functionsByFile.constBegin(); it != m_functionsByFile.constEnd();
             ++it) {
            if (!it.value().isEmpty())
                withFunctions.insert(it.key());
        }
        std::sort(m_allFiles.begin(), m_allFiles.end(),
                  [&](const QString &a, const QString &b) {
                      const int rankA = withVariables.contains(a) ? 0
                          : (withFunctions.contains(a) ? 1 : 2);
                      const int rankB = withVariables.contains(b) ? 0
                          : (withFunctions.contains(b) ? 1 : 2);
                      if (rankA != rankB)
                          return rankA < rankB;
                      return a.compare(b, Qt::CaseInsensitive) < 0;
                  });
        // "(未知文件)" always last so a real grouping is never buried.
        const QString unknown = QStringLiteral("(未知文件)");
        if (m_allFiles.removeAll(unknown) > 0)
            m_allFiles.append(unknown);
    }

    // Top level is one row per source file; the variables in it are created when the
    // file is opened. A flat 637-row list of names was unusable; file -> variable is
    // how the user knows where to look.
    // Chip-pack / compiler headers are hidden by default: they contain vendor tables
    // (CMSIS `Weights_*`, libc++ internals) that are not project state.
    if (!m_showSystemFiles) {
        QStringList userFiles;
        for (const QString &file : m_allFiles) {
            if (m_symbols.isSystemFile(file))
                continue;
            userFiles << file;
        }
        m_allFiles = userFiles;
    }

    // --- flat search results -------------------------------------------------
    // A search should hand back the numbers themselves. Leaving them nested inside
    // `file -> encoders[0] -> config_ -> ... -> leaf` means several clicks for a result
    // the user already typed by name.
    if (!needle.isEmpty()) {
        QVector<const WatchVariable *> hits;
        for (const WatchVariable &variable : m_symbols.variables) {
            if (!variable.usable())
                continue;
            if (matchesNeedle(variable.name, needle) || matchesNeedle(variable.typeLabel, needle))
                hits.append(&variable);
        }

        if (!hits.isEmpty()) {
            auto *group = new QTreeWidgetItem(m_variableTree);
            group->setText(0, QStringLiteral("搜索结果（%1 个可勾选）").arg(hits.size()));
            group->setFirstColumnSpanned(true);
            group->setFlags(Qt::ItemIsEnabled);
            group->setToolTip(0, QStringLiteral("直接勾选即加入采集，无需展开层级"));

            // Labels must be unambiguous: two rows reading `x` are useless. Compute the
            // short form for all hits, then give any colliding label its full path.
            QStringList labels;
            labels.reserve(hits.size());
            {
                QHash<QString, int> counts;
                for (const WatchVariable *variable : hits)
                    ++counts[shortNameFor(variable->name)];
                for (const WatchVariable *variable : hits) {
                    const QString shortLabel = shortNameFor(variable->name);
                    labels << (counts.value(shortLabel) > 1 ? displayNameFor(variable->name)
                                                            : shortLabel);
                }
            }

            for (int hitIndex = 0; hitIndex < hits.size(); ++hitIndex) {
                const WatchVariable *variable = hits.at(hitIndex);
                auto *row = new QTreeWidgetItem(group);
                row->setText(0, labels.at(hitIndex));
                row->setText(1, variable->typeLabel);
                row->setText(2, hexAddr(variable->address));
                row->setText(3, QStringLiteral("%1 B").arg(variable->span()));
                row->setToolTip(0, variable->name);

                // Same rule as a tree row: only a single number may be ticked, so an
                // array found by search gets no checkbox either. Granting it here would
                // have let a 256-element array eat the whole 8-variable HSS budget.
                Qt::ItemFlags flags = row->flags();
                if (variable->elementCount > 1)
                    flags &= ~Qt::ItemIsUserCheckable;
                else
                    flags |= Qt::ItemIsUserCheckable;
                row->setFlags(flags);
                if (variable->elementCount <= 1) {
                    row->setCheckState(0, m_selectedLeaves.contains(variable->name)
                                             ? Qt::Checked
                                             : Qt::Unchecked);
                }
                // Register it as a real sample point, exactly like a member row.
                m_memberVariables.insert(variable->name, *variable);
            }
            group->setExpanded(true);
        }
    }

    for (const QString &file : m_allFiles) {
        // Find the variables group for this file (may be absent for a header).
        FileGroup empty;
        empty.file = file;
        const FileGroup *group = &empty;
        for (const FileGroup &candidate : m_groups) {
            if (candidate.file == file) {
                group = &candidate;
                break;
            }
        }
        m_fileRowGroups.append(*group);
        makeFileItem(m_fileRowGroups.last(), m_fileRowGroups.size() - 1);
    }

    // A filter that matched inside a file should reveal the path to the match,
    // otherwise the user sees collapsed rows and cannot tell the search worked.
    if (!needle.isEmpty()) {
        for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_variableTree->topLevelItem(i);
            item->setExpanded(true);
            populateChildren(item);
            // Show the matched variables themselves (aggregates stay collapsed so a
            // big struct does not explode the view).
            for (int j = 0; j < item->childCount(); ++j) {
                QTreeWidgetItem *child = item->child(j);
                if (child->data(0, Qt::UserRole + 2).toBool())
                    continue;
                child->setHidden(false);
            }
        }
    }
    Q_UNUSED(blocker);

    // The tree was replaced wholesale, so the poller's idea of "what is on screen" is
    // stale. Background targets cover every addressable leaf, which is what makes the
    // 值 column work for rows that have never been expanded.
}

void ScopePage::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    // Rebuilding a 637-root tree per keystroke stutters; wait for a pause.
    m_filterTimer.start();
}

void ScopePage::filterForBench(const QString &text)
{
    m_filterEdit->setText(text);
    rebuildVariableList();
}

void ScopePage::onFilterDebounced()
{
    rebuildVariableList();
}

void ScopePage::onItemExpanded(QTreeWidgetItem *item)
{
    populateChildren(item);
}

QSet<QString> ScopePage::selectedLeafNames() const
{
    // The set is maintained by onVariableToggled from the model, so it also holds
    // leaves whose widgets have never been created. That is what makes "tick a
    // 500-member struct without expanding it" work.
    return m_selectedLeaves;
}

void ScopePage::applySelectionToItem(QTreeWidgetItem *item, const VariableNode &node,
                                    const QString &path)
{
    if (!item)
        return;
    if (node.children.isEmpty()) {
        if (node.samplable()) {
            item->setCheckState(0, m_selectedLeaves.contains(path) ? Qt::Checked
                                                                  : Qt::Unchecked);
        }
        return;
    }

    // Children were created in model order and skip the placeholder, so a simple
    // index walk keeps widget and model aligned.
    int modelIndex = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (child->data(0, kPlaceholderRole).toBool())
            continue;
        if (modelIndex >= node.children.size())
            break;
        applySelectionToItem(child, node.children.at(modelIndex),
                             child->data(0, Qt::UserRole).toString());
        ++modelIndex;
    }

    // An aggregate's own state is derived from the children it has materialised.
    int checked = 0;
    int unchecked = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        const QTreeWidgetItem *child = item->child(i);
        if (!(child->flags() & Qt::ItemIsUserCheckable))
            continue;
        if (child->checkState(0) == Qt::Checked)
            ++checked;
        else if (child->checkState(0) == Qt::Unchecked)
            ++unchecked;
    }
    const int total = checked + unchecked;
    Qt::CheckState state = Qt::PartiallyChecked;
    if (total == 0)
        state = Qt::Unchecked;
    else if (checked == total)
        state = Qt::Checked;
    else if (unchecked == total)
        state = Qt::Unchecked;
    item->setCheckState(0, state);
}

void ScopePage::setSubtreeChecked(QTreeWidgetItem *item, Qt::CheckState state)
{
    if (!item)
        return;
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (child->data(0, kPlaceholderRole).toBool())
            continue;
        if (child->flags() & Qt::ItemIsUserCheckable)
            child->setCheckState(0, state);
        setSubtreeChecked(child, state);
    }
}

void ScopePage::recomputeAncestors(QTreeWidgetItem *item)
{
    // A parent is Checked when every materialised child is Checked, Unchecked when
    // all are Unchecked, otherwise PartiallyChecked. Children that have not been
    // created yet (lazy tree) do not participate.
    for (QTreeWidgetItem *parent = item ? item->parent() : nullptr; parent;
         parent = parent->parent()) {
        if ((parent->flags() & Qt::ItemIsUserCheckable) == 0)
            continue;
        int checked = 0;
        int unchecked = 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            const QTreeWidgetItem *child = parent->child(i);
            if (!(child->flags() & Qt::ItemIsUserCheckable))
                continue; // the lazy placeholder
            const Qt::CheckState state = child->checkState(0);
            if (state == Qt::Checked)
                ++checked;
            else if (state == Qt::Unchecked)
                ++unchecked;
        }
        const int total = checked + unchecked;
        Qt::CheckState next = Qt::PartiallyChecked;
        if (total == 0)
            next = Qt::Unchecked;
        else if (checked == total)
            next = Qt::Checked;
        else if (unchecked == total)
            next = Qt::Unchecked;
        if (parent->checkState(0) != next) {
            const QSignalBlocker blocker(parent->treeWidget());
            parent->setCheckState(0, next);
        }
    }
}

void ScopePage::onVariableToggled(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || m_syncingChecks)
        return;

    // Propagate in both directions ourselves. Qt's auto-tristate only manages
    // children that already exist, which does not hold for a lazily built tree.
    const QScopedValueRollback<bool> guard(m_syncingChecks, true);

    const Qt::CheckState state = item->checkState(0);
    const QString path = item->data(0, Qt::UserRole).toString();
    const bool aggregate = item->data(0, Qt::UserRole + 2).toBool();

    if (aggregate && state != Qt::PartiallyChecked) {
        setSubtreeChecked(item, state);
        // Record the choice against every leaf of this subtree, including the ones
        // whose widgets do not exist yet -- otherwise ticking a collapsed struct
        // would sample nothing.
        QStringList leaves;
        const QVariant fileRole = item->data(0, kFileGroupRole);
        const int groupIndex = fileRole.isValid() ? fileRole.toInt() : -1;
        if (groupIndex >= 0 && groupIndex < m_groups.size()) {
            // A file row: every variable declared in that file.
            for (const VariableNode &variable : m_groups.at(groupIndex).variables) {
                collectLeafPathsAt(variable, childPath(QString(), variable.name), &leaves);
            }
        } else if (const VariableNode *node = m_nodeByPath.value(path, nullptr)) {
            collectLeafPathsAt(*node, path, &leaves);
        }
        for (const QString &leaf : leaves) {
            if (state == Qt::Checked)
                m_selectedLeaves.insert(leaf);
            else
                m_selectedLeaves.remove(leaf);
        }
    } else if (!aggregate) {
        if (state == Qt::Checked)
            m_selectedLeaves.insert(path);
        else
            m_selectedLeaves.remove(path);
    }
    recomputeAncestors(item);

    // A single tick on a big struct produces one itemChanged per descendant; fold
    // them into one frame-layout recomputation.
    if (!m_selectionTimer.isActive())
        m_selectionTimer.start();
}

QVector<WatchVariable> ScopePage::selectedVariables() const
{
    QVector<WatchVariable> chosen;
    for (const WatchVariable &variable : m_symbols.variables) {
        if (m_selectedLeaves.contains(variable.name))
            chosen.append(variable);
    }
    // Class members ticked from a type row are not in the flat list; they were
    // silently ignored before, so the checkbox did nothing.
    for (auto it = m_memberVariables.constBegin(); it != m_memberVariables.constEnd();
         ++it) {
        if (!m_selectedLeaves.contains(it.key()))
            continue;
        bool duplicate = false;
        for (const WatchVariable &already : chosen) {
            if (already.address == it.value().address && already.name == it.key()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            chosen.append(it.value());
    }
    // Frame order must match the generated firmware snippet, so keep it stable and
    // sorted by address rather than by click order.
    std::sort(chosen.begin(), chosen.end(),
              [](const WatchVariable &a, const WatchVariable &b) { return a.address < b.address; });
    return chosen;
}


int ScopePage::testSelectedCount() const
{
    return selectedVariables().size();
}

bool ScopePage::isWatchingForTest(const QString &path) const
{
    if (!m_symbolWatcher)
        return false;
    // Files and directories are separate lists: the directory watch is what survives a
    // linker replacing the .axf instead of editing it in place.
    return m_symbolWatcher->files().contains(path)
        || m_symbolWatcher->directories().contains(path);
}

QTreeWidgetItem *ScopePage::findRowForTest(const QString &label) const
{
    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item)
        -> QTreeWidgetItem * {
        if (!item)
            return nullptr;
        if (item->text(0) == label)
            return item;
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem *found = walk(item->child(i)))
                return found;
        }
        return nullptr;
    };
    for (int i = 0; i < m_variableTree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem *found = walk(m_variableTree->topLevelItem(i)))
            return found;
    }
    return nullptr;
}

} // namespace mrd
