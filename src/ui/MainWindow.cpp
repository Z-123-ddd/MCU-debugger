#include "ui/MainWindow.h"

#include "core/AppSettings.h"
#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/JLinkGdbProbe.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/MemoryProbe.h"
#include "core/PeripheralPresence.h"
#include "core/PartTable.h"
#include "core/PeripheralRoles.h"
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
#include "ui/ScopePage.h"
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
#include <QScopedValueRollback>
#include <QPushButton>
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

namespace {

/// Consecutive "session lost" read failures before polling is stopped.
constexpr int kDeadSessionThreshold = 3;

} // namespace

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
    connect(m_probe.get(), &ProbeController::earlyIdentified, this, &MainWindow::onEarlyIdentified);
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

    // One UI refresh per burst of read completions instead of one per read.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(30);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        m_refreshPending = false;
        refreshValues();
        refreshLiveValues();
        updateCpuStateLabel(); // the fault registers come through the same path
    });

    // Timer counters change slowly and each read is a full probe round trip, so
    // they are sampled at a lower rate than the registers.
    m_counterTimer = new QTimer(this);
    m_counterTimer->setInterval(qMax(500, m_pollIntervalMs * 2));
    connect(m_counterTimer, &QTimer::timeout, this, &MainWindow::pollTimerCounters);

    // Fault registers are six word reads; sampling them at the poll rate keeps
    // "did it just crash" responsive without meaningful bus traffic.
    m_faultTimer = new QTimer(this);
    m_faultTimer->setInterval(qMax(200, m_pollIntervalMs));
    connect(m_faultTimer, &QTimer::timeout, this, &MainWindow::pollCpuHealth);

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
    viewMenu->setObjectName(QStringLiteral("ViewMenu"));
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
    m_memoryLabel = new QLabel(this);
    m_memoryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // CPU health: the one thing you want to see when a target misbehaves.
    m_cpuStateLabel = new QLabel(this);
    m_cpuStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cpuStateLabel->setCursor(Qt::PointingHandCursor);
    m_cpuStateLabel->setToolTip(QStringLiteral("CPU / 故障状态，点击查看详细信息"));
    m_cpuStateLabel->installEventFilter(this);

    statusBar()->addWidget(m_deviceLabel, 1);
    statusBar()->addPermanentWidget(m_memoryLabel);
    statusBar()->addPermanentWidget(m_cpuStateLabel);
    statusBar()->addPermanentWidget(m_pollLabel);
    statusBar()->addPermanentWidget(m_connLabel);
    updateMemoryLabel();
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
    // Item 0 is "auto": attach with a generic core first, then resolve the part
    // from DBGMCU_IDCODE. Pre-filling the box with the *previous* board's name was
    // what made a freshly plugged part keep reporting the old one -- both as a
    // display ("it still says H473") and because the name was handed to J-Link.
    // Item 0 is the "auto" slot and starts as a short label; once a part is
    // identified its text becomes the part number itself ("N32G455CE"), which is
    // all that is worth showing there. The item's *data* stays empty, so
    // effectiveChip() still returns "" and the connection keeps using the generic
    // Cortex-M4 probe + DBGMCU_IDCODE path.
    m_chipBox->addItem(QStringLiteral("自动"), QString());
    for (const auto &entry : DeviceIdentifier::database())
        m_chipBox->addItem(entry.probeRsChip, entry.probeRsChip);
    m_chipBox->setToolTip(QStringLiteral(
        "默认按芯片的 DEV_ID 自动识别。\n"
        "只有自动识别不出来时，才需要在这里选/填具体料号（例：N32G455REL7）。"));
    AppSettings::migrateChipNameSetting();
    const QString savedChip = AppSettings::chipName();
    if (savedChip.isEmpty()) {
        m_chipBox->setCurrentIndex(0); // auto
    } else {
        m_chipBox->setCurrentText(savedChip);
    }
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

    // --- variable scope (Keil .axf + RTT) -----------------------------------
    // Appended last so the hardcoded setCurrentIndex(2) that jumps to the register
    // tab keeps pointing at the same page.
    m_scopePage = new ScopePage(m_tabs);
    m_tabs->addTab(m_scopePage, QStringLiteral("变量曲线 (J-Scope)"));
    m_scopePage->setProbe(m_probe.get());

    // Variable selection lives in its own dock rather than inside the tab: it is the
    // tallest thing on the page and would otherwise squeeze the waveform. A dock can
    // be dragged, floated into a second monitor, or hidden while plotting.
    m_variableDock = new QDockWidget(QStringLiteral("变量选择"), this);
    m_variableDock->setObjectName(QStringLiteral("VariableDock"));
    m_variableDock->setFeatures(QDockWidget::DockWidgetMovable
                                | QDockWidget::DockWidgetFloatable
                                | QDockWidget::DockWidgetClosable);
    m_variableDock->setMinimumWidth(260);
    m_variableDock->setWidget(m_scopePage->variableSelector());
    addDockWidget(Qt::LeftDockWidgetArea, m_variableDock);
    // Show it as a tab next to the peripheral tree, so opening it does not halve the
    // width available to the middle column.
    if (auto *treeDock = findChild<QDockWidget *>(QStringLiteral("PeripheralDock")))
        tabifyDockWidget(treeDock, m_variableDock);
    m_variableDock->hide(); // nothing to select before a symbol file is imported
    if (auto *viewMenu = findChild<QMenu *>(QStringLiteral("ViewMenu")))
        viewMenu->addAction(m_variableDock->toggleViewAction());
    connect(m_scopePage, &ScopePage::symbolsImported, this, [this] {
        // First import: reveal the selector, since that is the next thing to do.
        if (m_variableDock)
            m_variableDock->show();
    });
    connect(m_scopePage, &ScopePage::statusMessage, this, [this](const QString &text) {
        statusBar()->showMessage(text, 5000);
    });

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
    // Only an explicit choice is remembered; "auto" is stored as empty so a stale
    // part number never comes back as the default on the next start.
    AppSettings::setChipName(effectiveChip());
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

void MainWindow::updateMemoryLabel()
{
    // Flash / RAM of the attached part. Filled from data/parts.json by the
    // identification step; before that there is nothing truthful to show, so the
    // label stays empty rather than printing a placeholder size.
    if (!m_memoryLabel)
        return;

    const bool haveFlash = m_flashBytes > 0;
    const bool haveRam = m_ramBytes > 0;
    if (!haveFlash && !haveRam) {
        m_memoryLabel->clear();
        m_memoryLabel->setToolTip(QString());
        return;
    }

    // Shared with the self-test so the wording is covered.
    m_memoryLabel->setText(PartTable::memorySummary(m_flashBytes, m_ramBytes));

    QString tip = m_partNumber.isEmpty() ? QStringLiteral("当前器件")
                                         : QStringLiteral("当前器件 %1").arg(m_partNumber);
    if (haveFlash)
        tip += QStringLiteral("\nFlash: %1").arg(PartTable::formatSize(m_flashBytes));
    if (haveRam)
        tip += QStringLiteral("\nRAM:   %1").arg(PartTable::formatSize(m_ramBytes));
    // The pack prose spells out how the RAM total is composed (core SRAM + CCM),
    // which matters when sizing buffers.
    if (!m_sramText.isEmpty())
        tip += QStringLiteral("\n  SRAM %1").arg(m_sramText);
    if (!m_ccmText.isEmpty())
        tip += QStringLiteral("\n  CCM SRAM %1").arg(m_ccmText);
    m_memoryLabel->setToolTip(tip);
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

    parkDeviceViews(QStringLiteral("连接目标后自动读取设备树"));

    Logger::instance()->debug(QStringLiteral("已加载 %1 的 SVD；连接后将按目标回读设备树")
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
    if (!m_chipBox)
        return QString();
    // Item 0 means "auto": return empty so identification goes through DBGMCU_IDCODE
    // instead of trusting a remembered name. A typed value makes currentIndex() -1,
    // so typing still counts as an explicit override.
    if (m_chipBox->currentIndex() == 0)
        return QString();
    const QString text = m_chipBox->currentText().trimmed();
    if (text == m_chipBox->itemText(0))
        return QString(); // still showing the auto label
    return text;
}

void MainWindow::parkDeviceViews(const QString &reason)
{
    // Everything below is derived from the attached part, so it all has to go.
    m_currentPeripheral.clear();
    m_currentRegister = -1;

    if (m_inspector) {
        m_inspector->clear();          // title / table / editor back to "unselected"
        m_inspector->setConnected(m_session->connected());
    }
    if (m_strip)
        m_strip->setRegister(nullptr);
    if (m_registerView)
        m_registerView->setPeripheral(nullptr);
    if (m_portGrid)
        m_portGrid->clear();
    if (m_packagePage)
        m_packagePage->showWaiting(reason);
    if (m_tree)
        m_tree->showWaiting(reason);
}

void MainWindow::setConnectedUi(bool connected)
{
    m_inspector->setConnected(connected);
    m_connLabel->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    if (connected) {
        // A fresh session must not inherit counters from the previous target.
        m_lastCounter.clear();
    }
    if (!connected) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
        // Counter readings are meaningless once the probe is gone.
        if (m_tree)
            m_tree->clearLiveValues();
        m_lastCounter.clear();
    }
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
    m_configuredPins.clear();
    // The right-hand inspector kept the last register on screen after a
    // disconnect, which read as if it were still live.
    parkDeviceViews(QStringLiteral("已断开连接。重新连接后将自动识别并渲染引脚布局"));
    if (m_scopePage)
        m_scopePage->detachTarget();
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
    Logger::instance()->info(QStringLiteral("探针 %1 %2 SN %3")
                                 .arg(info.backend, info.version,
                                      info.probeSerials.isEmpty() ? QStringLiteral("?")
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

    AppSettings::setChipName(effectiveChip()); // empty when in auto mode

    // Auto mode already identified the part while attaching (it had to, to pick
    // the J-Link device name), so ask again only when in manual mode or when that
    // early attempt failed.
    if (m_identifyPending) {
        m_identifyPending = false;
        Logger::instance()->debug(
            QStringLiteral("复用连接时已完成的识别结果"));
        const IdentificationResult early = m_pendingIdentification;
        m_pendingIdentification = IdentificationResult();
        onIdentifyFinished(true, QString(), early);
        return;
    }
    m_probe->requestIdentify(effectiveChip());
}

void MainWindow::onEarlyIdentified(bool ok, const QString &error, const IdentificationResult &result)
{
    // Only remember it; onProbeConnectFinished runs right after and decides.
    m_pendingIdentification = result;
    m_identifyPending = ok;
    Q_UNUSED(error);
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

    // "Identified" with no part number (or no SVD) means the DEV_ID is not in the
    // database. Treating that as success used to leave the previous chip's SVD
    // loaded, so a freshly plugged G455 still looked like the old H473: the views
    // have to be parked instead of pretending.
    if (result.mcuName.isEmpty() || result.svdPath.isEmpty()) {
        const QString devIdText =
            result.devId != 0
                ? QStringLiteral("DEV_ID 0x%1").arg(result.devId, 3, 16, QLatin1Char('0'))
                : QStringLiteral("未读到 DEV_ID");
        Logger::instance()->warn(
            QStringLiteral("无法确定器件型号（%1）。原 SVD 已卸载，请在工具栏选择芯片后重连。")
                .arg(devIdText));
        statusBar()->showMessage(
            QStringLiteral("无法确定器件型号（%1）—— 请手动选择芯片").arg(devIdText));
        if (!m_svdPath.isEmpty()) {
            // Drop the stale device model, otherwise the register list and pinout
            // keep describing the previous part.
            m_device = SvdDevice();
            m_svdPath.clear();
            if (m_session)
                m_session->clearDevice();
        }
        m_capabilities.clear();
        m_flashBytes = 0;
        m_ramBytes = 0;
        m_sramText.clear();
        m_ccmText.clear();
        m_partNumber.clear();
        updateMemoryLabel();
        parkDeviceViews(QStringLiteral("无法确定器件型号（%1），请手动选择芯片后重连")
                            .arg(devIdText));
        m_deviceLabel->setText(QStringLiteral("未确定器件（%1）").arg(devIdText));

        // The family is known and the pack lists its order codes, so offer them as
        // a choice instead of making the user type a part number. Picking one is
        // remembered against this DEV_ID, so the next connection is automatic.
        if (!result.familyPartCandidates.isEmpty()) {
            QStringList labels;
            for (const QString &part : result.familyPartCandidates) {
                const PartTable::Entry entry = PartTable::lookup(part);
                QString detail = part;
                if (entry.valid) {
                    QStringList bits;
                    if (!entry.package.isEmpty())
                        bits << entry.package;
                    if (entry.flashBytes > 0)
                        bits << QStringLiteral("Flash %1").arg(PartTable::formatSize(entry.flashBytes));
                    if (entry.ramBytes > 0)
                        bits << QStringLiteral("RAM %1").arg(PartTable::formatSize(entry.ramBytes));
                    if (!bits.isEmpty())
                        detail += QStringLiteral("    (") + bits.join(QStringLiteral(" / "))
                                  + QLatin1Char(')');
                }
                labels << detail;
            }

            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this, QStringLiteral("选择具体料号"),
                QStringLiteral("已通过内存探测确定容量（%1），但同容量还有多个封装。\n"
                               "请选择芯片上印的料号（只需一次，之后会自动记住）：")
                    .arg(result.memoryProbe.valid() ? MemoryProbe::describe(result.memoryProbe)
                                                    : QStringLiteral("未知")),
                labels, 0, false, &ok);
            if (ok && !picked.isEmpty()) {
                // The label carries the part number first, then the detail.
                const QString part = picked.section(QLatin1Char(' '), 0, 0).trimmed();
                if (!part.isEmpty()) {
                    m_chipBox->setCurrentText(part);
                    if (result.devId != 0) {
                        QString error;
                        DeviceIdentifier::rememberDevId(result.devId, part, &error);
                    }
                    Logger::instance()->info(
                        QStringLiteral("已按选择使用 %1；下次连接 %2 将直接识别")
                            .arg(part, devIdText));
                    statusBar()->showMessage(
                        QStringLiteral("已选择 %1，正在重新识别…").arg(part), 4000);
                    // Re-run identification with the chosen chip name.
                    m_probe->requestIdentify(m_chipBox->currentText().trimmed());
                }
            }
        } else if (result.devId != 0) {
            Logger::instance()->info(
                QStringLiteral("提示: 在工具栏选中正确芯片并重连后，会记住 %1 对应的型号")
                    .arg(devIdText));
        }
        startPolling();
        return;
    }

    // Always show the part that is really attached. Keeping a stale name here was
    // half of the "plugged a G455 but it still says H473" problem: the box is fed
    // back into the next connection as the J-Link device name, and used to be
    // treated as a manual override too.
    const QString detectedChip =
        !result.partNumber.isEmpty() ? result.partNumber : result.probeRsChip;
    if (!detectedChip.isEmpty()) {
        if (effectiveChip().isEmpty()) {
            // Auto mode: show just the part number. Writing it as a *selection* would
            // turn the next connection into a manual override for this board -- the
            // original bug -- so only the label changes and the item's data stays
            // empty (still auto).
            m_chipBox->setItemText(0, detectedChip);
            m_chipBox->setToolTip(QStringLiteral(
                                      "已按 DEV_ID 自动识别为 %1。\n"
                                      "正常情况无需手动选择；识别失败时再在这里指定料号。")
                                      .arg(detectedChip));
            Logger::instance()->debug(
                QStringLiteral("工具栏显示 %1，模式仍为自动").arg(detectedChip));
        } else if (m_chipBox->currentText().trimmed() != detectedChip) {
            Logger::instance()->debug(
                QStringLiteral("手动指定 %1，实测为 %2 —— 按实测识别")
                    .arg(m_chipBox->currentText().trimmed(), detectedChip));
        }
    }

    if (!result.svdPath.isEmpty() && QFileInfo::exists(result.svdPath)
        && (m_svdPath.isEmpty() || QFileInfo(result.svdPath).fileName() != QFileInfo(m_svdPath).fileName())) {
        openSvdFile(result.svdPath);
    }

    // Vendor per-model peripheral counts, used by the static presence check to
    // drop blocks this model does not implement.
    const QString chip = !result.mcuName.isEmpty() ? result.mcuName : result.probeRsChip;
    m_capabilities = result.capabilities;
    // Flash / RAM for the status bar (0 = unknown, label stays blank).
    m_flashBytes = result.flashBytes;
    m_ramBytes = result.ramBytes;
    m_sramText = result.sramText;
    m_ccmText = result.ccmText;
    m_partNumber = result.partNumber;
    m_portProbe = result.portProbe;
    updateMemoryLabel();
    if (!m_capabilities.isEmpty()) {
        QStringList parts;
        for (auto it = m_capabilities.constBegin(); it != m_capabilities.constEnd(); ++it)
            parts << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        parts.sort();
        Logger::instance()->debug(QStringLiteral("厂商能力清单（%1）: %2")
                                      .arg(chip.isEmpty() ? QStringLiteral("未知器件") : chip,
                                           parts.join(QStringLiteral("  "))));
    }

    // Draw the pinout now: this is the first moment we know the part number and
    // the bonded IO count, so nothing is pre-rendered before the connection.
    if (m_packagePage) {
        // Packages that remain possible when only the family is known: offered in
        // the page's selector so the user does not have to type a part number.
        QStringList packageChoices;
        for (const QString &part : result.familyPartCandidates) {
            const PartTable::Entry entry = PartTable::lookup(part);
            if (entry.valid && !entry.package.isEmpty()) {
                for (const QString &name : entry.package.split(QLatin1Char('/'),
                                                               Qt::SkipEmptyParts))
                    packageChoices << name.trimmed();
            }
        }
        packageChoices.removeDuplicates();
        packageChoices.sort();

        const bool drawn = m_packagePage->renderForChip(chip, result.ioCount, result.pinCount,
                                                        result.packageName, packageChoices);
        if (drawn) {
            Logger::instance()->debug(QStringLiteral("引脚布局已渲染: %1 (IO %2 / 脚 %3)")
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
    // Give the scope page the RAM window so it can locate the RTT control block
    // (the ELF's _SEGGER_RTT symbol is preferred when a symbol file is loaded).
    if (m_scopePage) {
        const quint32 ramStart = 0x20000000u;
        const quint32 ramSize = result.ramBytes > 0 ? quint32(result.ramBytes)
                                                    : 128u * 1024u;
        m_scopePage->attachTarget(ramStart, ramSize);
    }

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
    // Keep the two bit-field strips in step: the one under the register page and
    // the one inside the inspector. Never push the selection back into the object
    // that emitted it -- that is an unbounded ping-pong
    // (inspector.fieldSelected -> here -> inspector.selectField -> fieldSelected
    //  -> here ...), which used to blow the stack on the first click.
    if (m_syncingFieldSelection)
        return;
    QScopedValueRollback<bool> guard(m_syncingFieldSelection, true);

    if (m_strip)
        m_strip->setSelectedField(index);
    if (m_inspector && m_inspector->selectedField() != index)
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

    // Feed the package view so pins are coloured by their live mode (one repaint
    // per port instead of one per pin).
    if (m_packagePage && m_packagePage->view())
        m_packagePage->view()->setLivePort(port, pins, m_session->connected());
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

void MainWindow::refreshLiveValues()
{
    if (!m_tree)
        return;

    if (!m_session->connected()) {
        m_tree->clearLiveValues();
        m_lastCounter.clear();
        return;
    }

    for (const auto &peripheral : m_device.peripherals) {
        if (!PeripheralRoles::isTimer(peripheral))
            continue;
        const SvdRegister *counter = PeripheralRoles::counterRegister(peripheral);
        if (!counter)
            continue; // e.g. WWDG-style timers without a readable count

        const quint32 address = peripheral.baseAddress + counter->addressOffset;
        const RegisterValue value = m_session->valueFor(address);
        if (!value.valid) {
            m_tree->setLiveValue(peripheral.name, QStringLiteral("—"));
            continue;
        }

        const quint32 count = value.value;
        const auto previous = m_lastCounter.constFind(peripheral.name);
        const bool hadPrevious = (previous != m_lastCounter.constEnd());
        const bool advanced = hadPrevious && previous.value() != count;
        m_lastCounter.insert(peripheral.name, count);

        // The arrow is the point of this column: a counter that never moves means
        // the peripheral is clocked but not running (or its clock is gated off).
        m_tree->setLiveValue(peripheral.name,
                             QStringLiteral("%1 %2")
                                 .arg(count)
                                 .arg(hadPrevious ? (advanced ? QStringLiteral("↑")
                                                             : QStringLiteral("·"))
                                                  : QStringLiteral(" ")));
        m_tree->setLiveToolTip(
            peripheral.name,
            QStringLiteral("%1 = %2 (0x%3)")
                .arg(counter->name,
                     QString::number(count),
                     QString::number(count, 16).toUpper()));
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_cpuStateLabel && event->type() == QEvent::MouseButtonRelease) {
        showFaultDetails();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::pollCpuHealth()
{
    if (!m_session || !m_session->connected()) {
        if (m_cpuHealth != CpuHealth::Unknown) {
            m_cpuHealth = CpuHealth::Unknown;
            m_fault = FaultSnapshot();
            updateCpuStateLabel();
        }
        return;
    }
    // Values come back through the normal read path and are cached by address.
    for (quint32 address : faultProbeAddresses())
        m_session->requestRawRead(address);
}

void MainWindow::updateCpuStateLabel()
{
    if (!m_cpuStateLabel)
        return;

    // Read whatever the last poll produced. Anything missing stays "unknown"
    // rather than being reported as healthy.
    FaultSnapshot snapshot;
    const auto fetch = [this](quint32 address, quint32 *out) {
        const RegisterValue value = m_session->valueFor(address);
        if (!value.valid)
            return false;
        *out = value.value;
        return true;
    };
    snapshot.cfsrValid = fetch(FaultStatus::kCfsr, &snapshot.cfsr);
    snapshot.hfsrValid = fetch(FaultStatus::kHfsr, &snapshot.hfsr);
    snapshot.dfsrValid = fetch(FaultStatus::kDfsr, &snapshot.dfsr);
    snapshot.shcsrValid = fetch(FaultStatus::kShcsr, &snapshot.shcsr);
    snapshot.mmfarValid = fetch(FaultStatus::kMmfar, &snapshot.mmfar)
                          && (snapshot.cfsr & (1u << 7)) != 0;
    snapshot.bfarValid = fetch(FaultStatus::kBfar, &snapshot.bfar)
                         && (snapshot.cfsr & (1u << 15)) != 0;
    m_fault = snapshot;

    const bool halted = m_session->connected() && m_cpuHalted;
    m_cpuHealth = FaultStatus::evaluate(snapshot, halted);

    const Palette palette = Theme::current();
    QString text;
    QString colour;
    switch (m_cpuHealth) {
    case CpuHealth::Unknown:
        text = QStringLiteral("CPU —");
        colour = palette.textDim.name();
        break;
    case CpuHealth::Normal:
        text = QStringLiteral("CPU 正常");
        colour = QStringLiteral("#15803D");
        break;
    case CpuHealth::Halted:
        text = QStringLiteral("CPU 已暂停");
        colour = QStringLiteral("#B45309");
        break;
    case CpuHealth::FaultFlags:
        text = QStringLiteral("CPU 有故障标志");
        colour = QStringLiteral("#B45309");
        break;
    case CpuHealth::HardFault:
        text = QStringLiteral("CPU HardFault");
        colour = QStringLiteral("#B91C1C");
        break;
    }
    m_cpuStateLabel->setText(text);
    m_cpuStateLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: %2;")
                                       .arg(colour,
                                            m_cpuHealth == CpuHealth::HardFault
                                                    || m_cpuHealth == CpuHealth::FaultFlags
                                                ? QStringLiteral("bold")
                                                : QStringLiteral("normal")));

    const QString detail = FaultStatus::summary(m_cpuHealth, snapshot);
    m_cpuStateLabel->setToolTip(QStringLiteral("%1%2%3")
                                    .arg(detail,
                                         snapshot.cfsrValid
                                             ? QStringLiteral("%1CFSR 0x%2  HFSR 0x%3")
                                                   .arg(QString::fromUtf8("\n"))
                                                   .arg(snapshot.cfsr, 8, 16, QLatin1Char('0'))
                                                   .arg(snapshot.hfsr, 8, 16, QLatin1Char('0'))
                                             : QString(),
                                         QStringLiteral("%1（点击查看详情）").arg(QString::fromUtf8("\n"))));
}

void MainWindow::showFaultDetails()
{
    // Everything needed to start debugging a crash: which fault fired, the
    // escalation source, and the faulting addresses when the hardware recorded
    // them as precise.
    const FaultSnapshot snapshot = m_fault;
    QStringList lines;
    lines << QStringLiteral("CPU 状态: %1").arg(FaultStatus::summary(m_cpuHealth, snapshot));

    const QStringList flags = FaultStatus::decode(snapshot);
    lines << QStringLiteral("置位的故障标志: %1")
                 .arg(flags.isEmpty() ? QStringLiteral("（无）") : flags.join(QStringLiteral(", ")));
    if (snapshot.cfsrValid)
        lines << QStringLiteral("CFSR = 0x%1").arg(snapshot.cfsr, 8, 16, QLatin1Char('0'));
    if (snapshot.hfsrValid)
        lines << QStringLiteral("HFSR = 0x%1").arg(snapshot.hfsr, 8, 16, QLatin1Char('0'));
    if (snapshot.dfsrValid)
        lines << QStringLiteral("DFSR = 0x%1").arg(snapshot.dfsr, 8, 16, QLatin1Char('0'));
    if (snapshot.mmfarValid)
        lines << QStringLiteral("MMFAR = 0x%1（出错的数据地址）")
                     .arg(snapshot.mmfar, 8, 16, QLatin1Char('0'));
    if (snapshot.bfarValid)
        lines << QStringLiteral("BFAR  = 0x%1（出错的访问地址）")
                     .arg(snapshot.bfar, 8, 16, QLatin1Char('0'));
    if (!snapshot.cfsrValid && !snapshot.hfsrValid)
        lines << QStringLiteral("尚未读到 SCB 故障寄存器（未连接或该后端不支持该地址）");

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("MCU 状态 / 故障详情"));
    box.setIcon(m_cpuHealth == CpuHealth::HardFault ? QMessageBox::Critical : QMessageBox::Information);
    box.setText(lines.join(QLatin1Char('\n')));
    QPushButton *clear = nullptr;
    if (!FaultStatus::clearWrites(snapshot).isEmpty()) {
        clear = box.addButton(QStringLiteral("清除故障标志"), QMessageBox::ActionRole);
        clear->setToolTip(QStringLiteral("向 CFSR/HFSR 写回当前值（写 1 清零）"));
    }
    box.addButton(QMessageBox::Close);
    box.exec();

    if (clear && box.clickedButton() == clear) {
        for (const auto &write : FaultStatus::clearWrites(snapshot)) {
            m_session->writeRawWord(write.first, write.second);
        }
        Logger::instance()->info(QStringLiteral("已请求清除故障标志"));
        pollCpuHealth();
    }
}

void MainWindow::onValuesChanged(quint32 firstAddress, quint32 lastAddress)
{
    if (!m_firstPollLogged && m_session->connected()) {
        m_firstPollLogged = true;
        Logger::instance()->info(QStringLiteral("首次读取完成: %1 .. %2（轮询间隔 %3 ms）")
                                     .arg(hexAddr(firstAddress), hexAddr(lastAddress))
                                     .arg(m_pollIntervalMs));
    }

    // A successful read proves the session is alive again.
    resetReadFailureState();

    // A poll produces one completion per peripheral plus one per timer counter;
    // refreshing per completion would rebuild the views a dozen times per tick.
    // Merge them: the timer fires once, 30 ms after the first completion.
    m_refreshPending = true;
    if (m_refreshTimer && !m_refreshTimer->isActive())
        m_refreshTimer->start();
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
    // "GDB Server 未连接" / "探针未打开" means the session is gone, not that one
    // register misbehaved. Repeating that line for every polled address buries the
    // real problem, so it is counted and reported once.
    const bool sessionLost =
        error.contains(QStringLiteral("未连接"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("not connected"), Qt::CaseInsensitive)
        || error.contains(QStringLiteral("探针未打开"), Qt::CaseInsensitive);

    if (!sessionLost) {
        Logger::instance()->warn(QStringLiteral("读取 %1 失败: %2").arg(hexAddr(address), error));
        return;
    }

    ++m_deadSessionReads;
    if (!m_readErrorLogged) {
        m_readErrorLogged = true;
        m_lastReadError = error;
        Logger::instance()->warn(
            QStringLiteral("读取 %1 失败: %2 —— 会话已断开迹象，连续 %3 次后自动停止轮询")
                .arg(hexAddr(address), error)
                .arg(kDeadSessionThreshold));
    }

    if (m_deadSessionReads < kDeadSessionThreshold)
        return;

    // Enough evidence: stop polling, park the views and say so once.
    Logger::instance()->error(
        QStringLiteral("连续 %1 次读取失败（%2），判定连接已丢失，已停止轮询")
            .arg(m_deadSessionReads)
            .arg(m_lastReadError));
    stopPolling();
    m_session->setConnected(false);
    setConnectedUi(false);
    parkDeviceViews(QStringLiteral("连接已丢失（%1）。请重新连接").arg(m_lastReadError));
    statusBar()->showMessage(QStringLiteral("连接已丢失：%1").arg(m_lastReadError), 10000);
}

void MainWindow::resetReadFailureState()
{
    m_deadSessionReads = 0;
    m_lastReadError.clear();
    m_readErrorLogged = false;
}

void MainWindow::onCpuStateChanged(bool halted, bool ok, const QString &error)
{
    m_cpuHalted = ok && halted;
    if (ok)
        updateCpuStateLabel();
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

    // Static presence: the SVD describes the die/family, so it lists blocks this
    // model does not have (SPI5 on a 48-pin N32H473CE). Vendor per-model counts
    // (CMSIS-Pack .pdsc) plus the package pin signals decide that, without ever
    // writing to the target -- proving absence by reading would require opening
    // the peripheral's clock, which can disturb a running bus.
    {
        PeripheralPresence::Inputs inputs;
        inputs.limits = m_capabilities;
        if (const PackageDescription *package = m_packagePage
                                                    ? m_packagePage->view()->currentDescription()
                                                    : nullptr;
            package && package->isValid())
            inputs.bondedSignals = package->bondedSignals();

        const PeripheralPresence::Result presence = PeripheralPresence::analyse(m_device, inputs);
        for (auto &status : annotated) {
            const auto it = presence.absent.constFind(status.name);
            status.exists = (it == presence.absent.constEnd());
            if (it != presence.absent.constEnd())
                status.absenceReason = it.value();
        }
        // Per-peripheral absence reasons stay at DEBUG: one INFO line per hidden
        // peripheral buried the connection summary, and the reason is already
        // visible in the tree tooltip.
        if (!presence.absent.isEmpty()) {
            for (auto it = presence.absent.constBegin(); it != presence.absent.constEnd(); ++it)
                Logger::instance()->debug(
                    QStringLiteral("%1 判定为本型号不具备: %2").arg(it.key(), it.value()));
            Logger::instance()->debug(
                QStringLiteral("静态判定剔除 %1 个本型号不具备的外设（依据厂商 .pdsc 能力清单 + 封装引脚）")
                    .arg(presence.absent.size()));
        }
        // Classes where the counts disagree but no pin table exists: we keep them
        // visible on purpose (hiding a usable peripheral is worse) and say so.
        for (const QString &note : presence.undecidable)
            Logger::instance()->warn(QStringLiteral("存在性判定: %1").arg(note));

        // GPIO ports the die does not implement. Measured on N32G455 silicon: such a
        // port does not fault, its register block simply reads back as all zeros
        // with the clock enabled, so the bus scan alone reports it as "present" and
        // the tree listed GPIOD..GPIOG on a 48-pin part.
        if (!m_portProbe.absent.isEmpty()) {
            int hidden = 0;
            for (auto &status : annotated) {
                if (!GpioCodec::isGpioPort(*m_device.findPeripheral(status.name)))
                    continue;
                const QString port = GpioCodec::portName(*m_device.findPeripheral(status.name));
                if (!m_portProbe.absent.contains(port))
                    continue;
                status.exists = false;
                status.absenceReason = QStringLiteral("die 上没有 %1 端口（寄存器块读回全 0）")
                                           .arg(port);
                ++hidden;
            }
            if (hidden > 0)
                Logger::instance()->debug(
                    QStringLiteral("按端口探测隐藏 %1 个本 die 不存在的 GPIO 端口").arg(hidden));
        }
    }

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
                Logger::instance()->debug(
                    QStringLiteral("%1 的寄存器可读，但 %2 封装未引出该端口 -> 视为不可用")
                        .arg(gpioName, package->packageName));
            }
        }
        if (overridden > 0) {
            Logger::instance()->debug(QStringLiteral("按封装键合数据修正了 %1 个 GPIO 端口")
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

    Logger::instance()->debug(QStringLiteral("设备树: 本芯片存在 %1/%2，时钟已开 %3，缺失 %4")
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
    if (m_counterTimer)
        m_counterTimer->start();
    if (m_faultTimer)
        m_faultTimer->start();
    onPollTimeout();
    pollTimerCounters();
    pollCpuHealth();
}

void MainWindow::stopPolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_counterTimer)
        m_counterTimer->stop();
    if (m_faultTimer)
        m_faultTimer->stop();
    if (m_refreshTimer)
        m_refreshTimer->stop();
    m_cpuHalted = false;
    m_cpuHealth = CpuHealth::Unknown;
    updateCpuStateLabel();
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

void MainWindow::pollTimerCounters()
{
    if (!m_session->connected())
        return;

    QStringList targets = m_pinned;
    if (!m_currentPeripheral.isEmpty() && !targets.contains(m_currentPeripheral, Qt::CaseInsensitive))
        targets << m_currentPeripheral;

    // One single-register read per timer, and only for timers whose clock is known
    // to be running: a gated timer cannot count, and reading it would just provoke
    // a bus fault. Reads already covered by a window read are skipped.
    for (const auto &status : m_statuses) {
        if (status.clockKnown && !status.clockEnabled)
            continue;
        if (targets.contains(status.name, Qt::CaseInsensitive))
            continue;
        const SvdPeripheral *peripheral = m_device.findPeripheral(status.name);
        if (!peripheral || !PeripheralRoles::isTimer(*peripheral))
            continue;
        const SvdRegister *counter = PeripheralRoles::counterRegister(*peripheral);
        if (counter)
            m_session->requestRegisterRead(*peripheral, *counter);
    }
}

} // namespace mrd
