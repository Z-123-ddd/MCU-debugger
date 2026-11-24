#include "ui/MainWindow.h"

#include "core/AppSettings.h"
#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/MemoryProbe.h"
#include "core/PeripheralPresence.h"
#include "core/FirmwareImage.h"
#include "core/JLinkDeviceName.h"
#include "ui/BurnDialog.h"
#include "ui/ChipPickerDialog.h"
#include "core/FirmwareBurner.h"
#include "core/PartTable.h"
#include "core/PeripheralRoles.h"
#include "core/ProbeController.h"
#include "core/JLinkDllApi.h"
#include "core/JLinkRunner.h"
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
#include "ui/SimulationPage.h"
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

    // No stale GDB server to reap any more: this application no longer launches one. It
    // also must not go killing JLinkGDBServerCL processes, because on a developer machine
    // the one that is running probably belongs to Keil or IAR.

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
    connect(m_probe.get(), &ProbeController::cpuStateChanged, this, &MainWindow::onCpuStateChanged);
    connect(m_probe.get(), &ProbeController::deviceTreeScanned, this, &MainWindow::onDeviceTreeScanned);
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

    // --- selected register: HSS first, fast polling as the fallback ---------
    // HSS is what the user actually asked for: the probe samples the register on its own
    // timebase while the core runs, so the host is not issuing a read per displayed value.
    // It is not always available (a DLL without the interface, a halted core) and it is a
    // single global session shared with the curve page, so the fast timer below stays as
    // the fallback and the tab arbitration decides which one is in charge.
    m_registerSampler = new HssSampler(this);
    m_registerSampler->setProbe(m_probe.get());
    connect(m_registerSampler, &HssSampler::drained, this, &MainWindow::onRegisterSampled);
    connect(m_registerSampler, &HssSampler::failed, this, [this](const QString &error) {
        // start() had already returned success here, so the refusal is the probe's own
        // answer. Remember it: re-running the arbitration would ask again and be refused
        // again, every time the timer fires.
        m_registerHssRefused = true;
        m_registerHssAddress = 0;
        Logger::instance()->warn(
            QStringLiteral("寄存器实时采样未能启动，退回轮询: %1").arg(error));
        updateRegisterSampling();
    });

    // The probe fills its buffer on its own period; this only picks up what accumulated.
    m_registerDrainTimer = new QTimer(this);
    m_registerDrainTimer->setInterval(30);
    connect(m_registerDrainTimer, &QTimer::timeout, this, &MainWindow::onRegisterDrainTick);

    // The selected register also gets its own polling timer, used when HSS cannot run.
    // It reads one word and nothing else, so it can run an order of magnitude faster than
    // the peripheral sweep without adding the traffic that sweep would cost.
    m_fastPollTimer = new QTimer(this);
    m_fastPollTimer->setInterval(kFastPollIntervalMs);
    connect(m_fastPollTimer, &QTimer::timeout, this, &MainWindow::onFastPollTimeout);

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

void MainWindow::onBurnFirmware()
{
    // Opening this while disconnected is deliberate: analysing an image and merging a
    // bootloader into it are pure file work that people want to do *before* wiring the
    // board up. Only the burn itself needs a probe, so only the burn button is gated --
    // refusing to open the dialog would take the merge feature away from anyone offline.
    const bool connected = m_session->connected();

    // The order code carries the real Flash size; the detected family name does not.
    const ProbeInfo info = m_probe ? m_probe->info() : ProbeInfo();
    const QString part = !effectiveChip().isEmpty() ? effectiveChip()
        : (!info.detectedChip.isEmpty() ? info.detectedChip : info.chipName);

    BurnDialog dialog(part, connected, this);
    if (dialog.exec() != QDialog::Accepted) {
        statusBar()->clearMessage();
        return;
    }

    // takeImage(), not image(): the burn is asynchronous and outlives this dialog, so the
    // scratch file must not be deleted by the dialog's destructor while J-Link is reading
    // it. From here MainWindow owns it and every exit path calls discardScratch().
    FirmwareImage fw = dialog.takeImage();

    // Defensive: the dialog disables its own 烧录 button when opened offline, so reaching
    // here disconnected means the state changed underneath us. Say so rather than erasing
    // a target that is not there.
    if (!dialog.canBurn() || !m_session->connected()) {
        statusBar()->showMessage(
            QStringLiteral("未连接目标：本次只做了合并/分析，未执行烧录"), 8000);
        Logger::instance()->info(
            QStringLiteral("未连接目标，跳过烧录（镜像分析/合并结果未写入芯片）"));
        fw.discardScratch();
        return;
    }

    if (!fw.isValid())
        return; // the burn button stays disabled until an analysis succeeded

    // Commander needs a name the J-Link software knows; the catalogue lookup drops the
    // package/temperature suffix the DLL does not list.
    const QStringList deviceCandidates = JLinkDeviceName::candidates(part);
    const QString jlinkDevice = deviceCandidates.isEmpty()
        ? (info.detectedChip.isEmpty() ? part : info.detectedChip)
        : deviceCandidates.first();

    const QString range = QStringLiteral("0x%1 .. 0x%2")
                              .arg(fw.startAddress, 8, 16, QLatin1Char('0'))
                              .arg(fw.endAddress, 8, 16, QLatin1Char('0'));
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认烧录"),
        QStringLiteral("即将擦除并写入 %1。\n\n范围：%2\n写入：%3\n\n继续？")
            .arg(jlinkDevice, range, PartTable::formatSize(fw.span())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        fw.discardScratch();
        return;
    }

    const QString commander = JLinkRunner::locate();
    if (commander.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法烧录"),
                             QStringLiteral("未找到 JLink.exe（J-Link Commander）。\n"
                                            "请先安装 SEGGER J-Link 软件。"));
        fw.discardScratch();
        return;
    }

    // Release the probe: Commander cannot open the J-Link while the app holds it.
    onDisconnectClicked();

    // --- burn asynchronously ---------------------------------------------
    // The flash cycle takes tens of seconds. Running it on the UI thread made Windows
    // paint "not responding" over the window, so the process is driven by signals and the
    // status bar carries the progress instead.
    statusBar()->showMessage(QStringLiteral("正在烧录 %1，请稍候…（不要断开调试器）").arg(range));
    Logger::instance()->info(QStringLiteral("开始烧录 %1 → %2").arg(jlinkDevice, range));

    // Not a QObject, so its lifetime is explicit. The callback keeps it alive until the
    // commander exits, which is what the shared_ptr capture below is for.
    auto burner = std::make_shared<FirmwareBurner>(commander);
    const QString partForCallback = part;
    burner->burnAsync(jlinkDevice, fw, AppSettings::speedKHz(), /*forceErase=*/true, 300000,
                      [this, burner, fw, range, jlinkDevice, partForCallback](
                          const BurnResult &result) mutable {

        Logger::instance()->info(QStringLiteral("烧录 -> %1：%2")
                                     .arg(partForCallback,
                                          result.ok ? QStringLiteral("成功") : result.error));

        if (!result.ok) {
            statusBar()->showMessage(QStringLiteral("烧录失败：%1").arg(result.error), 10000);
            QMessageBox::critical(this, QStringLiteral("烧录失败"),
                                  QStringLiteral("%1\n\nJ-Link 输出：\n%2")
                                      .arg(result.error, result.output.right(2000)));
            fw.discardScratch();
            return;
        }

        // A merged image is the only case worth writing back: it is the artifact that now
        // contains the bootloader, and only after the verify passed.
        const bool merged = fw.isMerged();
        QString writeBackNote;
        if (merged) {
            QString error;
            const QString written = fw.writeBackOverSource(&error);
            if (written.isEmpty()) {
                writeBackNote = QStringLiteral("\n\n未能覆盖原文件：%1").arg(error);
                Logger::instance()->warn(QStringLiteral("合并镜像回写失败：%1").arg(error));
            } else {
                writeBackNote = QStringLiteral("\n\n已把合并后的镜像覆盖写入：\n%1"
                                               "（原文件备份为 .bak）")
                                    .arg(written);
                Logger::instance()->info(
                    QStringLiteral("已用合并镜像覆盖 %1（含 bootloader）").arg(written));
            }
        }
        fw.discardScratch();

        statusBar()->showMessage(
            QStringLiteral("烧录成功（%1，校验%2），正在重连确认应用运行…")
                .arg(range, result.verified ? QStringLiteral("通过") : QStringLiteral("未确认")),
            8000);

        QMessageBox::information(
            this, QStringLiteral("烧录完成"),
            QStringLiteral("已写入 %1（%2）。\n校验：%3\n耗时：%4 ms%5")
                .arg(range, jlinkDevice,
                     result.verified ? QStringLiteral("通过") : QStringLiteral("未确认"))
                .arg(result.elapsedMs)
                .arg(writeBackNote));

        // Reconnect and check that the freshly burned application really runs. This is the
        // whole reason to merge a bootloader: the board must come back up by itself.
        m_pendingPostBurnCheck = true;
        m_postBurnRange = range;
        QTimer::singleShot(200, this, &MainWindow::onConnectClicked);
    });
}

void MainWindow::reportPostBurnState()
{
    if (!m_pendingPostBurnCheck)
        return;
    m_pendingPostBurnCheck = false;

    // The check runs after the scan/validation, so reaching here means the selected part
    // matched the silicon. What is still unknown is whether the application started: a
    // bootloader with a broken application vector stays in the bootloader forever.
    const bool running = m_session->connected() && !m_cpuHalted;
    QString verdict;
    if (running) {
        verdict = QStringLiteral("重连成功，目标在运行（%1）").arg(m_postBurnRange);
        statusBar()->showMessage(verdict, 10000);
        Logger::instance()->info(verdict);
    } else {
        verdict = QStringLiteral("重连成功但目标处于停机状态：应用可能没有正常运行");
        statusBar()->showMessage(verdict, 12000);
        Logger::instance()->warn(verdict);
    }
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
    m_viewMenu = viewMenu;
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

    auto *firmwareMenu = menuBar()->addMenu(QStringLiteral("固件(&W)"));
    auto *burnAction = firmwareMenu->addAction(QStringLiteral("烧录固件(&B)..."));
    burnAction->setToolTip(QStringLiteral("分析 .bin 的起止地址与容量，确认后擦除并写入"));
    connect(burnAction, &QAction::triggered, this, &MainWindow::onBurnFirmware);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于(&A)"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, QStringLiteral("关于"),
                           QStringLiteral("<b>%1</b> %2<br/>MCU 寄存器级 UI 调试器<br/><br/>"
                                          "后端: J-Link DLL<br/>描述: CMSIS-SVD<br/>Qt %3")
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

    // No backend chooser any more: the DLL is the only transport. A read-only label keeps
    // the toolbar honest about it instead of removing the text and leaving people guessing.
    m_toolBar->addWidget(new QLabel(QStringLiteral(" 后端 "), this));
    m_toolBar->addWidget(new QLabel(QStringLiteral("J-Link DLL"), this));

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
    // A button, not an editable combo: only real order codes from data/parts.json are
    // acceptable, because the same string also becomes the J-Link device name and the
    // pinout's package. Free text could satisfy one consumer and break the other.
    m_partButton = new QPushButton(QStringLiteral("选择型号…"), this);
    m_partButton->setMinimumWidth(170);
    m_partButton->setToolTip(
        QStringLiteral("选择系列与具体料号。\n"
                       "DEV_ID 只能确定系列，封装与 Flash 容量必须由料号决定 ——\n"
                       "选错封装会让引脚图对应到错误的型号。"));
    connect(m_partButton, &QPushButton::clicked, this, &MainWindow::onPickChipClicked);
    m_toolBar->addWidget(m_partButton);
    AppSettings::migrateChipNameSetting();
    AppSettings::dropObsoleteBackendKeys();
    updateChipLabel();
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
    // The left dock is a single window containing a tab widget, exactly like the
    // central pages: the peripheral tree and the variable selector are two tabs you
    // switch by clicking, not two panes competing for width. The dock itself keeps
    // Movable/Floatable so it can still be dragged out.
    m_leftDock = new QDockWidget(QStringLiteral("外设 / 变量"), this);
    m_leftDock->setObjectName(QStringLiteral("PeripheralDock"));
    m_leftDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
                            | QDockWidget::DockWidgetClosable);
    m_leftDock->setMinimumWidth(260);

    m_leftTabs = new QTabWidget(m_leftDock);
    m_leftTabs->setDocumentMode(true);
    m_tree = new PeripheralTree(m_leftTabs);
    m_leftTabs->addTab(m_tree, QStringLiteral("外设"));
    m_leftDock->setWidget(m_leftTabs);
    addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);
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
    resizeDocks({m_leftDock, inspectorDock}, {300, 380}, Qt::Horizontal);
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
    m_registerPage = registerPage; // the tab arbitration needs to recognise this page
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


    // --- variable scope (Keil .axf -> snapshot / HSS live curves) ------------
    // Appended last so the hardcoded setCurrentIndex(2) that jumps to the register
    // tab keeps pointing at the same page.
    m_scopePage = new ScopePage(m_tabs);
    m_tabs->addTab(m_scopePage, QStringLiteral("变量曲线"));
    m_scopePage->setProbe(m_probe.get());

    // The picker lives in its own dock rather than inside the tab: it is the tallest
    // thing here, and a dock can be dragged, floated onto a second monitor, or hidden
    // while a live capture is running.
    // Second tab of the same left window.
    // Model simulation: steps a model with the same live samples the chart draws, so the
    // data going into a model is what the hardware actually produced.
    m_simulationPage = new SimulationPage(m_tabs);
    m_simulationPage->setSource(m_scopePage);
    m_tabs->addTab(m_simulationPage, QStringLiteral("模型仿真"));

    m_leftTabs->addTab(m_scopePage->variableSelector(), QStringLiteral("变量选择"));
    if (m_viewMenu)
        m_viewMenu->addAction(m_leftDock->toggleViewAction());
    connect(m_scopePage, &ScopePage::symbolsImported, this,
            [this](const mrd::SymbolTable &symbols) {
                // Bring the selector tab to the front: after importing a symbol file
                // picking variables is the next step. The peripheral tree stays
                // reachable in the same tab strip.
                if (m_leftTabs) {
                    // Selecting the tab is the whole "show the selector" action now.
                    m_leftTabs->setCurrentIndex(1);
                    if (m_leftDock)
                        m_leftDock->show();
                }
            });
    connect(m_scopePage, &ScopePage::statusMessage, this, [this](const QString &text) {
        statusBar()->showMessage(text, 5000);
    });

    // One HSS session, two candidate owners. The visible tab decides.
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onMainTabChanged);
    connect(m_registerView, &RegisterBlockView::registerSelected, this, &MainWindow::onRegisterSelected);
    connect(m_registerView, &RegisterBlockView::registerDeselected, this,
            &MainWindow::onRegisterDeselected);
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
    // Filled from the part selected in the chip picker (see applyChipSelection()).
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
    // The picked order code, or empty when none has been chosen yet. Empty is a real
    // state: register access works from the core alone, but no SVD/pinout can be shown
    // until a part is picked, and pretending otherwise would describe the wrong chip.
    return AppSettings::chipName().trimmed();
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
    updateRegisterSampling(); // m_currentRegister is cleared above, so this stops it
}

void MainWindow::setConnectedUi(bool connected)
{
    m_inspector->setConnected(connected);
    m_connLabel->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    if (connected) {
        // A fresh session must not inherit counters from the previous target.
        m_lastCounter.clear();
        m_registerHssRefused = false; // new link: HSS may well work this time
    }
    if (!connected) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
        // Counter readings are meaningless once the probe is gone.
        if (m_tree)
            m_tree->clearLiveValues();
        m_lastCounter.clear();
    }
    updateRegisterSampling(); // no link -> nothing to sample
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
    if (path.isEmpty())
        return;
    if (!openSvdFile(path))
        return;

    // Opening an SVD by hand bypasses the part selection, so the register map and the
    // pinout would describe different chips. Keep them consistent: this is a diagnostic
    // view, and the next connection re-derives everything from the selected型号.
    parkDeviceViews(
        QStringLiteral("已手动加载 %1（诊断用）。下次连接会按所选型号 %2 重新加载。")
            .arg(QFileInfo(path).fileName(),
                 AppSettings::chipName().isEmpty() ? QStringLiteral("(未选择)")
                                                  : AppSettings::chipName()));
    statusBar()->showMessage(
        QStringLiteral("已手动加载 %1；连接时将以所选型号为准").arg(QFileInfo(path).fileName()),
        6000);
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

void MainWindow::updateChipLabel()
{
    if (!m_partButton)
        return;
    const QString part = AppSettings::chipName().trimmed();
    const QString package = AppSettings::chipPackage().trimmed();
    if (part.isEmpty()) {
        m_partButton->setText(QStringLiteral("选择型号…"));
        return;
    }
    // Both are shown: the same order code can be bonded differently, and the pinout
    // follows the package, so hiding it would hide part of the selection.
    m_partButton->setText(package.isEmpty() ? part
                                            : QStringLiteral("%1 · %2").arg(part, package));
}

void MainWindow::onPickChipClicked()
{
    ChipPickerDialog dialog(AppSettings::chipName(), AppSettings::chipPackage(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString part = dialog.selectedPart();
    const QString package = dialog.selectedPackage();
    if (part.isEmpty() || package.isEmpty())
        return;
    if (part == AppSettings::chipName() && package == AppSettings::chipPackage())
        return;

    AppSettings::setChipName(part);
    AppSettings::setChipPackage(package);
    updateChipLabel();
    Logger::instance()->info(QStringLiteral("已选择 %1 · %2").arg(part, package));

    // A different part or package changes the SVD, the capacity AND the pinout, so the
    // whole device view is rebuilt. Doing this through one entry point is the point:
    // before, the pinout and the register map could be refreshed by different paths and
    // end up describing different chips.
    if (m_session->connected()) {
        if (!applyChipSelection())
            return;
        // The tree now describes a different part, so re-check it against the silicon.
        scanAndValidateDeviceTree();
        statusBar()->showMessage(QStringLiteral("已切换到 %1 · %2，正在校验").arg(part, package));
    } else {
        statusBar()->showMessage(QStringLiteral("已选择 %1 · %2，连接后生效").arg(part, package));
    }
}

void MainWindow::onConnectClicked()
{
    AppSettings::setInterfaceName(m_interfaceBox->currentText());
    AppSettings::setSpeedKHz(m_speedBox->value());

    // One connection path: the J-Link DLL, loaded into this process. It is the only
    // transport that can reach HSS, and it skips the extra process plus the ASCII-hex RSP
    // hop that the removed GDB backend paid for on every register access.
    if (!JLinkDllApi::libraryAvailable()) {
        QMessageBox::warning(
            this, QStringLiteral("未找到 J-Link DLL"),
            QStringLiteral("没有找到 JLink_x64.dll。\n\n"
                           "已检查：程序设置、环境变量（JLINK_PATH 等）、PATH、注册表、"
                           "Program Files。\n\n"
                           "请安装 SEGGER J-Link 软件包：\n"
                           "https://www.segger.com/downloads/jlink/"));
        return;
    }
    // A part and a package are now required before attaching: the SVD, the pinout and the
    // J-Link device name all come from them, and connecting without them would leave the
    // app with nothing truthful to show.
    const QString part = AppSettings::chipName().trimmed();
    const QString package = AppSettings::chipPackage().trimmed();
    if (part.isEmpty() || package.isEmpty()) {
        ChipPickerDialog dialog(AppSettings::chipName(), AppSettings::chipPackage(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (dialog.selectedPart().isEmpty() || dialog.selectedPackage().isEmpty())
            return;
        AppSettings::setChipName(dialog.selectedPart());
        AppSettings::setChipPackage(dialog.selectedPackage());
        updateChipLabel();
    }

    statusBar()->showMessage(QStringLiteral("正在打开探针..."));
    m_probe->requestOpen(QString(), m_interfaceBox->currentText(), m_speedBox->value());
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
    parkDeviceViews(QStringLiteral("已断开连接。重新连接后将按所选型号渲染设备树与引脚图"));
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

    // The J-Link device name is the order code minus the suffix SEGGER does not list
    // ("N32G455CCL7" -> "N32G455CC"). Never the bare series: handing the DLL a name it
    // does not know crashes V8.26 rather than failing softly.
    const QString part = AppSettings::chipName().trimmed();
    const QStringList candidates = JLinkDeviceName::candidates(part);
    QString jlinkName;
    for (const QString &candidate : candidates) {
        if (!JLinkDeviceName::hasCatalogue() || JLinkDeviceName::isKnown(candidate)) {
            jlinkName = candidate;
            break;
        }
    }
    if (jlinkName.isEmpty()) {
        // Nothing in the catalogue: keep the generic core rather than risk the crash.
        // Registers and memory still work; only the vendor flash algorithm is missing.
        Logger::instance()->warn(
            QStringLiteral("J-Link 目录中没有 %1 的条目，以通用 Cortex-M4 连接").arg(part));
        jlinkName = QStringLiteral("Cortex-M4");
    }
    Logger::instance()->debug(QStringLiteral("J-Link 器件名 %1（料号 %2）").arg(jlinkName, part));
    m_probe->requestConnect(jlinkName);
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
    m_connLabel->setText(QStringLiteral("已连接 %1")
                             .arg(Logger::instance() ? AppSettings::chipName()
                                                     : info.detectedChip));

    // Do not claim a connection yet: apply the selection, then probe every peripheral the
    // part is supposed to have.
    if (!applyChipSelection()) {
        refuseConnection(QStringLiteral("无法为所选型号加载 SVD，已断开"));
        return;
    }

    // A wrong pick produces a confidently wrong register map and pinout, which is worse
    // than refusing, so the peripherals are checked before the session is usable.
    statusBar()->showMessage(QStringLiteral("已连接 %1，正在校验设备树…")
                                 .arg(AppSettings::chipName()));
    scanAndValidateDeviceTree();
}

void MainWindow::scanAndValidateDeviceTree()
{
    if (m_svdPath.isEmpty()) {
        parkDeviceViews(QStringLiteral("没有 SVD，无法扫描设备树"));
        return;
    }
    if (!m_probe || !m_session->connected())
        return;
    m_probe->requestScanDeviceTree(m_svdPath);
}

void MainWindow::refuseConnection(const QString &reason)
{
    // Refusing means actually going away: leaving the session up would let the polling,
    // the port grid and the write path operate on a device model that the silicon
    // already contradicted.
    Logger::instance()->error(QStringLiteral("拒绝连接：%1").arg(reason));
    stopPolling();
    m_probe->requestDisconnect();
    m_session->setConnected(false);
    m_session->clearDevice();
    m_device = SvdDevice();
    m_capabilities.clear();
    m_flashBytes = 0;
    m_ramBytes = 0;
    m_sramText.clear();
    m_ccmText.clear();
    m_partNumber.clear();
    updateMemoryLabel();
    setConnectedUi(false);
    parkDeviceViews(reason);
    m_deviceLabel->setText(QStringLiteral("未连接（型号校验未通过）"));
    statusBar()->showMessage(QStringLiteral("已拒绝连接：%1").arg(reason));
    QMessageBox::critical(this, QStringLiteral("型号校验失败"), reason);
}

bool MainWindow::applyChipSelection()
{
    const QString part = AppSettings::chipName().trimmed();
    const QString package = AppSettings::chipPackage().trimmed();
    if (part.isEmpty()) {
        parkDeviceViews(QStringLiteral("尚未选择型号：请点工具栏「选择型号…」"));
        m_deviceLabel->setText(QStringLiteral("未选择型号"));
        return false;
    }

    const PartTable::Entry entry = PartTable::lookup(part);
    if (!entry.valid) {
        parkDeviceViews(QStringLiteral("%1 不在器件表中").arg(part));
        m_deviceLabel->setText(QStringLiteral("%1（器件表无此料号）").arg(part));
        return false;
    }

    m_capabilities = entry.capabilities;
    m_flashBytes = entry.flashBytes;
    m_ramBytes = entry.ramBytes;
    m_sramText = entry.sram;
    m_ccmText = entry.ccmSram;
    m_partNumber = entry.part;
    updateMemoryLabel();

    // --- SVD: the vendor manifest maps the order code to its file -----------
    QString svdPath;
    SvdManifestEntry manifestEntry;
    QStringList ambiguous;
    SvdLibrary library;
    library.setRoots(AppSettings::svdRoots());
    library.loadManifest();
    if (library.lookupPart(part, &manifestEntry, &ambiguous)) {
        svdPath = manifestEntry.svdPath;
    } else if (!entry.svd.isEmpty()) {
        // parts.json carries the pack-relative path; resolve it against the SVD roots.
        for (const QString &root : AppSettings::svdRoots()) {
            QDir rootDir(root);
            const QString relative = QFileInfo(entry.svd).fileName();
            // The manifest entry and the pack path can differ in directory depth, so
            // search one level down for the file name as a last resort.
            const QString direct = QDir::cleanPath(rootDir.filePath(entry.svd));
            if (QFileInfo::exists(direct)) {
                svdPath = direct;
                break;
            }
            for (const QString &sub : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString candidate = QDir::cleanPath(
                    rootDir.filePath(sub + QLatin1Char('/') + relative));
                if (QFileInfo::exists(candidate)) {
                    svdPath = candidate;
                    break;
                }
            }
            if (!svdPath.isEmpty())
                break;
        }
    }

    if (svdPath.isEmpty() || !QFileInfo::exists(svdPath)) {
        parkDeviceViews(QStringLiteral("找不到 %1 对应的 SVD 文件").arg(part));
        m_deviceLabel->setText(QStringLiteral("%1（无 SVD）").arg(part));
        return false;
    }

    if (!openSvdFile(svdPath))
        return false;

    const QString packageLabel = package.isEmpty() ? entry.package : package;
    m_deviceLabel->setText(QStringLiteral("%1 · %2 · Flash %3 / RAM %4")
                               .arg(part, packageLabel,
                                    PartTable::formatSize(entry.flashBytes),
                                    PartTable::formatSize(entry.ramBytes)));

    // --- pinout for exactly this package -----------------------------------
    if (m_packagePage) {
        QStringList choices = PartTable::packagesOf(part);
        if (choices.isEmpty() && !packageLabel.isEmpty())
            choices << packageLabel;
        const bool drawn = m_packagePage->renderForChip(part, entry.ioCount, 0,
                                                        packageLabel, choices);
        if (drawn) {
            Logger::instance()->debug(QStringLiteral("引脚图: %1 · %2（%3 个 IO）")
                                          .arg(part, packageLabel)
                                          .arg(entry.ioCount));
        } else {
            // A missing pin table must not block the session: registers still work.
            Logger::instance()->warn(
                QStringLiteral("没有 %1 · %2 的引脚表，引脚图暂不可用").arg(part, packageLabel));
        }
    }
    return true;
}

void MainWindow::onPeripheralSelected(const QString &name)
{
    const SvdPeripheral *peripheral = m_device.findPeripheral(name);
    if (!peripheral)
        return;

    m_registerHssRefused = false; // new peripheral, new attempt
    m_currentPeripheral = name;
    m_currentRegister = peripheral->registers.isEmpty() ? -1 : 0;
    m_registerView->setPeripheral(peripheral);
    m_registerView->setValues(m_session->snapshot(*peripheral), m_session->connected());
    if (m_currentRegister >= 0)
        m_registerView->setSelectedRegister(m_currentRegister);

    refreshInspector();
    refreshPortGrid();

    // The whole-peripheral read goes first: it already covers register 0, and issuing it
    // before arming the fast poll lets the back-pressure check below skip a duplicate read
    // of the same word.
    if (m_session->connected())
        m_session->requestPeripheralRead(*peripheral);
    updateRegisterSampling();
}

void MainWindow::onPinnedChanged(const QStringList &names)
{
    m_pinned = names;
    AppSettings::setPinnedPeripherals(names);
    updatePollLabel();
}

void MainWindow::onRegisterSelected(int index)
{
    m_registerHssRefused = false; // a different register deserves a fresh attempt
    m_currentRegister = index;
    refreshInspector();
    // Selection is the operator saying "watch this one", so it takes effect immediately
    // rather than on the next sweep of the peripheral. When the fast poll is already
    // running, taking one sample now is what makes switching registers feel instant --
    // otherwise the new register shows its last 200 ms-old value until the next tick.
    updateRegisterSampling();
    // Polling path only: take one sample now so switching feels instant. Under HSS the
    // first drain arrives within a millisecond on its own.
    if (m_fastPollTimer && m_fastPollTimer->isActive())
        onFastPollTimeout();
}

void MainWindow::onRegisterDeselected()
{
    // Releasing the selection returns the register to the ordinary peripheral sweep. With
    // no selection both accelerated paths stop on their own: the HSS session is handed
    // back (so the curve page can take it) and the single-register timer stands down. The
    // 200 ms sweep already reads every register of the peripheral, so the released one
    // keeps updating -- just at the shared rate.
    m_currentRegister = -1;
    m_registerHssRefused = false; // a fresh selection would deserve a fresh attempt
    // The view has already cleared its own highlight (releaseSelection() emits this), so
    // pushing -1 back into it here would be a second owner of the same state.
    refreshInspector();
    updateRegisterSampling();
    statusBar()->showMessage(
        QStringLiteral("已取消选中寄存器：恢复 200 ms 轮询更新"), 3000);
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
    // A write can leave the core halted; refresh immediately.
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
    updateRegisterSampling(); // a halted core freezes every register
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

    // --- validate against the selection BEFORE anything is shown ----------
    // A wrong part would otherwise produce a register map and a pinout for hardware that
    // is not attached, which reads as authoritative and is the failure mode this whole
    // change exists to prevent.
    const QString part = AppSettings::chipName();
    const QString package = AppSettings::chipPackage();
    const ValidationVerdict verdict = validatePeripherals(annotated, m_device.peripherals.size());
    Logger::instance()->info(QStringLiteral("型号校验（%1 · %2）：%3")
                                 .arg(part, package, verdict.summary()));
    for (const QString &line : verdict.contradictions)
        Logger::instance()->warn(QStringLiteral("  对不上的外设: %1").arg(line));

    if (!verdict.accepted) {
        QStringList lines;
        lines << verdict.reason;
        if (!verdict.contradictions.isEmpty()) {
            lines << QString();
            lines << QStringLiteral("读不到的外设（前 %1 个）：").arg(verdict.contradictions.size());
            lines << verdict.contradictions.join(QLatin1Char('\n'));
        }
        lines << QString();
        lines << QStringLiteral("请确认板上型号：当前选的是 %1 · %2。").arg(part, package);
        lines << QStringLiteral("用「选择型号…」改成实际型号后重新连接。");
        refuseConnection(lines.join(QLatin1Char('\n')));
        return;
    }

    m_tree->setStatuses(annotated);
    m_statuses = annotated;

    // First moment the part's real peripheral set is known: fill the tree and views.
    if (m_tree->isWaiting())
        populateDeviceUi();

    int onChip = 0;
    int clocked = 0;
    for (const auto &status : statuses) {
        if (status.onChip())
            ++onChip;
        if (status.clockKnown && status.clockEnabled)
            ++clocked;
    }

    Logger::instance()->debug(QStringLiteral("设备树: 本芯片存在 %1/%2，时钟已开 %3")
                                 .arg(onChip)
                                 .arg(statuses.size())
                                 .arg(clocked));

    // Say what was actually proven, not just that something was read: a tree where
    // nothing answered is not a success, and the user needs to know the map is a guess.
    if (verdict.inconclusive()) {
        statusBar()->showMessage(
            QStringLiteral("已连接 %1，但设备树无法验证（没有外设响应）").arg(part), 8000);
        Logger::instance()->warn(verdict.reason);
    } else {
        statusBar()->showMessage(
            QStringLiteral("已连接 %1 · %2 — %3").arg(part, package, verdict.summary()), 8000);
    }

    // Refresh the current peripheral view in case it disappeared from this part.
    if (!m_currentPeripheral.isEmpty()) {
        const auto it = std::find_if(statuses.constBegin(), statuses.constEnd(),
                                     [this](const PeripheralStatus &status) {
                                         return status.name == m_currentPeripheral;
                                     });
        if (it != statuses.constEnd() && !it->onChip())
            Logger::instance()->warn(QStringLiteral("%1 在当前芯片上不存在").arg(m_currentPeripheral));
    }

    // Only now is the session real: polling reads registers that have been checked to
    // belong to this part.
    startPolling();

    // A burn reconnect lands here: this is the first moment the target is known to be the
    // selected part AND running, which is what "the application started" means.
    reportPostBurnState();
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

bool MainWindow::shouldFastPoll(bool connected, bool hasSelection, bool halted)
{
    // A halted core freezes every register, so sampling one of them 50 times a second
    // would report the same value for ever at the price of continuous bus traffic.
    return connected && hasSelection && !halted;
}

bool MainWindow::shouldUseRegisterHss(bool connected, bool hasSelection, bool halted,
                                      bool hssSupported, bool registerPageVisible)
{
    // Everything that makes fast polling pointless also makes HSS pointless, plus two
    // conditions of its own: the backend must expose the interface, and this page must be
    // the visible one -- HSS is a single session and the curve page needs it too, so the
    // register view only holds it while its own tab is in front.
    return connected && hasSelection && !halted && hssSupported && registerPageVisible;
}

void MainWindow::startPolling()
{
    if (!m_session->connected()) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
        return;
    }
    m_pollTimer->start();
    if (m_counterTimer)
        m_counterTimer->start();
    if (m_faultTimer)
        m_faultTimer->start();
    updateRegisterSampling();
    onPollTimeout();
    pollTimerCounters();
    pollCpuHealth();
}

void MainWindow::stopPolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_fastPollTimer)
        m_fastPollTimer->stop();
    if (m_registerDrainTimer)
        m_registerDrainTimer->stop();
    if (m_registerSampler && m_registerSampler->isRunning())
        m_registerSampler->stop();
    m_registerHssAddress = 0;
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

void MainWindow::onFastPollTimeout()
{
    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    if (!peripheral || !reg || !m_session->connected())
        return;

    // Backpressure, not blind firing. The timer says "at most this often", the link says
    // what is actually possible: a slower clock (200 kHz) or a long probe round trip can
    // easily take longer than one tick. Skipping while the previous read is outstanding
    // is what keeps the queue from growing -- and makes the effective rate follow the
    // probe instead of lying about it.
    const quint32 address = peripheral->baseAddress + reg->addressOffset;
    if (m_session->isReadPending(address))
        return;

    m_session->requestRegisterRead(*peripheral, *reg);
}

void MainWindow::updateFastPoll()
{
    if (!m_fastPollTimer)
        return;

    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    // HSS already delivers the value; polling it as well would double the traffic for the
    // same number on screen.
    const bool wanted = !registerHssInCharge()
                        && shouldFastPoll(m_session && m_session->connected(),
                                          peripheral && reg, m_cpuHalted);

    if (!wanted) {
        if (m_fastPollTimer->isActive())
            m_fastPollTimer->stop();
    } else if (!m_fastPollTimer->isActive()) {
        m_fastPollTimer->start();
        onFastPollTimeout(); // first sample now, so selecting feels immediate
    }
    updatePollLabel();
}

bool MainWindow::registerHssInCharge() const
{
    return m_registerSampler && m_registerSampler->isRunning();
}

void MainWindow::updateRegisterSampling()
{
    if (!m_registerSampler)
        return;

    const SvdPeripheral *peripheral = currentPeripheral();
    const SvdRegister *reg = currentRegister();
    const bool hasSelection = peripheral && reg;
    const bool connected = m_session && m_session->connected();

    // Which tab is in front. Index 2 is 寄存器全景 (see createCentralPages); looked up by
    // widget rather than by a stored literal so inserting a tab cannot silently point the
    // arbitration at the wrong page.
    const bool registerPageVisible =
        m_tabs && m_registerPage && m_tabs->currentWidget() == m_registerPage;

    const bool wantHss =
        !m_registerHssRefused
        && shouldUseRegisterHss(connected, hasSelection, m_cpuHalted,
                                m_probe && m_probe->supportsHss(), registerPageVisible);

    if (!wantHss) {
        if (m_registerSampler->isRunning() || m_registerSampler->isStarting()
            || (m_registerDrainTimer && m_registerDrainTimer->isActive())) {
            m_registerDrainTimer->stop();
            m_registerSampler->stop();
            m_registerHssAddress = 0;
        }
        // Hand the decision to the polling path, which also updates the label.
        updateFastPoll();
        return;
    }

    const quint32 address = peripheral->baseAddress + reg->addressOffset;
    // "Running or still starting" -- start() returns before the probe confirms, and a
    // second evaluation in that window would be refused as a duplicate start.
    const bool sessionBusy = m_registerSampler->isRunning() || m_registerSampler->isStarting();
    if (sessionBusy && m_registerHssAddress == address) {
        updateFastPoll(); // already sampling this register; the poll must stay off
        return;
    }

    // The block table is fixed at start(), so a different register means a new session.
    if (sessionBusy) {
        m_registerDrainTimer->stop();
        m_registerSampler->stop();
        m_registerHssAddress = 0;
    }

    // HSS is one global session: take it explicitly rather than relying on the DLL to
    // drop the previous block table. Otherwise the curve page would keep its "running"
    // state while receiving nothing.
    if (m_scopePage && m_scopePage->isLiveCapturing())
        m_scopePage->releaseHss();

    WatchVariable variable;
    variable.name = QStringLiteral("%1.%2").arg(peripheral->name, reg->name);
    variable.typeLabel = QStringLiteral("寄存器");
    variable.kind = ScalarKind::Unsigned;
    variable.address = address;
    variable.elementSize = qBound(1, reg->byteSize(), 4);
    variable.elementCount = 1;
    variable.totalSize = variable.elementSize;

    // 1 ms: fast enough that a counter or a status bit reads as live, and the value is
    // delivered by the probe, not by a read of ours per displayed frame.
    const Status st = m_registerSampler->start({variable}, 1000);
    if (!st) {
        // The interface exists but refused (bad target state, busy DLL). Say so once and
        // let the polling path carry the view instead of leaving it frozen.
        Logger::instance()->warn(
            QStringLiteral("%1 无法用 HSS 采样，退回轮询: %2").arg(variable.name, st.error));
        m_registerHssAddress = 0;
        updateFastPoll();
        return;
    }

    m_registerHssAddress = address;
    m_registerDrainTimer->start();
    Logger::instance()->debug(
        QStringLiteral("寄存器实时采样: %1（HSS 1 ms）").arg(variable.name));
    updateFastPoll(); // HSS owns the value now, so the timer must stay stopped
}

void MainWindow::onRegisterDrainTick()
{
    if (!m_registerSampler || !m_registerSampler->isRunning())
        return;
    // One drain may still be in flight; asking again would queue overlapping reads of the
    // same buffer.
    if (m_registerSampler->isDrainPending())
        return;
    // No history: the register view shows one number, not a curve.
    m_registerSampler->setDrainTarget(nullptr);
    m_registerSampler->requestDrain();
}

void MainWindow::onRegisterSampled(const HssSampler::Result &result)
{
    Q_UNUSED(result);
    if (!registerHssInCharge())
        return;
    const auto &channels = m_registerSampler->channels();
    if (channels.isEmpty() || m_registerHssAddress == 0)
        return;

    const HssSampler::Channel &channel = channels.first();
    if (channel.count == 0)
        return; // nothing decoded yet (first drain can be empty)

    // Go through the session so the value lands in the same cache and announces itself the
    // same way a polled read does; the views have one display path.
    m_session->applySampledValue(m_registerHssAddress,
                                 static_cast<quint32>(channel.latest));
}

void MainWindow::onMainTabChanged(int index)
{
    Q_UNUSED(index);
    // The visible page owns the HSS session.
    updateRegisterSampling();
}

void MainWindow::updatePollLabel()
{
    if (!m_pollLabel)
        return;
    if (!m_session || !m_session->connected()) {
        m_pollLabel->setText(QStringLiteral("轮询: 关闭"));
        return;
    }
    const bool hss = registerHssInCharge();
    const bool fast = m_fastPollTimer && m_fastPollTimer->isActive();
    const QString base =
        m_pinned.isEmpty()
            ? QStringLiteral("轮询: 仅当前外设 / %1 ms").arg(m_pollIntervalMs)
            : QStringLiteral("轮询: %1 个外设 / %2 ms").arg(m_pinned.size()).arg(m_pollIntervalMs);
    // Say which mechanism is actually feeding the selected register -- HSS and the
    // fallback timer look identical on screen but behave very differently on the bus.
    const QString suffix = hss ? QStringLiteral("（选中寄存器 HSS 1 ms）")
                               : (fast ? QStringLiteral("（选中寄存器 %1 ms）")
                                             .arg(kFastPollIntervalMs)
                                       : QString());
    m_pollLabel->setText(base + suffix);
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
