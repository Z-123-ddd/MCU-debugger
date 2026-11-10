#include "ui/MainWindow.h"

#include "core/AppSettings.h"
#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/JLinkGdbProbe.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeController.h"
#include "core/JLinkRunner.h"
#include "core/ProbeRsRunner.h"
#include "core/RegisterSession.h"
#include "core/ResourcePaths.h"
#include "core/SvdLibrary.h"
#include "core/SvdParser.h"
#include "ui/BitFieldStrip.h"
#include "ui/InspectorPanel.h"
#include "ui/PackageView.h"
#include "ui/PeripheralTree.h"
#include "ui/PortGridView.h"
#include "ui/RegisterBlockView.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace mrd {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("%1 %2").arg(QStringLiteral(APP_NAME), QStringLiteral(APP_VERSION)));
    resize(1500, 940);

    // Kill a GDB server orphaned by a previous crash before touching the probe.
    JLinkGdbProbe::reapStaleServers();

    m_probe = std::make_unique<ProbeController>(this);
    m_session = std::make_unique<RegisterSession>(m_probe.get(), this);

    createUi();
    createToolBar();
    createDocks();
    createCentralPages();

    connect(m_probe.get(), &ProbeController::opened, this, &MainWindow::onProbeOpened);
    connect(m_probe.get(), &ProbeController::closed, this, [this] {
        m_session->setConnected(false);
        setConnectedUi(false);
    });
    connect(m_probe.get(), &ProbeController::connectFinished, this, &MainWindow::onProbeConnectFinished);
    connect(m_probe.get(), &ProbeController::identifyFinished, this, &MainWindow::onIdentifyFinished);
    connect(m_probe.get(), &ProbeController::cpuStateChanged, this, &MainWindow::onCpuStateChanged);
    connect(m_probe.get(), &ProbeController::deviceTreeScanned, this, &MainWindow::onDeviceTreeScanned);
    connect(m_probe.get(), &ProbeController::chipValidated, this,
            [](const QString &chip, bool ok, const QString &message) {
                if (!ok)
                    Logger::instance()->warn(QStringLiteral("芯片名校验失败 %1: %2").arg(chip, message));
            });

    connect(m_session.get(), &RegisterSession::valuesChanged, this, &MainWindow::onValuesChanged);
    connect(m_session.get(), &RegisterSession::writeFinished, this, &MainWindow::onWriteFinished);
    connect(m_session.get(), &RegisterSession::readFailed, this, &MainWindow::onReadFailed);
    connect(m_session.get(), &RegisterSession::historyChanged, this, [this] {
        setWindowTitle(QStringLiteral("%1 %2%3")
                           .arg(QStringLiteral(APP_NAME), QStringLiteral(APP_VERSION),
                                m_session->canUndo() ? QStringLiteral("  *") : QString()));
    });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(m_pollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::onPollTimeout);

    connect(Logger::instance(), &Logger::messageLogged, this, &MainWindow::onLogMessage);

    loadUiState();
    Theme::apply(AppSettings::theme());
    setConnectedUi(false);

    if (DeviceIdentifier::database().isEmpty()) {
        QString error;
        if (!DeviceIdentifier::loadDatabase(&error))
            Logger::instance()->warn(QStringLiteral("器件识别库加载失败: %1").arg(error));
    }

    loadPackages(ResourcePaths::packageDirectory());

    const QString lastSvd = AppSettings::lastSvdPath();
    if (!lastSvd.isEmpty() && QFileInfo::exists(lastSvd))
        openSvdFile(lastSvd);
}

MainWindow::~MainWindow()
{
    stopPolling();
}

void MainWindow::createUi()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    auto *openAction = fileMenu->addAction(QStringLiteral("打开 SVD 文件(&O)..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenSvd);

    auto *searchAction = fileMenu->addAction(QStringLiteral("在 SVD 库中搜索(&S)..."));
    connect(searchAction, &QAction::triggered, this, &MainWindow::onSearchSvd);

    auto *packageAction = fileMenu->addAction(QStringLiteral("打开封装目录(&P)..."));
    connect(packageAction, &QAction::triggered, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择封装描述目录"),
                                                              ResourcePaths::packageDirectory());
        if (!dir.isEmpty()) {
            AppSettings::setPackageDir(dir);
            loadPackages(dir);
        }
    });

    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    auto *themeLight = viewMenu->addAction(QStringLiteral("浅色主题"));
    connect(themeLight, &QAction::triggered, this, [this] {
        AppSettings::setTheme(QStringLiteral("light"));
        Theme::apply(QStringLiteral("light"));
        refreshValues(true);
    });
    auto *themeDark = viewMenu->addAction(QStringLiteral("深色主题"));
    connect(themeDark, &QAction::triggered, this, [this] {
        AppSettings::setTheme(QStringLiteral("dark"));
        Theme::apply(QStringLiteral("dark"));
        refreshValues(true);
    });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于(&A)"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("关于"),
                           QStringLiteral("<b>%1</b> %2<br/>MCU 寄存器级 UI 调试器<br/><br/>"
                                          "后端: probe-rs CLI<br/>描述: CMSIS-SVD<br/>Qt %3")
                               .arg(QStringLiteral(APP_NAME), QStringLiteral(APP_VERSION),
                                    QString::fromLatin1(qVersion())));
    });

    m_connLabel = new QLabel(QStringLiteral("未连接"), this);
    m_pollLabel = new QLabel(QStringLiteral("轮询: 关闭"), this);
    m_deviceLabel = new QLabel(QStringLiteral("未加载 SVD"), this);
    statusBar()->addWidget(m_deviceLabel, 1);
    statusBar()->addPermanentWidget(m_pollLabel);
    statusBar()->addPermanentWidget(m_connLabel);
}

void MainWindow::createToolBar()
{
    m_toolBar = addToolBar(QStringLiteral("调试"));
    m_toolBar->setMovable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto *connectAction = m_toolBar->addAction(QStringLiteral("连接"));
    connect(connectAction, &QAction::triggered, this, &MainWindow::onConnectClicked);
    auto *disconnectAction = m_toolBar->addAction(QStringLiteral("断开"));
    connect(disconnectAction, &QAction::triggered, this, &MainWindow::onDisconnectClicked);
    m_toolBar->addSeparator();

    m_toolBar->addWidget(new QLabel(QStringLiteral(" 后端 "), this));
    m_backendBox = new QComboBox(this);
    m_backendBox->addItem(QStringLiteral("J-Link"), QStringLiteral("jlink"));
    m_backendBox->addItem(QStringLiteral("probe-rs"), QStringLiteral("probe-rs"));
    m_backendBox->setCurrentIndex(AppSettings::probeBackend() == QLatin1String("probe-rs") ? 1 : 0);
    m_toolBar->addWidget(m_backendBox);

    m_toolBar->addWidget(new QLabel(QStringLiteral(" 接口 "), this));
    m_interfaceBox = new QComboBox(this);
    m_interfaceBox->addItems({QStringLiteral("SWD"), QStringLiteral("JTAG")});
    m_interfaceBox->setCurrentText(AppSettings::interfaceName());
    m_toolBar->addWidget(m_interfaceBox);

    m_toolBar->addWidget(new QLabel(QStringLiteral(" 速度(kHz) "), this));
    m_speedBox = new QSpinBox(this);
    m_speedBox->setRange(100, 50000);
    m_speedBox->setSingleStep(500);
    m_speedBox->setValue(AppSettings::speedKHz());
    m_toolBar->addWidget(m_speedBox);

    m_toolBar->addWidget(new QLabel(QStringLiteral(" 芯片 "), this));
    m_chipBox = new QComboBox(this);
    m_chipBox->setEditable(true);
    m_chipBox->setMinimumWidth(180);
    m_chipBox->addItem(QString());
    for (const auto &entry : DeviceIdentifier::database())
        m_chipBox->addItem(entry.probeRsChip);
    m_chipBox->setCurrentText(AppSettings::chipName());
    m_toolBar->addWidget(m_chipBox);
    m_toolBar->addSeparator();

    auto *haltAction = m_toolBar->addAction(QStringLiteral("暂停"));
    connect(haltAction, &QAction::triggered, this, &MainWindow::onHaltClicked);
    auto *runAction = m_toolBar->addAction(QStringLiteral("运行"));
    connect(runAction, &QAction::triggered, this, &MainWindow::onRunClicked);
    auto *resetAction = m_toolBar->addAction(QStringLiteral("复位"));
    connect(resetAction, &QAction::triggered, this, &MainWindow::onResetClicked);
    auto *undoAction = m_toolBar->addAction(QStringLiteral("撤销写入"));
    connect(undoAction, &QAction::triggered, this, &MainWindow::onUndoClicked);
}

void MainWindow::createDocks()
{
    auto *treeDock = new QDockWidget(QStringLiteral("外设"), this);
    treeDock->setObjectName(QStringLiteral("PeripheralDock"));
    treeDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    treeDock->setMinimumWidth(120);
    m_tree = new PeripheralTree(treeDock);
    treeDock->setWidget(m_tree);
    addDockWidget(Qt::LeftDockWidgetArea, treeDock);
    connect(m_tree, &PeripheralTree::peripheralSelected, this, &MainWindow::onPeripheralSelected);
    connect(m_tree, &PeripheralTree::pinnedChanged, this, &MainWindow::onPinnedChanged);

    auto *inspectorDock = new QDockWidget(QStringLiteral("寄存器检查器"), this);
    inspectorDock->setObjectName(QStringLiteral("InspectorDock"));
    inspectorDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    // 200 px keeps the bit-field strip at two 16-bit rows instead of four 8-bit
    // ones when the dock is dragged narrow (the strip wraps, it never clips).
    inspectorDock->setMinimumWidth(200);
    m_inspector = new InspectorPanel(inspectorDock);
    inspectorDock->setWidget(m_inspector);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);
    connect(m_inspector, &InspectorPanel::fieldWriteRequested, this, &MainWindow::onFieldWrite);
    connect(m_inspector, &InspectorPanel::registerWriteRequested, this, &MainWindow::onRegisterWrite);
    connect(m_inspector, &InspectorPanel::registerReadRequested, this, &MainWindow::onRegisterRead);
    connect(m_inspector, &InspectorPanel::fieldSelected, this, &MainWindow::onFieldSelected);

    auto *logDock = new QDockWidget(QStringLiteral("日志"), this);
    logDock->setObjectName(QStringLiteral("LogDock"));
    m_logView = new QPlainTextEdit(logDock);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    logDock->setWidget(m_logView);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);

    // Give the three areas a usable initial split; the user can drag the
    // separators freely afterwards (no fixed sizes are enforced anywhere).
    resizeDocks({treeDock, inspectorDock}, {260, 380}, Qt::Horizontal);
    resizeDocks({logDock}, {160}, Qt::Vertical);
}

void MainWindow::createCentralPages()
{
    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);

    // --- package / pinout view ---
    m_packagePage = new PackagePage(m_tabs);
    m_tabs->addTab(m_packagePage, QStringLiteral("封装 / 引脚"));
    connect(m_packagePage->view(), &PackageView::pinActivated, this,
            [this](const QString &portName, int bit) {
                Q_UNUSED(bit);
                // Jump to the GPIO peripheral of that port.
                const QString gpioName = QStringLiteral("GPIO") + portName.mid(1);
                m_tree->selectPeripheral(gpioName);
                onPeripheralSelected(gpioName);
                m_tabs->setCurrentIndex(2);
            });

    // --- port matrix ---
    m_portGrid = new PortGridView(m_tabs);
    m_tabs->addTab(m_portGrid, QStringLiteral("端口矩阵 (IO)"));
    connect(m_portGrid, &PortGridView::pinConfigureRequested, this, &MainWindow::onPinConfigure);
    connect(m_portGrid, &PortGridView::pinLevelRequested, this, &MainWindow::onPinLevel);
    connect(m_portGrid, &PortGridView::portChanged, this, [this](const QString &portName) {
        const QString gpioName = QStringLiteral("GPIO") + portName.mid(1);
        const SvdPeripheral *peripheral = m_device.findPeripheral(gpioName);
        if (peripheral)
            m_session->requestPeripheralRead(*peripheral);
    });

    // --- register panorama ---
    auto *registerPage = new QWidget(m_tabs);
    auto *layout = new QVBoxLayout(registerPage);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_registerView = new RegisterBlockView(registerPage);
    layout->addWidget(m_registerView, 1);

    auto *stripContainer = new QWidget(registerPage);
    auto *stripLayout = new QVBoxLayout(stripContainer);
    stripLayout->setContentsMargins(6, 0, 6, 6);
    stripLayout->setSpacing(2);
    m_strip = new BitFieldStrip(stripContainer);
    stripLayout->addWidget(m_strip);
    layout->addWidget(stripContainer);

    m_tabs->addTab(registerPage, QStringLiteral("寄存器全景"));

    connect(m_registerView, &RegisterBlockView::registerSelected, this, &MainWindow::onRegisterSelected);
    connect(m_registerView, &RegisterBlockView::registerActivated, this, [this](int index) {
        Q_UNUSED(index);
        m_tabs->setCurrentIndex(2);
    });
    connect(m_strip, &BitFieldStrip::fieldSelected, this, &MainWindow::onFieldSelected);
}

void MainWindow::loadUiState()
{
    QSettings s;
    s.beginGroup(QStringLiteral("MainWindow"));
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    QMainWindow::restoreState(s.value(QStringLiteral("state")).toByteArray());
    s.endGroup();

    m_pollIntervalMs = AppSettings::pollIntervalMs();
    if (m_pollTimer)
        m_pollTimer->setInterval(m_pollIntervalMs);
    m_pinned = AppSettings::pinnedPeripherals();
    m_tree->setPinnedPeripherals(m_pinned);
}

void MainWindow::saveUiState()
{
    QSettings s;
    s.beginGroup(QStringLiteral("MainWindow"));
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("state"), QMainWindow::saveState());
    s.endGroup();

    AppSettings::setInterfaceName(m_interfaceBox->currentText());
    AppSettings::setSpeedKHz(m_speedBox->value());
    AppSettings::setChipName(m_chipBox->currentText());
    AppSettings::setPinnedPeripherals(m_pinned);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveUiState();
    stopPolling();
    QMainWindow::closeEvent(event);
}

bool MainWindow::openSvdFile(const QString &path)
{
    const auto parsed = SvdParser::parseFile(path);
    if (!parsed) {
        Logger::instance()->error(QStringLiteral("加载 SVD 失败: %1").arg(parsed.error));
        QMessageBox::warning(this, QStringLiteral("加载 SVD 失败"), parsed.error);
        return false;
    }

    m_device = parsed.value;
    m_svdPath = path;
    AppSettings::setLastSvdPath(path);

    Logger::instance()->info(QStringLiteral("已加载 %1: 外设 %2 / 寄存器 %3 / 字段 %4")
                                 .arg(m_device.name)
                                 .arg(m_device.peripherals.size())
                                 .arg(m_device.totalRegisters())
                                 .arg(m_device.totalFields()));
    for (int i = 0; i < qMin(5, m_device.warnings.size()); ++i)
        Logger::instance()->warn(m_device.warnings.at(i));

    m_session->setDevice(m_device);
    applyDevice();
    return true;
}

void MainWindow::applyDevice()
{
    // The SVD alone only describes a family; the tree, port grid and register
    // view stay parked until the attached part has been identified and its device
    // tree read back (populateDeviceUi()). Mirrors the package view behaviour.
    m_deviceLabel->setText(QStringLiteral("%1  (%2 外设, %3 寄存器)  %4")
                               .arg(m_device.name)
                               .arg(m_device.peripherals.size())
                               .arg(m_device.totalRegisters())
                               .arg(QFileInfo(m_svdPath).fileName()));

    m_tree->showWaiting(QStringLiteral("连接目标后自动读取设备树"));
    m_inspector->clear();
    m_strip->setRegister(nullptr);
    m_registerView->setPeripheral(nullptr);
    m_portGrid->setPorts({});

    Logger::instance()->info(QStringLiteral("已加载 %1 的 SVD；连接后将按目标回读设备树")
                                 .arg(m_device.name));
}

void MainWindow::populateDeviceUi()
{
    m_tree->setDevice(m_device);

    // Port matrix: only ports this package actually bonds out. Without a package
    // table we fall back to the SVD list (nothing better is known).
    QStringList ports;
    for (const auto &peripheral : m_device.peripherals) {
        if (!GpioCodec::isGpioPort(peripheral))
            continue;
        const QString port = GpioCodec::portName(peripheral);
        if (port.isEmpty() || ports.contains(port))
            continue;
        if (!m_bondedBits.isEmpty() && m_bondedBits.value(port).isEmpty())
            continue;
        ports << port;
    }
    ports.sort();
    m_portGrid->setPorts(ports);

    // Preselect a meaningful peripheral: first pinned, else first GPIO, else first.
    QString initial;
    if (!m_pinned.isEmpty())
        initial = m_pinned.first();
    if (initial.isEmpty() && !ports.isEmpty())
        initial = QStringLiteral("GPIO") + ports.first().mid(1);
    if (initial.isEmpty() && !m_device.peripherals.isEmpty())
        initial = m_device.peripherals.first().name;

    if (!initial.isEmpty()) {
        m_tree->selectPeripheral(initial);
        onPeripheralSelected(initial);
    }
}

void MainWindow::loadPackages(const QString &directory)
{
    // Only the catalogue is loaded here. The pinout itself is drawn later, once
    // J-Link reported the part number and IO count (see onIdentifyFinished).
    m_packagePage->view()->loadPackages(directory);
    m_packagePage->setPackages(m_packagePage->view()->packages());
    if (m_packagePage->view()->packages().isEmpty())
        Logger::instance()->warn(QStringLiteral("未找到封装描述文件: %1").arg(directory));
}

const SvdPeripheral *MainWindow::currentPeripheral() const
{
    if (m_currentPeripheral.isEmpty())
        return nullptr;
    return m_device.findPeripheral(m_currentPeripheral);
}

const SvdRegister *MainWindow::currentRegister() const
{
    const SvdPeripheral *peripheral = currentPeripheral();
    if (!peripheral || m_currentRegister < 0 || m_currentRegister >= peripheral->registers.size())
        return nullptr;
    return &peripheral->registers.at(m_currentRegister);
}

QString MainWindow::effectiveChip() const
{
    const QString typed = m_chipBox->currentText().trimmed();
    return typed;
}

void MainWindow::setConnectedUi(bool connected)
{
    m_inspector->setConnected(connected);
    m_connLabel->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    if (!connected)
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
}

void MainWindow::onLogMessage(const QString &text, int level)
{
    Q_UNUSED(level);
    if (!m_logView)
        return;
    m_logView->appendPlainText(text);
    if (auto *bar = m_logView->verticalScrollBar())
        bar->setValue(bar->maximum());
}

void MainWindow::onOpenSvd()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 CMSIS-SVD 文件"),
        m_svdPath.isEmpty() ? QString() : QFileInfo(m_svdPath).absolutePath(),
        QStringLiteral("SVD files (*.svd);;All files (*)"));
    if (!path.isEmpty())
        openSvdFile(path);
}

void MainWindow::onSearchSvd()
{
    SvdLibrary library;
    library.setRoots(AppSettings::svdRoots());
    library.scan(false);

    bool ok = false;
    const QString keyword = QInputDialog::getText(this, QStringLiteral("搜索 SVD"),
                                                 QStringLiteral("器件名/厂商/路径关键字（留空列出全部）:"),
                                                 QLineEdit::Normal, m_device.name, &ok);
    if (!ok)
        return;

    const QVector<SvdEntry> hits = library.search(keyword);
    if (hits.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("搜索结果"), QStringLiteral("没有匹配的 SVD 文件"));
        return;
    }

    QStringList labels;
    labels.reserve(hits.size());
    for (const auto &entry : hits)
        labels << QStringLiteral("%1  |  %2").arg(entry.deviceName, entry.path);

    bool chosen = false;
    const QString selection = QInputDialog::getItem(this, QStringLiteral("搜索结果"),
                                                    QStringLiteral("共 %1 个匹配，选择要加载的文件:").arg(hits.size()),
                                                    labels, 0, false, &chosen);
    if (!chosen)
        return;
    const int index = labels.indexOf(selection);
    if (index >= 0)
        openSvdFile(hits.at(index).path);
}

void MainWindow::scheduleAutoConnect()
{
    QTimer::singleShot(50, this, &MainWindow::onConnectClicked);
}

void MainWindow::onConnectClicked()
{
    const QString backend = m_backendBox->currentData().toString();
    AppSettings::setProbeBackend(backend);
    AppSettings::setInterfaceName(m_interfaceBox->currentText());
    AppSettings::setSpeedKHz(m_speedBox->value());

    QString binary;
    if (backend == QLatin1String("probe-rs")) {
        binary = ProbeRsRunner::locate(AppSettings::probeRsPath());
        if (binary.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("缺少 probe-rs"),
                QStringLiteral("未找到 probe-rs 可执行文件。\n\n"
                               "请运行:\n  powershell -ExecutionPolicy Bypass -File scripts\\fetch-probe-rs.ps1\n\n"
                               "或改用 J-Link 后端（使用已安装的 SEGGER 软件，无需换驱动）。"));
            return;
        }
    } else {
        binary = JLinkRunner::locate(AppSettings::jlinkPath());
        if (binary.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("缺少 JLink.exe"),
                                 QStringLiteral("未找到 SEGGER J-Link 的 JLink.exe。\n\n"
                                                "请安装 SEGGER J-Link 软件，或在设置中指定路径。"));
            return;
        }
    }

    statusBar()->showMessage(QStringLiteral("正在打开探针..."));
    m_probe->requestOpen(backend, binary, m_interfaceBox->currentText(), m_speedBox->value());
}

void MainWindow::onDisconnectClicked()
{
    stopPolling();
    m_probe->requestDisconnect();
    m_probe->requestClose();
    m_session->setConnected(false);
    setConnectedUi(false);
    if (m_packagePage)
        m_packagePage->showWaiting(QStringLiteral("已断开连接。重新连接后将自动识别并渲染引脚布局"));
    if (m_tree)
        m_tree->showWaiting(QStringLiteral("已断开连接。重新连接后将自动读取设备树"));
    m_configuredPins.clear();
    statusBar()->showMessage(QStringLiteral("已断开"));
}

void MainWindow::onHaltClicked()
{
    m_probe->requestHalt();
}

void MainWindow::onRunClicked()
{
    m_probe->requestRun();
}

void MainWindow::onResetClicked()
{
    m_probe->requestReset();
}

void MainWindow::onUndoClicked()
{
    m_session->undoLastWrite();
}

void MainWindow::onProbeOpened(bool ok, const QString &error, const ProbeInfo &info)
{
    if (!ok) {
        Logger::instance()->error(QStringLiteral("打开探针失败: %1").arg(error));
        statusBar()->showMessage(QStringLiteral("打开探针失败"));
        return;
    }
    Logger::instance()->info(QStringLiteral("%1 | %2 | %3")
                                 .arg(info.backend, info.version,
                                      info.probeSerials.isEmpty() ? QStringLiteral("SN ?")
                                                                  : info.probeSerials.join(QStringLiteral(", "))));
    statusBar()->showMessage(QStringLiteral("探针就绪，正在连接目标..."));
    m_probe->requestConnect(effectiveChip());
}

void MainWindow::onProbeConnectFinished(bool ok, const QString &error, const ProbeInfo &info)
{
    if (!ok) {
        Logger::instance()->error(QStringLiteral("连接目标失败: %1").arg(error));
        statusBar()->showMessage(QStringLiteral("连接目标失败"));
        setConnectedUi(false);
        return;
    }

    m_session->setConnected(true);
    setConnectedUi(true);
    m_connLabel->setText(QStringLiteral("已连接 %1").arg(info.detectedChip.isEmpty() ? info.chipName : info.detectedChip));
    statusBar()->showMessage(QStringLiteral("已连接，正在识别 MCU..."));

    if (!m_chipBox->currentText().trimmed().isEmpty())
        AppSettings::setChipName(m_chipBox->currentText().trimmed());

    m_probe->requestIdentify(effectiveChip());
}

void MainWindow::onIdentifyFinished(bool ok, const QString &error, const IdentificationResult &result)
{
    if (!ok) {
        Logger::instance()->warn(QStringLiteral("MCU 识别失败: %1").arg(error));
        statusBar()->showMessage(QStringLiteral("MCU 识别失败，请手动选择芯片/SVD"));
        if (m_packagePage)
            m_packagePage->showWaiting(
                QStringLiteral("MCU 识别失败，无法确定引脚布局: %1").arg(error));
        if (m_tree && !m_session->connected())
            m_tree->showWaiting(QStringLiteral("MCU 识别失败，无法确定设备树"));
        startPolling();
        return;
    }

    Logger::instance()->info(QStringLiteral("识别到 %1 (%2), SVD=%3")
                                 .arg(result.mcuName, result.coreName.isEmpty() ? QStringLiteral("-") : result.coreName,
                                      result.svdPath.isEmpty() ? QStringLiteral("<无>") : result.svdPath));

    if (!result.probeRsChip.isEmpty() && m_chipBox->currentText().trimmed().isEmpty())
        m_chipBox->setCurrentText(result.probeRsChip);

    if (!result.svdPath.isEmpty() && QFileInfo::exists(result.svdPath)
        && (m_svdPath.isEmpty() || QFileInfo(result.svdPath).fileName() != QFileInfo(m_svdPath).fileName())) {
        openSvdFile(result.svdPath);
    }

    // Draw the pinout now: this is the first moment we know the part number and
    // the bonded IO count, so nothing is pre-rendered before the connection.
    const QString chip = !result.mcuName.isEmpty() ? result.mcuName : result.probeRsChip;
    if (m_packagePage) {
        const bool drawn = m_packagePage->renderForChip(chip, result.ioCount, result.pinCount,
                                                        result.packageName);
        if (drawn) {
            Logger::instance()->info(QStringLiteral("引脚布局已按识别结果渲染: %1 (IO %2 / 脚 %3)")
                                         .arg(chip)
                                         .arg(result.ioCount > 0 ? QString::number(result.ioCount)
                                                                 : QStringLiteral("?"))
                                         .arg(result.pinCount > 0 ? QString::number(result.pinCount)
                                                                  : QStringLiteral("?")));
        } else {
            Logger::instance()->warn(QStringLiteral("无法为 %1 生成引脚布局（缺 IO/封装数据）")
                                         .arg(chip.isEmpty() ? QStringLiteral("未知器件") : chip));
        }
    }
    if (!result.probeSerial.isEmpty() && !result.svdPath.isEmpty())
        AppSettings::setBindingForSn(result.probeSerial, result.svdPath);

    // The SVD only describes the family; ask the target which peripherals it really
    // has (clock gates + bus decode) so the tree reflects this exact part.
    if (!m_svdPath.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("正在读取设备树..."));
        m_probe->requestScanDeviceTree(m_svdPath);
    }

    startPolling();
}

void MainWindow::onPeripheralSelected(const QString &name)
{
    const SvdPeripheral *peripheral = m_device.findPeripheral(name);
    if (!peripheral)
        return;

    m_currentPeripheral = name;
    m_currentRegister = peripheral->registers.isEmpty() ? -1 : 0;
    m_registerView->setPeripheral(peripheral);
    m_registerView->setValues(m_session->snapshot(*peripheral), m_session->connected());
    if (m_currentRegister >= 0)
        m_registerView->setSelectedRegister(m_currentRegister);

    refreshInspector();
    refreshPortGrid();

    if (m_session->connected())
        m_session->requestPeripheralRead(*peripheral);
}

void MainWindow::onPinnedChanged(const QStringList &names)
{
    m_pinned = names;
    AppSettings::setPinnedPeripherals(names);
    if (m_pinned.isEmpty()) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
    } else {
        m_pollLabel->setText(QStringLiteral("轮询: %1 个外设 / %2 ms").arg(m_pinned.size()).arg(m_pollIntervalMs));
    }
}

void MainWindow::onRegisterSelected(int index)
{
    m_currentRegister = index;
    refreshInspector();
}

void MainWindow::onFieldSelected(int index)
{
    Q_UNUSED(index);
    if (m_inspector)
        m_inspector->selectField(index);
}

void MainWindow::refreshInspector()
{
    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    if (!peripheral || !reg) {
        m_inspector->clear();
        m_strip->setRegister(nullptr);
        return;
    }

    const quint32 address = peripheral->baseAddress + reg->addressOffset;
    const RegisterValue value = m_session->valueFor(address);
    m_inspector->setRegister(reg, address, value.valid ? value.value : reg->resetValue, value.valid);
    m_inspector->setConnected(m_session->connected());

    m_strip->setRegister(reg);
    m_strip->setValue(value.valid ? value.value : reg->resetValue, value.valid);
}

void MainWindow::refreshPortGrid()
{
    const SvdPeripheral *peripheral = currentPeripheral();
    if (!peripheral || !GpioCodec::isGpioPort(*peripheral))
        return;
    const QString port = GpioCodec::portName(*peripheral);
    if (port.isEmpty())
        return;

    bool valid = false;
    // Pass the bonded bit list so the grid only draws pins that really exist on
    // this package (GPIOH has 16 bits on the die but only 0/1/6 are brought out).
    const QVector<int> bonded = m_bondedBits.value(port);
    const QVector<GpioPin> pins = GpioCodec::decode(*peripheral, m_session->snapshot(*peripheral), &valid,
                                                   m_configuredPins, bonded);
    m_portGrid->setPins(port, pins, valid && m_session->connected());

    // Feed the package view so pins are coloured by their live mode.
    if (m_packagePage && m_packagePage->view()) {
        for (const auto &pin : pins) {
            m_packagePage->view()->setLivePinState(port, pin.index, pin.mode, pin.outputHigh,
                                                   m_session->connected());
        }
    }
}

void MainWindow::refreshValues(bool force)
{
    Q_UNUSED(force);
    const SvdPeripheral *peripheral = currentPeripheral();
    if (!peripheral)
        return;
    m_registerView->setValues(m_session->snapshot(*peripheral), m_session->connected());
    refreshInspector();
    refreshPortGrid();
}

void MainWindow::onValuesChanged(quint32 firstAddress, quint32 lastAddress)
{
    if (!m_firstPollLogged && m_session->connected()) {
        m_firstPollLogged = true;
        Logger::instance()->info(QStringLiteral("首次读取完成: %1 .. %2（轮询间隔 %3 ms）")
                                     .arg(hexAddr(firstAddress), hexAddr(lastAddress))
                                     .arg(m_pollIntervalMs));
    }
    refreshValues();
}

void MainWindow::onWriteFinished(quint32 address, bool ok, const QString &error)
{
    if (!ok) {
        Logger::instance()->error(QStringLiteral("写入 %1 失败: %2").arg(hexAddr(address), error));
        return;
    }
    Logger::instance()->info(QStringLiteral("写入 %1 成功").arg(hexAddr(address)));
    // probe-rs leaves the core halted after a write; refresh immediately.
    const SvdPeripheral *peripheral = currentPeripheral();
    if (peripheral && m_session->connected())
        m_session->requestPeripheralRead(*peripheral);
}

void MainWindow::onReadFailed(quint32 address, const QString &error)
{
    Logger::instance()->warn(QStringLiteral("读取 %1 失败: %2").arg(hexAddr(address), error));
}

void MainWindow::onCpuStateChanged(bool halted, bool ok, const QString &error)
{
    if (!ok) {
        Logger::instance()->warn(QStringLiteral("运行控制失败: %1").arg(error));
        return;
    }
    statusBar()->showMessage(halted ? QStringLiteral("CPU 已暂停") : QStringLiteral("CPU 运行中"), 3000);
}

void MainWindow::onFieldWrite(const SvdField &field, quint32 value)
{
    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    if (!peripheral || !reg)
        return;
    m_session->writeField(*peripheral, *reg, field, value);
}

void MainWindow::onRegisterWrite(quint32 value)
{
    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    if (!peripheral || !reg)
        return;
    m_session->writeRegister(*peripheral, *reg, value);
}

void MainWindow::onRegisterRead()
{
    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    if (!peripheral || !reg)
        return;
    m_session->requestRegisterRead(*peripheral, *reg);
}

void MainWindow::onPinConfigure(const QString &portName, int index, const GpioPinConfig &config)
{
    const QString gpioName = QStringLiteral("GPIO") + portName.mid(1);
    const SvdPeripheral *peripheral = m_device.findPeripheral(gpioName);
    if (!peripheral) {
        Logger::instance()->warn(QStringLiteral("找不到外设 %1，无法配置 %2%3")
                                     .arg(gpioName, portName)
                                     .arg(index));
        return;
    }
    if (!m_session->connected()) {
        Logger::instance()->warn(QStringLiteral("目标未连接，无法配置引脚"));
        return;
    }

    const QHash<quint32, quint32> current = m_session->snapshot(*peripheral);
    const QVector<GpioWrite> writes = GpioCodec::encodePinConfig(*peripheral, index, config, current);
    if (writes.isEmpty()) {
        Logger::instance()->warn(
            QStringLiteral("%1%2 配置无变化或该端口布局不受支持").arg(portName).arg(index));
        return;
    }

    // Remember the pin so the decoder keeps showing the chosen mode instead of
    // falling back to "uninitialised" when the bits equal their reset value.
    m_configuredPins.insert(portName + QString::number(index));

    const QString label = GpioCodec::describeConfig(config);
    Logger::instance()->info(QStringLiteral("配置 %1%2 -> %3（%4 个寄存器写入）")
                                 .arg(portName)
                                 .arg(index)
                                 .arg(label)
                                 .arg(writes.size()));

    // Write back-to-front so a combined mode/pull/type change stays consistent.
    for (const GpioWrite &write : writes) {
        QByteArray data(4, Qt::Uninitialized);
        data.fill(0);
        const quint32 value = write.value;
        std::memcpy(data.data(), &value, 4);
        m_session->applyRegisterWrite(write.address, data);
    }
}

void MainWindow::onDeviceTreeScanned(const QVector<PeripheralStatus> &statuses, const QString &error)
{
    if (!error.isEmpty()) {
        Logger::instance()->warn(QStringLiteral("设备树读取失败: %1").arg(error));
        statusBar()->showMessage(QStringLiteral("设备树读取失败，外设列表回退为 SVD 全集"), 5000);
        // Connected but unreadable: still let the user browse the family list
        // rather than leaving an empty tree.
        if (m_session->connected() && m_tree->isWaiting())
            populateDeviceUi();
        return;
    }

    // Annotate with package bonding: bus-readable GPIO ports D/E/F/G still answer
    // on the die even when a 48-pin package bonds none of their pins, so the pin
    // table has to override the read result.
    QVector<PeripheralStatus> annotated = statuses;
    if (const PackageDescription *package = m_packagePage
                                                ? m_packagePage->view()->currentDescription()
                                                : nullptr;
        package && package->isValid()) {
        m_bondedBits.clear();
        int overridden = 0;
        // gpioPorts(), not bondedPorts(): a port bonded only through dedicated
        // pins (PH0=OSC_IN/PH1=OSC_OUT/PH6=NRST) is not a GPIO port, so it must
        // neither appear in the tree nor get a 16-cell matrix.
        const QHash<QString, QVector<int>> bondedPorts = package->gpioPorts();
        for (auto &status : annotated) {
            const QString gpioName = status.name;
            if (!gpioName.startsWith(QLatin1String("GPIO")))
                continue;
            const QString port = QStringLiteral("P") + gpioName.mid(4, 1);
            const int bonded = bondedPorts.value(port).size();
            status.bondedPins = bonded;
            m_bondedBits.insert(port, bondedPorts.value(port));
            if (bonded == 0 && status.reachability == PeripheralStatus::Readable) {
                ++overridden;
                Logger::instance()->info(
                    QStringLiteral("%1 的寄存器可读，但 %2 封装未引出该端口 -> 视为不可用")
                        .arg(gpioName, package->packageName));
            }
        }
        if (overridden > 0) {
            Logger::instance()->info(QStringLiteral("按封装键合数据修正了 %1 个 GPIO 端口")
                                         .arg(overridden));
        }
    }

    m_tree->setStatuses(annotated);
    m_statuses = annotated;

    // First moment we know what this part really has: fill the tree and views.
    if (m_tree->isWaiting())
        populateDeviceUi();

    int onChip = 0;
    int clocked = 0;
    int missing = 0;
    for (const auto &status : statuses) {
        if (status.onChip())
            ++onChip;
        if (status.clockKnown && status.clockEnabled)
            ++clocked;
        if (status.reachability == PeripheralStatus::Missing)
            ++missing;
    }

    Logger::instance()->info(QStringLiteral("设备树已更新: 本芯片存在 %1/%2，时钟已开 %3，缺失 %4")
                                 .arg(onChip)
                                 .arg(statuses.size())
                                 .arg(clocked)
                                 .arg(missing));
    statusBar()->showMessage(QStringLiteral("设备树: %1/%2 个外设存在，%3 个时钟已开启")
                                 .arg(onChip)
                                 .arg(statuses.size())
                                 .arg(clocked),
                             6000);

    // Refresh the current peripheral view in case it disappeared from this part.
    if (!m_currentPeripheral.isEmpty()) {
        const auto it = std::find_if(statuses.constBegin(), statuses.constEnd(),
                                     [this](const PeripheralStatus &s) {
                                         return s.name == m_currentPeripheral;
                                     });
        if (it != statuses.constEnd() && !it->onChip())
            Logger::instance()->warn(QStringLiteral("%1 在当前芯片上不存在").arg(m_currentPeripheral));
    }
}

void MainWindow::onPinLevel(const QString &portName, int index, bool high)
{
    const QString gpioName = QStringLiteral("GPIO") + portName.mid(1);
    const SvdPeripheral *peripheral = m_device.findPeripheral(gpioName);
    if (!peripheral) {
        Logger::instance()->warn(QStringLiteral("找不到外设 %1").arg(gpioName));
        return;
    }
    if (!m_session->connected()) {
        Logger::instance()->warn(QStringLiteral("目标未连接，无法改变电平"));
        return;
    }

    GpioWrite write;
    QString registerName;
    if (!GpioCodec::encodePinLevel(*peripheral, index, high, m_session->snapshot(*peripheral), &write,
                                   &registerName)) {
        Logger::instance()->warn(QStringLiteral("%1%2 没有输出数据寄存器").arg(portName).arg(index));
        return;
    }

    QByteArray data(4, Qt::Uninitialized);
    data.fill(0);
    const quint32 value = write.value;
    std::memcpy(data.data(), &value, 4);

    Logger::instance()->info(QStringLiteral("%1%2 -> %3: %4.%5 = %6")
                                 .arg(portName)
                                 .arg(index)
                                 .arg(high ? QStringLiteral("高电平") : QStringLiteral("低电平"),
                                      peripheral->name, registerName, hex32(value)));
    m_session->applyRegisterWrite(write.address, data);
}

void MainWindow::startPolling()
{
    if (!m_session->connected()) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
        return;
    }
    if (m_pinned.isEmpty()) {
        m_pollLabel->setText(QStringLiteral("轮询: 仅当前外设"));
    } else {
        m_pollLabel->setText(QStringLiteral("轮询: %1 个外设 / %2 ms").arg(m_pinned.size()).arg(m_pollIntervalMs));
    }
    m_pollTimer->start();
    onPollTimeout();
}

void MainWindow::stopPolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_pollLabel)
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
}

void MainWindow::onPollTimeout()
{
    if (!m_session->connected())
        return;

    QStringList targets = m_pinned;
    if (!m_currentPeripheral.isEmpty() && !targets.contains(m_currentPeripheral, Qt::CaseInsensitive))
        targets << m_currentPeripheral;

    for (const QString &name : targets) {
        const SvdPeripheral *peripheral = m_device.findPeripheral(name);
        if (peripheral)
            m_session->requestPeripheralRead(*peripheral);
    }
}

} // namespace mrd
