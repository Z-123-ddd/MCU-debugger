#pragma once

#include "core/SampleHistory.h"
#include "core/SimModel.h"

#include <QHash>
#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace mrd {

class GlPlotWidget;
class ProbeController;
class ScopePage;

/// Steps a simulation model with real device measurements and shows both curves.
///
/// The point of the page: a model is only trustworthy if it was fed what the hardware
/// actually produced. So there is no synthetic signal generator here -- the input values
/// are the ones the probe read from the addresses in the `.axf` variable table, arriving
/// through `ScopePage::liveSample()`. If sampling is not running, the page says so instead
/// of inventing data.
///
/// Supported models are the ones a real model can be exported to: an FMU (FMI 2.0
/// Co-Simulation, what Simulink's "Export to FMU" produces) or a Python file implementing
/// `Model.step(inputs)`. See SimModel for why `.slx` block diagrams are not parsed.
class SimulationPage : public QWidget
{
    Q_OBJECT

public:
    explicit SimulationPage(QWidget *parent = nullptr);
    ~SimulationPage() override;

    /// The live data source. Also used to start/stop sampling, so the user does not have
    /// to switch to the chart page first.
    void setSource(ScopePage *scope);

    // --- test hooks: drive the page the way the widgets do -------------------
    void setModelPathForTest(const QString &path);
    /// Click 加载模型.
    void loadModelForTest() { onLoadModel(); }
    /// Choose a device variable for one model input.
    void setMappingForTest(const QString &input, const QString &device);
    /// Click 开始仿真 / 停止仿真.
    void startStopForTest() { onStartStop(); }
    int stepsForTest() const { return m_steps; }
    bool isRunningForTest() const { return m_running; }
    QStringList modelInputsForTest() const { return m_modelInfo.inputs; }
    QStringList modelOutputsForTest() const { return m_modelInfo.outputs; }
    const SampleHistory *historyForTest() const { return m_history; }
    QStringList logLinesForTest() const;

public slots:
    /// One real sample from the probe (forwarded by ScopePage).
    void onLiveSample(qint64 timeUs, const QHash<QString, double> &values);
    /// Live sampling started/stopped.
    void onLiveCaptureStateChanged(bool running);

private slots:
    void onBrowseModel();
    void onLoadModel();
    void onStartStop();
    void onClear();
    void onMappingEdited();
    void refreshUi();

private:
    /// Rebuild the mapping table from the model's inputs and the live channel names.
    void rebuildMapping();
    /// One model step with the given device values; advances the plot.
    void stepModel(qint64 timeUs, const QHash<QString, double> &values);
    void stopModel();
    void plotRefresh();
    void log(const QString &line);

    ScopePage *m_source = nullptr;
    std::unique_ptr<SimModel> m_model;
    SimModel::Info m_modelInfo;

    QLineEdit *m_modelPath = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_loadButton = nullptr;
    QLabel *m_modelLabel = nullptr;

    QTableWidget *m_mapping = nullptr;      ///< model input | device variable | last value
    QPushButton *m_startButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QLabel *m_stateLabel = nullptr;

    QComboBox *m_outputBox = nullptr;       ///< which model output to plot
    QSpinBox *m_stepLimit = nullptr;        ///< how many samples to keep
    QPlainTextEdit *m_log = nullptr;

    GlPlotWidget *m_plot = nullptr;
    SampleHistory *m_history = nullptr;

    /// model input name -> device variable name (the mapping the user chose).
    QHash<QString, QString> m_inputMapping;
    /// Latest value seen per device variable, so the mapping table can show it.
    QHash<QString, double> m_lastDeviceValues;
    QStringList m_deviceVariableNames;

    bool m_running = false;
    bool m_samplingRunning = false;
    qint64 m_lastSampleUs = 0;
    double m_modelTime = 0.0;
    int m_steps = 0;
    int m_failures = 0;
};

} // namespace mrd
