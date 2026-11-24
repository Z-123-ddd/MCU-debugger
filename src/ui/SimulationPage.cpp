#include "ui/SimulationPage.h"

#include "ui/GlPlotWidget.h"
#include "ui/ScopePage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace mrd {

SimulationPage::SimulationPage(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // --- model selection -------------------------------------------------
    auto *modelRow = new QHBoxLayout();
    modelRow->setSpacing(6);
    m_modelPath = new QLineEdit(this);
    m_modelPath->setPlaceholderText(
        QStringLiteral("选择一个模型：.fmu（FMI 2.0 协仿真，Simulink「Export to FMU」的产物）或 .py"));
    m_browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_loadButton = new QPushButton(QStringLiteral("加载模型"), this);
    modelRow->addWidget(new QLabel(QStringLiteral("模型:"), this));
    modelRow->addWidget(m_modelPath, 1);
    modelRow->addWidget(m_browseButton);
    modelRow->addWidget(m_loadButton);
    root->addLayout(modelRow);

    m_modelLabel = new QLabel(QStringLiteral("尚未加载模型"), this);
    m_modelLabel->setWordWrap(true);
    root->addWidget(m_modelLabel);

    // --- data source note ------------------------------------------------
    auto *sourceNote = new QLabel(
        QStringLiteral("数据来源：<b>真实设备</b>。输入的每个数都是探针从 .axf 变量表里的地址读回来的，"
                       "与「变量曲线」页画的是同一批样本。本页不生成任何模拟信号。"),
        this);
    sourceNote->setWordWrap(true);
    root->addWidget(sourceNote);

    // --- mapping ---------------------------------------------------------
    auto *mappingBox = new QGroupBox(QStringLiteral("输入映射（模型输入 ← 设备变量）"), this);
    auto *mappingLayout = new QVBoxLayout(mappingBox);
    m_mapping = new QTableWidget(0, 3, mappingBox);
    m_mapping->setHorizontalHeaderLabels(
        {QStringLiteral("模型输入"), QStringLiteral("设备变量"), QStringLiteral("当前值")});
    m_mapping->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_mapping->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_mapping->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_mapping->verticalHeader()->setVisible(false);
    m_mapping->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mapping->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mapping->setMinimumHeight(110);
    mappingLayout->addWidget(m_mapping);
    root->addWidget(mappingBox);

    // --- run controls ----------------------------------------------------
    auto *runRow = new QHBoxLayout();
    runRow->setSpacing(6);
    m_startButton = new QPushButton(QStringLiteral("开始仿真"), this);
    m_startButton->setToolTip(
        QStringLiteral("用实时采样数据步进模型。\n"
                       "如果没有在采样，会先启动「变量曲线」页的采样（同一批地址）。"));
    m_clearButton = new QPushButton(QStringLiteral("清空"), this);
    runRow->addWidget(m_startButton);
    runRow->addWidget(m_clearButton);
    runRow->addWidget(new QLabel(QStringLiteral("绘制输出:"), this));
    m_outputBox = new QComboBox(this);
    m_outputBox->setMinimumWidth(140);
    runRow->addWidget(m_outputBox);
    runRow->addWidget(new QLabel(QStringLiteral("保留样本:"), this));
    m_stepLimit = new QSpinBox(this);
    m_stepLimit->setRange(200, 200000);
    m_stepLimit->setValue(4000);
    m_stepLimit->setSingleStep(500);
    m_stepLimit->setToolTip(QStringLiteral("图上保留多少个仿真步（超出后滚动）"));
    runRow->addWidget(m_stepLimit);
    runRow->addStretch(1);
    root->addLayout(runRow);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    root->addWidget(m_stateLabel);

    // --- chart -----------------------------------------------------------
    m_history = new SampleHistory(4000);
    m_plot = new GlPlotWidget(this);
    m_plot->setHistory(m_history);
    m_plot->setLiveMode(true);
    m_plot->setInteractive(true);
    m_plot->setAutoScale(true);
    m_plot->setXAxisMode(GlPlotWidget::XAxisMode::Time);
    m_plot->setXAxisLabel(QStringLiteral("时间"));
    root->addWidget(m_plot, 1);

    // --- log -------------------------------------------------------------
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(110);
    m_log->setPlaceholderText(QStringLiteral("模型日志"));
    root->addWidget(m_log);

    connect(m_browseButton, &QPushButton::clicked, this, &SimulationPage::onBrowseModel);
    connect(m_loadButton, &QPushButton::clicked, this, &SimulationPage::onLoadModel);
    connect(m_startButton, &QPushButton::clicked, this, &SimulationPage::onStartStop);
    connect(m_clearButton, &QPushButton::clicked, this, &SimulationPage::onClear);
    connect(m_mapping, &QTableWidget::cellChanged, this, &SimulationPage::onMappingEdited);
    connect(m_outputBox, &QComboBox::currentIndexChanged, this, [this](int) { refreshUi(); });
    connect(m_stepLimit, &QSpinBox::valueChanged, this, [this](int value) {
        // Changing the capacity clears the buffer (that is what SampleHistory does), so do
        // not apply it mid-run: the next start picks it up instead.
        if (m_running) {
            log(QStringLiteral("保留样本数将在下次开始时生效（运行中不改变缓冲）。"));
            return;
        }
        if (m_history)
            m_history->setCapacity(value);
    });

    refreshUi();
}

SimulationPage::~SimulationPage()
{
    stopModel();
}

void SimulationPage::setSource(ScopePage *scope)
{
    if (m_source == scope)
        return;
    if (m_source) {
        disconnect(m_source, nullptr, this, nullptr);
    }
    m_source = scope;
    if (m_source) {
        connect(m_source, &ScopePage::liveSample, this, &SimulationPage::onLiveSample);
        connect(m_source, &ScopePage::liveCaptureStateChanged, this,
                &SimulationPage::onLiveCaptureStateChanged);
    }
}

QStringList SimulationPage::logLinesForTest() const
{
    if (!m_log)
        return QStringList();
    return m_log->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void SimulationPage::setModelPathForTest(const QString &path)
{
    m_modelPath->setText(path);
}

void SimulationPage::setMappingForTest(const QString &input, const QString &device)
{
    for (int row = 0; row < m_mapping->rowCount(); ++row) {
        QTableWidgetItem *nameItem = m_mapping->item(row, 0);
        if (!nameItem || nameItem->text() != input)
            continue;
        auto *combo = qobject_cast<QComboBox *>(m_mapping->cellWidget(row, 1));
        if (!combo)
            return;
        int index = combo->findData(device);
        if (index < 0 && !device.isEmpty()) {
            // A device variable that was not pre-listed (no sampling has run yet).
            combo->addItem(device, device);
            index = combo->findData(device);
        }
        if (index >= 0)
            combo->setCurrentIndex(index);
        return;
    }
}

void SimulationPage::log(const QString &line)
{
    m_log->appendPlainText(line);
}

void SimulationPage::onBrowseModel()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择仿真模型"), m_modelPath->text(),
        SimModel::fileFilters().join(QStringLiteral(";;")));
    if (!path.isEmpty())
        m_modelPath->setText(path);
}

void SimulationPage::onLoadModel()
{
    stopModel();

    const QString path = m_modelPath->text().trimmed();
    if (path.isEmpty()) {
        log(QStringLiteral("请先选择模型文件。"));
        return;
    }

    if (!m_model) {
        auto opened = SimModel::open(path);
        if (!opened) {
            log(QStringLiteral("加载失败：%1").arg(opened.error));
            m_modelLabel->setText(QStringLiteral("加载失败：%1").arg(opened.error));
            return;
        }
        m_model = std::move(opened.value);
    }

    m_modelInfo = m_model->info();
    m_modelLabel->setText(
        QStringLiteral("<b>%1</b> · %2 · 输入 %3 / 输出 %4<br/>%5")
            .arg(m_modelInfo.displayName, m_modelInfo.kind)
            .arg(m_modelInfo.inputs.size())
            .arg(m_modelInfo.outputs.size())
            .arg(m_modelInfo.detail));
    log(QStringLiteral("已加载 %1（%2）").arg(m_modelInfo.displayName, m_modelInfo.kind));
    if (!m_modelInfo.inputs.isEmpty())
        log(QStringLiteral("模型输入：%1").arg(m_modelInfo.inputs.join(QStringLiteral(", "))));
    if (!m_modelInfo.outputs.isEmpty())
        log(QStringLiteral("模型输出：%1").arg(m_modelInfo.outputs.join(QStringLiteral(", "))));

    // Output selector.
    const QString previous = m_outputBox->currentText();
    m_outputBox->blockSignals(true);
    m_outputBox->clear();
    m_outputBox->addItems(m_modelInfo.outputs);
    m_outputBox->blockSignals(false);
    if (!previous.isEmpty()) {
        const int index = m_outputBox->findText(previous);
        if (index >= 0)
            m_outputBox->setCurrentIndex(index);
    }

    rebuildMapping();
    refreshUi();
}

void SimulationPage::rebuildMapping()
{
    m_mapping->blockSignals(true);
    m_mapping->setRowCount(0);

    // Suggestions for the device side: the live channel names are the .axf variables the
    // user ticked, which is exactly the data the model should see.
    QStringList deviceNames = m_deviceVariableNames;
    if (deviceNames.isEmpty() && m_source)
        deviceNames = m_source->liveChannelNames();

    for (const QString &input : m_modelInfo.inputs) {
        const int row = m_mapping->rowCount();
        m_mapping->insertRow(row);

        auto *nameItem = new QTableWidgetItem(input);
        nameItem->setFlags(Qt::ItemIsEnabled);
        m_mapping->setItem(row, 0, nameItem);

        auto *combo = new QComboBox(m_mapping);
        combo->addItem(QStringLiteral("（不连接）"), QString());
        for (const QString &deviceName : deviceNames)
            combo->addItem(deviceName, deviceName);
        // Preselect by name match, then by suffix (a channel may be `encoders[0].pos_`
        // while the model input is just `pos`).
        const QString saved = m_inputMapping.value(input);
        int chosen = 0;
        if (!saved.isEmpty())
            chosen = combo->findData(saved);
        if (chosen <= 0) {
            for (const QString &deviceName : deviceNames) {
                if (deviceName.compare(input, Qt::CaseInsensitive) == 0) {
                    chosen = combo->findData(deviceName);
                    break;
                }
            }
        }
        if (chosen > 0)
            combo->setCurrentIndex(chosen);
        m_mapping->setCellWidget(row, 1, combo);
        connect(combo, &QComboBox::currentIndexChanged, this, &SimulationPage::onMappingEdited);

        auto *valueItem = new QTableWidgetItem(QStringLiteral("—"));
        valueItem->setFlags(Qt::ItemIsEnabled);
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_mapping->setItem(row, 2, valueItem);
    }
    m_mapping->blockSignals(false);
    onMappingEdited();
}

void SimulationPage::onMappingEdited()
{
    m_inputMapping.clear();
    for (int row = 0; row < m_mapping->rowCount(); ++row) {
        const QString input = m_mapping->item(row, 0) ? m_mapping->item(row, 0)->text()
                                                      : QString();
        auto *combo = qobject_cast<QComboBox *>(m_mapping->cellWidget(row, 1));
        if (!combo)
            continue;
        const QString device = combo->currentData().toString();
        if (!input.isEmpty() && !device.isEmpty())
            m_inputMapping.insert(input, device);
    }
    refreshUi();
}

void SimulationPage::onLiveCaptureStateChanged(bool running)
{
    m_samplingRunning = running;
    if (!running) {
        m_lastSampleUs = 0; // a restart begins a new timeline
        if (m_running)
            log(QStringLiteral("实时采样已停止，仿真暂停。"));
    }
    refreshUi();
}

void SimulationPage::onStartStop()
{
    if (m_running) {
        stopModel();
        log(QStringLiteral("已停止仿真。"));
        refreshUi();
        return;
    }

    if (!m_model) {
        log(QStringLiteral("请先加载模型。"));
        return;
    }
    if (m_inputMapping.isEmpty()) {
        log(QStringLiteral("至少要把一个模型输入连接到设备变量。"));
        m_stateLabel->setText(QStringLiteral("没有输入映射，无法仿真。"));
        return;
    }

    const double sampleTime = 0.001; // nominal; each step uses the real timestamp delta
    const Status status = m_model->initialize(sampleTime);
    if (!status) {
        log(QStringLiteral("初始化失败：%1").arg(status.error));
        m_stateLabel->setText(QStringLiteral("模型初始化失败：%1").arg(status.error));
        return;
    }

    // Capacity first, then names: setCapacity() clears, so it has to happen before the
    // trace set is installed or the names would be dropped.
    if (m_stepLimit)
        m_history->setCapacity(m_stepLimit->value());
    m_history->setTraceNames(m_modelInfo.outputs);
    m_history->clear();
    m_modelTime = 0.0;
    m_steps = 0;
    m_failures = 0;
    m_lastSampleUs = 0;
    m_running = true;
    log(QStringLiteral("仿真已开始，用真实采样数据步进。"));

    // Sampling is the data source: start it if the chart page has not.
    if (m_source && !m_samplingRunning)
        m_source->requestLiveCaptureStart();

    refreshUi();
}

void SimulationPage::stopModel()
{
    if (m_model)
        m_model->shutdown();
    m_running = false;
}

void SimulationPage::onClear()
{
    if (m_history)
        m_history->clear();
    m_steps = 0;
    m_failures = 0;
    m_modelTime = 0.0;
    m_lastSampleUs = 0;
    plotRefresh();
    refreshUi();
}

void SimulationPage::onLiveSample(qint64 timeUs, const QHash<QString, double> &values)
{
    // Keep the last device values even when idle, so the mapping table shows what the
    // model *would* be fed.
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        m_lastDeviceValues.insert(it.key(), it.value());

    if (!m_running || !m_model)
        return;

    stepModel(timeUs, values);
}

void SimulationPage::stepModel(qint64 timeUs, const QHash<QString, double> &values)
{
    // dt from the target's own timestamps: the model advances by the time that actually
    // passed between two samples, not by an assumed period.
    double dt = m_lastSampleUs > 0 ? double(timeUs - m_lastSampleUs) / 1e6 : 0.0;
    if (dt <= 0.0 || dt > 5.0) {
        // First sample, or a gap the probe already reported: use a nominal step so the
        // model does not integrate across a hole.
        dt = 0.001;
    }
    m_lastSampleUs = timeUs;

    QHash<QString, double> inputs;
    for (auto it = m_inputMapping.constBegin(); it != m_inputMapping.constEnd(); ++it) {
        const auto value = values.constFind(it.value());
        if (value != values.constEnd())
            inputs.insert(it.key(), value.value());
    }
    if (inputs.isEmpty())
        return; // none of the mapped variables arrived in this sample

    QHash<QString, double> outputs;
    const Status status = m_model->step(m_modelTime, dt, inputs, &outputs);
    if (!status) {
        ++m_failures;
        log(QStringLiteral("步进失败：%1").arg(status.error));
        // One failure is enough to stop: continuing would emit a partly-updated curve that
        // looks like real model behaviour.
        stopModel();
        refreshUi();
        return;
    }

    m_modelTime += dt;
    ++m_steps;

    // Plot: one curve per model output, x = simulated time.
    QVector<double> row;
    row.reserve(m_modelInfo.outputs.size());
    for (const QString &name : m_modelInfo.outputs)
        row.append(outputs.value(name, std::numeric_limits<double>::quiet_NaN()));
    m_history->append(qint64(m_modelTime * 1e6), row);

    if (m_steps % 20 == 0 || m_steps == 1)
        plotRefresh();
    if (m_steps % 100 == 0)
        refreshUi();
}

void SimulationPage::plotRefresh()
{
    if (!m_plot)
        return;
    m_plot->followLatest();
    m_plot->refresh();
}

void SimulationPage::refreshUi()
{
    if (m_startButton) {
        m_startButton->setText(m_running ? QStringLiteral("停止仿真")
                                         : QStringLiteral("开始仿真"));
        m_startButton->setEnabled(m_model != nullptr);
    }
    if (m_clearButton)
        m_clearButton->setEnabled(true);
    // Capacity is NOT synced here: SampleHistory::setCapacity() clears the buffer, so
    // doing it from a periodic refresh deleted the curve every 100 steps.

    // Per-input current value, refreshed from the last device sample.
    for (int row = 0; row < m_mapping->rowCount(); ++row) {
        auto *combo = qobject_cast<QComboBox *>(m_mapping->cellWidget(row, 1));
        QTableWidgetItem *valueItem = m_mapping->item(row, 2);
        if (!combo || !valueItem)
            continue;
        const QString device = combo->currentData().toString();
        if (device.isEmpty()) {
            valueItem->setText(QStringLiteral("—"));
            continue;
        }
        const auto it = m_lastDeviceValues.constFind(device);
        valueItem->setText(it == m_lastDeviceValues.constEnd()
                               ? QStringLiteral("（未采样）")
                               : QString::number(it.value(), 'g', 7));
    }

    QStringList parts;
    if (m_model) {
        parts << QStringLiteral("步进 %1 次").arg(m_steps);
        parts << QStringLiteral("模型时间 %1 s").arg(m_modelTime, 0, 'f', 3);
        if (m_failures > 0)
            parts << QStringLiteral("失败 %1 次").arg(m_failures);
    }
    if (!m_running) {
        parts << (m_model ? QStringLiteral("已停止") : QStringLiteral("未加载模型"));
    } else if (!m_samplingRunning) {
        parts << QStringLiteral("等待实时采样…");
    }
    if (m_stateLabel)
        m_stateLabel->setText(parts.join(QStringLiteral(" · ")));
}

} // namespace mrd
