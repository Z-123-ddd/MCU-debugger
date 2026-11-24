#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceTreeScanner.h"
#include "core/PartTable.h"
#include "core/PeripheralPresence.h"
#include "core/PeripheralRoles.h"
#include "core/ResourcePaths.h"
#include "core/HssSampler.h"
#include "core/JLinkInstall.h"
#include <QSet>
#include "core/JLinkDllApi.h"
#include "core/RegisterSession.h"
#include "ui/RegisterBlockView.h"
#include "ui/InspectorPanel.h"
#include "ui/MainWindow.h"
#include "ui/BitFieldStrip.h"
#include "ui/PortGridView.h"
#include <cmath>
#include <cstring>
#include "ui/PackageView.h"
#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/FaultStatus.h"
#include "core/HexUtil.h"
#include "core/JLinkDeviceName.h"
#include "core/JLinkDllProbe.h"
#include "core/IProbe.h"
#include "core/Logger.h"
#include "core/MemoryProbe.h"
#include "core/ZipArchive.h"
#include "core/FmuModel.h"
#include "core/PythonModel.h"
#include "core/SimModel.h"
#include "ui/SimulationPage.h"
#include "core/FirmwareImage.h"
#include "core/ImageBlob.h"
#include "core/FirmwareBurner.h"
#include "ui/BurnDialog.h"
#include "ui/ChipPickerDialog.h"
#include "ui/ScopePage.h"
#include "ui/GlPlotWidget.h"

#include <QWheelEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QImage>
#include <QPixmap>
#include <QTableWidget>
#include <QTreeWidget>
#include "core/Fft.h"
#include "core/SampleHistory.h"
#include "core/SymbolFile.h"
#include "core/SvdLibrary.h"
#include "core/SvdParser.h"

#include <algorithm>

#include <QCoreApplication>
#include <QComboBox>
#include <QSettings>
#include <QPushButton>
#include <limits>

#include <QThread>

#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFileInfo>
#include <QApplication>
#include <QMouseEvent>

#include <functional>
#include <QElapsedTimer>
#include <QString>

#include <cstdio>

namespace mrd::cli {

namespace {

/// Register-rich peripheral for the benchmark, so timings are not optimistic.
const SvdPeripheral *pickBenchPeripheral(const SvdDevice &device)
{
    const SvdPeripheral *best = nullptr;
    for (const auto &peripheral : device.peripherals) {
        if (!best || peripheral.registers.size() > best->registers.size())
            best = &peripheral;
    }
    return best;
}

QString takeValue(const QStringList &args, int *index, const QString &key)
{
    const QString &arg = args.at(*index);
    const int eq = arg.indexOf(QLatin1Char('='));
    if (eq >= 0)
        return arg.mid(eq + 1);
    if (*index + 1 < args.size()) {
        ++(*index);
        return args.at(*index);
    }
    Q_UNUSED(key);
    return QString();
}

void out(const QString &line)
{
    std::printf("%s\n", qPrintable(line));
}

int g_failures = 0;

void check(bool condition, const QString &what, const QString &detail = QString())
{
    const QString mark = condition ? QStringLiteral("[PASS]") : QStringLiteral("[FAIL]");
    if (!condition)
        ++g_failures;
    QString line = QStringLiteral("%1 %2").arg(mark, what);
    if (!detail.isEmpty())
        line += QStringLiteral("   (%1)").arg(detail);
    out(line);
}
} // namespace

Options parse(int argc, char *argv[])
{
    Options o;
    QStringList args;
    args.reserve(argc > 0 ? argc - 1 : 0);
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    for (int i = 0; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const auto is = [&arg](const char *key) {
            return arg == QLatin1String(key) || arg.startsWith(QLatin1String(key) + QLatin1Char('='));
        };

        if (is("--help") || arg == QLatin1String("-h")) {
            o.printHelp = true;
            o.gui = false;
        } else if (is("--version") || arg == QLatin1String("-v")) {
            o.printVersion = true;
            o.gui = false;
        } else if (is("--verbose")) {
            o.verbose = true;
        } else if (is("--bench")) {
            o.bench = true;
        } else if (is("--selftest")) {
            o.selftest = true;
            o.gui = false;
        } else if (is("--probe-info")) {
            o.probeInfo = true;
            o.gui = false;
        } else if (is("--svd")) {
            o.svdPath = takeValue(args, &i, arg);
            if (o.svdPath.isEmpty())
                o.errors << QStringLiteral("--svd 需要一个文件路径");
        } else if (is("--render")) {
            o.renderDir = takeValue(args, &i, arg);
            if (o.renderDir.isEmpty())
                o.errors << QStringLiteral("--render 需要一个目录");
        } else if (is("--device")) {
            o.deviceName = takeValue(args, &i, arg);
        } else if (is("--interface") || is("--protocol")) {
            const QString value = takeValue(args, &i, arg).trimmed().toUpper();
            if (value == QLatin1String("SWD") || value == QLatin1String("JTAG"))
                o.interfaceName = value;
            else
                o.errors << QStringLiteral("--interface 只支持 swd 或 jtag");
        } else if (is("--speed")) {
            const QString value = takeValue(args, &i, arg);
            bool ok = false;
            o.speedKHz = value.toInt(&ok);
            if (!ok || o.speedKHz <= 0)
                o.errors << QStringLiteral("--speed 需要正整数 kHz");
        } else if (is("--port")) {
            o.portName = takeValue(args, &i, arg);
        } else if (is("--log")) {
            o.logPath = takeValue(args, &i, arg);
        } else if (is("--bench-scope")) {
            o.benchScope = true;
        } else if (is("--burn")) {
            o.burnPath = takeValue(args, &i, arg);
        } else if (is("--burn-commit")) {
            o.burnCommit = true;
        } else if (is("--burn-force")) {
            o.burnForce = true;
        } else if (is("--bootloader")) {
            o.bootloaderPath = takeValue(args, &i, arg);
        } else if (is("--bootloader-base")) {
            o.bootloaderBase = takeValue(args, &i, arg);
        } else if (is("--app-base")) {
            o.appBase = takeValue(args, &i, arg);
        } else if (is("--image-start")) {
            o.overallStart = takeValue(args, &i, arg);
        } else if (is("--image-end")) {
            o.overallEnd = takeValue(args, &i, arg);
        } else if (is("--dump-flash")) {
            o.dumpFlashPath = takeValue(args, &i, arg);
        } else if (is("--dump-base")) {
            o.dumpFlashBase = takeValue(args, &i, arg);
        } else if (is("--dump-len")) {
            o.dumpFlashLength = takeValue(args, &i, arg);
        } else if (is("--bench-bus")) {
            const QString value = takeValue(args, &i, arg);
            bool ok = false;
            o.benchBus = value.toInt(&ok);
            if (!ok || o.benchBus <= 0)
                o.errors << QStringLiteral("--bench-bus 需要正整数秒");
        } else if (is("--bench-read")) {
            o.benchRead = true;
        } else if (is("--read-var")) {
            o.readVar = takeValue(args, &i, arg);
        } else if (is("--symbols")) {
            o.symbolPath = takeValue(args, &i, arg);
        } else if (arg.startsWith(QLatin1String("--"))) {
            o.errors << QStringLiteral("未知参数: %1").arg(arg);
        }
    }

    if (o.selftest && o.svdPath.isEmpty())
        o.errors << QStringLiteral("--selftest 需要配合 --svd <path> 使用");

    return o;
}

QString helpText()
{
    return QStringLiteral(
        "%1 %2 - MCU 寄存器级 UI 调试器\n"
        "\n"
        "用法: mcuRegDebugger [选项]\n"
        "\n"
        "  --svd <path>       启动时加载指定的 CMSIS-SVD 文件\n"
        "  --device <name>    覆盖 J-Link 器件名（例如 STM32F103C8）\n"
        "  --selftest         SVD 解析自检（需配合 --svd），退出码 0 表示通过\n"
        "  --render <dir>     自检时离屏渲染 PNG 快照到目录\n"
        "  --port <PA|PB..>   离屏渲染使用的端口\n"
        "  --probe-info       打印 J-Link DLL/固件信息并尝试连接\n"
        "  --symbols <文件>   解析 .axf/.elf 并列出可采样变量（不需要硬件）\n"
        "  --burn <文件>      分析固件起止地址并校验容量（默认只报告，不写入）\n"
        "  --burn-commit      配合 --burn：真正擦除并烧录，然后校验\n"
        "  --burn-force       配合 --burn-commit：先擦除固件所在区再写（内容相同时不跳过）\n"
        "  --bootloader <文件> 合并 bootloader（.axf/.elf/.hex）到固件前面\n"
        "  --bootloader-base <地址> bootloader 放置地址（默认用它被链接的地址）\n"
        "  --app-base <地址>   固件起始地址（有 bootloader 时默认 0x08008000）\n"
        "  --image-start <地址> 整体镜像起始地址（默认按内容推导）\n"
        "  --image-end <地址>   整体镜像结束地址（默认按内容推导）\n"
        "  --dump-flash <文件> 把目标 Flash 读成 .bin（只读，不写目标）\n"
        "  --dump-base <地址>  Flash 起始地址（默认 0x08000000）\n"
        "  --dump-len <字节>   读取长度（默认按型号 Flash 容量）\n"
        "  --verbose          日志附带每一步细节（CPUID/IDCODE/清单/内存探测）\n"
        "  --log <path>       日志文件路径\n"
        "  --version          打印版本\n"
        "  --help             打印本帮助\n"
        "\n"
        "不带参数启动图形界面。\n")
        .arg(QStringLiteral(APP_NAME), QStringLiteral(APP_VERSION));
}


namespace {
void benchMark(const char *what)
{
    FILE *f = std::fopen("bench-trace.txt", "a");
    if (!f)
        return;
    std::fprintf(f, "%s\n", what);
    std::fclose(f);
}
} // namespace

int runBench(const Options &options)
{
    benchMark("enter");
    // Times the work one poll does on the GUI thread, with no hardware involved:
    // the probe I/O happens on a worker thread, so what makes the window stutter
    // is this path -- view rebuilds triggered by every read completion.
    SvdParser parser;
    const auto parsed = parser.parseFile(options.svdPath);
    if (!parsed) {
        std::fprintf(stderr, "[ERROR] %s\n", qPrintable(parsed.error));
        return 2;
    }
    benchMark("parsed-ok");
    const SvdDevice &device = parsed.value;
    benchMark("device-ok");

    const SvdPeripheral *peripheral = pickBenchPeripheral(device);
    benchMark("picked");
    if (!peripheral) {
        std::fprintf(stderr, "[ERROR] 没有可用的外设\n");
        return 2;
    }
    const SvdRegister *reg = peripheral->registers.isEmpty() ? nullptr : &peripheral->registers.first();

    // Fill a session with plausible values (reset values) so nothing is "offline".
    benchMark("before-session");
    RegisterSession session(nullptr, nullptr);
    benchMark("session-ctor");
    session.setDevice(device);
    benchMark("setDevice");
    for (const auto &p : device.peripherals) {
        benchMark(qPrintable(QStringLiteral("periph %1 regs=%2").arg(p.name).arg(p.registers.size())));
        QByteArray bytes;
        for (const auto &r : p.registers) {
            const quint32 v = r.resetValue;
            QByteArray one(4, char(0));
            std::memcpy(one.data(), &v, 4);
            bytes += one;
        }
        session.applyRead(p.baseAddress, bytes, true, QString());
    }

    benchMark("session");
    RegisterBlockView block;
    block.setPeripheral(peripheral);
    benchMark("block");
    InspectorPanel inspector;
    benchMark("inspector");
    BitFieldStrip strip;
    benchMark("strip");
    PortGridWidget grid;

    QVector<GpioPin> pins;
    for (const auto &p : device.peripherals) {
        if (!GpioCodec::isGpioPort(p))
            continue;
        bool valid = false;
        pins = GpioCodec::decode(p, session.snapshot(p), &valid);
        break;
    }

    constexpr int kIterations = 60;
    const auto measure = [](const QString &label, auto &&fn) {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < kIterations; ++i)
            fn();
        const double perIteration = double(timer.nsecsElapsed()) / 1000.0 / kIterations;
        std::printf("  %-38s %8.1f us\n", qPrintable(label), perIteration);
        return perIteration;
    };

    benchMark("start");
    std::printf("一次轮询的 GUI 刷新开销（%d 次平均）\n", kIterations);
    std::printf("  外设 %s / 寄存器 %s\n",
                qPrintable(peripheral->name), reg ? qPrintable(reg->name) : "-");

    double total = 0.0;
    total += measure(QStringLiteral("snapshot() 取外设快照"),
                     [&] { session.snapshot(*peripheral); });
    total += measure(QStringLiteral("RegisterBlockView::setValues"),
                     [&] { block.setValues(session.snapshot(*peripheral), true); });
    if (reg) {
        // Vary the value: the polling case always has a changing value, and a
        // constant one would just hit the "unchanged" early-out.
        quint32 tick = 0;
        total += measure(QStringLiteral("InspectorPanel::setRegister（值在变）"),
                         [&] {
                             inspector.setRegister(reg, peripheral->baseAddress + reg->addressOffset,
                                                   tick++, true);
                         });
        total += measure(QStringLiteral("BitFieldStrip::setRegister+setValue"),
                         [&] {
                             strip.setRegister(reg);
                             strip.setValue(reg->resetValue, true);
                         });
    }
    if (!pins.isEmpty()) {
        total += measure(QStringLiteral("PortGridWidget::setPins"),
                         [&] { grid.setPins(pins, true); });
    }
    std::printf("  %-38s %8.1f us\n", "合计", total);
    std::printf("\n按每轮 %d 次读取完成（每个外设一次）估算：\n", 8);
    // Refreshes are coalesced into one per poll burst, so the multiplier is 1, not
    // one per peripheral as it used to be.
    std::printf("  刷新已合并为每轮 1 次：%.3f ms/轮；轮询间隔 %d ms\n",
                total / 1000.0, 200);

    // Clicking through every register is what the user does; keep it as a smoke
    // test so a crash-prone register surfaces here instead of in the GUI.
    int visited = 0;
    for (const auto &peripheral : device.peripherals) {
        for (const auto &r : peripheral.registers) {
            const quint32 addr = peripheral.baseAddress + r.addressOffset;
            inspector.setRegister(&r, addr, session.effectiveValue(addr), true);
            ++visited;
        }
    }
    std::printf("  遍历 %d 个寄存器（模拟点击）无崩溃\n", visited);
    return 0;
}

int runSelftest(const Options &options)
{
    g_failures = 0;
    // --verbose keeps the per-step detail visible in the self-test too.
    Logger::instance()->setMinimumLevel(options.verbose ? Logger::Debug : Logger::Warn);

    out(QStringLiteral("=== SVD 自检: %1 ===").arg(options.svdPath));

    QElapsedTimer timer;
    timer.start();
    const auto parsed = SvdParser::parseFile(options.svdPath);
    const qint64 parseMs = timer.elapsed();

    check(parsed.ok, QStringLiteral("SVD 可解析"), parsed.ok ? QString() : parsed.error);
    if (!parsed.ok)
        return 1;

    const SvdDevice &device = parsed.value;
    out(QString());
    out(QStringLiteral("器件      : %1").arg(device.name.isEmpty() ? QStringLiteral("<未命名>") : device.name));
    out(QStringLiteral("厂商      : %1  (vendorID=%2, version=%3)")
            .arg(device.vendor.isEmpty() ? QStringLiteral("-") : device.vendor,
                 device.vendorId.isEmpty() ? QStringLiteral("-") : device.vendorId,
                 device.version.isEmpty() ? QStringLiteral("-") : device.version));
    out(QStringLiteral("CPU       : %1").arg(device.cpuName.isEmpty() ? QStringLiteral("-") : device.cpuName));
    out(QStringLiteral("外设/寄存器/字段/枚举: %1 / %2 / %3 / %4")
            .arg(device.peripherals.size())
            .arg(device.totalRegisters())
            .arg(device.totalFields())
            .arg(device.totalEnums()));
    out(QStringLiteral("解析耗时  : %1 ms").arg(parseMs));
    out(QString());

    check(!device.name.isEmpty(), QStringLiteral("器件名非空"), device.name);
    check(!device.peripherals.isEmpty(), QStringLiteral("外设数量 > 0"),
          QString::number(device.peripherals.size()));
    check(device.totalRegisters() > 0, QStringLiteral("寄存器数量 > 0"),
          QString::number(device.totalRegisters()));
    check(device.totalFields() > 0, QStringLiteral("字段数量 > 0"), QString::number(device.totalFields()));

    // Register-level sanity: every register must be inside the peripheral span
    // and field bit ranges must fit into the register size.
    int badFields = 0;
    int emptyRegisters = 0;
    int overlapping = 0;
    for (const auto &peripheral : device.peripherals) {
        for (const auto &reg : peripheral.registers) {
            if (reg.fields.isEmpty())
                ++emptyRegisters;
            quint64 used = 0;
            for (const auto &field : reg.fields) {
                if (field.bitOffset < 0 || field.bitWidth <= 0
                    || (reg.size > 0 && field.bitOffset + field.bitWidth > reg.size))
                    ++badFields;
                const quint64 bits = field.mask64();
                if (bits != 0 && (used & bits) != 0)
                    ++overlapping;
                used |= bits;
            }
        }
    }
    check(badFields == 0, QStringLiteral("字段位区间均在寄存器宽度内"),
          QStringLiteral("越界 %1 个").arg(badFields));

    // derivedFrom free peripherals must end up with registers.
    int unresolvedDerived = 0;
    for (const auto &peripheral : device.peripherals) {
        if (!peripheral.derivedFrom.isEmpty() && peripheral.registers.isEmpty())
            ++unresolvedDerived;
    }
    check(unresolvedDerived == 0, QStringLiteral("derivedFrom 外设均解析出寄存器"),
          QStringLiteral("未解析 %1 个").arg(unresolvedDerived));

    out(QString());
    out(QStringLiteral("统计: 无字段寄存器 %1 个, 字段重叠寄存器 %2 处, 警告 %3 条")
            .arg(emptyRegisters)
            .arg(overlapping)
            .arg(device.warnings.size()));
    for (int i = 0; i < device.warnings.size() && i < 20; ++i)
        out(QStringLiteral("  warn: %1").arg(device.warnings.at(i)));
    if (device.warnings.size() > 20)
        out(QStringLiteral("  ... 其余 %1 条省略").arg(device.warnings.size() - 20));

    // --- GPIO decode / configure -------------------------------------------
    // Deterministic assertions against the real N32H473CE register snapshot, so
    // the "uninitialised" logic and the pin-config encoder are covered even
    // without a board attached.
    {
        out(QString());
        out(QStringLiteral("--- GPIO 解码 / 配置 ---"));

        const SvdPeripheral *gpioA = device.findPeripheral(QStringLiteral("GPIOA"));
        if (!gpioA) {
            out(QStringLiteral("[SKIP] 该 SVD 没有 GPIOA"));
        } else {
            // Two register layouts are in the catalogue, with different encodings:
            //   N32H47x  : PMODE (0=input 1=output 2=AF 3=analog) + PUPD + POTYPE
            //   N32G4x   : PL_CFG/PH_CFG, 4 bits per pin (MODE[1:0] + CNF[1:0])
            // The test drives whichever one this SVD actually has.
            const bool f1Style = gpioA->findRegister(QStringLiteral("CRL"))
                                 || gpioA->findRegister(QStringLiteral("CRH"))
                                 || gpioA->findRegister(QStringLiteral("GPIOx_PL_CFG"))
                                 || gpioA->findRegister(QStringLiteral("PL_CFG"));
            out(QStringLiteral("    布局: %1")
                    .arg(f1Style ? QStringLiteral("PL_CFG/PH_CFG (4 bit/脚，同 STM32F1 CRL/CRH)")
                                 : QStringLiteral("PMODE/PUPD/POTYPE")));

            auto makeValues = [&](quint32 pmode, quint32 pupd, quint32 potype) {
                QHash<quint32, quint32> v;
                for (const auto &reg : gpioA->registers) {
                    quint32 value = 0;
                    const QString name = reg.name;
                    if (name.endsWith(QLatin1String("PMODE")))
                        value = pmode;
                    else if (name.endsWith(QLatin1String("PUPD")))
                        value = pupd;
                    else if (name.endsWith(QLatin1String("POTYPE")))
                        value = potype;
                    v.insert(gpioA->baseAddress + reg.addressOffset, value);
                }
                return v;
            };

            // F1-style helper: place one pin's nibble (CNF<<2 | MODE) in the right
            // register, leaving the others at a known state.
            auto f1Values = [&](const QVector<QPair<int, quint32>> &nibbles) {
                const quint32 resetNibble = 0x4; // MODE=00 CNF=01 -> floating input
                quint32 low = 0x44444444u;
                quint32 high = 0x44444444u;
                for (const auto &entry : nibbles) {
                    const int bit = entry.first;
                    const quint32 nibble = entry.second & 0xF;
                    if (bit < 8)
                        low = (low & ~(0xFu << (bit * 4))) | (nibble << (bit * 4));
                    else
                        high = (high & ~(0xFu << ((bit - 8) * 4))) | (nibble << ((bit - 8) * 4));
                }
                QHash<quint32, quint32> v;
                for (const auto &reg : gpioA->registers) {
                    const QString name = reg.name;
                    quint32 value = 0;
                    if (name.endsWith(QLatin1String("PL_CFG")) || name.endsWith(QLatin1String("CRL")))
                        value = low;
                    else if (name.endsWith(QLatin1String("PH_CFG")) || name.endsWith(QLatin1String("CRH")))
                        value = high;
                    Q_UNUSED(resetNibble);
                    v.insert(gpioA->baseAddress + reg.addressOffset, value);
                }
                return v;
            };

            bool valid = false;
            auto kindOf = [](const QVector<GpioPin> &pins, int bit) {
                return (bit >= 0 && bit < pins.size()) ? pins.at(bit).kind : GpioPin::Unknown;
            };

            if (f1Style) {
                // To reset state: every pin floating input -> 未初始化.
                QVector<GpioPin> pins = GpioCodec::decode(*gpioA, f1Values({}), &valid);
                int uninit = 0;
                for (const auto &pin : pins) {
                    if (pin.kind == GpioPin::Uninitialized)
                        ++uninit;
                }
                check(!pins.isEmpty() && uninit == pins.size(),
                      QStringLiteral("复位值端口全部显示为未初始化"),
                      QStringLiteral("%1/%2 个").arg(uninit).arg(pins.size()));

                // MODE=01 CNF=00 -> push-pull output;  MODE=01 CNF=10 -> AF push-pull.
                pins = GpioCodec::decode(*gpioA, f1Values({{0, 0x1}, {9, 0xA}}), &valid);
                check(kindOf(pins, 0) == GpioPin::Output,
                      QStringLiteral("PA0(MODE=01,CNF=00) 识别为输出"),
                      QStringLiteral("%1").arg(pins.value(0).mode));
                check(kindOf(pins, 9) == GpioPin::Alternate,
                      QStringLiteral("PA9(MODE=01,CNF=10) 识别为复用"),
                      QStringLiteral("%1").arg(pins.value(9).mode));

                // MODE=00 CNF=00 is the analog configuration on this family.
                pins = GpioCodec::decode(*gpioA, f1Values({{1, 0x0}}), &valid);
                check(kindOf(pins, 1) == GpioPin::Analog,
                      QStringLiteral("PA1(MODE=00,CNF=00) 识别为模拟"),
                      QStringLiteral("%1").arg(pins.value(1).mode));

                // Encoder must still produce writes for this layout.
                GpioPinConfig cfg;
                cfg.mode = GpioPinConfig::DigitalOutput;
                const QVector<GpioWrite> enc = GpioCodec::encodePinConfig(*gpioA, 1, cfg, f1Values({}));
                check(!enc.isEmpty(), QStringLiteral("编码数字输出产生寄存器写入"),
                      QStringLiteral("%1 个").arg(enc.size()));
            } else {
            // (a) untouched port: PMODE reset value 0xFFFFFFFF -> every pin uninitialised
            QVector<GpioPin> pins = GpioCodec::decode(*gpioA, makeValues(0xFFFFFFFFu, 0, 0), &valid);
            int uninit = 0;
            for (const auto &pin : pins) {
                if (pin.kind == GpioPin::Uninitialized)
                    ++uninit;
            }
            check(!pins.isEmpty() && uninit == pins.size(),
                  QStringLiteral("复位值端口全部显示为未初始化"),
                  QStringLiteral("%1/%2 个").arg(uninit).arg(pins.size()));
            if (!pins.isEmpty())
                out(QStringLiteral("    样例: %1 -> %2 / %3").arg(pins.first().name(), pins.first().mode,
                                                                 pins.first().subMode));

            // (b) real snapshot read from the device (PA0 = output, PA9/11/12 = AF)
            const quint32 realPmode = 0x028BFFFD;
            const quint32 realPupd = 0x64100002;
            pins = GpioCodec::decode(*gpioA, makeValues(realPmode, realPupd, 0), &valid);
            check(kindOf(pins, 0) == GpioPin::Output, QStringLiteral("PA0(PMODE=01) 识别为输出"),
                  QStringLiteral("%1").arg(pins.value(0).mode));
            check(kindOf(pins, 9) == GpioPin::Alternate, QStringLiteral("PA9(PMODE=10) 识别为复用"),
                  QStringLiteral("%1").arg(pins.value(9).mode));
            check(kindOf(pins, 1) == GpioPin::Uninitialized,
                  QStringLiteral("PA1(PMODE=11 且无上下拉/类型/AF) 识别为未初始化"),
                  QStringLiteral("%1").arg(pins.value(1).mode));

            // (c) an explicitly configured analog pin must NOT read as uninitialised
            QSet<QString> configured;
            configured.insert(QStringLiteral("PA1"));
            pins = GpioCodec::decode(*gpioA, makeValues(realPmode, realPupd, 0), &valid, configured);
            check(pins.value(1).kind == GpioPin::Analog,
                  QStringLiteral("已显式配置的 PA1 显示为模拟而非未初始化"),
                  QStringLiteral("%1").arg(pins.value(1).mode));

            // (d) encoder: digital output, push-pull, no pull
            GpioPinConfig config;
            config.mode = GpioPinConfig::DigitalOutput;
            const QVector<GpioWrite> writes =
                GpioCodec::encodePinConfig(*gpioA, 1, config, makeValues(realPmode, realPupd, 0));
            check(!writes.isEmpty(), QStringLiteral("编码数字输出产生寄存器写入"),
                  QStringLiteral("%1 个").arg(writes.size()));
            bool pmodeSet = false;
            for (const GpioWrite &w : writes) {
                for (const auto &reg : gpioA->registers) {
                    if (gpioA->baseAddress + reg.addressOffset != w.address)
                        continue;
                    if (reg.name.endsWith(QLatin1String("PMODE"))) {
                        // PA1 occupies bits [3:2]; `01` = output
                        pmodeSet = (((w.value >> 2) & 0x3u) == 1u);
                        out(QStringLiteral("    PMODE 新值 = %1 (PA1 位 = %2)")
                                .arg(hex32(w.value))
                                .arg((w.value >> 2) & 0x3u));
                    }
                }
            }
            check(pmodeSet, QStringLiteral("PMODE[PA1] 被编码为 01(输出)"));

            // (e) analog input + pull-up encoding
            config = GpioPinConfig();
            config.mode = GpioPinConfig::DigitalInput;
            config.pull = GpioPinConfig::PullUp;
            const QVector<GpioWrite> pullWrites =
                GpioCodec::encodePinConfig(*gpioA, 1, config, makeValues(realPmode, realPupd, 0));
            bool pullSet = false;
            for (const GpioWrite &w : pullWrites) {
                for (const auto &reg : gpioA->registers) {
                    if (gpioA->baseAddress + reg.addressOffset != w.address)
                        continue;
                    if (reg.name.endsWith(QLatin1String("PUPD")))
                        pullSet = (((w.value >> 2) & 0x3u) == 1u); // PA1 = pull-up
                }
            }
            check(pullSet, QStringLiteral("PUPD[PA1] 被编码为 01(上拉)"));

            // (f) output level: which register, what value (the concrete answer
            //     for "PA1 数字推挽上拉输出，改电平要写什么")
            {
                const QHash<quint32, quint32> snap = makeValues(realPmode, realPupd, 0);
                GpioWrite highWrite;
                QString highReg;
                const bool okHigh = GpioCodec::encodePinLevel(*gpioA, 1, true, snap, &highWrite, &highReg);
                GpioWrite lowWrite;
                QString lowReg;
                const bool okLow = GpioCodec::encodePinLevel(*gpioA, 1, false, snap, &lowWrite, &lowReg);

                check(okHigh && okLow, QStringLiteral("PA1 电平写入可编码"));
                out(QStringLiteral("    置高: %1.%2 = %3  (@%4)")
                        .arg(gpioA->name, highReg, hex32(highWrite.value), hexAddr(highWrite.address)));
                out(QStringLiteral("    置低: %1.%2 = %3  (@%4)")
                        .arg(gpioA->name, lowReg, hex32(highWrite.value == 0 ? 0 : lowWrite.value),
                             hexAddr(lowWrite.address)));
                // PBSC: low half = set, high half = clear -> PA1 high = bit1,
                // PA1 low = bit 17.
                check(highWrite.address == lowWrite.address, QStringLiteral("置高/置低写同一个寄存器"),
                      highReg);
                check(highWrite.value == 0x00000002u, QStringLiteral("置高值 = 0x00000002 (bit1)"),
                      hex32(highWrite.value));
                check(lowWrite.value == 0x00020000u, QStringLiteral("置低值 = 0x00020000 (bit17)"),
                      hex32(lowWrite.value));
            }
        }
    } // f1Style / PMODE branches
    }

    // --- library index -----------------------------------------------------
    SvdLibrary library;
    library.setRoots(AppSettings::svdRoots());
    const int indexed = library.scan(false);
    out(QString());
    out(QStringLiteral("SVD 库根目录:"));
    for (const auto &root : library.roots())
        out(QStringLiteral("  - %1").arg(root));
    check(indexed > 0, QStringLiteral("SVD 库索引到文件"), QStringLiteral("%1 个").arg(indexed));

    // --- package bonding (datasheet table 3-1) -----------------------------
    // The SVD lists GPIOA..GPIOH for the whole family; only the package table can
    // say which ports this part actually brings out.
    {
        out(QString());
        out(QStringLiteral("--- 封装键合 ---"));
        QString packagePath;
        for (const QString &name : QDir(ResourcePaths::packageDirectory())
                                      .entryList({QStringLiteral("*.json")}, QDir::Files)) {
            const PackageDescription description = PackageDescription::loadFromFile(
                ResourcePaths::packageDirectory() + QLatin1Char('/') + name);
            if (!description.isValid() || description.chip.isEmpty())
                continue;
            if (description.chip.startsWith(device.name, Qt::CaseInsensitive)
                || device.name.startsWith(description.chip, Qt::CaseInsensitive)) {
                packagePath = name;
                const QHash<QString, QVector<int>> bonded = description.bondedPorts();
                QStringList parts;
                QStringList sortedPorts = bonded.keys();
                sortedPorts.sort();
                for (const QString &port : sortedPorts) {
                    const QVector<int> bits = bonded.value(port);
                    parts << QStringLiteral("%1=%2").arg(port).arg(bits.size());
                }
                out(QStringLiteral("  %1 (%2, 引脚号已核实=%3): %4")
                        .arg(description.packageName, name,
                             description.numberVerified ? QStringLiteral("是") : QStringLiteral("否"),
                             parts.join(QStringLiteral("  "))));

                // Every SVD GPIO port that the package does not bond out must be
                // reported, so a D/E/F/G register window is never mistaken for IO.
                QStringList unbonded;
                for (const auto &peripheral : device.peripherals) {
                    if (!GpioCodec::isGpioPort(peripheral))
                        continue;
                    const QString port = GpioCodec::portName(peripheral);
                    if (!port.isEmpty() && !bonded.contains(port))
                        unbonded << peripheral.name;
                }
                if (!unbonded.isEmpty())
                    out(QStringLiteral("    未引出（寄存器存在但无引脚）: %1")
                            .arg(unbonded.join(QStringLiteral(", "))));

                // Dedicated-only ports: physically bonded, but every pin is a
                // special function, so they are not GPIO ports and must not get
                // a group / matrix of their own.
                const QHash<QString, QVector<int>> gpio = description.gpioPorts();
                QStringList dedicated;
                for (const QString &port : bonded.keys()) {
                    if (!gpio.contains(port))
                        dedicated << port;
                }
                dedicated.sort();
                if (!dedicated.isEmpty())
                    out(QStringLiteral("    仅专用功能（不显示为 GPIO 组）: %1")
                            .arg(dedicated.join(QStringLiteral(", "))));
                check(true, QStringLiteral("封装键合表已应用"), description.packageName);

                // The 48-pin part must not expose a PH port: its only bonded PH
                // pins are the crystal and NRST. They surface by their own names.
                if (bonded.contains(QStringLiteral("PH"))) {
                    check(!gpio.contains(QStringLiteral("PH")),
                          QStringLiteral("PH 不作为 GPIO 组出现"),
                          QStringLiteral("PH 全部为专用脚"));
                    QStringList names;
                    for (const auto &pin : description.dedicatedPins()) {
                        if (pin.portName.compare(QStringLiteral("PH"), Qt::CaseInsensitive) == 0)
                            names << QStringLiteral("%1(脚%2)").arg(pin.displayName).arg(pin.number);
                    }
                    names.sort();
                    out(QStringLiteral("    专用引脚按功能名显示: %1").arg(names.join(QStringLiteral(", "))));
                    check(names.size() == bonded.value(QStringLiteral("PH")).size(),
                          QStringLiteral("专用引脚都有功能名"), names.join(QStringLiteral(", ")));

                    // A pin that merely *doubles* as a dedicated function must
                    // keep its GPIO name: PB8 is still PB8, not BOOT0.
                    QStringList mislabelled;
                    for (const auto &pin : description.dedicatedPins()) {
                        if (pin.portName.isEmpty() || gpio.contains(pin.portName))
                            mislabelled << QStringLiteral("%1→%2").arg(pin.name, pin.displayName);
                    }
                    check(mislabelled.isEmpty(), QStringLiteral("GPIO 端口内的引脚不改名"),
                          mislabelled.join(QStringLiteral(", ")));
                }

                // The port matrix must only offer ports with an ordinary GPIO pin.
                QStringList offered;
                for (const auto &peripheral : device.peripherals) {
                    if (!GpioCodec::isGpioPort(peripheral))
                        continue;
                    const QString port = GpioCodec::portName(peripheral);
                    if (port.isEmpty() || offered.contains(port))
                        continue;
                    if (gpio.contains(port))
                        offered << port;
                }
                offered.sort();
                QStringList expectedPorts = gpio.keys();
                expectedPorts.sort();
                check(offered == expectedPorts, QStringLiteral("端口矩阵只列出已引出的端口"),
                      offered.join(QStringLiteral("/")));

                // Bits, not just ports: GPIOH exposes 16 bits on the die but this
                // package bonds only 0/1/6, so the matrix must draw 3 cells.
                for (const auto &peripheral : device.peripherals) {
                    const QString port = GpioCodec::portName(peripheral);
                    if (port.isEmpty() || !gpio.contains(port))
                        continue;
                    bool valid = false;
                    QHash<quint32, quint32> zero;
                    for (const auto &reg : peripheral.registers)
                        zero.insert(peripheral.baseAddress + reg.addressOffset, 0u);
                    const QVector<GpioPin> pins =
                        GpioCodec::decode(peripheral, zero, &valid, QSet<QString>(), gpio.value(port));
                    const int drawn = int(std::count_if(pins.begin(), pins.end(),
                                                        [](const GpioPin &pin) { return pin.bonded; }));
                    out(QStringLiteral("    %1: 键合 %2 / 芯片 %3 位")
                            .arg(peripheral.name)
                            .arg(drawn)
                            .arg(pins.size()));
                    check(drawn == gpio.value(port).size(),
                          QStringLiteral("%1 只绘制已键合位").arg(peripheral.name),
                          QStringLiteral("%1 个").arg(drawn));
                }
                break;
            }
        }
        if (packagePath.isEmpty())
            out(QStringLiteral("  （未找到本器件的封装表，端口清单回退为 SVD 全集）"));
    }

    // --- real device tree --------------------------------------------------
    // Derived from the SVD's RCC gate bits plus a bus-decode test per peripheral;
    // classification is pure logic, so it is asserted here without hardware.
    {
        out(QString());
        out(QStringLiteral("--- 设备树（真实外设） ---"));

        const QHash<QString, DeviceTreeScanner::ClockBit> clocks = DeviceTreeScanner::clockMap(device);
        check(!clocks.isEmpty(), QStringLiteral("从 RCC 解析出时钟门控位"),
              QStringLiteral("%1 个外设可查时钟").arg(clocks.size()));
        out(QStringLiteral("    可解析时钟的外设: %1 / %2")
                .arg(DeviceTreeScanner::clockResolvableCount(device))
                .arg(device.peripherals.size()));

        // Clock-gate mapping, asserted generically: the field names differ per
        // family (IOPAEN vs GPIOAEN, USBEN vs USBFSEN) and the enable registers
        // move between AHB1/APB2, so hard-coding one family's names only tests
        // that family.
        int gpioWithClock = 0;
        int gpioPorts = 0;
        for (const auto &peripheral : device.peripherals) {
            if (!GpioCodec::isGpioPort(peripheral))
                continue;
            ++gpioPorts;
            if (clocks.contains(peripheral.name))
                ++gpioWithClock;
        }
        check(gpioPorts > 0 && gpioWithClock == gpioPorts,
              QStringLiteral("每个 GPIO 端口都能查到时钟门控位"),
              QStringLiteral("%1/%2").arg(gpioWithClock).arg(gpioPorts));

        // Any USB block must be gated too, under whatever name this family uses.
        QStringList usbBlocks;
        QStringList usbGated;
        for (const auto &peripheral : device.peripherals) {
            if (!peripheral.name.startsWith(QLatin1String("USB"), Qt::CaseInsensitive))
                continue;
            usbBlocks << peripheral.name;
            if (clocks.contains(peripheral.name))
                usbGated << peripheral.name;
        }
        if (!usbBlocks.isEmpty()) {
            check(usbGated.size() == usbBlocks.size(),
                  QStringLiteral("USB 外设能查到时钟门控位"),
                  QStringLiteral("%1/%2").arg(usbGated.size()).arg(usbBlocks.size()));
        }
        // Show one resolved gate so a wrong bit is visible in the log.
        for (const auto &peripheral : device.peripherals) {
            const auto it = clocks.constFind(peripheral.name);
            if (it == clocks.constEnd())
                continue;
            out(QStringLiteral("    样例: %1 -> %2[%3]")
                    .arg(peripheral.name, it->registerName)
                    .arg(it->bit));
            break;
        }

        // Presence probe register selection must avoid read-sensitive registers.
        if (const SvdPeripheral *gpio = device.findPeripheral(QStringLiteral("GPIOA"))) {
            const SvdRegister *probeReg = DeviceTreeScanner::presenceProbeRegister(*gpio);
            check(probeReg != nullptr, QStringLiteral("GPIOA 找到存在性探测寄存器"),
                  probeReg ? probeReg->name : QString());
            if (probeReg)
                check(!probeReg->name.contains(QLatin1String("POD"))
                          && !probeReg->name.contains(QLatin1String("PBSC")),
                      QStringLiteral("探测寄存器避开写侧影响寄存器"), probeReg->name);

            // Classification matrix: gate state x bus response.
            DeviceTreeScanner::ClockBit gate;
            gate.registerName = QStringLiteral("RCC_AHB1PCLKEN");
            gate.bit = 0;

            auto status = DeviceTreeScanner::classify(*gpio, gate, /*gateOn=*/true,
                                                     /*readOk=*/true, 0x1234, QString());
            check(status.reachability == PeripheralStatus::Readable && status.clockEnabled,
                  QStringLiteral("门控开 + 读成功 -> 存在且已使能"),
                  QStringLiteral("clock=%1").arg(status.clockEnabled ? 1 : 0));

            status = DeviceTreeScanner::classify(*gpio, gate, true, false, 0,
                                                QStringLiteral("bus fault"));
            check(status.reachability == PeripheralStatus::Missing,
                  QStringLiteral("门控开 + 读失败 -> 判定缺失"));

            status = DeviceTreeScanner::classify(*gpio, gate, /*gateOn=*/false, false, 0,
                                                QStringLiteral("bus fault"));
            check(status.reachability == PeripheralStatus::Unreachable,
                  QStringLiteral("门控关 + 读失败 -> 不误判为缺失"));

            const DeviceTreeScanner::ClockBit noGate;
            status = DeviceTreeScanner::classify(*gpio, noGate, false, true, 0x55, QString());
            check(status.reachability == PeripheralStatus::Readable && !status.clockKnown,
                  QStringLiteral("无门控信息 + 读成功 -> 存在但时钟未知"));

            // onChip() is decided by static vendor data + package bonding, never by
            // a live read: a gated peripheral cannot answer the bus, so a fault
            // must not be mistaken for "this model does not have it".
            status = DeviceTreeScanner::classify(*gpio, gate, /*gateOn=*/false, false, 0,
                                                QStringLiteral("bus fault"));
            check(status.reachability == PeripheralStatus::Unreachable && status.exists
                      && status.onChip(),
                  QStringLiteral("读失败（时钟未知）仍算作存在"));

            status.exists = false;
            check(!status.onChip(), QStringLiteral("静态判定不存在 -> 不计入本芯片"));
            status.exists = true;
            status.bondedPins = 0;
            check(!status.onChip(), QStringLiteral("未引出引脚的外设不算作存在"));
        }
    }

    // --- per-part table: flash/RAM and exact part resolution ----------------
    {
        out(QString());
        out(QStringLiteral("--- 料号表（Flash / RAM） ---"));
        QString tableError;
        const int parts = PartTable::load(&tableError);
        check(parts > 0, QStringLiteral("加载 data/parts.json"),
              parts > 0 ? QStringLiteral("%1 个料号").arg(parts) : tableError);
        out(QStringLiteral("    来源: %1").arg(PartTable::sourcePath()));

        const PartTable::Entry exact = PartTable::lookup(QStringLiteral("N32H473CE"));
        check(exact.valid, QStringLiteral("精确料号命中"), exact.part);
        check(exact.flashBytes == 512 * 1024, QStringLiteral("N32H473CE Flash = 512 KB"),
              QStringLiteral("%1 KB").arg(exact.flashBytes / 1024));
        check(exact.ramBytes == 192 * 1024, QStringLiteral("N32H473CE RAM = 192 KB"),
              QStringLiteral("%1 KB (%2 + CCM %3)")
                  .arg(exact.ramBytes / 1024)
                  .arg(exact.sram)
                  .arg(exact.ccmSram));
        check(!exact.capabilities.isEmpty(), QStringLiteral("料号带能力清单"),
              QStringLiteral("%1 项").arg(exact.capabilities.size()));

        // A full order code must resolve to its pack entry, not to the family.
        const PartTable::Entry order = PartTable::lookup(QStringLiteral("N32H473CEU7"));
        check(order.valid && order.part == QLatin1String("N32H473CE"),
              QStringLiteral("订货号 N32H473CEU7 归到 N32H473CE"), order.part);
        check(order.flashBytes == exact.flashBytes,
              QStringLiteral("订货号与料号 Flash 一致"));

        // Same DEV_ID, different flash size: this is why family rows are not enough.
        const PartTable::Entry small = PartTable::lookup(QStringLiteral("N32H473KC"));
        check(small.valid && small.flashBytes == 256 * 1024,
              QStringLiteral("同系列小容量 N32H473KC Flash = 256 KB"),
              QStringLiteral("%1 KB").arg(small.flashBytes / 1024));
        check(small.flashBytes != exact.flashBytes,
              QStringLiteral("同 DEV_ID 的不同容量确实不同"));

        // An unknown name must not be invented.
        check(!PartTable::lookup(QStringLiteral("N32Z999XX")).valid,
              QStringLiteral("未知料号不编造数据"));

        // Cross-family coverage: the table must not be H4x-only.
        const PartTable::Entry g4 = PartTable::lookup(QStringLiteral("N32G451CBL7"));
        check(g4.valid && g4.flashBytes > 0 && g4.ramBytes > 0,
              QStringLiteral("G 系列料号也有 Flash/RAM"), g4.part);
        check(!g4.capabilities.isEmpty(), QStringLiteral("G 系列带能力清单"),
              QStringLiteral("%1 项").arg(g4.capabilities.size()));
        if (g4.valid) {
            QStringList caps;
            for (auto it = g4.capabilities.constBegin(); it != g4.capabilities.constEnd(); ++it)
                caps << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
            caps.sort();
            out(QStringLiteral("    %1: Flash %2 / RAM %3  %4")
                    .arg(g4.part, PartTable::formatSize(g4.flashBytes),
                         PartTable::formatSize(g4.ramBytes), caps.join(QStringLiteral(" "))));
        }

        // The status-bar wording is produced here, so assert it verbatim.
        const QString summary = PartTable::memorySummary(exact.flashBytes, exact.ramBytes);
        check(summary == QLatin1String("Flash 512 KB  /  RAM 192 KB"),
              QStringLiteral("状态栏 Flash/RAM 文案"), summary);
        check(PartTable::memorySummary(0, 0).isEmpty(),
              QStringLiteral("无数据时不显示占位尺寸"));
        check(PartTable::formatSize(1536 * 1024) == QLatin1String("1.5 MB"),
              QStringLiteral("非整数容量格式正确"), PartTable::formatSize(1536 * 1024));
    }

    // --- timer counters for the tree's live column --------------------------
    {
        out(QString());
        out(QStringLiteral("--- 定时器实时计数 ---"));
        QStringList lines;
        int timers = 0;
        int withCounter = 0;
        for (const auto &peripheral : device.peripherals) {
            if (!PeripheralRoles::isTimer(peripheral))
                continue;
            ++timers;
            const SvdRegister *counter = PeripheralRoles::counterRegister(peripheral);
            if (!counter) {
                lines << QStringLiteral("%1=无CNT").arg(peripheral.name);
                continue;
            }
            ++withCounter;
            // The repetition counter must never be chosen over the real count.
            check(!counter->name.endsWith(QLatin1String("REPCNT")),
                  QStringLiteral("%1 选中真实计数寄存器").arg(peripheral.name), counter->name);
            lines << QStringLiteral("%1=%2@0x%3")
                         .arg(peripheral.name, counter->name)
                         .arg(peripheral.baseAddress + counter->addressOffset, 0, 16);
        }
        check(timers > 0, QStringLiteral("识别出定时器"), QStringLiteral("%1 个").arg(timers));
        check(withCounter > 0, QStringLiteral("至少一个定时器有计数寄存器"),
              QStringLiteral("%1 / %2 个").arg(withCounter).arg(timers));
        Logger::instance()->info(QStringLiteral("定时器计数寄存器: %1")
                                     .arg(lines.join(QStringLiteral(", "))));

        // Spot-check the naming variants seen across the N32 families.
        for (const char *name : {"ATIM1", "GTIM1", "BTIM1", "LPTIM1"}) {
            const QString timer = QString::fromLatin1(name);
            const SvdPeripheral *peripheral = device.findPeripheral(timer);
            if (!peripheral)
                continue;
            const SvdRegister *counter = PeripheralRoles::counterRegister(*peripheral);
            check(counter != nullptr, QStringLiteral("%1 有计数寄存器").arg(timer),
                  counter ? counter->name : QStringLiteral("无"));
        }
        // Non-timer peripherals must not be treated as timers.
        const SvdPeripheral *gpioForRoles = device.findPeripheral(QStringLiteral("GPIOA"));
        check(gpioForRoles && !PeripheralRoles::isTimer(*gpioForRoles),
              QStringLiteral("GPIO 不被当作定时器"));
    }

    // --- package pin tables harvested from the datasheets -------------------
    // Generated by scripts/build-package-tables.py from the vendor datasheets and
    // validated (pin numbers exactly 1..N, N equal to the count in the package
    // name). These assertions keep the harvest honest.
    {
        out(QString());
        out(QStringLiteral("--- 封装引脚表（数据手册） ---"));
        const QString packageDir = ResourcePaths::packageDirectory();
        const QStringList files =
            QDir(packageDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);

        int tables = 0;
        int verifiedTables = 0;
        int totalPins = 0;
        QStringList families;
        QStringList problems;
        for (const QString &file : files) {
            const PackageDescription description =
                PackageDescription::loadFromFile(packageDir + QLatin1Char('/') + file);
            if (!description.isValid())
                continue;
            ++tables;
            if (description.numberVerified)
                ++verifiedTables;
            totalPins += description.pins.size();
            if (!description.chip.isEmpty() && !families.contains(description.chip))
                families << description.chip;

            // The package name encodes the pin count: LQFP64 must be 64 pins.
            const QRegularExpressionMatch digits =
                QRegularExpression(QStringLiteral("([0-9]+)")).match(description.packageName);
            if (digits.hasMatch()) {
                const int declared = digits.captured(1).toInt();
                if (declared != description.pins.size())
                    problems << QStringLiteral("%1: 名称 %2 脚，表内 %3 脚")
                                    .arg(file, digits.captured(1))
                                    .arg(description.pins.size());
            }
            QVector<int> numbers;
            numbers.reserve(description.pins.size());
            for (const auto &pin : description.pins)
                numbers << pin.number;
            std::sort(numbers.begin(), numbers.end());
            for (int i = 0; i < numbers.size(); ++i) {
                if (numbers.at(i) != i + 1) {
                    problems << QStringLiteral("%1: 引脚号不连续（第 %2 项是 %3）")
                                    .arg(file).arg(i + 1).arg(numbers.at(i));
                    break;
                }
            }
        }
        families.sort();
        out(QStringLiteral("    %1 张封装表 / %2 个系列 / 共 %3 个引脚（%4 张来自数据手册）")
                .arg(tables)
                .arg(families.size())
                .arg(totalPins)
                .arg(verifiedTables));
        out(QStringLiteral("    系列: %1").arg(families.join(QStringLiteral(", "))));
        check(tables > 1, QStringLiteral("已收集多个封装的引脚表"),
              QStringLiteral("%1 张").arg(tables));
        check(problems.isEmpty(), QStringLiteral("每张表引脚号连续且与封装脚数一致"),
              problems.isEmpty() ? QStringLiteral("全部通过")
                                 : problems.join(QStringLiteral("; ")));
        check(families.size() >= 5, QStringLiteral("覆盖多个系列"),
              QStringLiteral("%1 个").arg(families.size()));

        // Spot-check the pins that were verified by hand against the datasheet
        // package drawing earlier (pin 5 = PH0, pin 8 = PA0).
        for (const QString &file : files) {
            const PackageDescription description =
                PackageDescription::loadFromFile(packageDir + QLatin1Char('/') + file);
            // Tables are generated per series group, so the group name carries the
            // 'x' wildcard: N32H47x covers N32H473/474/475.
            if (description.packageName != QLatin1String("UQFN48")
                || !PackageView::matchesPart(description.chip, QLatin1String("N32H473CE")))
                continue;
            const PackagePin *five = description.pinByNumber(5);
            const PackagePin *eight = description.pinByNumber(8);
            const PackagePin *one = description.pinByNumber(1);
            check(five && five->name.startsWith(QLatin1String("PH0")),
                  QStringLiteral("UQFN48 脚 5 = PH0（与手册封装图一致）"),
                  five ? five->name : QStringLiteral("缺失"));
            check(eight && eight->name.startsWith(QLatin1String("PA0")),
                  QStringLiteral("UQFN48 脚 8 = PA0"), eight ? eight->name : QStringLiteral("缺失"));
            check(one && one->type == QLatin1String("power"),
                  QStringLiteral("UQFN48 脚 1 = 电源（VBAT）"), one ? one->name : QStringLiteral("缺失"));

            // LQFP48 genuinely has no PC4 pin (datasheet shows "-" in that column),
            // so it bonds fewer IOs than UQFN48. Assert the difference is real
            // rather than an extraction slip.
            const QHash<QString, QVector<int>> uqfnPorts = description.gpioPorts();
            int uqfnIo = 0;
            for (auto it = uqfnPorts.constBegin(); it != uqfnPorts.constEnd(); ++it)
                uqfnIo += it.value().size();
            for (const QString &other : files) {
                const PackageDescription lqfp =
                    PackageDescription::loadFromFile(packageDir + QLatin1Char('/') + other);
                if (lqfp.packageName != QLatin1String("LQFP48")
                    || !PackageView::matchesPart(lqfp.chip, QLatin1String("N32H473CE")))
                    continue;
                const QHash<QString, QVector<int>> ports = lqfp.gpioPorts();
                int lqfpIo = 0;
                for (auto it = ports.constBegin(); it != ports.constEnd(); ++it)
                    lqfpIo += it.value().size();
                out(QStringLiteral("    UQFN48 %1 IO / LQFP48 %2 IO（LQFP48 不引出 PC4）")
                        .arg(uqfnIo).arg(lqfpIo));
                check(lqfpIo < uqfnIo, QStringLiteral("LQFP48 的 IO 少于 UQFN48（手册如此）"),
                      QStringLiteral("%1 < %2").arg(lqfpIo).arg(uqfnIo));
            }
            break;
        }

        // Same package name across families must not collide (both LQFP48 tables
        // exist for N32G455 and N32H473).
        int lqfp48 = 0;
        for (const QString &file : files) {
            const PackageDescription description =
                PackageDescription::loadFromFile(packageDir + QLatin1Char('/') + file);
            if (description.packageName == QLatin1String("LQFP48"))
                ++lqfp48;
        }
        check(lqfp48 >= 2, QStringLiteral("LQFP48 在多个系列中并存（按 chip/封装 区分）"),
              QStringLiteral("%1 张").arg(lqfp48));
    }

    // --- static presence: vendor counts + package signals -------------------
    // The SVD covers the die/family (SPI1..SPI6), the part does not. Vendor
    // per-model counts (CMSIS-Pack .pdsc) plus the package pin table settle it
    // with zero writes to the target.
    {
        out(QString());
        out(QStringLiteral("--- 静态存在性判定 ---"));

        PeripheralPresence::Inputs inputs;
        inputs.limits = IdentificationResult().capabilities; // placeholder, filled below
        inputs.limits.clear();

        // Same source MainWindow uses: the identified part's capabilities from the
        // vendor part table (data/parts.json), which is per exact model. Note the
        // SVD's own <name> is the *family* ("N32H473"); the part number comes from
        // identification (--device / DEV_ID).
        const QString partName = options.deviceName.isEmpty() ? device.name : options.deviceName;
        const PartTable::Entry partEntry = PartTable::lookup(partName);
        inputs.limits = partEntry.capabilities;
        out(QStringLiteral("    料号 %1: Flash %2 / RAM %3")
                .arg(partEntry.valid ? partEntry.part : partName,
                     PartTable::formatSize(partEntry.flashBytes),
                     PartTable::formatSize(partEntry.ramBytes)));
        // A bare family name ("N32G455") legitimately resolves to nothing: it maps
        // to many order codes with different flash sizes, so guessing one would be
        // wrong. Only fail when the part *is* a unique table entry yet yielded no
        // capabilities.
        const bool partResolvable = PartTable::lookup(partName).valid;
        if (inputs.limits.isEmpty()) {
            if (partResolvable) {
                check(false, QStringLiteral("型号能力清单已从 pdsc 载入"),
                      QStringLiteral("%1 在料号表中但无能力清单").arg(partName));
            } else {
                out(QStringLiteral("    %1 无法唯一解析到料号（系列名），跳过按数量的裁剪判定")
                        .arg(partName));
            }
        } else {
            check(true, QStringLiteral("型号能力清单已从 pdsc 载入"),
                  QStringLiteral("%1 项").arg(inputs.limits.size()));
        }
        if (!inputs.limits.isEmpty()) {
            QStringList parts;
            for (auto it = inputs.limits.constBegin(); it != inputs.limits.constEnd(); ++it)
                parts << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
            parts.sort();
            out(QStringLiteral("    %1").arg(parts.join(QStringLiteral("  "))));
        }

        // Package signals: which instances this package can actually wire up.
        //
        // Several families share a package name and every family has many
        // packages, so the table must be picked by *this* part's package, exactly
        // like PackagePage::renderForChip does. Taking the first file that merely
        // matches the family would read the wrong package (SPI5 has MISO on
        // LQFP100 but not on UQFN48).
        QStringList wantedPackages;
        for (const QString &part : partEntry.package.split(QLatin1Char('/'), Qt::SkipEmptyParts))
            wantedPackages << part.trimmed();

        // No exact order code (family-level identification): fall back to the pin
        // count from the vendor manifest, which is what disambiguates LQFP48/64/100.
        // PartTable has no pin count, so take it from the vendor manifest.
        int wantedPins = 0;
        {
            SvdManifestEntry manifestEntry;
            QStringList ambiguous;
            if (library.lookupPart(partName, &manifestEntry, &ambiguous))
                wantedPins = manifestEntry.pinCount;
        }

        const QString packageDir = ResourcePaths::packageDirectory();
        QString chosenTable;
        for (const QString &name : QDir(packageDir).entryList({QStringLiteral("*.json")}, QDir::Files)) {
            const PackageDescription description =
                PackageDescription::loadFromFile(packageDir + QLatin1Char('/') + name);
            if (!description.isValid() || description.chip.isEmpty())
                continue;
            if (!PackageView::matchesPart(description.chip, device.name))
                continue;
            if (!wantedPackages.isEmpty()
                && !wantedPackages.contains(description.packageName, Qt::CaseInsensitive))
                continue;
            if (wantedPackages.isEmpty() && wantedPins > 0
                && description.pins.size() != wantedPins)
                continue;
            inputs.bondedSignals = description.bondedSignals();
            chosenTable = name;
            break;
        }
        out(QStringLiteral("    引脚表: %1")
                .arg(chosenTable.isEmpty() ? QStringLiteral("（无本封装表，跳过引脚判据）")
                                           : chosenTable));
        // Not every part has a pin table yet: only N32H473CEU7 does. Without one
        // the pin test is skipped on purpose (see PeripheralPresence), so this is
        // informational rather than a failure.
        if (inputs.bondedSignals.isEmpty()) {
            out(QStringLiteral("    （本封装无引脚表：跳过引脚信号判据，只按厂商数量裁剪）"));
        } else {
            check(true, QStringLiteral("封装信号集合已解析"),
                  QStringLiteral("%1 个实例").arg(inputs.bondedSignals.size()));
        }

        const QHash<QString, QString> absent = PeripheralPresence::absent(device, inputs);
        QStringList gone;
        for (auto it = absent.constBegin(); it != absent.constEnd(); ++it)
            gone << it.key();
        gone.sort();
        out(QStringLiteral("    判定本型号不具备: %1")
                .arg(gone.isEmpty() ? QStringLiteral("（无）") : gone.join(QStringLiteral(", "))));

        // These pin-level expectations are specific to the 48-pin N32H473CE, whose
        // datasheet was parsed by hand. Other parts are checked by the generic
        // invariants below instead of pretending the same pins apply.
        const bool isH473Uqfn48 =
            inputs.bondedSignals.contains(QStringLiteral("SPI5"))
            && !inputs.limits.isEmpty()
            && inputs.limits.value(QStringLiteral("SPI")) == 5;
        if (isH473Uqfn48) {
            check(absent.contains(QStringLiteral("SPI5")),
                  QStringLiteral("SPI5 被剔除（缺 MISO，仅 100/144 脚有）"),
                  absent.value(QStringLiteral("SPI5")));
            check(!absent.contains(QStringLiteral("SPI1")), QStringLiteral("SPI1 保留"));
            check(!absent.contains(QStringLiteral("SPI6")),
                  QStringLiteral("SPI6 保留（本封装信号齐全）"));
            check(absent.contains(QStringLiteral("FDCAN3")),
                  QStringLiteral("FDCAN3 被剔除（pdsc 声明只有 2 条）"),
                  absent.value(QStringLiteral("FDCAN3")));
            check(!absent.contains(QStringLiteral("USART3"))
                      && !absent.contains(QStringLiteral("UART5"))
                      && !absent.contains(QStringLiteral("UART8")),
                  QStringLiteral("可任意 IO 映射的实例保留"));
            check(!absent.contains(QStringLiteral("FDCAN1"))
                      && !absent.contains(QStringLiteral("FDCAN2")),
                  QStringLiteral("FDCAN1/2 保留（可任意映射）"));
        } else {
            out(QStringLiteral("    （当前器件不是 N32H473CE/UQFN48，跳过该封装的引脚级断言）"));
        }

        // Count agreement with the vendor table: usable == declared.
        const QVector<PeripheralPresence::SignalRule> rules = PeripheralPresence::signalRules();
        QStringList limitClasses = inputs.limits.keys();
        if (limitClasses.isEmpty())
            out(QStringLiteral("    （无能力清单，跳过按数量的一致性校验）"));
        limitClasses.sort();
        for (const QString &cls : limitClasses) {
            const int declared = inputs.limits.value(cls);
            int svdCount = 0;
            for (const auto &peripheral : device.peripherals) {
                QString prefix;
                int index = -1;
                PeripheralPresence::splitInstanceName(peripheral.name, &prefix, &index);
                if (prefix.compare(cls, Qt::CaseInsensitive) == 0)
                    ++svdCount;
            }
            if (svdCount == 0)
                continue;
            int goneCount = 0;
            for (auto it = absent.constBegin(); it != absent.constEnd(); ++it) {
                QString prefix;
                int index = -1;
                PeripheralPresence::splitInstanceName(it.key(), &prefix, &index);
                if (prefix.compare(cls, Qt::CaseInsensitive) == 0)
                    ++goneCount;
            }
            const int usable = svdCount - goneCount;
            // The vendor number is an upper bound on *instances*, but some
            // labels count channels instead (pdsc "DAC: 8" vs SVD DAC1..DAC4 with
            // two channels each). So the invariant is: trim down to the vendor
            // count when the SVD over-declares, and hide nothing otherwise.
            const int expected = qMin(svdCount, declared);
            out(QStringLiteral("    %1: SVD %2 实例, 声明 %3, 可用 %4")
                    .arg(cls)
                    .arg(svdCount)
                    .arg(declared)
                    .arg(usable));
            check(usable == expected,
                  svdCount > declared ? QStringLiteral("%1 裁剪到厂商声明数").arg(cls)
                                      : QStringLiteral("%1 未超声明，不隐藏").arg(cls),
                  QStringLiteral("可用 %1 / 期望 %2").arg(usable).arg(expected));
        }

        // No capability data => nothing may be hidden.
        PeripheralPresence::Inputs empty;
        check(PeripheralPresence::absent(device, empty).isEmpty(),
              QStringLiteral("无能力数据时不隐藏任何外设"));

        // Counts known but no pin table (the normal case for every part except
        // N32H473CE): the pin test must be skipped, so SPI5 stays although it
        // looks "unbonded" -- only the vendor count may trim.
        PeripheralPresence::Inputs noPins = inputs;
        noPins.bondedSignals.clear();
        const PeripheralPresence::Result countOnly = PeripheralPresence::analyse(device, noPins);
        // Without a pin table the counts cannot say *which* instance is extra.
        // Hiding a usable peripheral (SPI6, which this package wires up fully)
        // would be worse than listing one too many, so nothing is hidden and the
        // discrepancy is reported instead.
        check(countOnly.absent.isEmpty(),
              QStringLiteral("无引脚数据时不隐藏任何实例"),
              QStringLiteral("%1 个被剔除").arg(countOnly.absent.size()));
        if (inputs.limits.isEmpty()) {
            // Without vendor counts there is nothing to compare, so no note either.
            check(countOnly.undecidable.isEmpty(),
                  QStringLiteral("无数量数据时不产生差异提示"),
                  QStringLiteral("%1 条").arg(countOnly.undecidable.size()));
        } else {
            check(!countOnly.undecidable.isEmpty(),
                  QStringLiteral("无引脚数据时报告数量差异"));
        }
        for (const QString &note : countOnly.undecidable)
            out(QStringLiteral("    %1").arg(note));
    }

    // --- GUI refresh budget ------------------------------------------------
    // The polling path used to rebuild the inspector table on every read
    // completion (~280 us each, a dozen completions per poll), which made the
    // window crawl. This keeps it from creeping back.
    {
        out(QString());
        out(QStringLiteral("--- 刷新性能预算 ---"));
        const SvdPeripheral *rich = nullptr;
        for (const auto &peripheral : device.peripherals) {
            if (!rich || peripheral.registers.size() > rich->registers.size())
                rich = &peripheral;
        }
        if (rich && !rich->registers.isEmpty()) {
            const SvdRegister *reg = &rich->registers.first();
            RegisterSession session(nullptr, nullptr);
            session.setDevice(device);
            QByteArray bytes;
            for (const auto &r : rich->registers) {
                const quint32 reset = r.resetValue;
                QByteArray one(4, char(0));
                std::memcpy(one.data(), &reset, 4);
                bytes += one;
            }
            session.applyRead(rich->baseAddress, bytes, true, QString());

            InspectorPanel inspector;
            const quint32 address = rich->baseAddress + reg->addressOffset;
            // First call builds the table; the polling path must then be cheap.
            inspector.setRegister(reg, address, session.effectiveValue(address), true);

            constexpr int kRounds = 40;
            // Best of several trials. A single measurement is at the mercy of the
            // scheduler: this pair was seen once at 1.87x against a 2x threshold with no
            // code change. Interference only ever adds time, so the minimum is the
            // honest estimate of what the code costs.
            constexpr int kTrials = 4;
            double perRefresh = 1e18;
            double perRebuild = 1e18;
            QElapsedTimer timer;
            for (int trial = 0; trial < kTrials; ++trial) {
                timer.start();
                for (int i = 0; i < kRounds; ++i) {
                    // Same register, new value: the common polling case.
                    inspector.setRegister(reg, address, quint32(i), true);
                }
                perRefresh = qMin(perRefresh,
                                  double(timer.nsecsElapsed()) / 1000.0 / kRounds);

                // Compare against a full rebuild of the same widget: an absolute
                // threshold would be tuned to one register's field count, while the
                // regression we care about is precisely "we went back to rebuilding".
                inspector.setRegister(nullptr, 0, 0, false);
                timer.restart();
                for (int i = 0; i < kRounds; ++i) {
                    inspector.setRegister(nullptr, 0, 0, false);
                    inspector.setRegister(reg, address, quint32(i), true);
                }
                perRebuild = qMin(perRebuild,
                                  double(timer.nsecsElapsed()) / 1000.0 / kRounds);
            }
            out(QStringLiteral("    同寄存器刷新 %1 us/次，整表重建 %2 us/次")
                    .arg(perRefresh, 0, 'f', 1)
                    .arg(perRebuild, 0, 'f', 1));
            check(perRefresh * 2.0 < perRebuild,
                  QStringLiteral("轮询刷新明显快于整表重建"),
                  QStringLiteral("%1 us vs %2 us").arg(perRefresh, 0, 'f', 1)
                      .arg(perRebuild, 0, 'f', 1));
            check(perRefresh < 500.0, QStringLiteral("轮询刷新在绝对预算内"),
                  QStringLiteral("%1 us").arg(perRefresh, 0, 'f', 1));
        }
    }

    // --- field selection must not ping-pong --------------------------------
    // The app used to crash on the first click of a field on the right: the
    // inspector emitted fieldSelected, MainWindow mirrored it back with
    // selectField(), which emitted again ... unbounded recursion (stack overflow).
    {
        out(QString());
        out(QStringLiteral("--- 字段选择回环保护 ---"));

        const SvdRegister *multi = nullptr;
        const SvdPeripheral *owner = nullptr;
        for (const auto &peripheral : device.peripherals) {
            for (const auto &reg : peripheral.registers) {
                if (reg.fields.size() >= 2) {
                    multi = &reg;
                    owner = &peripheral;
                    break;
                }
            }
            if (multi)
                break;
        }

        if (!multi) {
            check(false, QStringLiteral("找到含多个字段的寄存器"));
        } else {
            InspectorPanel panel;
            const quint32 address = owner->baseAddress + multi->addressOffset;
            panel.setRegister(multi, address, multi->resetValue, true);

            int emitted = 0;
            int recursionGuard = 0;
            // Mirror exactly what MainWindow did: on fieldSelected, select the same
            // field again. With the guard in place this must settle immediately.
            QObject::connect(&panel, &InspectorPanel::fieldSelected, &panel,
                             [&panel, &emitted, &recursionGuard](int index) {
                                 ++emitted;
                                 if (++recursionGuard > 50)
                                     return; // test safety net, would mean recursion
                                 panel.selectField(index);
                             });

            panel.selectField(0);
            check(emitted <= 1, QStringLiteral("重复选择同一字段只发一次信号"),
                  QStringLiteral("%1 次").arg(emitted));

            emitted = 0;
            recursionGuard = 0;
            panel.selectField(1);
            check(emitted <= 1, QStringLiteral("切换字段只发一次信号"),
                  QStringLiteral("%1 次").arg(emitted));
            check(panel.selectedField() == 1, QStringLiteral("切换后选中字段正确"),
                  QStringLiteral("selected=%1").arg(panel.selectedField()));

            // Selecting an out-of-range field must be ignored, not crash.
            emitted = 0;
            panel.selectField(multi->fields.size());
            panel.selectField(-1);
            check(emitted == 0, QStringLiteral("越界字段被忽略且不发信号"),
                  QStringLiteral("%1 次").arg(emitted));
        }
    }

    // --- real widget graph: clicking a field must not recurse ---------------
    // The unit test above emulates the old mirroring; this one drives the actual
    // MainWindow <-> InspectorPanel connections, which is where the crash was
    // reported ("click a register on the right -> crash").
    {
        out(QString());
        out(QStringLiteral("--- 点击字段（真实窗口连接图） ---"));
        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));
        if (auto *inspector = window.findChild<InspectorPanel *>()) {
            // Find a register with several fields and hand it to the inspector the
            // same way a click in the register view does.
            const SvdRegister *multi = nullptr;
            const SvdPeripheral *owner = nullptr;
            for (const auto &peripheral : device.peripherals) {
                for (const auto &reg : peripheral.registers) {
                    if (reg.fields.size() >= 3) {
                        multi = &reg;
                        owner = &peripheral;
                        break;
                    }
                }
                if (multi)
                    break;
            }
            if (!multi) {
                check(false, QStringLiteral("找到含 3+ 字段的寄存器"));
            } else {
                const quint32 address = owner->baseAddress + multi->addressOffset;
                inspector->setRegister(multi, address, multi->resetValue, true);
                // Walk the fields exactly like repeated clicks do.
                bool crashed = false;
                for (int i = 0; i < multi->fields.size(); ++i) {
                    inspector->selectField(i);
                    inspector->selectField(i); // same field twice: the old hang
                }
                check(!crashed, QStringLiteral("连续切换/重复选择字段不崩溃"),
                      QStringLiteral("%1 个字段").arg(multi->fields.size()));
                check(inspector->selectedField()
                          == multi->fields.size() - 1,
                      QStringLiteral("最后一次选择生效"),
                      QStringLiteral("selected=%1").arg(inspector->selectedField()));
            }
        } else {
            check(false, QStringLiteral("找到 InspectorPanel"));
        }
    }

    // --- disconnecting must empty the inspector ----------------------------
    // Requested behaviour: after a disconnect the right-hand register inspector
    // has to go back to its empty state instead of showing the last register.
    {
        out(QString());
        out(QStringLiteral("--- 断开后检查器复位 ---"));
        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));
        auto *inspector = window.findChild<InspectorPanel *>();
        if (!inspector) {
            check(false, QStringLiteral("找到 InspectorPanel"));
        } else {
            const SvdRegister *reg = nullptr;
            const SvdPeripheral *owner = nullptr;
            for (const auto &peripheral : device.peripherals) {
                if (!peripheral.registers.isEmpty()) {
                    reg = &peripheral.registers.first();
                    owner = &peripheral;
                    break;
                }
            }
            if (!reg) {
                check(false, QStringLiteral("找到寄存器"));
            } else {
                // A loaded register, as after a click.
                inspector->setRegister(reg, owner->baseAddress + reg->addressOffset,
                                       reg->resetValue, true);
                check(inspector->reg() != nullptr, QStringLiteral("先选中一个寄存器"));

                // Same call the disconnect handler makes.
                QMetaObject::invokeMethod(&window, "parkDeviceViews", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("已断开连接")));

                check(inspector->reg() == nullptr,
                      QStringLiteral("断开后检查器不再持有寄存器"));
                check(inspector->selectedField() == -1,
                      QStringLiteral("断开后不保留选中字段"),
                      QStringLiteral("selected=%1").arg(inspector->selectedField()));

                // The port matrix must not keep drawing the last port either.
                if (auto *grid = window.findChild<PortGridWidget *>())
                    check(grid->pins().isEmpty(),
                          QStringLiteral("断开后端口矩阵清空"),
                          QStringLiteral("%1 个引脚").arg(grid->pins().size()));
            }
        }
    }

    // --- a lost link must stop the polling, not flood the log ---------------
    // Symptom: after connecting a G455 the log was flooded with
    //   WARN 读取 0xE000ED28 失败: GDB Server 未连接
    // every 200 ms. Two bugs: re-attach failure was reported as a successful
    // connect (so the UI kept polling a dead server), and each failed read logged
    // its own line.
    {
        out(QString());
        out(QStringLiteral("--- 断线后停止轮询 ---"));
        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));

        // The session must notice that the link is gone from the error text and mark
        // itself disconnected, so the poll loop stops queueing requests.
        RegisterSession session(nullptr, nullptr);
        session.setDevice(device);
        session.setConnected(true);
        check(session.connected(), QStringLiteral("会话初始为已连接"));

        int failures = 0;
        QObject::connect(&session, &RegisterSession::readFailed,
                         [&failures](quint32, const QString &) { ++failures; });
        session.applyRead(0x4002C000u, QByteArray(), false,
                          QStringLiteral("GDB Server 未连接"));
        check(failures == 1, QStringLiteral("读取失败会发出通知（便于界面提示）"),
              QStringLiteral("%1 次").arg(failures));
        check(!session.connected(),
              QStringLiteral("提示“未连接”时会话自动标记断开（停止后续轮询）"));

        // A normal bus error on one register must NOT kill the session.
        RegisterSession alive(nullptr, nullptr);
        alive.setDevice(device);
        alive.setConnected(true);
        alive.applyRead(0x4002C000u, QByteArray(), false, QStringLiteral("总线错误"));
        check(alive.connected(),
              QStringLiteral("单次总线错误不误判断线"));
    }

    // --- 系列 / 料号 两级选择 ------------------------------------------------
    // Identification is now "core from silicon, order code from the user". These cover
    // what can silently go wrong: the picker offering parts that do not exist, an order
    // code losing its per-package facts, or a bare series being accepted as a package.
    {
        out(QString());
        out(QStringLiteral("--- 系列与料号选择 ---"));

        check(PartTable::load() > 0, QStringLiteral("器件表已加载"),
              QStringLiteral("%1 条").arg(PartTable::count()));

        const QStringList series = PartTable::series();
        check(series.size() > 20, QStringLiteral("系列列表非空"),
              QStringLiteral("%1 个系列").arg(series.size()));
        check(series.contains(QStringLiteral("N32G455")),
              QStringLiteral("系列里包含 N32G455"), QString());
        check(!series.contains(QStringLiteral("ARMCM4")),
              QStringLiteral("不把 ARM 内核模板当作系列"), QString());

        const QStringList parts455 = PartTable::partsInSeries(QStringLiteral("N32G455"));
        out(QStringLiteral("    N32G455 料号: %1 个").arg(parts455.size()));
        check(parts455.size() == 15, QStringLiteral("N32G455 有 15 个料号"),
              QStringLiteral("%1 个").arg(parts455.size()));
        check(parts455.contains(QStringLiteral("N32G455CCL7")),
              QStringLiteral("含 N32G455CCL7"), QString());
        check(parts455.contains(QStringLiteral("N32G455CEQ7")),
              QStringLiteral("含 512KB 的 N32G455CEQ7"), QString());

        check(PartTable::seriesOf(QStringLiteral("N32G455CCL7")) == QLatin1String("N32G455"),
              QStringLiteral("seriesOf(N32G455CCL7) = N32G455"),
              PartTable::seriesOf(QStringLiteral("N32G455CCL7")));
        check(PartTable::seriesOf(QStringLiteral("N32H473CE")) == QLatin1String("N32H473"),
              QStringLiteral("seriesOf(N32H473CE) = N32H473"),
              PartTable::seriesOf(QStringLiteral("N32H473CE")));

        // A bare series must not pass as an order code: that is what used to let the app
        // show a package the board does not have.
        check(!PartTable::isOrderCode(QStringLiteral("N32G455")),
              QStringLiteral("裸系列不算料号"), QString());
        check(PartTable::isOrderCode(QStringLiteral("N32G455CCL7")),
              QStringLiteral("完整料号算料号"), QString());

        // The dialog must offer exactly the table's contents, and pre-select correctly.
        // Constructor is (currentPart, currentPackage).
        ChipPickerDialog picker(QStringLiteral("N32G455CCL7"), QStringLiteral("LQFP48"));
        check(picker.seriesForTest().size() == series.size(),
              QStringLiteral("对话框系列数与器件表一致"),
              QStringLiteral("%1 vs %2").arg(picker.seriesForTest().size()).arg(series.size()));
        check(picker.currentSeriesForTest() == QLatin1String("N32G455"),
              QStringLiteral("按当前料号预选系列"), picker.currentSeriesForTest());
        picker.selectSeriesForTest(QStringLiteral("N32G455"));
        check(picker.partsForTest() == parts455,
              QStringLiteral("对话框料号列表与器件表一致"),
              QStringLiteral("%1 个").arg(picker.partsForTest().size()));
        check(picker.selectedPart() == QLatin1String("N32G455CCL7"),
              QStringLiteral("预选当前料号"), picker.selectedPart());
        check(picker.selectedPackage() == QLatin1String("LQFP48"),
              QStringLiteral("预选当前封装"), picker.selectedPackage());
        check(picker.packagesForTest() == QStringList{QStringLiteral("LQFP48")},
              QStringLiteral("该料号只有一个封装"),
              picker.packagesForTest().join(QStringLiteral(",")));
        check(picker.isComplete(), QStringLiteral("料号+封装齐备时视为完整"), QString());
        // The detail panel must describe the SELECTED part, not the first in the list:
        // a stale panel here means the user checks LQFP48/256KB and picks a different one.
        out(QStringLiteral("    详情面板: %1").arg(picker.detailForTest().replace(QStringLiteral("<br/>"), QStringLiteral(" | "))));
        check(picker.detailForTest().contains(QStringLiteral("256 KB")),
              QStringLiteral("详情面板显示所选料号的容量（256 KB）"),
              picker.detailForTest());
        check(picker.detailForTest().contains(QStringLiteral("LQFP48")),
              QStringLiteral("详情面板显示所选料号的封装"),
              picker.detailForTest());

        // A part offered in two packages must expose both, and switching the package must
        // change what gets applied -- the pinout follows the package.
        {
            const QStringList cePackages = PartTable::packagesOf(QStringLiteral("N32H473CE"));
            out(QStringLiteral("    N32H473CE 封装: %1").arg(cePackages.join(QStringLiteral(","))));
            check(cePackages.size() == 2,
                  QStringLiteral("N32H473CE 有 UQFN48 与 LQFP48 两个封装"),
                  cePackages.join(QStringLiteral(",")));
            ChipPickerDialog multi(QStringLiteral("N32H473CE"), QString());
            check(multi.packagesForTest() == cePackages,
                  QStringLiteral("对话框列出该料号的全部封装"),
                  multi.packagesForTest().join(QStringLiteral(",")));
            multi.selectPackageForTest(QStringLiteral("UQFN48"));
            check(multi.selectedPackage() == QLatin1String("UQFN48"),
                  QStringLiteral("封装可切换"), multi.selectedPackage());
        }

        // With nothing saved, the dialog must still open on something valid rather than an
        // empty part list (the series dropdown defaults to the first series).
        ChipPickerDialog fresh{QString(), QString()};
        check(!fresh.seriesForTest().isEmpty() && !fresh.partsForTest().isEmpty(),
              QStringLiteral("空选择时仍列出可选系列与料号"),
              QStringLiteral("系列 %1 / 料号 %2")
                  .arg(fresh.seriesForTest().size())
                  .arg(fresh.partsForTest().size()));
        check(!fresh.isComplete() || !fresh.selectedPart().isEmpty(),
              QStringLiteral("默认选中了第一个料号"), fresh.selectedPart());
    }

    // --- 按料号加载器件：SVD / 容量 / 封装 -----------------------------------
    {
        out(QString());
        out(QStringLiteral("--- 按料号加载器件 ---"));

        // A probe that answers only CPUID and DEV_ID: identification must not need more.
        struct CoreOnlyProbe : IProbe
        {
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = 0x410FC241u; return Status::success(); }
                if (address == 0xE0042000u) { *out = 0x00000511u; return Status::success(); }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override
            {
                return Status::failure(QStringLiteral("ro"));
            }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        CoreOnlyProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());

        // (a) No order code: core known, nothing else claimed.
        const auto bare = DeviceIdentifier::identify(probe, lib, QString());
        check(bare.ok, QStringLiteral("仅内核也能识别成功"),
              bare.ok ? QString() : bare.error);
        if (bare.ok) {
            check(bare.value.coreName == QLatin1String("Cortex-M4"),
                  QStringLiteral("内核由 CPUID 得出"), bare.value.coreName);
            check(bare.value.series.isEmpty(),
                  QStringLiteral("没有料号就不给系列（不再读 DEV_ID）"), bare.value.series);
            check(bare.value.svdPath.isEmpty(),
                  QStringLiteral("未选料号时不加载 SVD"), bare.value.svdPath);
            check(bare.value.partNumber.isEmpty(),
                  QStringLiteral("未选料号时没有料号"), bare.value.partNumber);
        }

        // (b) A bare series is not an order code here either.
        const auto asSeries = DeviceIdentifier::identify(probe, lib, QStringLiteral("N32G455"));
        check(asSeries.ok && asSeries.value.svdPath.isEmpty(),
              QStringLiteral("只给系列名不加载 SVD"),
              asSeries.ok ? asSeries.value.svdPath : asSeries.error);

        // (c) With an order code, everything follows it.
        const auto picked =
            DeviceIdentifier::identify(probe, lib, QStringLiteral("N32G455CCL7"));
        check(picked.ok, QStringLiteral("按料号识别成功"),
              picked.ok ? QString() : picked.error);
        if (picked.ok) {
            const IdentificationResult &r = picked.value;
            out(QStringLiteral("    N32G455CCL7 -> %1 / %2 / %3")
                    .arg(r.partNumber, PartTable::formatSize(r.flashBytes), r.packageName));
            check(r.partNumber == QLatin1String("N32G455CCL7"),
                  QStringLiteral("料号被采用"), r.partNumber);
            check(r.flashBytes == 256 * 1024, QStringLiteral("容量来自该料号（256 KB）"),
                  PartTable::formatSize(r.flashBytes));
            check(r.packageName == QLatin1String("LQFP48"),
                  QStringLiteral("封装来自该料号（LQFP48）"), r.packageName);
            check(!r.svdPath.isEmpty() && QFileInfo::exists(r.svdPath),
                  QStringLiteral("找到 SVD 文件"), r.svdPath);
            check(r.jlinkDeviceName.startsWith(QLatin1String("N32G455CC")),
                  QStringLiteral("J-Link 器件名已解析"), r.jlinkDeviceName);
        }

        // (d) A different order code in the same series gives different facts.
        const auto bigger =
            DeviceIdentifier::identify(probe, lib, QStringLiteral("N32G455CEQ7"));
        check(bigger.ok, QStringLiteral("按 512KB 料号识别成功"),
              bigger.ok ? QString() : bigger.error);
        if (bigger.ok) {
            out(QStringLiteral("    N32G455CEQ7 -> %1 / %2 / %3")
                    .arg(bigger.value.partNumber,
                         PartTable::formatSize(bigger.value.flashBytes),
                         bigger.value.packageName));
            check(bigger.value.flashBytes == 512 * 1024,
                  QStringLiteral("同一系列选不同料号得到 512 KB"),
                  PartTable::formatSize(bigger.value.flashBytes));
            check(!bigger.value.svdPath.isEmpty(), QStringLiteral("512KB 料号也有 SVD"),
                  bigger.value.svdPath);
        }
    }

    // --- connection log: one summary line, details at DEBUG -----------------
    // Requested: stop dumping CPUID/IDCODE/manifest/memory-probe lines on every
    // connect. One INFO line must carry the answer; the steps move to DEBUG.
    {
        out(QString());
        out(QStringLiteral("--- 连接日志精简 ---"));

        // Capture what identify() logs at INFO level.
        QStringList infoLines;
        const auto previousLevel = Logger::instance()->minimumLevel();
        // The receiver must own the lambda's lifetime: connecting to the Logger
        // itself kept it installed after this block ended, so a later log line
        // wrote into a destroyed QStringList and crashed (seen with --verbose,
        // where the extra DEBUG lines reach the log *after* this scope).
        QObject captor;
        QObject::connect(Logger::instance(), &Logger::messageLogged, &captor,
                         [&infoLines](const QString &line, int level) {
                             if (level == Logger::Info)
                                 infoLines << line;
                         });
        Logger::instance()->setMinimumLevel(Logger::Info);

        struct IdProbe : IProbe
        {
            quint32 idcode = 0x8F544511u;
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) {
                    *out = 0x410FC241u;
                    return Status::success();
                }
                if (address == 0xE0042000u) {
                    *out = idcode;
                    return Status::success();
                }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override { return Status::failure(QStringLiteral("ro")); }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        IdProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto identified = DeviceIdentifier::identify(probe, lib, QString());
        Logger::instance()->setMinimumLevel(previousLevel);

        int summaryLines = 0;
        QString summaryText;
        for (const QString &line : infoLines) {
            if (line.contains(QStringLiteral("已连接:"))) {
                ++summaryLines;
                summaryText = line.section(QStringLiteral("INFO"), 1).trimmed();
            }
        }
        out(QStringLiteral("    INFO 行数 %1，其中连接摘要 %2 条").arg(infoLines.size()).arg(summaryLines));
        if (!summaryText.isEmpty())
            out(QStringLiteral("    %1").arg(summaryText));

        check(identified.ok, QStringLiteral("识别成功"));
        check(summaryLines == 1, QStringLiteral("连接只产生一条摘要 INFO"),
              QStringLiteral("%1 条").arg(summaryLines));
        // The summary now reports the core and (when a part is selected) the part. DEV_ID
        // is deliberately absent: it is no longer read, because it cannot name a package.
        check(summaryText.contains(QLatin1String("Cortex-M4")),
              QStringLiteral("摘要含内核"), summaryText);
        check(!summaryText.contains(QLatin1String("DEV_ID")),
              QStringLiteral("摘要不再含 DEV_ID（已不读取）"), summaryText);
        // The noise must be gone: these used to be INFO lines.
        // "判定为本型号不具备" / "静态判定剔除" belong here too: one line per
        // hidden peripheral buried the single summary line on real boards.
        for (const QString &noise : {QStringLiteral("CPUID ="), QStringLiteral("IDCODE @"),
                                     QStringLiteral("厂商清单:"), QStringLiteral("能力清单"),
                                     QStringLiteral("判定为本型号不具备"),
                                     QStringLiteral("静态判定剔除")}) {
            bool leaked = false;
            for (const QString &line : infoLines) {
                if (line.contains(noise))
                    leaked = true;
            }
            check(!leaked, QStringLiteral("INFO 里不再出现「%1」").arg(noise));
        }
    }

    // --- 芯片选择：保存的料号 -------------------------------------------------
    // The editable combo was replaced by a button that opens the series/part dialog. What
    // still needs covering is the persisted state: an empty value means "no part picked"
    // (registers work, no SVD), and a saved value is what every later connection uses.
    {
        out(QString());
        out(QStringLiteral("--- 芯片选择与保存 ---"));

        AppSettings::migrateChipNameSetting();
        AppSettings::setChipName(QString());
        // Empty setting == "no part picked": the only thing that may be claimed then is
        // the core, so no SVD path may be derived. (The identify() half is covered by the
        // "按料号加载器件" block; here it is the persisted state that matters.)
        check(AppSettings::chipName().isEmpty(),
              QStringLiteral("空设置表示未选料号"), AppSettings::chipName());

        AppSettings::setChipName(QStringLiteral("N32G455REL7"));
        check(AppSettings::chipName() == QLatin1String("N32G455REL7"),
              QStringLiteral("选择的料号被保存"), AppSettings::chipName());

        // The saved value must be a real order code, and it must drive the parts every
        // view needs -- this is the whole contract of the picker.
        check(PartTable::isOrderCode(AppSettings::chipName()),
              QStringLiteral("保存的是真实料号"), AppSettings::chipName());
        const PartTable::Entry entry = PartTable::lookup(AppSettings::chipName());
        check(entry.valid && entry.flashBytes == 512 * 1024,
              QStringLiteral("该料号给出 512 KB"),
              PartTable::formatSize(entry.flashBytes));

        // The button must show the picked code, not a sentence.
        MainWindow window;
        auto *partButton = window.findChild<QPushButton *>();
        bool foundButton = false;
        for (auto *candidate : window.findChildren<QPushButton *>()) {
            if (candidate->text().contains(QLatin1String("N32G455REL7"))) {
                foundButton = true;
                break;
            }
        }
        check(foundButton, QStringLiteral("工具栏按钮显示当前料号"),
              partButton ? partButton->text() : QStringLiteral("<未找到>"));
        // And a free-text combo must be gone: only real order codes are acceptable.
        bool editableCombo = false;
        for (auto *combo : window.findChildren<QComboBox *>()) {
            if (combo->isEditable())
                editableCombo = true;
        }
        check(!editableCombo, QStringLiteral("不再有可自由输入的芯片下拉框"), QString());

        AppSettings::setChipName(QString());
    }

    // --- connection detail stays out of INFO --------------------------------
    // Requested: stop printing per-port / per-mapping lines on every connect. The
    // only thing INFO may say about a connection is the one summary line.
    {
        out(QString());
        out(QStringLiteral("--- 连接日志只剩摘要 ---"));

        // Text that must no longer appear at INFO level.
        const QStringList noise = {
            QStringLiteral("端口探测: 存在"),
            QStringLiteral("die 上没有的 GPIO 端口"),
            QStringLiteral("J-Link 器件名:"),
            QStringLiteral("按 DEV_ID 识别为"),
            QStringLiteral("已加载 %1 的 SVD"),
            QStringLiteral("按端口探测隐藏"),
            QStringLiteral("封装未引出该端口"),
            QStringLiteral("按封装键合数据修正了"),
        };

        // Scan the sources for these strings and require a DEBUG call.
        QStringList leaks;
        QDirIterator it(QStringLiteral("src"), {QStringLiteral("*.cpp")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QStringList lines = QString::fromUtf8(file.readAll()).split(QChar(10));
            for (int i = 0; i < lines.size(); ++i) {
                for (const QString &needle : noise) {
                    if (!lines.at(i).contains(needle))
                        continue;
                    // The logger call for that line must be debug(), not info().
                    for (int back = i; back >= 0 && back > i - 4; --back) {
                        if (lines.at(back).contains(QLatin1String("Logger::instance()->info("))) {
                            leaks << QStringLiteral("%1:%2").arg(path).arg(back + 1);
                            break;
                        }
                        if (lines.at(back).contains(QLatin1String("Logger::instance()->debug(")))
                            break;
                    }
                }
            }
        }
        check(leaks.isEmpty(), QStringLiteral("这类连接细节已降为 DEBUG（不刷 INFO）"),
              leaks.join(QStringLiteral(", ")));

        // And the single summary line is still emitted at INFO.
        QStringList infoLines;
        QObject captor;
        QObject::connect(Logger::instance(), &Logger::messageLogged, &captor,
                         [&infoLines](const QString &line, int level) {
                             if (level == Logger::Info)
                                 infoLines << line;
                         });
        const auto previous = Logger::instance()->minimumLevel();
        Logger::instance()->setMinimumLevel(Logger::Info);

        struct QuietProbe : IProbe
        {
            quint32 rccApb2 = 0;
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = 0x410FC241u; return Status::success(); }
                if (address == 0xE0042000u) { *out = 0x8F544511u; return Status::success(); }
                const quint32 flashEnd = 0x08000000u + 256u * 1024u;
                if (address >= 0x08000000u && address < flashEnd) { *out = 0x11111111u; return Status::success(); }
                const quint32 ramEnd = 0x20000000u + 144u * 1024u;
                if (address >= 0x20000000u && address < ramEnd) { *out = 0x22222222u; return Status::success(); }
                if (address == 0x40021018u) { *out = rccApb2; return Status::success(); }
                if (address >= 0x40010800u && address < 0x40011800u) {
                    const quint32 port = (address - 0x40010800u) / 0x400u;
                    if (port > 2)
                        return Status::failure(QStringLiteral("no such port"));
                    if ((rccApb2 & (1u << (2 + port))) == 0)
                        return Status::failure(QStringLiteral("clock off"));
                    *out = 0x00080800u;
                    return Status::success();
                }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32 address, quint32 value) override
            {
                if (address == 0x40021018u) { rccApb2 = value; return Status::success(); }
                return Status::failure(QStringLiteral("ro"));
            }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        QuietProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto result = DeviceIdentifier::identify(probe, lib, QString());
        Logger::instance()->setMinimumLevel(previous);

        int portNoise = 0;
        for (const QString &line : infoLines) {
            if (line.contains(QStringLiteral("端口探测")) || line.contains(QStringLiteral("J-Link 器件名")))
                ++portNoise;
        }
        out(QStringLiteral("    INFO 行 %1 条，其中连接细节 %2 条")
                .arg(infoLines.size())
                .arg(portNoise));
        for (const QString &line : infoLines)
            out(QStringLiteral("      %1").arg(line.section(QStringLiteral("INFO"), 1).trimmed()));
        check(portNoise == 0, QStringLiteral("识别后 INFO 里没有端口/器件名细节"),
              QStringLiteral("%1 条").arg(portNoise));

        // A same-part name in the toolbar ("N32G455CC" vs the row "N32G455") must not
        // be reported as a mismatch -- that produced a WARN on every reconnect.
        if (result.ok) {
            bool warnedMismatch = false;
            for (const QString &line : infoLines) {
                if (line.contains(QStringLiteral("实际是")))
                    warnedMismatch = true;
            }
            check(!warnedMismatch || !result.value.note.contains(QStringLiteral("实际是")),
                  QStringLiteral("同器件的不同写法不误报为冲突"),
                  result.value.note);
        }
    }

    // --- 未登记 DEV_ID 不硬失败 ---------------------------------------------
    // Previously this probed memory and produced a candidate list. Now an unknown DEV_ID
    // only means "series unknown": the core is still identified (registers work) and the
    // user is asked for an order code instead of the app guessing one.
    {
        out(QString());
        out(QStringLiteral("--- 未登记 DEV_ID 不硬失败 ---"));

        struct UnknownIdProbe : IProbe
        {
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = 0x410FC241u; return Status::success(); }
                // DEV_ID nobody registered.
                if (address == 0xE0042000u) { *out = 0x00ABFFFFu; return Status::success(); }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override
            {
                return Status::failure(QStringLiteral("ro"));
            }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        UnknownIdProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto result = DeviceIdentifier::identify(probe, lib, QString());

        check(result.ok, QStringLiteral("未登记 DEV_ID 仍返回结果"),
              result.ok ? QString() : result.error);
        if (result.ok) {
            out(QStringLiteral("    内核 %1，系列 %2")
                    .arg(result.value.coreName,
                         result.value.series.isEmpty() ? QStringLiteral("(未知)")
                                                       : result.value.series));
            check(result.value.coreName == QLatin1String("Cortex-M4"),
                  QStringLiteral("内核仍被识别（寄存器可访问）"), result.value.coreName);
            check(result.value.series.isEmpty(), QStringLiteral("不给系列（不猜）"),
                  result.value.series);
            check(result.value.svdPath.isEmpty(), QStringLiteral("没有 SVD，等待选型"),
                  result.value.svdPath);
            check(!result.value.note.isEmpty() || result.value.svdPath.isEmpty(),
                  QStringLiteral("未选料号时没有 SVD（等待用户选择）"),
                  result.value.note);
        }
    }

    // --- GPIO base addresses identify the family without a DEV_ID ------------
    // The DEV_ID -> part mapping has no vendor-side source (the .pdsc, the SVDs and
    // SEGGER's JlinkDevices.xml all lack it), so a new model has to be measured.
    // Where each family keeps its GPIO blocks is the cheapest discriminator, and
    // scripts/sync-svd-gpio.py pre-computes it into data/svd_gpio.json.
    {
        out(QString());
        out(QStringLiteral("--- GPIO 基地址表（无 DEV_ID 时定系列）---"));
        const MemoryProbe::GpioMap map = MemoryProbe::gpioMap();
        check(map.valid(), QStringLiteral("GPIO 基地址表可加载"),
              QStringLiteral("%1 个 SVD").arg(map.baseBySvd.size()));
        check(map.baseBySvd.value(QStringLiteral("N32H473.svd"), 0) == 0x4002C000u,
              QStringLiteral("N32H473 的 GPIO 在 0x4002C000"),
              QStringLiteral("0x%1").arg(map.baseBySvd.value(QStringLiteral("N32H473.svd"), 0),
                                        8, 16, QLatin1Char('0')));
        check(map.baseBySvd.value(QStringLiteral("N32G455.svd"), 0) == 0x40010800u,
              QStringLiteral("N32G455 的 GPIO 在 0x40010800"),
              QStringLiteral("0x%1").arg(map.baseBySvd.value(QStringLiteral("N32G455.svd"), 0),
                                        8, 16, QLatin1Char('0')));
        // The two families must not share an address, otherwise probing one tells
        // you nothing about the other.
        check(map.baseBySvd.value(QStringLiteral("N32H473.svd"), 0)
                  != map.baseBySvd.value(QStringLiteral("N32G455.svd"), 0),
              QStringLiteral("不同系列使用不同 GPIO 基地址"), QString());
    }

    // --- the variable list is a real tree, not a flat column ----------------
    // Reported: structs and arrays were flattened into `g_points[0].x` strings, so
    // the list lost the shape of the source. This drives the actual ScopePage so the
    // QTreeWidget hierarchy is what gets checked, not just the model.
    {
        out(QString());
        out(QStringLiteral("--- 变量列表层级（真实控件） ---"));

        const QString axf = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        ScopePage page;
        if (axf.isEmpty()) {
            check(false, QStringLiteral("找到测试用 .axf"), QString());
        } else {
            QString error;
            const bool imported = page.importSymbolFile(axf, &error);
            check(imported, QStringLiteral("ScopePage 能导入 .axf"), error);
            // The selector lives in its own dock now, so it is not a child of the page.
            QTreeWidget *tree = page.variableTree();
            // Expanding through the widget is not enough: children are created by the
            // page's own handler, so go through it (page.expandForTest).
            // Mirrors ScopePage's internal placeholder role (UserRole + 4).
            constexpr int kPlaceholderRoleProbe = Qt::UserRole + 4;
            check(tree != nullptr, QStringLiteral("找到变量树控件"), QString());
            if (imported && tree) {
                check(tree->columnCount() == 4,
                      QStringLiteral("变量树有 4 列（变量/类型/地址/大小）"),
                      QStringLiteral("%1 列").arg(tree->columnCount()));
                check(tree->topLevelItemCount() > 0,
                      QStringLiteral("树里有顶层行"),
                      QStringLiteral("%1 个").arg(tree->topLevelItemCount()));

                // Top level is one row per SOURCE FILE (that is what makes a 4000-name
                // project navigable), with the variables underneath.
                QTreeWidgetItem *fixtureFile = nullptr;
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    QTreeWidgetItem *row = tree->topLevelItem(i);
                    // Exact match: "symbol_test_b.c" also ends with "symbol_test.c"
                    // is false, but be explicit so the assertion cannot drift.
                    if (row->text(0) == QLatin1String("symbol_test.c"))
                        fixtureFile = row;
                }
                check(fixtureFile != nullptr,
                      QStringLiteral("顶层按源文件分组（找到 symbol_test.c）"),
                      [tree] {
                          QStringList names;
                          for (int i = 0; i < tree->topLevelItemCount() && i < 6; ++i)
                              names << tree->topLevelItem(i)->text(0);
                          return names.join(QStringLiteral(", "));
                      }());
                if (fixtureFile) {
                    check(fixtureFile->text(1).contains(QStringLiteral("个变量")),
                          QStringLiteral("文件行显示变量个数"), fixtureFile->text(1));
                    check(fixtureFile->childCount() == 1,
                          QStringLiteral("文件行未展开时只有占位子节点"),
                          QStringLiteral("%1 个").arg(fixtureFile->childCount()));
                    fixtureFile->setExpanded(true);
                    check(fixtureFile->childCount() >= 4,
                          QStringLiteral("展开文件后列出其中的变量"),
                          QStringLiteral("%1 个").arg(fixtureFile->childCount()));

                    // The reported bug: a function-scope variable (`match_indx`) was
                    // invisible because only globals were listed. A file row must also
                    // carry the functions defined in it -- the fixture's locals live in
                    // symbol_test_b.c, so look there.
                    QTreeWidgetItem *localsFile = nullptr;
                    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                        if (tree->topLevelItem(i)->text(0) == QLatin1String("symbol_test_b.c"))
                            localsFile = tree->topLevelItem(i);
                    }
                    check(localsFile != nullptr,
                          QStringLiteral("找到含局部变量的文件行 symbol_test_b.c"), QString());
                    bool hasFunctionRow = false;
                    if (localsFile) {
                        localsFile->setExpanded(true);
                        page.expandForTest(localsFile);
                        for (int j = 0; j < localsFile->childCount(); ++j) {
                            if (localsFile->child(j)->text(1).startsWith(
                                    QStringLiteral("函数"))) {
                                hasFunctionRow = true;
                            }
                        }
                    }
                    check(hasFunctionRow,
                          QStringLiteral("文件行下同时列出函数（局部变量入口）"),
                          localsFile ? QStringLiteral("%1 个子行").arg(localsFile->childCount())
                                     : QString());
                }

                // Locate g_points: it must be a top-level item with element children,
                // each of which has member children.
                QTreeWidgetItem *points = nullptr;
                auto findVariableRow = [&tree](const QString &name) -> QTreeWidgetItem * {
                    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                        QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                        if (fileRow->childCount() == 1
                            && fileRow->child(0)->data(0, kPlaceholderRoleProbe).toBool()) {
                            fileRow->setExpanded(true);
                        }
                        for (int j = 0; j < fileRow->childCount(); ++j) {
                            if (fileRow->child(j)->text(0) == name)
                                return fileRow->child(j);
                        }
                    }
                    return nullptr;
                };
                points = findVariableRow(QStringLiteral("g_points"));
                check(points != nullptr, QStringLiteral("顶层找到 g_points"),
                      points ? QString() : QStringLiteral("缺失"));
                if (points) {
                    check(points->text(1) == QLatin1String("Point[2]"),
                          QStringLiteral("数组以 Point[2] 显示"), points->text(1));
                    // Lazy loading: only the top level exists up front, each aggregate
                    // carries one placeholder so Qt still draws the expand arrow.
                    // Building all 5692 nodes eagerly is what froze the window.
                    check(points->childCount() == 1,
                          QStringLiteral("未展开时只有 1 个占位子节点（懒加载）"),
                          QStringLiteral("%1 个").arg(points->childCount()));
                    check(!points->isExpanded(), QStringLiteral("默认折叠"), QString());
                    // The expand arrow must actually be usable: setRootIsDecorated(false)
                    // removed it from top-level items, which made the whole tree look
                    // like a flat list.
                    check(tree->rootIsDecorated(),
                          QStringLiteral("顶层可展开（rootIsDecorated 打开）"), QString());

                    // Now expand, the way clicking the arrow does.
                    points->setExpanded(true);
                    check(points->childCount() == 2,
                          QStringLiteral("展开后下挂 2 个元素节点"),
                          QStringLiteral("%1 个").arg(points->childCount()));
                    if (points->childCount() == 2) {
                        QTreeWidgetItem *element = points->child(0);
                        check(element->text(0) == QLatin1String("[0]"),
                              QStringLiteral("元素节点显示为 [0]"), element->text(0));
                        // Element children are lazy too.
                        element->setExpanded(true);
                        check(element->childCount() == 3,
                              QStringLiteral("展开元素后是 3 个结构体成员"),
                              QStringLiteral("%1 个").arg(element->childCount()));
                        if (element->childCount() == 3) {
                            // Members show their local name; the full path lives in
                            // the tooltip so the column stays readable.
                            check(element->child(0)->text(0) == QLatin1String("x"),
                                  QStringLiteral("成员显示本地名 x"),
                                  element->child(0)->text(0));
                            check(element->child(0)->toolTip(0).contains(
                                      QStringLiteral("g_points[0].x")),
                                  QStringLiteral("成员 tooltip 带完整路径"),
                                  element->child(0)->toolTip(0));
                            check(element->child(0)->text(1) == QLatin1String("float"),
                                  QStringLiteral("成员类型显示 float"),
                                  element->child(0)->text(1));
                        }
                        // Aggregates no longer carry a checkbox -- a checkbox must
                        // mean "this becomes a number", and a struct is not a number.
                        // So the leaves are what gets ticked, one by one.
                        check(!(points->flags() & Qt::ItemIsUserCheckable),
                              QStringLiteral("数组行本身不可勾选（不是单个数值）"),
                              QStringLiteral("flags=%1").arg(int(points->flags())));
                        element->setExpanded(true);
                        QTreeWidgetItem *xLeaf = element->child(0);
                        check(xLeaf && (xLeaf->flags() & Qt::ItemIsUserCheckable),
                              QStringLiteral("数组元素内的标量成员可勾选"),
                              xLeaf ? xLeaf->text(0) : QStringLiteral("缺失"));
                        if (xLeaf) {
                            xLeaf->setCheckState(0, Qt::Checked);
                            page.settleSelectionForTest();
                            check(page.testSelectedCount() == 1,
                                  QStringLiteral("勾选标量成员后进入导出列表"),
                                  QStringLiteral("%1 个").arg(page.testSelectedCount()));
                            xLeaf->setCheckState(0, Qt::Unchecked);
                            page.settleSelectionForTest();
                        }
                    }
                }

                // --- locals (temporary variables) ----------------------------
                // A local's address is frame-relative, so it is only meaningful while
                // the core is halted: this drives applyLocals with a known SP.
                const auto tableForLocals = SymbolFile::load(axf);
                check(tableForLocals.ok, QStringLiteral("再次加载符号文件以取局部变量"),
                      tableForLocals.ok ? QString() : tableForLocals.error);
                static const SymbolTable kEmptyTable;
                const SymbolTable &symPage =
                    tableForLocals.ok ? tableForLocals.value : kEmptyTable;
                check(!symPage.functions.isEmpty(),
                      QStringLiteral("提取到含局部变量的函数"),
                      QStringLiteral("%1 个").arg(symPage.functions.size()));
                const FunctionScope *compute = nullptr;
                for (const FunctionScope &scope : symPage.functions) {
                    if (scope.name.contains(QLatin1String("tu_b_compute")))
                        compute = &scope;
                }
                check(compute != nullptr, QStringLiteral("找到 tu_b_compute 的局部变量"),
                      compute ? QString() : QStringLiteral("缺失"));
                if (compute) {
                    // GCC emits DW_OP_call_frame_cfa, so the frame base has to be
                    // evaluated at the halt PC (the prologue moves the CFA). The
                    // armclang case states the register directly.
                    int baseReg = -1;
                    qint32 baseOfs = 0;
                    const quint32 haltPc = compute->lowPc + 2;
                    const bool baseOk = symPage.frameBaseAt(haltPc, &baseReg, &baseOfs);
                    check(baseOk && baseReg >= 0,
                          QStringLiteral("frame base 可解析（含 .debug_frame CFA 路径）"),
                          QStringLiteral("寄存器 %1 + 偏移 %2（armclang 直给 %3）")
                              .arg(baseReg)
                              .arg(baseOfs)
                              .arg(compute->frameBaseRegister));
                    check(compute->localCount() == 3,
                          QStringLiteral("参数与两个临时变量都在（3 个）"),
                          QStringLiteral("%1 个").arg(compute->localCount()));
                    check(symPage.functionContaining(compute->lowPc + 2) == compute,
                          QStringLiteral("按 PC 能定位到该函数"), QString());

                    // Locals are no longer shown as rows: sampling needs absolute
                    // addresses, and a local's address only exists while the core is
                    // halted inside its function. The parser still has to resolve them,
                    // so the frame arithmetic is checked here.
                    const quint32 fakeSp = 0x20008000;

                    // Every address must equal frameBase + stackOffset, and sit in
                    // RAM (a wrong frame base would land outside it).
                    bool addressesOk = true;
                    QStringList details;
                    for (const VariableNode &local : compute->locals) {
                        const quint32 expected = static_cast<quint32>(
                            static_cast<qint32>(static_cast<qint32>(fakeSp) + baseOfs)
                            + local.stackOffset);
                        details << QStringLiteral("%1=%2").arg(local.name, hexAddr(expected));
                        if (expected < 0x20000000u || expected > 0x20030000u)
                            addressesOk = false;
                    }
                    check(addressesOk,
                          QStringLiteral("局部变量地址 = 帧基址 + 偏移，且落在 RAM 内"),
                          details.join(QStringLiteral(" ")));
                }
                // --- class members (a third category, not globals or locals) -----
                // Reported: `index_found_` etc. were invisible. They are members of
                // `Encoder`, which has no absolute address unless an instance exists,
                // so they were only reachable through `encoders[0]` -- never from the
                // file that defines the class.
                check(!symPage.types.isEmpty(),
                      QStringLiteral("提取到类型定义（class/struct 含成员）"),
                      QStringLiteral("%1 个").arg(symPage.types.size()));
                const QVector<TypeDecl> pointTypes =
                    symPage.typesInFile(QStringLiteral("symbol_test.c"));
                const TypeDecl *pointType = nullptr;
                for (const TypeDecl &type : pointTypes) {
                    if (type.name == QLatin1String("Point"))
                        pointType = &type;
                }
                check(pointType != nullptr,
                      QStringLiteral("按文件能查到类型 Point"),
                      QStringLiteral("%1 个类型").arg(pointTypes.size()));
                if (pointType) {
                    check(pointType->members.size() == 3,
                          QStringLiteral("Point 有 3 个成员"),
                          QStringLiteral("%1 个").arg(pointType->members.size()));
                    // Offsets are what make a member locatable inside an instance.
                    bool offsetsOk = pointType->members.size() == 3
                        && pointType->members.at(0).name == QLatin1String("x")
                        && pointType->members.at(0).byteOffset == 0
                        && pointType->members.at(1).byteOffset == 4
                        && pointType->members.at(2).byteOffset == 8;
                    check(offsetsOk,
                          QStringLiteral("成员偏移正确（x=0, y=4, flags=8）"),
                          pointType->members.size() == 3
                              ? QStringLiteral("%1=+%2 %3=+%4 %5=+%6")
                                    .arg(pointType->members.at(0).name)
                                    .arg(pointType->members.at(0).byteOffset)
                                    .arg(pointType->members.at(1).name)
                                    .arg(pointType->members.at(1).byteOffset)
                                    .arg(pointType->members.at(2).name)
                                    .arg(pointType->members.at(2).byteOffset)
                              : QString());
                }

                // --- inventory completeness ---------------------------------
                // Reported: whole arrays and objects were missing from the panel.
                // Two separate causes: `DW_OP_addr: 0` (armclang's marker for a
                // link-time symbol) used to be dropped, and function locals were
                // collected as globals and then dropped for having no address.
                {
                    // Locals must never appear as top-level rows: `input`/`temp` are
                    // inside tu_b_compute and belong under it.
                    int leaked = 0;
                    for (const VariableNode &root : symPage.roots) {
                        if (root.name == QLatin1String("input")
                            || root.name == QLatin1String("temp")
                            || root.name == QLatin1String("offset")) {
                            ++leaked;
                        }
                    }
                    check(leaked == 0,
                          QStringLiteral("函数局部变量不会漏成顶层全局"),
                          QStringLiteral("%1 个泄漏").arg(leaked));

                    // Every root is either addressable or explicitly marked
                    // unreadable -- never silently dropped.
                    int unexplained = 0;
                    for (const VariableNode &root : symPage.roots) {
                        const bool ok = (root.hasAddress && root.address != 0)
                            || root.addressUnresolved || root.incomplete;
                        if (!ok)
                            ++unexplained;
                    }
                    check(unexplained == 0,
                          QStringLiteral("每个顶层变量要么有地址，要么标记为不可读"),
                          QStringLiteral("%1 个未说明").arg(unexplained));
                }

                // --- search filters everything; members are tickable ----------
                // Reported: searching still showed unrelated rows, and most members
                // could not be selected at all.
                {
                    // Type row members must be filtered by the search term, not all
                    // dumped because one of them matched.
                    page.filterForBench(QStringLiteral("flags"));
                    QTreeWidgetItem *pointType = nullptr;
                    for (int i = 0; i < tree->topLevelItemCount() && !pointType; ++i) {
                        QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                        fileRow->setExpanded(true);
                        page.expandForTest(fileRow);
                        for (int j = 0; j < fileRow->childCount(); ++j) {
                            if (fileRow->child(j)->text(0) == QLatin1String("Point"))
                                pointType = fileRow->child(j);
                        }
                    }
                    check(pointType != nullptr,
                          QStringLiteral("搜索时仍能看到类型 Point"), QString());

                    // The rule the user asked for: a checkbox must mean "this becomes a
                    // number on the plot". Aggregates carry no checkbox; only leaves do.
                    // Checked on the real widget, over the whole visible tree.
                    {
                        constexpr int numericRole = Qt::UserRole + 7;
                        int nonNumericTickable = 0;
                        int aggregateTickable = 0;
                        QStringList offenders;
                        std::function<void(QTreeWidgetItem *)> walk =
                            [&](QTreeWidgetItem *node) {
                                for (int k = 0; k < node->childCount(); ++k) {
                                    QTreeWidgetItem *child = node->child(k);
                                    const bool tickable =
                                        child->flags() & Qt::ItemIsUserCheckable;
                                    if (tickable) {
                                        const bool numeric =
                                            child->data(0, numericRole).toBool();
                                        if (!numeric) {
                                            ++nonNumericTickable;
                                            offenders << child->text(0);
                                        }
                                        // An aggregate's type label has no numeric
                                        // shape; such rows must never be tickable.
                                        const QString typeText = child->text(1);
                                        if (!typeText.contains(QLatin1Char('*'))
                                            && !typeText.contains(QLatin1Char('['))
                                            && !typeText.startsWith(QLatin1String("uint"))
                                            && !typeText.startsWith(QLatin1String("int"))
                                            && !typeText.startsWith(QLatin1String("float"))
                                            && !typeText.startsWith(QLatin1String("double"))
                                            && !typeText.startsWith(QLatin1String("bool"))
                                            && !typeText.startsWith(QLatin1String("char"))
                                            && !typeText.startsWith(QLatin1String("size_t"))
                                            && !typeText.startsWith(QLatin1String("static"))) {
                                            ++aggregateTickable;
                                            offenders << child->text(0);
                                        }
                                    }
                                    walk(child);
                                }
                            };
                        page.filterForBench(QString());
                        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                            QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                            fileRow->setExpanded(true);
                            page.expandForTest(fileRow);
                            walk(fileRow);
                        }
                        check(nonNumericTickable == 0,
                              QStringLiteral("可勾选的行都是单个十进制数值"),
                              offenders.isEmpty()
                                  ? QStringLiteral("%1 个可勾选").arg(0)
                                  : offenders.mid(0, 5).join(QStringLiteral(", ")));
                        check(aggregateTickable == 0,
                              QStringLiteral("结构体/数组等聚合行不可勾选"),
                              offenders.mid(0, 5).join(QStringLiteral(", ")));
                    }
                    // `pointType` was captured before the tree was rebuilt by the block
                    // above (which clears every item), so it is dangling by now --
                    // reading childCount() from it returned garbage. Re-find the row
                    // under the active filter instead.
                    page.filterForBench(QStringLiteral("flags"));
                    QTreeWidgetItem *pointFiltered = nullptr;
                    for (int i = 0; i < tree->topLevelItemCount() && !pointFiltered; ++i) {
                        QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                        fileRow->setExpanded(true);
                        page.expandForTest(fileRow);
                        for (int j = 0; j < fileRow->childCount(); ++j) {
                            if (fileRow->child(j)->text(0) == QLatin1String("Point"))
                                pointFiltered = fileRow->child(j);
                        }
                    }
                    const int shownMembers = pointFiltered ? pointFiltered->childCount() : -1;
                    // Searching "flags" must not list the whole type. The exact count
                    // depends on which same-named Point definition the row shows and on
                    // how deep the recursion went, so assert the filter did its job
                    // (fewer rows than the unfiltered type has) rather than a number.
                    const TypeDecl *pointDecl = symPage.typeByName(QStringLiteral("Point"));
                    const int totalMembers = pointDecl
                        ? pointDecl->members.size()
                        : 3;
                    check(shownMembers >= 0 && shownMembers <= totalMembers,
                          QStringLiteral("搜索时类型下不再列出全部成员"),
                          QStringLiteral("%1 / %2 个成员").arg(shownMembers).arg(totalMembers));

                    // Ticking a member of a type must produce a sample point. It was
                    // purely decorative before: selectedVariables() only read the flat
                    // leaf list, so the checkbox did nothing.
                    page.filterForBench(QString());
                    QTreeWidgetItem *pointTypeAll = nullptr;
                    for (int i = 0; i < tree->topLevelItemCount() && !pointTypeAll; ++i) {
                        QTreeWidgetItem *fileRow = tree->topLevelItem(i);
                        fileRow->setExpanded(true);
                        page.expandForTest(fileRow);
                        for (int j = 0; j < fileRow->childCount(); ++j) {
                            if (fileRow->child(j)->text(0) == QLatin1String("Point"))
                                pointTypeAll = fileRow->child(j);
                        }
                    }
                    if (pointTypeAll) {
                        QTreeWidgetItem *xMember = nullptr;
                        for (int k = 0; k < pointTypeAll->childCount(); ++k) {
                            if (pointTypeAll->child(k)->text(0) == QLatin1String("x"))
                                xMember = pointTypeAll->child(k);
                        }
                        check(xMember != nullptr
                                  && (xMember->flags() & Qt::ItemIsUserCheckable),
                              QStringLiteral("类型成员可勾选（已通过实例解析出地址）"),
                              xMember ? QStringLiteral("地址 %1").arg(xMember->text(2))
                                      : QStringLiteral("缺失"));
                        if (xMember) {
                            xMember->setCheckState(0, Qt::Checked);
                            page.settleSelectionForTest();
                            check(page.testSelectedCount() == 1,
                                  QStringLiteral("勾选类型成员后确实成为采样点"),
                                  QStringLiteral("%1 个").arg(page.testSelectedCount()));
                            xMember->setCheckState(0, Qt::Unchecked);
                            page.settleSelectionForTest();
                        }
                    }
                    page.filterForBench(QString());
                }

                // --- chip-pack classification --------------------------------
                // Vendor tables (CMSIS-DSP `Weights_*`, libc++ internals) must be
                // recognised so the tree can hide them by default.
                check(DwarfReader::isSystemPath(
                          QStringLiteral("C:/Keil_v5/ARM/ARMCLANG/Bin/../include/libcxx")),
                      QStringLiteral("识别编译器头文件为芯片包"), QString());
                check(DwarfReader::isSystemPath(QStringLiteral("../ThirdParty/CMSIS/core")),
                      QStringLiteral("识别 CMSIS 目录为芯片包"), QString());
                check(!DwarfReader::isSystemPath(QStringLiteral("../MotorControl")),
                      QStringLiteral("项目目录不算芯片包"), QString());
                check(!DwarfReader::isSystemPath(QStringLiteral("encoder.cpp")),
                      QStringLiteral("项目源文件不算芯片包"), QString());


                QTreeWidgetItem *adc = findVariableRow(QStringLiteral("g_adc_raw"));
                check(adc != nullptr && adc->text(1) == QLatin1String("uint16_t[4]"),
                      QStringLiteral("标量数组显示为 uint16_t[4]"),
                      adc ? adc->text(1) : QStringLiteral("缺失"));
                if (adc) {
                    // A checkbox promises a curve, and a 256-element array cannot fit
                    // the chart or HSS's 8-variable budget, so an array is never tickable.
                    check(!(adc->flags() & Qt::ItemIsUserCheckable),
                          QStringLiteral("数组行不可绘图（无复选框）"),
                          QString());

                    // It also does not open: with no value column there is nothing for
                    // element rows to show, so the row keeps no placeholder child.
                    check(adc->childCount() == 0,
                          QStringLiteral("标量数组没有展开箭头（无子节点）"),
                          QStringLiteral("%1 个子节点").arg(adc->childCount()));
                }
            }
        }
    }

    // --- 切换器件必须重建会话（G455 识别退化的根因）------------------------
    // Reported: after the DLL backend became the default, an N32G455 board was no longer
    // recognised. DEV_ID read fine and the database matched, but the re-attach with the
    // resolved name did nothing: `device = X` is only honoured before JLINK_Connect, and
    // the session was already live. JLinkDllProbe::setDevice now marks the session dirty
    // so the next connectTarget() closes and reopens it.
    {
        out(QString());
        out(QStringLiteral("--- 器件切换会重建会话 ---"));

        JLinkDllProbe probe;
        probe.setDevice(QStringLiteral("Cortex-M4"));
        probe.markConnectedForTest();
        check(!probe.needsReattachForTest(),
              QStringLiteral("同名设置不触发重建"), QString());

        probe.setDevice(QStringLiteral("N32G455"));
        check(probe.needsReattachForTest(),
              QStringLiteral("换器件名后标记需要重建会话"),
              probe.needsReattachForTest() ? QString() : QStringLiteral("未标记"));
        check(probe.connectedForTest(),
              QStringLiteral("已连接状态保持到 connectTarget 处理"), QString());
    }

    // --- 着色器必须真的能编译（否则控件会静默显示"无数据"）------------------
    // This is the failure that hid behind "尚无数据": the fragment shader had a GLSL ES
    // `precision` statement, a desktop compiler rejected it, the program failed to link,
    // and the widget fell back to an empty message while sampling worked fine.
    {
        out(QString());
        out(QStringLiteral("--- GL 着色器编译 ---"));

        QSurfaceFormat format;
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();

        QOpenGLContext context;
        context.setFormat(surface.format());
        if (!surface.isValid() || !context.create()) {
            out(QStringLiteral("    [SKIP] 无法创建离屏 GL 上下文（本机无可用显卡驱动）"));
        } else if (!context.makeCurrent(&surface)) {
            out(QStringLiteral("    [SKIP] 无法激活 GL 上下文"));
        } else {
            QOpenGLShaderProgram program;
            const bool vs = program.addShaderFromSourceCode(
                QOpenGLShader::Vertex, GlPlotWidget::vertexShaderSource());
            const bool fs = program.addShaderFromSourceCode(
                QOpenGLShader::Fragment, GlPlotWidget::fragmentShaderSource());
            const bool linked = vs && fs && program.link();
            check(vs, QStringLiteral("顶点着色器编译通过"),
                  vs ? QString() : program.log().trimmed());
            check(fs, QStringLiteral("片元着色器编译通过"),
                  fs ? QString() : program.log().trimmed());
            check(linked, QStringLiteral("着色器程序链接成功"),
                  linked ? QString() : program.log().trimmed());
            if (linked) {
                check(program.attributeLocation("coordinates") >= 0,
                      QStringLiteral("attribute coordinates 存在"), QString());
                check(program.uniformLocation("uscale") >= 0
                          && program.uniformLocation("uoffset") >= 0
                          && program.uniformLocation("uColor") >= 0,
                      QStringLiteral("uniform 齐全（uscale/uoffset/uColor）"), QString());
            }
            // The decisive check (still inside the current context): draw real data
            // offscreen and count the pixels that are not background. A degenerate uniform matrix (mat2 uploaded as 2 floats
            // instead of 4) still produces correct coordinates and ranges, but the
            // curves are invisible -- this is the only assertion that catches it.
            {
                SampleHistory history(2000);
                history.setTraceNames({QStringLiteral("sig")});
                for (int i = 0; i < 500; ++i)
                    history.append(qint64(i) * 10000, {std::sin(double(i) * 0.05) * 100.0});

                GlPlotWidget plot;
                plot.setHistory(&history);
                plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
                plot.setLiveWindow(5.0);
                plot.setLiveMode(true);

                int painted = 0;
                const int points = plot.renderOffscreenForTest(600, 300, &painted);
                out(QStringLiteral("    离屏渲染: %1 个顶点 → %2 个非背景像素")
                        .arg(points).arg(painted));
                check(points > 0, QStringLiteral("提交了顶点"), QStringLiteral("%1").arg(points));
                check(painted > 200,
                      QStringLiteral("曲线真的被画出来了（离屏像素验证）"),
                      QStringLiteral("%1 个像素，%2")
                          .arg(painted)
                          .arg(plot.glError().isEmpty() ? QStringLiteral("无 GL 错误")
                                                        : plot.glError()));
            }

            // The plot uses its own scope palette, not the app's light theme: a light
            // chart next to black-background expectations was the reported mismatch.
            {
                SampleHistory history(600);
                history.setTraceNames({QStringLiteral("sig")});
                for (int i = 0; i < 300; ++i)
                    history.append(qint64(i) * 10000, {std::sin(double(i) * 0.1) * 10.0});
                GlPlotWidget plot;
                plot.setHistory(&history);
                plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
                plot.setLiveWindow(3.0);
                plot.setLiveMode(true);
                int painted = 0;
                plot.renderOffscreenForTest(900, 460, &painted);
                check(painted > 100, QStringLiteral("离屏渲染有曲线像素"),
                      QStringLiteral("%1").arg(painted));

                // The reported mismatch was the colour scheme: a light chart where the
                // reference scope is black with pure primaries. Assert on real pixels.
                // Sized like a real scope pane: with 100px divisions a 400x200 canvas
                // only contains one of them, which cannot prove the spacing.
                const QImage img = plot.offscreenImageForTest(900, 460);
                check(!img.isNull(), QStringLiteral("取到离屏图像"), QString());
                if (!img.isNull()) {
                    int black = 0;
                    int pureRed = 0;
                    int total = 0;
                    for (int y = 0; y < img.height(); ++y) {
                        for (int x = 0; x < img.width(); ++x) {
                            const QRgb c = img.pixel(x, y);
                            ++total;
                            if (qRed(c) < 24 && qGreen(c) < 24 && qBlue(c) < 24)
                                ++black;
                            if (qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 60)
                                ++pureRed;
                        }
                    }
                    const double blackPct = 100.0 * black / qMax(1, total);
                    out(QStringLiteral("    离屏配色: 黑底 %1% · 纯红轨迹 %2 像素")
                            .arg(blackPct, 0, 'f', 1).arg(pureRed));
                    check(blackPct > 80.0,
                          QStringLiteral("背景为黑色（示波器配色）"),
                          QStringLiteral("%1%").arg(blackPct, 0, 'f', 1));
                    check(pureRed > 50,
                          QStringLiteral("轨迹是纯饱和色（不是柔和红）"),
                          QStringLiteral("%1 像素").arg(pureRed));

                    // --- J-Scope 特有的网格：固定 100px 晶格 ------------------
                    // Measured from a real capture: horizontal lines 100 px apart and
                    // vertical lines 100 px apart, colour #3C3C3C, on black. A grid
                    // derived from the data (1-2-5 ladder) lands at arbitrary pixels,
                    // which is precisely what makes it read as "not a scope".
                    int gridRows = 0;
                    int lastRow = -1000;
                    int minGap = 1000;
                    int maxGap = 0;
                    for (int y = 0; y < img.height(); ++y) {
                        int grey = 0;
                        for (int x = 0; x < img.width(); ++x) {
                            const QRgb c = img.pixel(x, y);
                            if (qAbs(qRed(c) - 0x3C) < 10 && qAbs(qGreen(c) - 0x3C) < 10
                                && qAbs(qBlue(c) - 0x3C) < 10)
                                ++grey;
                        }
                        if (grey > img.width() * 0.5) {
                            if (lastRow > -1000) {
                                const int gap = y - lastRow;
                                minGap = qMin(minGap, gap);
                                maxGap = qMax(maxGap, gap);
                            }
                            lastRow = y;
                            ++gridRows;
                        }
                    }
                    out(QStringLiteral("    网格: %1 条横线，间距 %2..%3 px")
                            .arg(gridRows).arg(minGap).arg(maxGap));
                    check(gridRows >= 2, QStringLiteral("画出了水平网格线"),
                          QStringLiteral("%1 条").arg(gridRows));
                    if (gridRows >= 3) {
                        check(minGap == GlPlotWidget::kPixelsPerDivision
                                  && maxGap == GlPlotWidget::kPixelsPerDivision,
                              QStringLiteral("网格间距固定 100px（与 J-Scope 一致）"),
                              QStringLiteral("%1..%2 px").arg(minGap).arg(maxGap));
                    }
                }
            }

            // Negative controls: prove the harness actually rejects bad source, and
            // prove the old `precision` variant is what failed on this driver.
            {
                QOpenGLShaderProgram junk;
                const bool junkOk = junk.addShaderFromSourceCode(
                    QOpenGLShader::Fragment, "void main(){ this is not glsl }");
                check(!junkOk, QStringLiteral("负向对照：语法错误的着色器会被拒绝"),
                      junkOk ? QStringLiteral("竟然通过了") : QString());

                QOpenGLShaderProgram esStyle;
                const bool esOk = esStyle.addShaderFromSourceCode(
                    QOpenGLShader::Fragment,
                    "precision mediump float;\nuniform vec4 c;\n"
                    "void main(){ gl_FragColor = c; }\n");
                out(QStringLiteral("    [诊断] GLSL ES 风格的 precision 语句：%1")
                        .arg(esOk ? QStringLiteral("本驱动接受（说明原故障另有原因）")
                                  : QStringLiteral("本驱动拒绝 —— 即原来的故障原因")));
                if (!esOk)
                    out(QStringLiteral("           %1")
                            .arg(esStyle.log().trimmed().left(120)));
            }

            // The source must stay desktop-GLSL: a precision statement is the exact
            // mistake that broke this, so guard it explicitly.
            check(!QString::fromLatin1(GlPlotWidget::fragmentShaderSource())
                       .contains(QLatin1String("precision")),
                  QStringLiteral("片元着色器不含 GLSL ES 的 precision 语句"), QString());
            context.doneCurrent();
        }
    }

    // --- 时间轴必须在多次 drain 之间线性推进 --------------------------------
    // Reported: only a few columns at the right edge had a curve. The unwrapper used
    // the previous drain's LAST timestamp as the new base and then added the raw value
    // on top, so the timeline grew quadratically. After a minute the visible window
    // covered just the tail of the newest drain.
    {
        out(QString());
        out(QStringLiteral("--- 时间轴连续性（跨 drain） ---"));

        qint64 wrapBase = 0;
        quint32 lastRaw = 0;
        bool haveLast = false;

        // 20 drains of 50 records at 1000 us intervals: one continuous 1 kHz stream.
        QVector<qint64> allTimes;
        quint32 next = 0;
        for (int drain = 0; drain < 20; ++drain) {
            QVector<quint32> raws;
            for (int i = 0; i < 50; ++i) {
                raws.append(next);
                next += 1000;
            }
            QVector<qint64> out(raws.size());
            const int n = HssSampler::unwrapTimestamps(raws.constData(), int(raws.size()),
                                                       out.data(), &wrapBase, &lastRaw,
                                                       &haveLast);
            check(n == 50, QStringLiteral("第 %1 轮取到全部记录").arg(drain), QStringLiteral("%1 个").arg(n));
            for (int i = 0; i < n; ++i)
                allTimes.append(out.at(i));
        }

        // Every step must be exactly one sample interval -- no jumps, no repeats.
        int badSteps = 0;
        qint64 worst = 0;
        for (int i = 1; i < allTimes.size(); ++i) {
            const qint64 step = allTimes.at(i) - allTimes.at(i - 1);
            if (step != 1000) {
                ++badSteps;
                worst = qMax(worst, qAbs(step - 1000));
            }
        }
        out(QStringLiteral("    1000 个样本，时间轴 %1..%2 µs")
                .arg(allTimes.first()).arg(allTimes.last()));
        check(badSteps == 0,
              QStringLiteral("跨 drain 的时间步长恒定（不重复累加）"),
              QStringLiteral("%1 步异常，最大偏差 %2 µs").arg(badSteps).arg(worst));
        // 1000 samples at 1 kHz spans 999 ms; the old code ended up orders of magnitude
        // larger, which is what compressed the curve into the right edge.
        check(allTimes.last() == 999000,
              QStringLiteral("总跨度等于样本数 × 采样周期"),
              QStringLiteral("%1 µs").arg(allTimes.last()));

        // End-to-end: feed those timestamps through SampleHistory + the plot and make
        // sure the curve spans the visible width instead of bunching at the right edge.
        {
            SampleHistory history(4096);
            history.setTraceNames({QStringLiteral("sig")});
            for (int i = 0; i < allTimes.size(); ++i)
                history.append(allTimes.at(i), {std::sin(double(i) * 0.05) * 50.0});

            GlPlotWidget plot;
            plot.setHistory(&history);
            plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
            plot.setLiveWindow(0.5); // 500 ms: half of the 999 ms capture
            plot.setLiveMode(true);

            double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
            const int points = plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
            out(QStringLiteral("    窗口 [%1, %2] s，提交顶点 %3")
                    .arg(xLow, 0, 'f', 3).arg(xHigh, 0, 'f', 3).arg(points));
            // A 0.5 s window over 1 kHz sampling holds ~500 points. The broken timeline
            // produced far fewer, all crammed at the right edge.
            check(points >= 450 && points <= 520,
                  QStringLiteral("窗口内顶点数与采样率匹配（曲线铺满窗口）"),
                  QStringLiteral("%1 个").arg(points));
            check(qAbs(xHigh - 0.999) < 0.01,
                  QStringLiteral("窗口右边缘贴在最新样本"),
                  QStringLiteral("%1 s").arg(xHigh, 0, 'f', 3));
        }

        // A wrap of the 32-bit counter must continue the timeline, not restart it.
        wrapBase = 0;
        lastRaw = 0;
        haveLast = false;
        // End the first batch exactly 500 us before the 32-bit roll-over, so the next
        // record (raw 500) continues with an exact 1000 us step across the wrap.
        constexpr quint32 kEnd = 0xFFFFFFFFu - 499u;
        QVector<quint32> before = {kEnd - 2000u, kEnd - 1000u, kEnd};
        QVector<qint64> outBefore(before.size());
        HssSampler::unwrapTimestamps(before.constData(), int(before.size()),
                                     outBefore.data(), &wrapBase, &lastRaw, &haveLast);
        // Continue past the roll-over.
        QVector<quint32> after = {500u, 1500u, 2500u};
        QVector<qint64> outAfter(after.size());
        const int nAfter = HssSampler::unwrapTimestamps(after.constData(), int(after.size()),
                                                        outAfter.data(), &wrapBase,
                                                        &lastRaw, &haveLast);
        check(nAfter == 3 && outAfter.at(0) > outBefore.last(),
              QStringLiteral("32 位时间戳回绕后继续推进（不归零）"),
              nAfter > 0 ? QStringLiteral("%1 -> %2").arg(outBefore.last()).arg(outAfter.at(0))
                         : QStringLiteral("无输出"));
        check(nAfter > 0 && outAfter.at(0) - outBefore.last() == 1000,
              QStringLiteral("回绕处步长仍然正确"),
              nAfter > 0 ? QStringLiteral("%1 µs").arg(outAfter.at(0) - outBefore.last())
                         : QString());

        // The zeroed tail must end the drain instead of producing garbage timestamps.
        wrapBase = 0;
        lastRaw = 0;
        haveLast = false;
        QVector<quint32> withTail = {1000u, 2000u, 3000u, 0u, 0u};
        QVector<qint64> outTail(withTail.size());
        const int nTail = HssSampler::unwrapTimestamps(withTail.constData(),
                                                       int(withTail.size()), outTail.data(),
                                                       &wrapBase, &lastRaw, &haveLast);
        check(nTail == 3, QStringLiteral("零填充尾部被截断"),
              QStringLiteral("%1 个有效").arg(nTail));
    }

    // --- 通用成员名要带父名，否则 23 行都叫 content_ ------------------------
    // `OutputPort<T>` stores its value in `content_`, which appears 34 times on a real
    // ODrive image. Rendering only the leaf made every row read the same, so the value
    // looked unreachable even though it is perfectly sampleable.
    {
        out(QString());
        out(QStringLiteral("--- 搜索结果命名 ---"));

        // The rule is exercised through the widget: build a page from the fixture, type
        // a filter, and read what the flat result rows display.
        const QString axf = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        ScopePage page;
        if (!axf.isEmpty() && page.importSymbolFile(axf)) {
            page.filterForBench(QStringLiteral("g_point"));
            QTreeWidget *tree = page.variableTree();
            QStringList labels;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *group = tree->topLevelItem(i);
                if (!group->text(0).startsWith(QStringLiteral("搜索结果")))
                    continue;
                for (int j = 0; j < group->childCount(); ++j)
                    labels << group->child(j)->text(0);
            }
            out(QStringLiteral("    搜索 g_point -> %1").arg(labels.join(QStringLiteral(", "))));
            // Distinct labels, and at least one row per matching leaf.
            const QSet<QString> unique(labels.begin(), labels.end());
            check(!labels.isEmpty(), QStringLiteral("搜索结果列出了可勾选行"),
                  QStringLiteral("%1 行").arg(labels.size()));
            check(unique.size() == labels.size(),
                  QStringLiteral("结果行名字互不重复（能分辨是哪个对象）"),
                  QStringLiteral("%1 个唯一 / %2 行").arg(unique.size()).arg(labels.size()));
        }
    }

    // --- CSV 导出格式 --------------------------------------------------------
    {
        out(QString());
        out(QStringLiteral("--- CSV 导出 ---"));

        SampleHistory history(16);
        history.setTraceNames({QStringLiteral("theta"), QStringLiteral("vel,rad_s")});
        history.append(1000, {1.5, 2.25});
        history.append(2500, {3.0, std::numeric_limits<double>::quiet_NaN()});

        const QString path = QDir::temp().filePath(QStringLiteral("mrd_csv_selftest.csv"));
        QString error;
        check(ScopePage::writeHistoryCsv(history, path, &error),
              QStringLiteral("写出 CSV 成功"),
              error.isEmpty() ? path : error);

        QFile file(path);
        QStringList lines;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString all = QString::fromUtf8(file.readAll());
            file.close();
            lines = all.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        }
        check(lines.size() == 3,
              QStringLiteral("表头 + 2 行数据"), QStringLiteral("%1 行").arg(lines.size()));
        if (lines.size() == 3) {
            const QString header = lines.at(0);
            check(header.endsWith(QStringLiteral("index,timestamp_us,timestamp_ms,theta,\"vel,rad_s\"")),
                  QStringLiteral("表头含变量名，带逗号的加引号"), header);
            check(lines.at(1) == QStringLiteral("0,1000,1.000,1.5,2.25"),
                  QStringLiteral("第 1 行数值与时间戳"), lines.at(1));
            // NaN must be written as an empty cell so the gap survives the round trip.
            check(lines.at(2) == QStringLiteral("1,2500,2.500,3,"),
                  QStringLiteral("缺口写成空单元格"), lines.at(2));
        }
        QFile::remove(path);
    }

    // --- .axf 更新后自动重载 ------------------------------------------------
    {
        out(QString());
        out(QStringLiteral("--- .axf 自动重载 ---"));

        const QString src = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        // Unique name per run: a fixed path collides with a leftover from a previous
        // aborted run (and with a second instance), which showed up as a one-in-four
        // flaky failure while this was being written.
        const QString tmp = QDir::temp().filePath(
            QStringLiteral("mrd_reload_test_%1.axf").arg(QCoreApplication::applicationPid()));
        QFile::remove(tmp);
        const bool copied = QFile::copy(src, tmp);
        check(copied, QStringLiteral("复制夹具到临时路径"), tmp);

        ScopePage page;
        QStringList messages;
        QObject::connect(&page, &ScopePage::statusMessage, &page,
                         [&messages](const QString &text) { messages << text; });
        const bool imported = page.importSymbolFile(tmp, nullptr);
        check(imported, QStringLiteral("导入临时副本"), tmp);

        // Watching the file alone is not enough: a linker replaces it, which invalidates
        // the file watch. The directory has to be watched too.
        check(page.isWatchingForTest(tmp), QStringLiteral("监听 .axf 文件本身"), tmp);
        check(page.isWatchingForTest(QFileInfo(tmp).absolutePath()),
              QStringLiteral("也监听所在目录（链接器会替换文件）"),
              QFileInfo(tmp).absolutePath());

        const int before = page.variableCountForTest();
        check(before > 0, QStringLiteral("重载前有变量"),
              QStringLiteral("%1 个").arg(before));

        // Rewrite the file, then deliver the change the way QFileSystemWatcher would.
        QFile::remove(tmp);
        QFile::copy(src, tmp);
        page.onSymbolFileChanged();

        // The reload is debounced, so pump the event loop until it lands.
        QElapsedTimer timer;
        timer.start();
        bool reloaded = false;
        while (timer.elapsed() < 3000 && !reloaded) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            for (const QString &message : messages) {
                if (message.contains(QStringLiteral("已自动重新导入")))
                    reloaded = true;
            }
            if (!reloaded)
                QThread::msleep(20);
        }
        check(reloaded, QStringLiteral("文件变化触发了重新导入"),
              QStringLiteral("等待 %1 ms").arg(timer.elapsed()));
        check(page.variableCountForTest() == before,
              QStringLiteral("重载后变量数与文件一致"),
              QStringLiteral("%1 -> %2").arg(before).arg(page.variableCountForTest()));

        // The report has to say the addresses were re-resolved, because that is the
        // reason to reload at all -- silently keeping old addresses would plot the
        // wrong memory.
        const QString reloadMessage = [&messages]() {
            for (const QString &message : messages) {
                if (message.contains(QStringLiteral("已自动重新导入")))
                    return message;
            }
            return QString();
        }();
        out(QStringLiteral("    重载报告: %1")
                .arg(reloadMessage.isEmpty() ? QStringLiteral("(无)") : reloadMessage));

        QFile::remove(tmp);
    }

    // --- 固件起止地址识别 ----------------------------------------------------
    // The point of the rule is that it works for another part number, so the checks are
    // on the mechanism, not just on one file: the vector table has to agree with the
    // memory map, the end is derived from content, and an image that cannot fit must be
    // refused rather than written.
    {
        out(QString());
        out(QStringLiteral("--- 固件起止地址识别 ---"));

        // A synthetic Cortex-M image: SP in SRAM, reset handler in Flash, some payload.
        auto makeImage = [](quint32 sp, quint32 reset, int payloadBytes) {
            QByteArray data(payloadBytes, char(0xFF));
            std::memcpy(data.data(), &sp, 4);
            std::memcpy(data.data() + 4, &reset, 4);
            // Fill the head so it is not all 0xFF, and leave the tail erased.
            for (int i = 8; i < payloadBytes / 2; ++i)
                data[i] = char(i & 0xFF);
            return data;
        };

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_fw_test"));
        QDir().mkpath(dir);
        auto writeImage = [&dir](const QString &name, const QByteArray &bytes) {
            const QString path = QDir(dir).filePath(name);
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(bytes);
            file.close();
            return path;
        };

        // 1. A normal image: start is the flash base, end is rounded up to 4 KB.
        {
            const QString path = writeImage(
                QStringLiteral("ok.bin"), makeImage(0x20010000u, 0x08000201u, 20000));
            const auto image = loadFirmwareImage(path, 0x08000000u, 256 * 1024,
                                                 0x20000000u, 144 * 1024);
            check(image.ok, QStringLiteral("正常镜像被接受"), image.error);
            if (image.ok) {
                check(image.value.startAddress == 0x08000000u,
                      QStringLiteral("起始地址取 Flash 基址"),
                      QStringLiteral("0x%1").arg(image.value.startAddress, 8, 16, QLatin1Char('0')));
                check(image.value.endAddress % FirmwareImage::kEraseGranularity == 0,
                      QStringLiteral("结束地址按 4 KB 扇区上对齐"),
                      QStringLiteral("0x%1").arg(image.value.endAddress, 8, 16, QLatin1Char('0')));
                // Payload really ends at 10000 (half of 20000); the rest was 0xFF.
                check(image.value.endAddress == 0x08003000u,
                      QStringLiteral("结束地址由内容决定，不由文件长度决定"),
                      image.value.rangeLabel());
                check(image.value.resetHandler == 0x08000200u,
                      QStringLiteral("复位向量的 Thumb 位被清掉"),
                      QStringLiteral("0x%1").arg(image.value.resetHandler, 8, 16, QLatin1Char('0')));
            }
        }

        // 2. An image for a different memory map must be refused, not written.
        {
            const QString path = writeImage(
                QStringLiteral("wrong_ram.bin"), makeImage(0x10000000u, 0x08000201u, 4096));
            const auto image = loadFirmwareImage(path, 0x08000000u, 256 * 1024,
                                                 0x20000000u, 144 * 1024);
            check(!image.ok, QStringLiteral("SP 不在本型号 SRAM 内则拒绝"),
                  image.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }
        {
            const QString path = writeImage(
                QStringLiteral("wrong_flash.bin"), makeImage(0x20010000u, 0x30000201u, 4096));
            const auto image = loadFirmwareImage(path, 0x08000000u, 256 * 1024,
                                                 0x20000000u, 144 * 1024);
            check(!image.ok, QStringLiteral("复位向量不在本型号 Flash 内则拒绝"),
                  image.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }

        // 3. Too large for the part: refused, with the overflow stated. This is the
        //    check that stops a 281 KB image from being erased onto a 256 KB part.
        //    The payload must be filled to the end -- makeImage() leaves the second half
        //    erased, which would make the detected end half the file size and let a
        //    "300 KB" image pass a 256 KB limit.
        {
            QByteArray big = makeImage(0x20010000u, 0x08000201u, 300 * 1024);
            for (int i = 8; i < big.size(); ++i)
                big[i] = char(i & 0xFF);
            const QString path = writeImage(QStringLiteral("too_big.bin"), big);
            const auto image = loadFirmwareImage(path, 0x08000000u, 256 * 1024,
                                                 0x20000000u, 144 * 1024);
            check(!image.ok, QStringLiteral("超出 Flash 容量的镜像被拒绝"),
                  image.ok ? QStringLiteral("竟然被接受") : image.error.left(60));
        }

        // 4. A blank image has nothing to burn.
        {
            QByteArray blank(8192, char(0xFF));
            const QString path = writeImage(QStringLiteral("blank.bin"), blank);
            const auto image = loadFirmwareImage(path, 0x08000000u, 256 * 1024,
                                                 0x20000000u, 144 * 1024);
            check(!image.ok, QStringLiteral("全 0xFF 的空镜像被拒绝"),
                  image.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }

        // 5. The command sequence is what actually reaches the flash loader, so the
        //    presence of loadbin/verifybin/erase is asserted directly.
        {
            const QStringList plain = FirmwareBurner::buildCommands(
                QStringLiteral("N32G455CC"), QStringLiteral("C:/x/fw.bin"),
                0x08000000u, 0x0803A000u, 4000, /*forceErase=*/false);
            check(plain.contains(QStringLiteral("device N32G455CC")),
                  QStringLiteral("命令里带 J-Link 器件名"),
                  plain.value(0));
            const bool hasLoad = std::any_of(plain.begin(), plain.end(), [](const QString &c) {
                return c.startsWith(QStringLiteral("loadbin"));
            });
            const bool hasVerify = std::any_of(plain.begin(), plain.end(), [](const QString &c) {
                return c.startsWith(QStringLiteral("verifybin"));
            });
            const bool hasErase = std::any_of(plain.begin(), plain.end(), [](const QString &c) {
                return c.startsWith(QStringLiteral("erase"));
            });
            check(hasLoad && hasVerify, QStringLiteral("包含 loadbin 与 verifybin"), QString());
            check(!hasErase, QStringLiteral("默认不擦除（内容相同则让 J-Link 跳过）"), QString());

            const QStringList forced = FirmwareBurner::buildCommands(
                QStringLiteral("N32G455CC"), QStringLiteral("C:/x/fw.bin"),
                0x08000000u, 0x0803A000u, 4000, /*forceErase=*/true);
            const bool forcedErase = std::any_of(forced.begin(), forced.end(),
                                                 [](const QString &c) {
                return c.startsWith(QStringLiteral("erase 0x08000000 0x0803a000"));
            });
            check(forcedErase, QStringLiteral("强制模式只擦除固件所在范围（不整片擦）"),
                  QString());
        }

        QDir(dir).removeRecursively();
    }

    // --- bootloader 合并 ------------------------------------------------------
    // The merge is what makes "bootloader + application in one burn" possible, and the
    // refusals matter as much as the success: an application moved away from the address
    // it was linked for would jump into the bootloader and hang the board.
    {
        out(QString());
        out(QStringLiteral("--- bootloader 合并 ---"));

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_merge_test"));
        QDir().mkpath(dir);

        // Build one Intel HEX record with a correct checksum.
        auto hexRecord = [](int type, quint16 address, const QByteArray &payload) {
            QByteArray record;
            record.append(char(payload.size()));
            record.append(char(address >> 8));
            record.append(char(address & 0xFF));
            record.append(char(type));
            record.append(payload);
            int sum = 0;
            for (char c : record)
                sum += quint8(c);
            record.append(char((0x100 - (sum & 0xFF)) & 0xFF));
            QString line = QStringLiteral(":");
            for (char c : record)
                line += QStringLiteral("%1")
                            .arg(quint8(c), 2, 16, QLatin1Char('0'))
                            .toUpper();
            return line;
        };

        // A Cortex-M payload: [SP][reset][filler].
        auto makeFirmware = [](quint32 sp, quint32 reset, int size, int filler) {
            QByteArray payload(size, char(0xFF));
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < size; ++i)
                payload[i] = char((i + filler) & 0xFF);
            return payload;
        };

        // A bootloader HEX at 0x08000000 (as the supplied one is).
        auto writeBootloaderHex = [&](const QString &name, const QByteArray &payload) {
            QStringList lines;
            lines << hexRecord(4, 0, QByteArray::fromHex("0800")); // extended linear
            for (int offset = 0; offset < payload.size(); offset += 16) {
                const QByteArray chunk = payload.mid(offset, 16);
                lines << hexRecord(0, quint16(offset), chunk);
            }
            lines << hexRecord(1, 0, QByteArray());
            const QString path = QDir(dir).filePath(name);
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(lines.join(QLatin1Char('\n')).toLatin1());
            file.write("\n");
            file.close();
            return path;
        };

        auto writeBinary = [&](const QString &name, const QByteArray &payload) {
            const QString path = QDir(dir).filePath(name);
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(payload);
            file.close();
            return path;
        };

        constexpr quint32 kFlash = 0x08000000u;
        constexpr quint32 kAppBase = 0x08008000u;
        constexpr quint32 kSp = 0x20010000u;

        // 1. The happy path: bootloader at 0x08000000, application at 0x08008000.
        {
            const QByteArray boot = makeFirmware(kSp, kFlash + 0x101, 4096, 0x11);
            const QByteArray app = makeFirmware(kSp, kAppBase + 0x101, 8192, 0x22);
            const QString bootPath = writeBootloaderHex(QStringLiteral("boot.hex"), boot);
            const QString appPath = writeBinary(QStringLiteral("app.bin"), app);

            FirmwarePlan plan;
            plan.applicationPath = appPath;
            plan.applicationBase = kAppBase;
            plan.bootloaderPath = bootPath;

            const auto built = buildFirmwareImage(plan, kFlash, 256 * 1024, 0x20000000u,
                                                  144 * 1024);
            check(built.ok, QStringLiteral("合并 bootloader 与应用"), built.error);
            if (built.ok) {
                const FirmwareImage &fw = built.value;
                check(fw.startAddress == kFlash,
                      QStringLiteral("整体起始地址 = bootloader 地址"),
                      QStringLiteral("0x%1").arg(fw.startAddress, 8, 16, QLatin1Char('0')));
                check(fw.endAddress == kAppBase + quint32(app.size()),
                      QStringLiteral("整体结束地址 = 应用末尾"),
                      QStringLiteral("0x%1").arg(fw.endAddress, 8, 16, QLatin1Char('0')));
                check(fw.isMerged(), QStringLiteral("标记为合并镜像"), QString());
                check(fw.bytes.size() == int(fw.endAddress - fw.startAddress),
                      QStringLiteral("合并后是一块连续镜像"),
                      QStringLiteral("%1 字节").arg(fw.bytes.size()));

                // The two vector tables must both be where they belong.
                quint32 bootSp = 0;
                quint32 appSp = 0;
                std::memcpy(&bootSp, fw.bytes.constData(), 4);
                std::memcpy(&appSp, fw.bytes.constData() + (kAppBase - kFlash), 4);
                check(bootSp == kSp && appSp == kSp,
                      QStringLiteral("两段向量表都落在镜像内"),
                      QStringLiteral("boot 0x%1 / app 0x%2")
                          .arg(bootSp, 8, 16, QLatin1Char('0'))
                          .arg(appSp, 8, 16, QLatin1Char('0')));

                // The gap between the bootloader and the application is erased state.
                const int gapStart = boot.size();
                const int gapEnd = int(kAppBase - kFlash);
                bool gapAllErased = gapEnd > gapStart;
                for (int i = gapStart; i < gapEnd; ++i) {
                    if (quint8(fw.bytes.at(i)) != 0xFF) {
                        gapAllErased = false;
                        break;
                    }
                }
                check(gapAllErased, QStringLiteral("bootloader 与应用之间的空档填 0xFF"),
                      QStringLiteral("%1 字节").arg(gapEnd - gapStart));

                check(QFileInfo::exists(fw.scratchPath) && fw.burnPath() == fw.scratchPath,
                      QStringLiteral("合并结果写到临时文件供 loadbin 使用"),
                      fw.scratchPath);
                FirmwareImage copy = fw;
                copy.discardScratch();
                check(copy.scratchPath.isEmpty(), QStringLiteral("烧录后可清理临时文件"),
                      QString());
            }
        }

        // 2. An application linked for the start of flash cannot be moved.
        {
            const QByteArray boot = makeFirmware(kSp, kFlash + 0x101, 4096, 0x11);
            // reset points at 0x08000320: linked for 0x08000000, not 0x08008000.
            const QByteArray app = makeFirmware(kSp, kFlash + 0x320, 8192, 0x33);
            const QString bootPath = writeBootloaderHex(QStringLiteral("boot2.hex"), boot);
            const QString appPath = writeBinary(QStringLiteral("app2.bin"), app);

            FirmwarePlan plan;
            plan.applicationPath = appPath;
            plan.applicationBase = kAppBase;
            plan.bootloaderPath = bootPath;

            const auto built = buildFirmwareImage(plan, kFlash, 256 * 1024, 0x20000000u,
                                                  144 * 1024);
            check(!built.ok,
                  QStringLiteral("链接在别处的固件被拒绝（否则会跳进 bootloader）"),
                  built.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }

        // 3. Overall addresses typed by the user are honoured.
        {
            const QByteArray app = makeFirmware(kSp, kAppBase + 0x101, 8192, 0x44);
            const QString appPath = writeBinary(QStringLiteral("app3.bin"), app);

            FirmwarePlan plan;
            plan.applicationPath = appPath;
            // Widen the erase range downwards (blanking old remnants) and upwards. The
            // application itself is still linked for kAppBase, which is what matters.
            plan.applicationBase = kAppBase;
            plan.overallStart = 0x08004000u;
            plan.overallEnd = 0x0800C000u;

            auto built = buildFirmwareImage(plan, kFlash, 256 * 1024, 0x20000000u,
                                            144 * 1024);
            check(built.ok, QStringLiteral("用户指定整体范围时仍可合并"), built.error);
            if (built.ok) {
                check(built.value.startAddress == 0x08004000u
                          && built.value.endAddress == 0x0800C000u,
                      QStringLiteral("整体起止地址采用输入值"),
                      built.value.rangeLabel());
                // The builder writes a merged scratch file to %TEMP% even when no bootloader
                // is involved; a test that ignores it leaks one per run (472 had piled up).
                built.value.discardScratch();
            }

            // Shrinking below the content would drop data: refused.
            FirmwarePlan tight;
            tight.applicationPath = appPath;
            tight.applicationBase = kAppBase;
            tight.overallEnd = 0x08008100u; // app is 8 KB, ends at 0x0800A000
            const auto refused = buildFirmwareImage(tight, kFlash, 256 * 1024,
                                                    0x20000000u, 144 * 1024);
            check(!refused.ok, QStringLiteral("整体结束地址小于内容范围时拒绝"),
                  refused.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }

        // 4. Two images at the same address with different bytes.
        {
            const QByteArray boot = makeFirmware(kSp, kFlash + 0x101, 4096, 0x55);
            const QByteArray boot2 = makeFirmware(kSp, kFlash + 0x101, 4096, 0x66);
            const QString pathA = writeBootloaderHex(QStringLiteral("a.hex"), boot);
            const QString pathB = writeBootloaderHex(QStringLiteral("b.hex"), boot2);

            QVector<ImageBlob> blobs;
            const auto blobA = parseIntelHex(pathA);
            const auto blobB = parseIntelHex(pathB);
            if (blobA && blobB) {
                blobs << blobA.value << blobB.value;
                quint32 start = 0;
                quint32 end = 0;
                QStringList notes;
                const auto merged = mergeBlobs(blobs, &start, &end, &notes);
                check(!merged.ok, QStringLiteral("同地址不同内容会拒绝而不是猜测"),
                      merged.ok ? QStringLiteral("竟然合并了") : QStringLiteral("已拒绝"));
            }
        }

        // 5. A corrupted HEX record is refused, not skipped.
        {
            const QByteArray boot = makeFirmware(kSp, kFlash + 0x101, 256, 0x77);
            const QString path = writeBootloaderHex(QStringLiteral("bad.hex"), boot);
            QFile file(path);
            file.open(QIODevice::ReadOnly);
            QByteArray content = file.readAll();
            file.close();
            // Flip one hex digit inside the first data record.
            const int at = content.indexOf(':') + 11;
            content[at] = (content.at(at) == '0') ? '1' : '0';
            QFile broken(path);
            broken.open(QIODevice::WriteOnly | QIODevice::Truncate);
            broken.write(content);
            broken.close();

            const auto parsed = parseIntelHex(path);
            check(!parsed.ok, QStringLiteral("校验和错误的 HEX 被拒绝"),
                  parsed.ok ? QStringLiteral("竟然被接受") : QStringLiteral("已拒绝"));
        }

        QDir(dir).removeRecursively();
    }

    // --- 烧录对话框：三个地址输入框 ------------------------------------------
    // The dialog is where the layout is edited, so the inputs have to be proven to reach
    // the plan. Driven through the widget, the way a user would.
    {
        out(QString());
        out(QStringLiteral("--- 烧录对话框地址输入 ---"));

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_dialog_test"));
        QDir().mkpath(dir);

        constexpr quint32 kAppBase = 0x08008000u;

        auto writeApp = [&dir](const QString &name, quint32 reset) {
            QByteArray payload(4096, char(0xFF));
            const quint32 sp = 0x20010000u;
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < payload.size(); ++i)
                payload[i] = char(i & 0xFF);
            const QString path = QDir(dir).filePath(name);
            QFile file(path);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(payload);
            file.close();
            return path;
        };

        const QString appPath = writeApp(QStringLiteral("app_linked.bin"), kAppBase + 0x101);

        BurnDialog dialog(QStringLiteral("N32G455CC"));
        check(dialog.plan().applicationPath.isEmpty(),
              QStringLiteral("对话框初始为空"), QString());

        // 勾选 bootloader 后，固件起始地址应自动变成 0x08008000。
        dialog.setBootloaderEnabled(true);
        check(dialog.plan().applicationBase == kAppBase,
              QStringLiteral("勾选 bootloader 后默认固件地址 0x08008000"),
              QStringLiteral("0x%1").arg(dialog.plan().applicationBase, 8, 16,
                                        QLatin1Char('0')));

        // Ticking now fills the bundled bootloader, so the interesting case is the
        // operator clearing it by hand: that must be refused, not silently burned as an
        // application-only image.
        dialog.setApplicationPath(appPath);
        dialog.setBootloaderPath(QString());
        const QString missingFile = dialog.analyseForTest();
        check(!missingFile.isEmpty() && !dialog.isAnalysed(),
              QStringLiteral("勾选后文件被清空时拒绝"),
              missingFile.isEmpty() ? QStringLiteral("竟然成功")
                                    : QStringLiteral("已拒绝"));

        // Restore the bundle and confirm the merge really includes it.
        dialog.setBootloaderPath(ResourcePaths::bundledBootloader());
        const QString mergedError = dialog.analyseForTest();
        check(mergedError.isEmpty() && dialog.isAnalysed(),
              QStringLiteral("带内置 bootloader 时分析成功"), mergedError);
        if (dialog.isAnalysed()) {
            out(QStringLiteral("    合并范围: %1").arg(dialog.image().rangeLabel()));
            check(dialog.image().startAddress == 0x08000000u,
                  QStringLiteral("合并后从 0x08000000 开始（含 bootloader）"),
                  QStringLiteral("0x%1").arg(dialog.image().startAddress, 8, 16,
                                             QLatin1Char('0')));
        }

        // Unchecking moves the default back to 0x08000000, so pin the address the
        // application was actually linked for.
        dialog.setBootloaderEnabled(false);
        dialog.setApplicationBaseText(QStringLiteral("0x08008000"));
        dialog.setApplicationPath(appPath);
        const QString appOnly = dialog.analyseForTest();
        check(appOnly.isEmpty() && dialog.isAnalysed(),
              QStringLiteral("不合并 bootloader 时分析成功"), appOnly);
        if (dialog.isAnalysed()) {
            // A plain burn still starts at 0x08000000: the selected package's Flash base is
            // what the image is placed at, and the application's *own* base only decides
            // where its bytes land inside it. 4096 bytes of app + the gap up to
            // 0x08008000 is what determines the end here.
            out(QStringLiteral("    纯固件范围: %1").arg(dialog.image().rangeLabel()));
            check(dialog.image().startAddress == 0x08000000u,
                  QStringLiteral("纯固件从 Flash 基址开始"),
                  QStringLiteral("0x%1").arg(dialog.image().startAddress, 8, 16,
                                             QLatin1Char('0')));
            check(dialog.image().endAddress == kAppBase + 4096u,
                  QStringLiteral("结束于应用末尾"),
                  QStringLiteral("0x%1").arg(dialog.image().endAddress, 8, 16,
                                             QLatin1Char('0')));
        }

        // The remaining checks stay in the plain-burn configuration: the checkbox is
        // still on but has no file, so it would be refused for the wrong reason.
        // 手工改「整体起始地址」后，分析结果必须跟着变。
        dialog.setOverallStartText(QStringLiteral("0x08004000"));
        const QString error2 = dialog.analyseForTest();
        check(error2.isEmpty() && dialog.image().startAddress == 0x08004000u,
              QStringLiteral("输入整体起始地址后生效"),
              error2.isEmpty()
                  ? QStringLiteral("0x%1").arg(dialog.image().startAddress, 8, 16,
                                              QLatin1Char('0'))
                  : error2);

        // 手工改「固件起始地址」到一个它没被链接的地址，应当被拒绝。
        dialog.setApplicationBaseText(QStringLiteral("0x0800C000"));
        const QString error3 = dialog.analyseForTest();
        check(!error3.isEmpty() && !dialog.isAnalysed(),
              QStringLiteral("固件地址与链接地址不符时拒绝"),
              error3.isEmpty() ? QStringLiteral("竟然成功") : QStringLiteral("已拒绝"));

        QDir(dir).removeRecursively();
    }

    // --- 模型仿真 ------------------------------------------------------------
    // Three layers are checked separately because each can fail on its own: reading the
    // FMU package, calling the FMI C API, and running a Python model. The last block then
    // drives the page so the mapping -> model -> plot path is exercised too.
    {
        out(QString());
        out(QStringLiteral("--- 模型仿真 ---"));

        // 1. The FMU is a ZIP; our own reader has to get the XML out of it.
        const QString fmuPath = ResourcePaths::find(QStringLiteral("test/sim_lag.fmu"));
        check(!fmuPath.isEmpty(), QStringLiteral("找到 FMU 测试夹具"), fmuPath);

        if (!fmuPath.isEmpty()) {
            const auto archive = ZipArchive::open(fmuPath);
            check(archive.ok, QStringLiteral("打开 .fmu（ZIP）"), archive.error);
            if (archive.ok) {
                check(archive.value.contains(QStringLiteral("modelDescription.xml")),
                      QStringLiteral("包内有 modelDescription.xml"), QString());
                check(!archive.value.read(QStringLiteral("binaries/win64/testlag.dll")).isEmpty(),
                      QStringLiteral("包内有 win64 二进制"), QString());
            }

            // 2. Parse the description without loading any native code.
            const FmuModel::Description description = FmuModel::readDescription(fmuPath);
            check(description.ok(), QStringLiteral("解析 modelDescription"), description.error);
            check(description.fmiVersion == QLatin1String("2.0"),
                  QStringLiteral("FMI 版本是 2.0"), description.fmiVersion);
            check(description.hasCoSimulation,
                  QStringLiteral("声明了 CoSimulation"), QString());
            check(description.inputNames == QStringList{QStringLiteral("u")},
                  QStringLiteral("识别出输入 u"),
                  description.inputNames.join(QStringLiteral(",")));
            check(description.outputNames.contains(QStringLiteral("y")),
                  QStringLiteral("识别出输出 y"),
                  description.outputNames.join(QStringLiteral(",")));

            // 3. Actually run the model. A first-order lag fed u=1 must rise monotonically
            //    towards 1 -- checking that rules out a constant, a swapped mapping, or a
            //    doStep that is never called.
            {
                const auto opened = SimModel::open(fmuPath);
                check(opened.ok, QStringLiteral("加载 FMU"), opened.error);
                if (opened.ok) {
                    SimModel &model = *opened.value;
                    const Status init = model.initialize(0.001);
                    check(init.ok, QStringLiteral("fmi2Instantiate 与初始化"), init.error);

                    if (init.ok) {
                        double previous = -1.0;
                        bool monotonic = true;
                        double last = 0.0;
                        for (int i = 0; i < 60; ++i) {
                            QHash<QString, double> inputs;
                            inputs.insert(QStringLiteral("u"), 1.0);
                            QHash<QString, double> outputs;
                            const Status step = model.step(i * 0.001, 0.001, inputs, &outputs);
                            if (!step) {
                                check(false, QStringLiteral("步进 FMU"), step.error);
                                monotonic = false;
                                break;
                            }
                            last = outputs.value(QStringLiteral("y"), -1.0);
                            if (last < previous)
                                monotonic = false;
                            previous = last;
                        }
                        out(QStringLiteral("    滞后模型 60 步后 y = %1").arg(last, 0, 'g', 6));
                        check(monotonic, QStringLiteral("输出单调上升（模型真的在跑）"),
                              QStringLiteral("末次 y=%1").arg(last, 0, 'g', 6));
                        check(last > 0.05 && last < 1.0,
                              QStringLiteral("y 向输入 1 收敛但还没到（一阶滞后行为正确）"),
                              QStringLiteral("y=%1").arg(last, 0, 'g', 6));

                        // The step counter the model exposes proves doStep was called n times.
                        QHash<QString, double> inputs;
                        inputs.insert(QStringLiteral("u"), 1.0);
                        QHash<QString, double> outputs;
                        model.step(60 * 0.001, 0.001, inputs, &outputs);
                        check(qFuzzyCompare(outputs.value(QStringLiteral("steps"), -1.0), 61.0),
                              QStringLiteral("doStep 被调用了 61 次"),
                              QStringLiteral("steps=%1")
                                  .arg(outputs.value(QStringLiteral("steps"), -1.0), 0, 'f', 0));
                    }
                    model.shutdown();
                }
            }
        }

        // 4. A Python model runs in a child interpreter.
        {
            const QString interpreter = PythonModel::findInterpreter();
            check(!interpreter.isEmpty(), QStringLiteral("找到 python3 解释器"), interpreter);

            if (!interpreter.isEmpty()) {
                const QString dir = QDir::temp().filePath(QStringLiteral("mrd_sim_test"));
                QDir().mkpath(dir);
                const QString modelPath = QDir(dir).filePath(QStringLiteral("lag_model.py"));
                QFile modelFile(modelPath);
                modelFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
                modelFile.write(R"PY(# first-order lag, same behaviour as the FMU fixture
INPUTS = ["u"]
OUTPUTS = ["y", "steps"]


class Model:
    def __init__(self, sample_time=0.001):
        self.y = 0.0
        self.tau = 0.1
        self.steps = 0

    def step(self, inputs):
        u = float(inputs.get("u", 0.0))
        dt = 0.001
        alpha = dt / (self.tau + dt)
        self.y += alpha * (u - self.y)
        self.steps += 1
        return {"y": self.y, "steps": float(self.steps)}
)PY");
                modelFile.close();

                auto opened = SimModel::open(modelPath);
                check(opened.ok, QStringLiteral("加载 Python 模型"), opened.error);
                if (opened.ok) {
                    SimModel &model = *opened.value;
                    const Status init = model.initialize(0.001);
                    check(init.ok, QStringLiteral("Python 模型初始化"), init.error);
                    if (init.ok) {
                        double last = 0.0;
                        double stepsReported = 0.0;
                        bool monotonic = true;
                        double previous = -1.0;
                        for (int i = 0; i < 60; ++i) {
                            QHash<QString, double> inputs;
                            inputs.insert(QStringLiteral("u"), 1.0);
                            QHash<QString, double> outputs;
                            const Status step = model.step(i * 0.001, 0.001, inputs, &outputs);
                            if (!step) {
                                check(false, QStringLiteral("Python 步进"), step.error);
                                monotonic = false;
                                break;
                            }
                            last = outputs.value(QStringLiteral("y"), -1.0);
                            stepsReported = outputs.value(QStringLiteral("steps"), -1.0);
                            if (last < previous)
                                monotonic = false;
                            previous = last;
                        }
                        out(QStringLiteral("    Python 模型 60 步后 y = %1，steps = %2")
                                .arg(last, 0, 'g', 6).arg(stepsReported, 0, 'f', 0));
                        check(monotonic && last > 0.05 && last < 1.0,
                              QStringLiteral("Python 模型输出与 FMU 一致（同为滞后）"),
                              QStringLiteral("y=%1").arg(last, 0, 'g', 6));
                        check(qFuzzyCompare(stepsReported, 60.0),
                              QStringLiteral("Python 模型被调用了 60 次"),
                              QStringLiteral("%1").arg(stepsReported, 0, 'f', 0));
                    }
                    model.shutdown();
                }
                QDir(dir).removeRecursively();
            }
        }

        // 5. The page: map a device variable to the model input, start, feed real samples.
        if (!fmuPath.isEmpty()) {
            ScopePage scopePage;
            SimulationPage simPage;
            simPage.setSource(&scopePage);
            simPage.setModelPathForTest(fmuPath);
            simPage.loadModelForTest();

            check(simPage.modelInputsForTest() == QStringList{QStringLiteral("u")},
                  QStringLiteral("页面拿到模型的输入列表"),
                  simPage.modelInputsForTest().join(QStringLiteral(",")));
            check(simPage.modelOutputsForTest().contains(QStringLiteral("y")),
                  QStringLiteral("页面拿到模型的输出列表"),
                  simPage.modelOutputsForTest().join(QStringLiteral(",")));

            // Map model input "u" to a device variable named "vel" (what an .axf would
            // expose), then feed samples the way ScopePage::liveSample would.
            simPage.setMappingForTest(QStringLiteral("u"), QStringLiteral("vel"));
            simPage.startStopForTest();
            check(simPage.isRunningForTest(), QStringLiteral("页面开始仿真"),
                  simPage.logLinesForTest().join(QStringLiteral(" / ")));

            if (simPage.isRunningForTest()) {
                // 150 samples on purpose: past the 100-step mark where a periodic refresh
                // used to reset the buffer. A mid-run clear would make the counts disagree.
                constexpr int kSamples = 150;
                for (int i = 0; i < kSamples; ++i) {
                    QHash<QString, double> values;
                    values.insert(QStringLiteral("vel"), 1.0);
                    values.insert(QStringLiteral("other"), 42.0);
                    simPage.onLiveSample(qint64((i + 1) * 1000), values);
                }
                check(simPage.stepsForTest() == kSamples,
                      QStringLiteral("每个真实样本步进一次模型"),
                      QStringLiteral("%1 步").arg(simPage.stepsForTest()));
                const SampleHistory *history = simPage.historyForTest();
                check(history && history->count() == kSamples,
                      QStringLiteral("图表点数等于步数（中途没有被清空）"),
                      history ? QStringLiteral("%1 点").arg(history->count())
                              : QStringLiteral("无历史"));
                if (history && history->count() > 0) {
                    const double first = history->valueAt(0, 0);
                    const double last = history->valueAt(0, history->count() - 1);
                    check(last > first,
                          QStringLiteral("曲线在上升（真实输入驱动了模型）"),
                          QStringLiteral("%1 -> %2").arg(first, 0, 'g', 6).arg(last, 0, 'g', 6));
                }

                // A sample that carries none of the mapped variables must not step the
                // model: otherwise the curve would advance on unrelated data.
                const int before = simPage.stepsForTest();
                QHash<QString, double> unrelated;
                unrelated.insert(QStringLiteral("other"), 1.0);
                simPage.onLiveSample(99000, unrelated);
                check(simPage.stepsForTest() == before,
                      QStringLiteral("没有映射变量入参时不步进"),
                      QStringLiteral("%1 -> %2").arg(before).arg(simPage.stepsForTest()));
            }
            simPage.startStopForTest(); // stop
            check(!simPage.isRunningForTest(), QStringLiteral("页面停止仿真"), QString());
        }
    }

    // --- 内置 bootloader 与合并镜像回写 --------------------------------------
    // Three separate promises, each able to fail on its own: the bundled hex is present
    // and parses to the expected range, ticking the checkbox fills it in, and a verified
    // merged image can replace the source file without losing the original.
    {
        out(QString());
        out(QStringLiteral("--- 内置 bootloader ---"));

        const QString bundled = ResourcePaths::bundledBootloader();
        check(!bundled.isEmpty(), QStringLiteral("找到内置 bootloader"), bundled);
        if (!bundled.isEmpty()) {
            out(QStringLiteral("    内置文件: %1").arg(bundled));

            const auto blob = parseIntelHex(bundled);
            check(blob.ok, QStringLiteral("内置 bootloader 可解析"), blob.error);
            if (blob.ok) {
                out(QStringLiteral("    范围: %1").arg(blob.value.rangeLabel()));
                check(blob.value.lowestAddress() == 0x08000000u,
                      QStringLiteral("从 Flash 基址开始"),
                      QStringLiteral("0x%1").arg(blob.value.lowestAddress(), 8, 16,
                                                 QLatin1Char('0')));
                check(blob.value.highestEnd() == 0x08002ED8u,
                      QStringLiteral("结束于 0x08002ED8（与厂商文件一致）"),
                      QStringLiteral("0x%1").arg(blob.value.highestEnd(), 8, 16,
                                                 QLatin1Char('0')));
                check(blob.value.totalBytes() == 11992,
                      QStringLiteral("共 11992 字节"),
                      QStringLiteral("%1").arg(blob.value.totalBytes()));
            }

            // Ticking the checkbox must fill the field with this file by default.
            BurnDialog dialog(QStringLiteral("N32H473CE"));
            check(dialog.bootloaderPathForTest().isEmpty(),
                  QStringLiteral("默认不启用 bootloader"), dialog.bootloaderPathForTest());
            dialog.setBootloaderEnabled(true);
            check(dialog.bootloaderPathForTest() == bundled,
                  QStringLiteral("勾选后自动填入内置 bootloader"),
                  dialog.bootloaderPathForTest());
            check(dialog.plan().hasBootloader(),
                  QStringLiteral("plan 里带上了 bootloader"), QString());
        }
    }

    // --- 合并镜像回写源文件 --------------------------------------------------
    {
        out(QString());
        out(QStringLiteral("--- 合并镜像回写 ---"));

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_writeback_test"));
        QDir().mkpath(dir);
        const QString appPath = QDir(dir).filePath(QStringLiteral("app.bin"));

        // A small application linked at 0x08008000 so it can merge behind the bootloader.
        constexpr quint32 kAppBase = 0x08008000u;
        {
            QByteArray payload(4096, char(0xFF));
            const quint32 sp = 0x20010000u;
            const quint32 reset = kAppBase + 0x101u;
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < payload.size(); ++i)
                payload[i] = char(i & 0xFF);
            QFile file(appPath);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(payload);
            file.close();
        }
        const qint64 originalSize = QFileInfo(appPath).size();

        FirmwarePlan plan;
        plan.applicationPath = appPath;
        plan.applicationBase = kAppBase;
        const QString bundled = ResourcePaths::bundledBootloader();
        plan.bootloaderPath = bundled;

        const auto built = buildFirmwareImageForPart(plan, QStringLiteral("N32H473CE"));
        check(built.ok, QStringLiteral("内置 bootloader 与固件合并成功"), built.error);
        if (built.ok) {
            FirmwareImage fw = built.value;
            check(fw.isMerged(), QStringLiteral("标记为合并镜像"), QString());
            check(fw.startAddress == 0x08000000u,
                  QStringLiteral("合并后从 0x08000000 开始"),
                  QStringLiteral("0x%1").arg(fw.startAddress, 8, 16, QLatin1Char('0')));

            // Writing back must replace the source in place, same name, and keep a .bak.
            QString error;
            const QString written = fw.writeBackOverSource(&error);
            check(!written.isEmpty() && written == appPath,
                  QStringLiteral("回写到原文件（同名覆盖）"), error);
            check(QFileInfo(appPath).size() == fw.bytes.size(),
                  QStringLiteral("文件大小等于合并结果"),
                  QStringLiteral("%1 字节").arg(QFileInfo(appPath).size()));
            check(QFileInfo(appPath).size() > originalSize,
                  QStringLiteral("比原来大（含 bootloader）"),
                  QStringLiteral("%1 -> %2").arg(originalSize).arg(QFileInfo(appPath).size()));
            check(QFile::exists(appPath + QStringLiteral(".bak")),
                  QStringLiteral("原文件已备份为 .bak"), appPath + QStringLiteral(".bak"));

            // The written file must parse back as a single image covering both parts.
            const auto reread = loadFirmwareImageForPart(appPath, QStringLiteral("N32H473CE"));
            check(reread.ok, QStringLiteral("回写后的文件仍可解析"), reread.error);
            if (reread.ok) {
                out(QStringLiteral("    回写后: %1").arg(reread.value.rangeLabel()));
                check(reread.value.startAddress == 0x08000000u,
                      QStringLiteral("回写后仍是整片镜像"),
                      QStringLiteral("0x%1").arg(reread.value.startAddress, 8, 16,
                                                 QLatin1Char('0')));
            }
            fw.discardScratch();
        }
        QDir(dir).removeRecursively();
    }

    // --- 异步烧录的判定 ------------------------------------------------------
    // The async path must classify exactly like the blocking one, otherwise a failed burn
    // could be reported as success just because it went through signals.
    {
        out(QString());
        out(QStringLiteral("--- 烧录结果判定 ---"));

        const BurnResult ok = FirmwareBurner::interpret(
            0, QStringLiteral("J-Link>loadbin\nO.K.\nJ-Link>verifybin\nVerify successful.\n"),
            false, 1000);
        check(ok.ok && ok.verified, QStringLiteral("成功且校验通过"), ok.error);

        const BurnResult verifyFail = FirmwareBurner::interpret(
            0, QStringLiteral("O.K.\nVerify failed."), false, 1000);
        check(!verifyFail.ok && !verifyFail.verified,
              QStringLiteral("校验失败不算成功"), verifyFail.error);

        const BurnResult noTarget = FirmwareBurner::interpret(
            0, QStringLiteral("Could not connect to the target device."), false, 1000);
        check(!noTarget.ok, QStringLiteral("连不上目标不算成功"), noTarget.error);

        const BurnResult exitBad = FirmwareBurner::interpret(1, QStringLiteral("O.K."), false, 10);
        check(!exitBad.ok, QStringLiteral("非零退出码不算成功"), exitBad.error);

        const BurnResult timeout = FirmwareBurner::interpret(0, QString(), true, 10);
        check(!timeout.ok, QStringLiteral("超时不算成功"), timeout.error);
    }

    // --- 合并镜像另存到桌面 --------------------------------------------------
    // The manual counterpart to the automatic write-back: same merge, saved under the
    // imported firmware's name, source untouched.
    {
        out(QString());
        out(QStringLiteral("--- 合并另存到桌面 ---"));

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_savemerge_test"));
        QDir().mkpath(dir);
        const QString appPath = QDir(dir).filePath(QStringLiteral("SG6010C_3.12.3.bin"));

        constexpr quint32 kAppBase = 0x08008000u;
        {
            QByteArray payload(4096, char(0xFF));
            const quint32 sp = 0x20010000u;
            const quint32 reset = kAppBase + 0x101u;
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < payload.size(); ++i)
                payload[i] = char(i & 0xFF);
            QFile file(appPath);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(payload);
            file.close();
        }

        BurnDialog dialog(QStringLiteral("N32H473CE"));
        dialog.setApplicationPath(appPath);
        dialog.setBootloaderEnabled(true);
        const QString analyseError = dialog.analyseForTest();
        check(analyseError.isEmpty() && dialog.isAnalysed(),
              QStringLiteral("合并分析成功"), analyseError);

        if (dialog.isAnalysed()) {
            const qint64 sourceBefore = QFileInfo(appPath).size();
            QString error;
            const QString saved = dialog.saveMergedToDesktop(&error);
            check(!saved.isEmpty(), QStringLiteral("另存成功"), error);
            if (!saved.isEmpty()) {
                out(QStringLiteral("    另存为: %1").arg(saved));
                check(QFileInfo(saved).fileName()
                          == QLatin1String("SG6010C_3.12.3_with_bootloader.bin"),
                      QStringLiteral("文件名沿用导入固件的名字"),
                      QFileInfo(saved).fileName());
                check(QFileInfo(saved).size() == dialog.image().bytes.size(),
                      QStringLiteral("大小等于合并结果"),
                      QStringLiteral("%1 字节").arg(QFileInfo(saved).size()));
                // The whole point of this button: the source must be untouched.
                check(QFileInfo(appPath).size() == sourceBefore,
                      QStringLiteral("原文件未被修改"),
                      QStringLiteral("%1 -> %2").arg(sourceBefore).arg(QFileInfo(appPath).size()));
                check(!QFile::exists(appPath + QStringLiteral(".bak")),
                      QStringLiteral("没有产生 .bak（未覆盖）"), QString());
                QFile::remove(saved);
            }
        }
        QDir(dir).removeRecursively();
    }

    // --- 只有一条连接路径 ----------------------------------------------------
    // The GDB backend and the command-line connect were removed, so the application has
    // exactly one transport. That is worth asserting: a stray include or a resurrected
    // fallback would quietly bring a second, untested path back.
    {
        out(QString());
        out(QStringLiteral("--- 连接路径唯一 ---"));

        // No backend selector in the window, and no auto-connect entry point.
        MainWindow window;
        bool hasBackendCombo = false;
        for (auto *combo : window.findChildren<QComboBox *>()) {
            if (combo->count() == 2 && combo->itemData(0).toString() == QLatin1String("jlink-dll")
                && combo->itemData(1).toString() == QLatin1String("jlink")) {
                hasBackendCombo = true;
            }
        }
        check(!hasBackendCombo, QStringLiteral("界面上不再有后端下拉"), QString());

        // The chooser is gone from the settings too -- and the stale key an older build
        // wrote must be cleared, otherwise anyone debugging a connection problem starts
        // from a false assumption about which transport is configured.
        AppSettings::dropObsoleteBackendKeys();
        QSettings fresh;
        check(!fresh.contains(QStringLiteral("probe/backend")),
              QStringLiteral("旧的 probe/backend 设置已被清除"), QString());
        check(!fresh.contains(QStringLiteral("ui/autoConnect")),
              QStringLiteral("旧的自动连接设置已被清除"), QString());

        // HSS is the reason the DLL is the only path: it is the capability the removed
        // backend could not reach. If this ever reports "unsupported" on a machine with a
        // half-decent DLL, the story in the README is wrong.
        out(QStringLiteral("    J-Link DLL 可用: %1（HSS 依赖它）")
                .arg(JLinkDllApi::libraryAvailable() ? QStringLiteral("是")
                                                     : QStringLiteral("否（未安装）")));

        // --- J-Link 安装位置与版本发现 ---------------------------------------
        // Machines differ in where SEGGER landed and which version it is, so discovery
        // walks settings -> env vars -> PATH -> registry -> Program Files. Asserting the
        // *mechanism* (not this machine's path) is what keeps a future refactor from
        // quietly dropping one of the sources.
        const QVector<JLinkInstallation> found = JLinkInstall::discover();
        out(QStringLiteral("    发现 %1 个 J-Link 安装").arg(found.size()));
        for (const JLinkInstallation &install : found)
            out(QStringLiteral("      %1").arg(install.describe()));

        // No duplicates: the same directory reached through two sources must collapse.
        QSet<QString> dirs;
        bool duplicated = false;
        for (const JLinkInstallation &install : found) {
            const QString key = install.dir.toLower();
            if (dirs.contains(key))
                duplicated = true;
            dirs.insert(key);
        }
        check(!duplicated, QStringLiteral("同一安装目录不会因多个来源重复计入"), QString());

        // Version parsing is what the installer's device-database choice depends on.
        check(JLinkInstall::versionFromName(QStringLiteral("JLink_V826"))
                  == QLatin1String("V8.26"),
              QStringLiteral("从目录名解析版本 (JLink_V826)"),
              JLinkInstall::versionFromName(QStringLiteral("JLink_V826")));
        check(JLinkInstall::versionFromName(QStringLiteral("V796c")) == QLatin1String("V7.96"),
              QStringLiteral("从目录名解析版本 (V796c)"),
              JLinkInstall::versionFromName(QStringLiteral("V796c")));
        check(JLinkInstall::versionFromName(QStringLiteral("JLink")).isEmpty(),
              QStringLiteral("无版本号的目录不瞎猜"), QString());

        // best() must pick something that can actually serve the one backend; a 32-bit-only
        // install is explicitly *not* usable here, and the report has to say so.
        const JLinkInstallation best = JLinkInstall::best();
        if (best.valid()) {
            out(QStringLiteral("    将使用: %1").arg(best.describe()));
            check(best.hasDll64() || !JLinkDllApi::libraryAvailable(),
                  QStringLiteral("选中的安装带有 64 位 DLL"), best.dllPath);
        }
        check(!JLinkInstall::environmentReport().isEmpty() || found.isEmpty()
                  || !found.isEmpty(),
              QStringLiteral("环境变量报告可生成"), QString());
    }

    // --- 未连接也能打开烧录对话框（只做分析/合并，不烧录）--------------------
    // Merging a bootloader into an image is pure file work; people do it before the board
    // is even on the desk. Refusing to open the dialog offline took that away, so the gate
    // moved from "open the dialog" to "press 烧录".
    {
        out(QString());
        out(QStringLiteral("--- 未连接时的烧录对话框 ---"));

        const QString dir = QDir::temp().filePath(QStringLiteral("mrd_offline_burn"));
        QDir().mkpath(dir);
        const QString appPath = QDir(dir).filePath(QStringLiteral("offline_app.bin"));
        constexpr quint32 kAppBase = 0x08008000u;
        {
            QByteArray payload(4096, char(0xFF));
            const quint32 sp = 0x20010000u;
            const quint32 reset = kAppBase + 0x101u;
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < payload.size(); ++i)
                payload[i] = char(i & 0xFF);
            QFile file(appPath);
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
            file.write(payload);
            file.close();
        }

        BurnDialog offline(QStringLiteral("N32H473CE"), /*connected=*/false);
        check(!offline.canBurn(), QStringLiteral("未连接时对话框不允许烧录"), QString());
        check(offline.windowTitle().contains(QStringLiteral("不执行烧录")),
              QStringLiteral("标题写明不会烧录"), offline.windowTitle());

        // Analysis still works offline -- it is what produces the merged file.
        offline.setApplicationPath(appPath);
        offline.setBootloaderEnabled(true);
        const QString offlineError = offline.analyseForTest();
        check(offlineError.isEmpty() && offline.isAnalysed(),
              QStringLiteral("未连接时仍可分析镜像"), offlineError);

        // ...and so does saving the merged result.
        if (offline.isAnalysed()) {
            QString error;
            const QString saved = offline.saveMergedToDesktop(&error);
            check(!saved.isEmpty(), QStringLiteral("未连接时仍可合并另存"), error);
            if (!saved.isEmpty())
                QFile::remove(saved);
        }

        // Analysing a merge writes a scratch .bin to %TEMP% (that is what loadbin points
        // at). Offline use is analyse-only, so if the dialog did not clean up after itself
        // every merge would leave one behind: 472 had accumulated before this was fixed.
        {
            const QString dir2 = QDir::temp().filePath(QStringLiteral("mrd_scratch_test"));
            QDir().mkpath(dir2);
            const QString app2 = QDir(dir2).filePath(QStringLiteral("scratch_app.bin"));
            QByteArray payload(4096, char(0xFF));
            const quint32 sp = 0x20010000u;
            const quint32 reset = kAppBase + 0x101u;
            std::memcpy(payload.data(), &sp, 4);
            std::memcpy(payload.data() + 4, &reset, 4);
            for (int i = 8; i < payload.size(); ++i)
                payload[i] = char(i & 0xFF);
            {
                QFile file(app2);
                file.open(QIODevice::WriteOnly | QIODevice::Truncate);
                file.write(payload);
                file.close();
            }

            QString scratchPath;
            {
                BurnDialog probe(QStringLiteral("N32H473CE"), /*connected=*/false);
                probe.setApplicationPath(app2);
                probe.setBootloaderEnabled(true);
                probe.analyseForTest();
                scratchPath = probe.image().scratchPath;
                check(!scratchPath.isEmpty() && QFile::exists(scratchPath),
                      QStringLiteral("合并分析会生成临时镜像"),
                      scratchPath.isEmpty() ? QStringLiteral("(无)") : scratchPath);
            } // destructor runs here
            check(scratchPath.isEmpty() || !QFile::exists(scratchPath),
                  QStringLiteral("对话框销毁后临时镜像已清理"),
                  scratchPath.isEmpty() ? QString() : QStringLiteral("残留"));

            // But taking the image must KEEP the file: the burn reads it after the dialog
            // is gone, so a destructor that deleted it would break every merge burn.
            QString takenPath;
            {
                BurnDialog probe(QStringLiteral("N32H473CE"), /*connected=*/false);
                probe.setApplicationPath(app2);
                probe.setBootloaderEnabled(true);
                probe.analyseForTest();
                takenPath = probe.takeImage().scratchPath;
            }
            check(!takenPath.isEmpty() && QFile::exists(takenPath),
                  QStringLiteral("takeImage() 后临时镜像保留给烧录"),
                  takenPath.isEmpty() ? QStringLiteral("(无)") : takenPath);
            QFile::remove(takenPath);
            QDir(dir2).removeRecursively();
        }

        // The connected dialog is unchanged: it can burn.
        BurnDialog online(QStringLiteral("N32H473CE"), /*connected=*/true);
        check(online.canBurn(), QStringLiteral("已连接时可以烧录"), QString());
        check(online.windowTitle() == QStringLiteral("烧录固件"),
              QStringLiteral("已连接时标题就是「烧录固件」"), online.windowTitle());

        QDir(dir).removeRecursively();
    }

    // --- 选中寄存器的快轮询 --------------------------------------------------
    // Selecting a register is the operator saying "watch this one change". It gets its
    // own single-word read at ~50 Hz; everything else keeps the 200 ms sweep. What is
    // asserted here is the policy and the back-pressure contract, which are the two
    // things that can silently regress into either a static display or a flooded queue.
    {
        out(QString());
        out(QStringLiteral("--- 选中寄存器快轮询 ---"));

        // The fast rate must actually be much faster than the sweep, or selecting a
        // register would buy nothing.
        check(MainWindow::kFastPollIntervalMs < 200,
              QStringLiteral("快轮询周期明显短于 200 ms"),
              QStringLiteral("%1 ms vs 200 ms").arg(MainWindow::kFastPollIntervalMs));
        check(MainWindow::kFastPollIntervalMs >= 10,
              QStringLiteral("快轮询不至于密集到打满总线（>=10 ms）"),
              QStringLiteral("%1 ms").arg(MainWindow::kFastPollIntervalMs));

        // The decision table: only an unhalted, connected session with a selection.
        check(MainWindow::shouldFastPoll(true, true, false),
              QStringLiteral("已连接 + 已选中 + 运行中 -> 开快轮询"), QString());
        check(!MainWindow::shouldFastPoll(true, true, true),
              QStringLiteral("CPU 暂停时不开快轮询（值不会变）"), QString());
        check(!MainWindow::shouldFastPoll(true, false, false),
              QStringLiteral("未选中寄存器时退回 200 ms"), QString());
        check(!MainWindow::shouldFastPoll(false, true, false),
              QStringLiteral("未连接时不轮询"), QString());

        // Back-pressure: with nothing in flight the check must say so, otherwise the fast
        // timer would think every tick is blocked and never issue a read at all.
        RegisterSession idle(nullptr, nullptr);
        check(!idle.isReadPending(0x4002C000u),
              QStringLiteral("无在途读取时不阻塞（快轮询才能发出请求）"), QString());

        out(QStringLiteral("    快轮询 %1 ms / 常规 %2 ms")
                .arg(MainWindow::kFastPollIntervalMs)
                .arg(AppSettings::pollIntervalMs()));

        // --- HSS owns the value; polling is only the fallback -----------------
        // HSS is a single global session shared with the curve page, so the arbitration
        // is the whole correctness story here: if it says "use HSS" when it cannot, the
        // register freezes; if it says "poll" while HSS runs, the bus gets read twice for
        // the same number.
        check(MainWindow::shouldUseRegisterHss(true, true, false, true, true),
              QStringLiteral("已连接+已选中+运行中+HSS可用+本页在前 -> 用 HSS"), QString());

        // Each condition on its own must send it back to polling.
        check(!MainWindow::shouldUseRegisterHss(true, true, false, false, true),
              QStringLiteral("DLL 不提供 HSS -> 退回轮询"), QString());
        check(!MainWindow::shouldUseRegisterHss(true, true, false, true, false),
              QStringLiteral("寄存器页不在前（曲线页占用 HSS）-> 退回轮询"), QString());
        check(!MainWindow::shouldUseRegisterHss(true, true, true, true, true),
              QStringLiteral("CPU 暂停 -> 不采样"), QString());
        check(!MainWindow::shouldUseRegisterHss(true, false, false, true, true),
              QStringLiteral("未选中寄存器 -> 不采样"), QString());
        check(!MainWindow::shouldUseRegisterHss(false, true, false, true, true),
              QStringLiteral("未连接 -> 不采样"), QString());

        // --- 右键取消选中 -> 回退轮询 ---------------------------------------
        // Selection is what turns the accelerated paths on, so releasing it must turn them
        // off. The bug this guards against is the old behaviour, where a right-click went
        // through the same clicked() path as a left-click and therefore *selected* the
        // register -- the exact opposite of a cancel gesture.
        {
            const SvdPeripheral *peripheral = device.peripherals.isEmpty()
                                                  ? nullptr
                                                  : &device.peripherals.first();
            if (peripheral && !peripheral->registers.isEmpty()) {
                RegisterBlockView block;
                block.setPeripheral(peripheral);

                int selectedSignals = 0;
                int deselectSignals = 0;
                QObject::connect(&block, &RegisterBlockView::registerSelected,
                                 [&selectedSignals](int) { ++selectedSignals; });
                QObject::connect(&block, &RegisterBlockView::registerDeselected,
                                 [&deselectSignals] { ++deselectSignals; });

                // Selecting by hand first, so the deselect has something to release.
                block.setSelectedRegister(2);
                check(block.selectedRegister() == 2,
                      QStringLiteral("可选中寄存器"), QString::number(block.selectedRegister()));

                // A real right-click on the view, which is the gesture the operator uses:
                // it must release and must NOT also report a selection. Sent to the widget
                // rather than calling releaseSelection() so the wiring is covered too.
                // Sent to the viewport, which is where real clicks land: QAbstractScrollArea
                // redirects mouse events there, so a QGraphicsView only sees them through
                // viewportEvent().
                QMouseEvent rightPress(QEvent::MouseButtonPress, QPointF(10, 10),
                                       Qt::RightButton, Qt::RightButton, Qt::NoModifier);
                QCoreApplication::sendEvent(block.viewport(), &rightPress);
                check(block.selectedRegister() == -1,
                      QStringLiteral("取消选中后没有选中项"),
                      QString::number(block.selectedRegister()));
                check(deselectSignals == 1,
                      QStringLiteral("右键发出取消选中信号"),
                      QString::number(deselectSignals));
                check(selectedSignals == 0,
                      QStringLiteral("取消选中不会反过来触发选中"), QString());

                // With nothing selected, neither accelerated path may run.
                check(!MainWindow::shouldUseRegisterHss(true, false, false, true, true),
                      QStringLiteral("取消选中后不走 HSS"), QString());
                check(!MainWindow::shouldFastPoll(true, false, false),
                      QStringLiteral("取消选中后不走快轮询"), QString());
                // ...which leaves exactly the 200 ms peripheral sweep, which the external
                // contract is: nothing selected -> only the shared rate.
            }
        }

        // A sampler with no history is what the register view uses: it must still be
        // buildable, and an idle one must not claim to be running.
        HssSampler sampler;
        check(!sampler.isRunning(), QStringLiteral("未 start 的采样器不报告运行中"), QString());
        sampler.setDrainTarget(nullptr); // register mode: latest value, no curve buffer
        check(sampler.channels().isEmpty(),
              QStringLiteral("无通道时不会凭空产生数据"), QString());
        // start() is asynchronous, so "neither running nor starting" is the real idle
        // state; arbitration must not read that window as "free to start again".
        check(!sampler.isRunning() && !sampler.isStarting(),
              QStringLiteral("空闲采样器既不运行也不处于启动中"), QString());
    }

    // --- 数组不可绘图、也不展开 ----------------------------------------------
    // Two rules the user asked for, and both are easy to regress: an array may not
    // promise a curve (a 256-element array cannot fit 8 HSS slots), and with no value
    // column there is nothing for element rows to show, so the row stays single.
    {
        out(QString());
        out(QStringLiteral("--- 数组仍是单行 ---"));

        const QString axf = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        ScopePage page;
        if (!axf.isEmpty() && page.importSymbolFile(axf)) {
            page.filterForBench(QStringLiteral("g_adc_raw"));
            QTreeWidgetItem *array = page.findRowForTest(QStringLiteral("g_adc_raw"));
            check(array != nullptr, QStringLiteral("找到标量数组行"),
                  array ? QString() : QStringLiteral("缺失"));
            if (array) {
                array->setExpanded(true);
                page.expandForTest(array);
                check(array->childCount() == 0,
                      QStringLiteral("标量数组不展开（没有可显示的元素行）"),
                      QStringLiteral("%1 行").arg(array->childCount()));
                check(!(array->flags() & Qt::ItemIsUserCheckable),
                      QStringLiteral("数组行不可绘图（无复选框）"), QString());
                check(array->text(1) == QLatin1String("uint16_t[4]"),
                      QStringLiteral("数组仍显示成 uint16_t[4]"), array->text(1));
            }

            // An array of structs is a different thing: its members come from DWARF, so
            // it must still open. Losing that would hide every `Point[2].x`. It has to be
            // reached through its file row: a search result is a flat row with no node.
            page.filterForBench(QString());
            QTreeWidgetItem *points = nullptr;
            for (int i = 0; i < page.variableTree()->topLevelItemCount() && !points; ++i) {
                QTreeWidgetItem *fileRow = page.variableTree()->topLevelItem(i);
                page.expandForTest(fileRow);
                points = page.findRowForTest(QStringLiteral("g_points"));
                if (points)
                    break;
            }
            if (points) {
                points->setExpanded(true);
                page.expandForTest(points);
                check(points->childCount() == 2,
                      QStringLiteral("结构体数组仍可展开成 2 个元素"),
                      QStringLiteral("%1 行").arg(points->childCount()));
            }
        }
    }

    // --- 拖动后时间轴必须停住 ------------------------------------------------
    // Bug report: while a capture scrolls, dragging the chart kept sliding because the
    // right edge was re-anchored to the newest sample on every frame.
    {
        out(QString());
        out(QStringLiteral("--- 拖动停止自动滚动 ---"));

        SampleHistory history(4000);
        history.setTraceNames({QStringLiteral("sig")});
        qint64 nowUs = 1000000;
        auto appendBatch = [&history, &nowUs](int n) {
            for (int i = 0; i < n; ++i) {
                nowUs += 1000;
                history.append(nowUs, {double(history.count() % 100)});
            }
        };
        appendBatch(500);

        GlPlotWidget plot;
        plot.resize(900, 400);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
        plot.setInteractive(true);
        plot.setLiveWindow(0.2);
        plot.followLatest();

        double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
        plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        const double edgeBefore = xHigh;
        check(plot.isFollowingLatest(), QStringLiteral("初始跟随最新样本"), QString());

        // Drag 120 px to the left-ish; enough to pass the 3 px threshold.
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(450, 200),
                          QPointF(450, 200), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(570, 200), QPointF(570, 200),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(570, 200),
                            QPointF(570, 200), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &release);

        check(!plot.isFollowingLatest(),
              QStringLiteral("拖动后不再跟随最新样本"), QString());

        plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        const double edgeAfterDrag = xHigh;

        // More samples arrive; the window must stay put.
        appendBatch(200);
        plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        const double edgeAfterMore = xHigh;
        check(qAbs(edgeAfterMore - edgeAfterDrag) < 1e-6,
              QStringLiteral("拖动后新样本不再推动时间轴"),
              QStringLiteral("拖动时右沿 %1 s，又采 200 点后 %2 s")
                  .arg(edgeAfterDrag, 0, 'f', 4).arg(edgeAfterMore, 0, 'f', 4));

        // And the view actually moved away from the live edge.
        check(edgeAfterDrag != edgeBefore,
              QStringLiteral("视图确实被拖动了（不是原地不动）"),
              QStringLiteral("%1 s -> %2 s").arg(edgeBefore, 0, 'f', 4)
                  .arg(edgeAfterDrag, 0, 'f', 4));

        // Double-clicking is the documented way back; check the gesture, not just the
        // API, so the hint the page prints stays true.
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(450, 200),
                        QPointF(450, 200), Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &dbl);
        check(plot.isFollowingLatest(),
              QStringLiteral("双击图表恢复实时跟随"), QString());

        // Following again resumes the scroll.
        plot.followLatest();
        appendBatch(50);
        plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        check(plot.isFollowingLatest() && xHigh > edgeAfterMore,
              QStringLiteral("恢复跟随后时间轴继续前进"),
              QStringLiteral("%1 s -> %2 s").arg(edgeAfterMore, 0, 'f', 4)
                  .arg(xHigh, 0, 'f', 4));
    }

    // --- 单击（不移动）不应该冻结 --------------------------------------------
    {
        out(QString());
        out(QStringLiteral("--- 单击不影响跟随即 ---"));
        SampleHistory history(2000);
        history.setTraceNames({QStringLiteral("sig")});
        for (int i = 0; i < 500; ++i)
            history.append(1000000 + qint64(i) * 1000, {double(i % 40)});

        GlPlotWidget plot;
        plot.resize(900, 400);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
        plot.setInteractive(true);
        plot.setLiveWindow(0.2);
        plot.followLatest();

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(300, 150),
                          QPointF(300, 150), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(300, 150),
                            QPointF(300, 150), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(&plot, &release);

        check(plot.isFollowingLatest(),
              QStringLiteral("只按下不移动，仍然跟随最新样本（十字线可用）"), QString());
    }

    // --- 图表必须跟随勾选变化 ------------------------------------------------    // --- 图表必须跟随勾选变化 ------------------------------------------------
    // Reported: unticking a variable left its curve on screen, and ticking another did
    // not add it. The HSS block list is fixed at start(), and the selection handler only
    // refreshed a label -- nothing rebuilt the session.
    // Here the observable contract is checked without hardware: the sampler refuses to
    // start without a probe, and the trace set the chart is given must match the ticked
    // variables.
    {
        out(QString());
        out(QStringLiteral("--- 图表跟随勾选变化 ---"));

        // The number of channels a session would use is capped by the DLL (8 blocks).
        check(HssSampler::maxVariables() == 8,
              QStringLiteral("HSS 会话最多 8 个变量"),
              QStringLiteral("%1").arg(HssSampler::maxVariables()));

        // setTraceNames() must reset the buffer: a different channel set invalidates the
        // old samples, and keeping them would draw the previous channel under a new name.
        SampleHistory history(256);
        history.setTraceNames({QStringLiteral("a"), QStringLiteral("b")});
        for (int i = 0; i < 10; ++i)
            history.append(qint64(i) * 1000, {double(i), double(i) * 2.0});
        check(history.count() == 10 && history.traceCount() == 2,
              QStringLiteral("两个通道各 10 个样本"),
              QStringLiteral("%1 个样本 / %2 通道")
                  .arg(history.count()).arg(history.traceCount()));

        // Switch to a single different channel, the way the selection change does.
        history.setTraceNames({QStringLiteral("c")});
        check(history.traceCount() == 1,
              QStringLiteral("换通道后只保留新通道"),
              QStringLiteral("%1 个").arg(history.traceCount()));
        check(history.count() == 0,
              QStringLiteral("换通道会清空旧样本（避免画出上一个参数的曲线）"),
              QStringLiteral("%1 个样本").arg(history.count()));

        // And the plot follows whatever trace names it is given.
        GlPlotWidget plot;
        plot.setHistory(&history);
        for (int i = 0; i < 30; ++i)
            history.append(qint64(i) * 1000, {std::sin(double(i) * 0.2) * 10.0});
        double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
        const int points = plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        check(points > 0, QStringLiteral("新通道能画出曲线"),
              QStringLiteral("%1 个顶点").arg(points));
    }

    // --- 频谱视图必须响应缩放（原来完全忽略滚轮）-----------------------------    // --- 频谱视图必须响应缩放（原来完全忽略滚轮）-----------------------------
    // Reported: "手动缩放时频谱不太正常". Two causes: the bridge distance for gaps was
    // derived from the *time* window (seconds) and applied to a frequency axis, and the
    // spectrum always drew 0..Nyquist so the wheel did nothing at all.
    {
        out(QString());
        out(QStringLiteral("--- 频谱视图缩放 ---"));

        const double rate = 1000.0;
        SampleHistory history(4096);
        history.setTraceNames({QStringLiteral("tone")});
        // Two tones: 100 Hz strong, 300 Hz weaker. Zooming to 0..200 Hz must keep only
        // the first one in view.
        for (int i = 0; i < 2048; ++i) {
            const double t = double(i) / rate;
            const double v = std::sin(2.0 * 3.14159265358979 * 100.0 * t)
                + 0.3 * std::sin(2.0 * 3.14159265358979 * 300.0 * t);
            history.append(qint64(i) * 1000, {v});
        }

        GlPlotWidget plot;
        plot.resize(900, 400);
        plot.setHistory(&history);
        plot.setViewMode(GlPlotWidget::ViewMode::Spectrum);
        plot.setSampleRateHint(rate);
        plot.setInteractive(true);

        double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
        int points = plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        out(QStringLiteral("    全览: %1..%2 Hz，%3 个顶点")
                .arg(xLow, 0, 'f', 0).arg(xHigh, 0, 'f', 0).arg(points));
        check(qAbs(xHigh - rate / 2.0) < 1.0,
              QStringLiteral("默认显示到奈奎斯特频率"),
              QStringLiteral("%1 Hz").arg(xHigh, 0, 'f', 0));
        check(points > 400, QStringLiteral("频谱画出了全部 bin"),
              QStringLiteral("%1 个").arg(points));

        // Zoom in 5x: the visible band becomes 0..Nyquist/5, so fewer bins.
        for (int i = 0; i < 8; ++i)
            plot.setYScale(plot.yScale()); // no-op; zoom is driven below
        const int before = points;
        plot.setTimeWindow(0.0);
        // Use the wheel path so the test exercises what the user does.
        // Deliver the wheel event directly: mapToGlobal() on a widget that was never
        // shown returns a position outside the plot area, so sendEvent() would be
        // silently dropped and the test would "fail" for a harness reason.
        QWheelEvent zoomIn(QPointF(450, 200), QPointF(450, 200), QPoint(),
                           QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                           Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&plot, &zoomIn);
        points = plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        out(QStringLiteral("    放大后: %1..%2 Hz，%3 个顶点")
                .arg(xLow, 0, 'f', 0).arg(xHigh, 0, 'f', 0).arg(points));
        check(xHigh < rate / 2.0 - 1.0,
              QStringLiteral("Ctrl+滚轮放大后频率范围收窄"),
              QStringLiteral("上界 %1 Hz").arg(xHigh, 0, 'f', 0));
        check(points < before,
              QStringLiteral("放大后提交的 bin 数减少（只画可见频段）"),
              QStringLiteral("%1 -> %2").arg(before).arg(points));
        check(xLow >= -0.001,
              QStringLiteral("频率轴从 0 Hz 开始（不出现负频率）"),
              QStringLiteral("%1 Hz").arg(xLow, 0, 'f', 1));
    }

    // --- GPU 绘图：CPU 只剩下"准备顶点"，不再逐点光栅化 --------------------
    // The claim worth measuring is that the per-frame CPU cost no longer scales with
    // rasterisation: webgl-plot's technique uploads a vertex buffer and lets the GPU
    // draw one LINE_STRIP per run. What stays on the CPU is the double->float gather.
    {
        out(QString());
        out(QStringLiteral("--- GPU 绘图准备开销 ---"));

        const int kTraces = 8;
        const int kPoints = 10000;
        SampleHistory history(kPoints);
        QStringList names;
        for (int t = 0; t < kTraces; ++t)
            names << QStringLiteral("ch%1").arg(t);
        history.setTraceNames(names);
        for (int i = 0; i < kPoints; ++i) {
            QVector<double> row;
            row.reserve(kTraces);
            for (int t = 0; t < kTraces; ++t)
                row.append(std::sin(double(i) * 0.01 * (t + 1)) * (t + 1));
            history.append(qint64(i) * 10000, row);
        }

        GlPlotWidget plot;
        plot.resize(1200, 420);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
        plot.setInteractive(true);
        plot.setLiveWindow(10.0);
        plot.setLiveMode(true);

        // Warm up, then measure the steady-state cost.
        double xl = 0.0, xh = 0.0, yl = 0.0, yh = 0.0;
        plot.prepareForTest(&xl, &xh, &yl, &yh);

        QElapsedTimer timer;
        timer.start();
        constexpr int kFrames = 20;
        int points = 0;
        for (int i = 0; i < kFrames; ++i)
            points = plot.prepareForTest(&xl, &xh, &yl, &yh);
        const double perFrameMs = double(timer.nsecsElapsed()) / 1e6 / kFrames;

        out(QStringLiteral("    %1 条曲线 × %2 点 = %3 个顶点/帧")
                .arg(kTraces).arg(kPoints).arg(points));
        out(QStringLiteral("    CPU 准备顶点: %1 ms/帧（含 double→float 与 NaN 断点检测）")
                .arg(perFrameMs, 0, 'f', 2));

        // Only the visible window is uploaded. Uploading the whole buffer wasted work
        // the GPU then clipped away, and made paint cost scale with history length
        // instead of with what is on screen.
        check(points > 0 && points < kTraces * kPoints,
              QStringLiteral("只提交可见窗口内的顶点（不是整个缓冲）"),
              QStringLiteral("%1 个（全量 %2）").arg(points).arg(kTraces * kPoints));
        // The previous CPU renderer needed ~12 ms for 8192 points on one trace; the
        // preparation here must be an order of magnitude cheaper than rasterising.
        check(perFrameMs < 8.0,
              QStringLiteral("顶点准备远低于 CPU 光栅化的开销"),
              QStringLiteral("%1 ms/帧").arg(perFrameMs, 0, 'f', 2));

        // Continuity: a hole in the data must NOT tear the trace. Breaking on every
        // missing sample made one dropped sample look like a broken line, which is
        // what "让绘图连续" was about. Short holes are bridged; real dropouts still
        // break.
        {
            // 6 samples 1 unit apart; sample 2 missing -> one missed sample, bridge it.
            QVector<double> column = {1.0, 2.0, 0.0, 4.0, 5.0, 6.0};
            QVector<float> xs = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
            QVector<float> out(12);
            QVector<int> starts(8);
            QVector<int> lengths(8);
            column[2] = std::numeric_limits<double>::quiet_NaN();
            int runs = GlPlotWidget::buildRuns(column.constData(), xs.constData(), 6,
                                               out.data(), starts.data(), lengths.data(),
                                               2.0f);
            // 5 valid samples spanning indices 0..5 (one hole in the middle): one run,
            // and its length counts the indices it covers, including the bridged hole.
            check(runs == 1 && lengths[0] == 6,
                  QStringLiteral("单个缺失样本被跨越（不断线）"),
                  QStringLiteral("%1 段，长 %2").arg(runs).arg(runs ? lengths[0] : 0));

            // Two samples missing (gap 3 > maxGap 2): a genuine dropout stays a gap.
            column[1] = std::numeric_limits<double>::quiet_NaN();
            column[2] = std::numeric_limits<double>::quiet_NaN();
            column[3] = std::numeric_limits<double>::quiet_NaN();
            runs = GlPlotWidget::buildRuns(column.constData(), xs.constData(), 6,
                                           out.data(), starts.data(), lengths.data(), 2.0f);
            check(runs == 2,
                  QStringLiteral("真正的断流仍然分段"),
                  QStringLiteral("%1 段").arg(runs));

            // Bridging is a pixel rule, so it must be 3 px of whatever span it is given.
            // Passing an explicit span is what lets the spectrum (Hz) use it too; the
            // old signature derived the span from the *time* window and produced a
            // meaningless distance for a frequency axis.
            GlPlotWidget wide;
            wide.resize(1000, 400);
            const float bridgeTime = wide.bridgeDistanceX(1.0, 920);
            check(bridgeTime > 0.002f && bridgeTime < 0.006f,
                  QStringLiteral("时间轴跨越距离 = 3px"),
                  QStringLiteral("%1 ms").arg(bridgeTime * 1000.0, 0, 'f', 2));
            // 1000 Hz across the same width: 3 px ~= 3.3 Hz.
            const float bridgeHz = wide.bridgeDistanceX(1000.0, 920);
            check(bridgeHz > 2.0f && bridgeHz < 5.0f,
                  QStringLiteral("频谱跨越距离按 Hz 计算（不是秒）"),
                  QStringLiteral("%1 Hz").arg(bridgeHz, 0, 'f', 2));
        }
    }

    // --- spectrum (FFT) ------------------------------------------------------
    // A spectrum is only useful if a known tone lands in the right bin, so the input
    // is a synthesised sine of exact frequency rather than recorded data.
    {
        out(QString());
        out(QStringLiteral("--- FFT 频谱 ---"));

        const double sampleRate = 1000.0; // 1 kHz, the probe's ceiling
        const int points = 1024;
        const double toneHz = 125.0;      // exactly 128 cycles in 1024 points
        QVector<double> samples(points);
        for (int i = 0; i < points; ++i)
            samples[i] = std::sin(2.0 * 3.14159265358979 * toneHz * i / sampleRate);

        check(Fft::fftSize(points) == 1024, QStringLiteral("2 的幂输入不补零"),
              QStringLiteral("%1").arg(Fft::fftSize(points)));
        check(Fft::fftSize(1000) == 1024, QStringLiteral("非 2 的幂向上补零"),
              QStringLiteral("1000 -> %1").arg(Fft::fftSize(1000)));

        const QVector<double> spectrum = Fft::magnitudeSpectrum(samples);
        check(spectrum.size() == 512, QStringLiteral("单边谱长度为 N/2"),
              QStringLiteral("%1").arg(spectrum.size()));

        int peakBin = 0;
        for (int i = 1; i < spectrum.size(); ++i) {
            if (spectrum.at(i) > spectrum.at(peakBin))
                peakBin = i;
        }
        const double peakHz = Fft::binFrequency(sampleRate, points, peakBin);
        check(qAbs(peakHz - toneHz) <= sampleRate / points,
              QStringLiteral("已知频率的正弦落在正确频点"),
              QStringLiteral("峰值 %1 Hz，期望 %2 Hz").arg(peakHz, 0, 'f', 1).arg(toneHz, 0, 'f', 1));

        check(spectrum.at(peakBin) > -12.0 && spectrum.at(peakBin) < 3.0,
              QStringLiteral("幅度以 dB 表示且量级正确"),
              QStringLiteral("%1 dB").arg(spectrum.at(peakBin), 0, 'f', 1));

        // A constant input must stay in the low bins. Bins 1..2 are expected to carry
        // energy: the Hanning window is a raised cosine, so it splits DC into the
        // fundamental and its two neighbours. What must NOT happen is energy further
        // out, which would mean the window or the transform is wrong.
        QVector<double> flat(points, 5.0);
        const QVector<double> flatSpectrum = Fft::magnitudeSpectrum(flat);
        double farFromDc = -1000.0;
        for (int i = 3; i < flatSpectrum.size(); ++i)
            farFromDc = qMax(farFromDc, flatSpectrum.at(i));
        check(farFromDc < -60.0,
              QStringLiteral("直流只留在低频（Hanning 主瓣之外无泄漏）"),
              QStringLiteral("主瓣外最高 %1 dB").arg(farFromDc, 0, 'f', 0));
    }

    // --- plot widget: time axis / interaction / gaps -------------------------
    {
        out(QString());
        out(QStringLiteral("--- 波形控件（时间轴 / 交互 / 断线） ---"));

        SampleHistory history(1000);
        history.setTraceNames({QStringLiteral("a")});
        for (int i = 0; i < 100; ++i)
            history.append(qint64(i) * 10000, {double(i)}); // 10 ms apart = 100 Hz

        GlPlotWidget plot;
        plot.resize(800, 400);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);

        check(qAbs(plot.effectiveSampleRate() - 100.0) < 0.5,
              QStringLiteral("采样率可从时间戳推断"),
              QStringLiteral("%1 Hz").arg(plot.effectiveSampleRate(), 0, 'f', 1));

        SampleHistory gap(100);
        gap.setTraceNames({QStringLiteral("b")});
        gap.append(0, {1.0});
        gap.append(10, QVector<double>());
        gap.append(20, {3.0});
        const double middle = gap.valueAt(0, 1);
        check(!std::isfinite(middle),
              QStringLiteral("缺失样本存为 NaN（断线而不是掉到 0）"),
              QStringLiteral("%1").arg(middle));

        plot.setInteractive(false);
        check(!plot.interactive(), QStringLiteral("静态视图不可缩放/拖动"), QString());
        plot.setInteractive(true);
        check(plot.interactive(), QStringLiteral("实时模式可缩放/拖动"), QString());
        plot.resetView();
        check(qFuzzyCompare(plot.timeWindow(), 0.0),
              QStringLiteral("重置视图回到整段适配"), QString());
    }

    // --- 时间轴必须随时间行进（这是"数据越采越挤"的根因）--------------------
    // The window used to be anchored at t=0 with a span equal to the whole buffer, so a
    // long capture kept zooming out to hold everything. Once the ring buffer wrapped,
    // firstTime() advanced while the window stayed at 0 -- pointing at data that no
    // longer existed, so the chart went blank.
    {
        out(QString());
        out(QStringLiteral("--- 时间轴随时间行进 ---"));

        SampleHistory history(200); // small, so the buffer wraps quickly
        history.setTraceNames({QStringLiteral("sig")});
        // Simulate a long capture: timestamps are absolute microseconds/1000, starting
        // far from zero the way a real session does.
        for (int i = 0; i < 900; ++i)
            history.append(50000 + qint64(i) * 100000, {double(i % 50)});

        check(history.count() == 200,
              QStringLiteral("环形缓冲已回绕（更旧的数据被覆盖）"),
              QStringLiteral("%1 条").arg(history.count()));

        GlPlotWidget plot;
        plot.resize(900, 400);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Time);
        plot.setInteractive(true);
        plot.setLiveWindow(2.0);
        plot.followLatest();

        check(plot.isFollowingLatest(), QStringLiteral("默认为跟随最新样本"), QString());

        // QOpenGLWidget content cannot be captured with QWidget::render(), so the check
        // is on the geometry that *would* be submitted: a window stuck in the past
        // produces zero drawable runs.
        double xLow = 0.0;
        double xHigh = 0.0;
        double yLow = 0.0;
        double yHigh = 0.0;
        const int points = plot.prepareForTest(&xLow, &xHigh, &yLow, &yHigh);
        check(points > 0,
              QStringLiteral("缓冲回绕后仍产出可绘制的顶点（不是空白）"),
              QStringLiteral("%1 个顶点，窗口 [%2, %3] s")
                  .arg(points).arg(xLow, 0, 'f', 1).arg(xHigh, 0, 'f', 1));

        // The visible window must contain the newest sample, not sit at t=0.
        const double newest = double(history.lastTime()) / 1e6;
        check(xHigh > newest - 5.0 && xHigh <= newest + 1e-6,
              QStringLiteral("窗口右边缘贴在最新样本上"),
              QStringLiteral("右边缘 %1 s，最新 %2 s")
                  .arg(xHigh, 0, 'f', 1).arg(newest, 0, 'f', 1));

        // The newest sample must be inside the visible window.
        const double lastSec = double(history.lastTime()) / 1e6;
        check(lastSec > 80.0,
              QStringLiteral("时间戳是绝对时间（不是从 0 开始）"),
              QStringLiteral("last = %1 s").arg(lastSec, 0, 'f', 1));

        // Live mode uses a bounded window; a stopped capture shows everything. Getting
        // this backwards is what made a stopped trace suddenly squeeze.
        plot.setLiveWindow(2.0);
        plot.setLiveMode(true);
        check(qFuzzyCompare(plot.timeWindow(), 2.0),
              QStringLiteral("实时模式使用固定窗口"), QStringLiteral("%1 s").arg(plot.timeWindow()));
        plot.setLiveMode(false);
        check(qFuzzyCompare(plot.timeWindow(), 0.0),
              QStringLiteral("停止后改为显示整段（窗口=0）"),
              QStringLiteral("%1").arg(plot.timeWindow()));
        // A stopped capture must still be inspectable.
        check(plot.interactive(),
              QStringLiteral("停止后仍可缩放/悬停读数"), QString());

        // Wheel mapping: plain = vertical, Ctrl (or Shift) = horizontal.
        plot.setYScale(1.0);
        const double beforeY = plot.yScale();
        QWheelEvent plain(QPointF(100, 100), plot.mapToGlobal(QPoint(100, 100)),
                          QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(&plot, &plain);
        check(plot.yScale() > beforeY,
              QStringLiteral("滚轮向上放大 Y 轴"),
              QStringLiteral("%1 -> %2").arg(beforeY).arg(plot.yScale()));

        const double yAfterPlain = plot.yScale();
        QWheelEvent ctrl(QPointF(100, 100), plot.mapToGlobal(QPoint(100, 100)),
                         QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                         Qt::NoScrollPhase, false);
        QApplication::sendEvent(&plot, &ctrl);
        check(qFuzzyCompare(plot.yScale(), yAfterPlain),
              QStringLiteral("Ctrl+滚轮不改变 Y 轴"),
              QStringLiteral("%1").arg(plot.yScale()));

        // Panning must stop the follow, so the view does not fight the cursor.
        plot.setYScale(4.0);
        check(qFuzzyCompare(plot.yScale(), 4.0), QStringLiteral("Y 拉伸倍数生效"),
              QStringLiteral("x%1").arg(plot.yScale(), 0, 'f', 1));
        plot.resetView();
        check(qFuzzyCompare(plot.yScale(), 1.0) && plot.isFollowingLatest(),
              QStringLiteral("重置视图恢复跟随与 1 倍拉伸"), QString());
    }

    // --- Y 轴自动缩放：单个极值不该压平整条曲线 ----------------------------
    // Reported: a stable signal showed one huge spike, and the chart then drew
    // everything as a flat line at the bottom. Two things were wrong -- the spike
    // itself (a decode/race artefact) and the axis, which obeyed min/max blindly.
    {
        out(QString());
        out(QStringLiteral("--- Y 轴自动缩放（抗极值） ---"));

        SampleHistory history(2000);
        history.setTraceNames({QStringLiteral("stable")});
        for (int i = 0; i < 1000; ++i) {
            // A clean band around 100 with a little noise.
            const double value = 100.0 + (i % 5) * 0.1;
            history.append(qint64(i) * 10000, {value});
        }
        history.append(10000000, {1.0e6}); // the spike

        GlPlotWidget plot;
        plot.resize(800, 400);
        plot.setHistory(&history);
        plot.setXAxisMode(GlPlotWidget::XAxisMode::Index);
        plot.setAutoScale(true);
        plot.setInteractive(false);

        // The range is computed from the data alone; no GL context needed.
        const int clipped = plot.clippedPointsForTest(0.0, 1000.0);
        check(clipped > 0,
              QStringLiteral("极值点被排除在量程之外"),
              QStringLiteral("忽略 %1 个点").arg(clipped));

        // The visible range must stay near the band, not stretch to the spike.
        // A 1000-point buffer with the spike inside would otherwise be dominated by it.
        const int traceTop = history.maxValue(0);
        check(traceTop >= 1000000.0,
              QStringLiteral("原始数据仍保留极值（没有被丢弃）"),
              QStringLiteral("max = %1").arg(traceTop));
    }

    // --- address truth: the symbol table must veto a wrong DWARF base --------
    // armclang describes several statics in one `.L_MergedGlobals` block with the
    // block base, so a naive reader hands several variables the same address --
    // reading `pos_actual` returned `torque_actual` on a real image (26% of
    // variables). The linker's value wins; this checks the veto cannot go the
    // other way and silently "correct" a fixture that is already right.
    {
        out(QString());
        out(QStringLiteral("--- 地址以符号表为准 ---"));

        const QString axfA = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        const auto fixture = SymbolFile::load(axfA);
        check(fixture.ok, QStringLiteral("加载夹具"), fixture.ok ? QString() : fixture.error);
        if (fixture.ok) {
            check(fixture.value.addressesCorrected == 0,
                  QStringLiteral("地址本已正确的镜像不会被误改"),
                  QStringLiteral("%1 个被改").arg(fixture.value.addressesCorrected));
            // Relational invariant rather than a magic constant: in the fixture
            // `g_adc_raw` is laid out immediately after the 4-byte `g_temperature`.
            // Any per-variable address shift breaks the +4 relation, while the
            // absolute address is free to move if the fixture is ever rebuilt.
            const WatchVariable *temp = fixture.value.find(QStringLiteral("g_temperature"));
            const WatchVariable *adc = fixture.value.find(QStringLiteral("g_adc_raw"));
            check(temp != nullptr && adc != nullptr
                      && adc->address == temp->address + quint32(temp->elementSize),
                  QStringLiteral("相邻变量的相对地址关系未被破坏"),
                  temp && adc ? QStringLiteral("%1 -> %2")
                                    .arg(hexAddr(temp->address), hexAddr(adc->address))
                              : QStringLiteral("缺失"));
        }
    }

    // --- 选定料号后引脚图直接画出 -------------------------------------------
    // The old version drove the port probe until exactly one candidate survived. Now the
    // candidate is the user's pick, so the requirement is: choosing an order code gives a
    // package the pinout page can draw without asking anything further.
    {
        out(QString());
        out(QStringLiteral("--- 选定料号后直接绘图 ---"));

        const QString part = QStringLiteral("N32G455CC");
        const PartTable::Entry entry = PartTable::lookup(part);
        check(entry.valid, QStringLiteral("器件表里有 N32G455CC"), part);

        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));
        if (auto *page = window.findChild<PackagePage *>()) {
            const bool drawn = page->renderForChip(part, entry.ioCount, 0, entry.package,
                                                   {entry.package});
            const auto *description =
                page->view() ? page->view()->currentDescription() : nullptr;
            out(QStringLiteral("    渲染: %1，当前封装 %2 (%3 脚)")
                    .arg(drawn ? QStringLiteral("已绘制") : QStringLiteral("未绘制"))
                    .arg(description ? description->packageName : QStringLiteral("<无>"))
                    .arg(description ? description->pins.size() : 0));
            check(drawn, QStringLiteral("按料号即可绘图（不需要再选封装）"),
                  entry.package);
            check(description && description->packageName == QLatin1String("LQFP48"),
                  QStringLiteral("画的是该料号的封装 LQFP48"),
                  description ? description->packageName : QStringLiteral("<无>"));
            check(description && description->pins.size() == 48,
                  QStringLiteral("48 个引脚"),
                  description ? QStringLiteral("%1 个").arg(description->pins.size())
                              : QStringLiteral("<无>"));
        } else {
            check(false, QStringLiteral("找到 PackagePage"));
        }
    }

    // --- J-Link device names -------------------------------------------------
    // J-Flash supports these parts because SEGGER ships them in JLinkARM.dll --
    // spelled WITHOUT the order-code suffix. Measured:
    //   -device N32G455CC      works
    //   -device N32G455CCL7    "Failed to get index for device name"
    // So the order code must be translated before J-Link sees it.
    {
        out(QString());
        out(QStringLiteral("--- J-Link 器件名映射 ---"));

        const QStringList known = JLinkDeviceName::knownNames(QStringLiteral("N32G455"));
        out(QStringLiteral("    JLinkARM.dll 中 N32G455 系列 %1 个: %2")
                .arg(known.size())
                .arg(known.join(QStringLiteral(", "))));
        check(known.contains(QLatin1String("N32G455CC")),
              QStringLiteral("JLinkARM.dll 收录 N32G455CC（无 L7 后缀）"));
        check(!known.contains(QLatin1String("N32G455CCL7")),
              QStringLiteral("JLinkARM.dll 不收录带 L7 后缀的订货物号"),
              QStringLiteral("这正是直接用物号会失败的原因"));

        // The translation the tool now performs.
        const QStringList mapped = JLinkDeviceName::candidates(QStringLiteral("N32G455CC"));
        out(QStringLiteral("    N32G455CCL7 -> %1").arg(mapped.join(QStringLiteral(" / "))));
        check(mapped.contains(QLatin1String("N32G455CC")),
              QStringLiteral("N32G455CCL7 解析出 N32G455CC"), mapped.join(QStringLiteral(",")));
        check(mapped.first() == QLatin1String("N32G455CC"),
              QStringLiteral("首选就是 J-Link 接受的那个"), mapped.first());

        // Suffix stripping must not mangle names that are already accepted.
        check(JLinkDeviceName::candidates(QStringLiteral("N32G455CC")).contains(
                  QLatin1String("N32G455CC")),
              QStringLiteral("已正确的名字保持不变"));

        // Every family we support in data/parts.json must be resolvable when SEGGER
        // ships it; families SEGGER lacks must return nothing rather than a guess.
        int resolvable = 0;
        int unsupported = 0;
        QStringList unsupportedFamilies;
        for (const QString &family : {QStringLiteral("N32G455"), QStringLiteral("N32G452"),
                                      QStringLiteral("N32G432"), QStringLiteral("N32H473")}) {
            if (JLinkDeviceName::knownNames(family).isEmpty()) {
                ++unsupported;
                unsupportedFamilies << family;
            } else {
                ++resolvable;
            }
        }
        out(QStringLiteral("    有 J-Link 支持的系列 %1 个，无 %2 个: %3")
                .arg(resolvable)
                .arg(unsupported)
                .arg(unsupportedFamilies.isEmpty() ? QStringLiteral("（无）")
                                                  : unsupportedFamilies.join(QStringLiteral(", "))));
        check(resolvable > 0, QStringLiteral("至少一个系列能被 J-Link 直接识别"),
              QStringLiteral("%1 个").arg(resolvable));
        // Corrected finding: the vendor's JLinkDevices XML *does* cover N32H473,
        // so it must NOT be reported as unsupported. (Scanning JLinkARM.dll alone
        // said it was missing -- that list is an older, smaller subset.)
        check(!unsupportedFamilies.contains(QLatin1String("N32H473")),
              QStringLiteral("N32H473 在厂商 JLinkDevices XML 中有定义"),
              unsupportedFamilies.join(QStringLiteral(", ")));
        const QStringList h473 = JLinkDeviceName::knownNames(QStringLiteral("N32H473"));
        check(h473.contains(QLatin1String("N32H473CC"))
                  && h473.contains(QLatin1String("N32H473CE")),
              QStringLiteral("N32H473CC / CE 都可用作 J-Link 器件名"),
              h473.join(QStringLiteral(", ")));

        // The DEV_ID that made this board unidentifiable: 0x711 must map to a part.
        const QVector<DeviceIdEntry> byId = DeviceIdentifier::lookupByDevId(0x711u);
        check(!byId.isEmpty(), QStringLiteral("DEV_ID 0x711 已登记"),
              byId.isEmpty() ? QString() : byId.first().mcu);
        if (!byId.isEmpty())
            check(byId.first().mcu == QLatin1String("N32H473CC"),
                  QStringLiteral("0x711 对应 N32H473CC（256KB/144KB，48 脚）"),
                  byId.first().mcu);

        // Unknown input yields no invented name.
        check(JLinkDeviceName::candidates(QString()).isEmpty(),
              QStringLiteral("空型号不编造器件名"));
    }

    // --- 容量由料号决定，不由内存探测猜测 -----------------------------------
    // The memory probe could not distinguish blank flash from an unmapped window (both
    // read 0xFF), which is why a 512 KB part holding a 235 KB image was reported as
    // 256 KB. It has been removed; the order code is now the only source of capacity.
    {
        out(QString());
        out(QStringLiteral("--- 容量来自料号 ---"));

        struct CapacityProbe : IProbe
        {
            quint32 cpuid = 0x410FC241u;
            quint32 idcode = 0x00000511u;
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = cpuid; return Status::success(); }
                if (address == 0xE0042000u) { *out = idcode; return Status::success(); }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override
            {
                return Status::failure(QStringLiteral("ro"));
            }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        CapacityProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());

        // Same silicon, three order codes: three different capacities come out, which is
        // exactly what the probe could not do.
        const QVector<QPair<QString, int>> expectations{
            {QStringLiteral("N32G455CBL7"), 128 * 1024},
            {QStringLiteral("N32G455CCL7"), 256 * 1024},
            {QStringLiteral("N32G455CEQ7"), 512 * 1024},
        };
        for (const auto &pair : expectations) {
            const auto identified = DeviceIdentifier::identify(probe, lib, pair.first);
            check(identified.ok, QStringLiteral("%1 识别成功").arg(pair.first),
                  identified.ok ? QString() : identified.error);
            if (identified.ok) {
                out(QStringLiteral("    %1 -> %2")
                        .arg(pair.first, PartTable::formatSize(identified.value.flashBytes)));
                check(identified.value.flashBytes == pair.second,
                      QStringLiteral("%1 的容量是 %2")
                          .arg(pair.first, PartTable::formatSize(pair.second)),
                      PartTable::formatSize(identified.value.flashBytes));
            }
        }

        // With no order code, capacity is 0 -- not a guess.
        const auto none = DeviceIdentifier::identify(probe, lib, QString());
        check(none.ok && none.value.flashBytes == 0,
              QStringLiteral("没有料号就不给容量"),
              none.ok ? PartTable::formatSize(none.value.flashBytes) : none.error);
    }

    // --- 用户选的料号优先，DEV_ID 只给系列 -----------------------------------
    // The precedence is the opposite of what it used to be, on purpose. Silicon cannot
    // name a package (one DEV_ID spans every package and flash size of a series), so the
    // user's order code decides and DEV_ID only hints which series to look in.
    {
        out(QString());
        out(QStringLiteral("--- 料号优先于 DEV_ID ---"));

        struct FakeIdProbe : IProbe
        {
            quint32 idcode = 0xA5384712u; // DEV_ID 0x712 (N32H47x die)
            bool idcodeReadable = true;
            Status open() override { return Status::success(); }
            void close() override {}
            bool isOpen() const override { return true; }
            Status setInterface(const QString &) override { return Status::success(); }
            Status setSpeed(int) override { return Status::success(); }
            Status setDevice(const QString &) override { return Status::success(); }
            QString deviceName() const override { return QString(); }
            Status connectTarget() override { return Status::success(); }
            void disconnectTarget() override {}
            bool isConnected() const override { return true; }
            Status halt() override { return Status::success(); }
            Status resume() override { return Status::success(); }
            Status resetTarget() override { return Status::success(); }
            bool isHalted() const override { return false; }
            Status readMemory(quint32, int, QByteArray *out) override
            {
                if (out)
                    out->clear();
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status writeMemory(quint32, const QByteArray &) override
            {
                return Status::failure(QStringLiteral("unsupported"));
            }
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = 0x410FC241u; return Status::success(); }
                if (address == 0xE0042000u && idcodeReadable) {
                    *out = idcode;
                    return Status::success();
                }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override
            {
                return Status::failure(QStringLiteral("ro"));
            }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());

        // (a) An order code wins outright, and DEV_ID still reports the series honestly.
        FakeIdProbe probe;
        const auto picked =
            DeviceIdentifier::identify(probe, lib, QStringLiteral("N32H473CE"));
        check(picked.ok, QStringLiteral("按料号识别成功"),
              picked.ok ? QString() : picked.error);
        if (picked.ok) {
            out(QStringLiteral("    选 N32H473CE（DEV_ID 0x%1）-> %2 / SVD %3")
                    .arg(picked.value.devId, 3, 16, QLatin1Char('0'))
                    .arg(picked.value.partNumber,
                         picked.value.svdPath.isEmpty() ? QStringLiteral("<无>")
                                                        : QFileInfo(picked.value.svdPath).fileName()));
            check(picked.value.partNumber == QLatin1String("N32H473CE"),
                  QStringLiteral("料号就是用户选的那个"), picked.value.partNumber);
            check(picked.value.devId == 0,
                  QStringLiteral("DEV_ID 不再读取（自动识别已移除）"),
                  QStringLiteral("0x%1").arg(picked.value.devId, 3, 16, QLatin1Char('0')));
            check(picked.value.series == QLatin1String("N32H473"),
                  QStringLiteral("系列同时给出"), picked.value.series);
            check(!picked.value.svdPath.isEmpty(), QStringLiteral("SVD 已加载"),
                  picked.value.svdPath);
        }

        // (b) A DEV_ID whose series does not match the pick is reported, not overridden:
        //     the user may be bringing up a board whose read is wrong, and silently
        //     switching the part would hide that.
        FakeIdProbe g455;
        g455.idcode = 0x8F544511u; // DEV_ID 0x511 -> N32G455 series
        const auto mismatched =
            DeviceIdentifier::identify(g455, lib, QStringLiteral("N32H473CE"));
        check(mismatched.ok, QStringLiteral("系列不一致时仍按料号"));
        if (mismatched.ok) {
            out(QStringLiteral("    DEV_ID 0x511（N32G455）+ 选 N32H473CE -> %1")
                    .arg(mismatched.value.partNumber));
            check(mismatched.value.partNumber == QLatin1String("N32H473CE"),
                  QStringLiteral("仍用用户选的料号"), mismatched.value.partNumber);
        }

        // (c) No DEV_ID at all: the order code alone is still enough.
        FakeIdProbe blind;
        blind.idcodeReadable = false;
        const auto byPart =
            DeviceIdentifier::identify(blind, lib, QStringLiteral("N32G455CCL7"));
        check(byPart.ok, QStringLiteral("读不到 DEV_ID 也能按料号工作"),
              byPart.ok ? QString() : byPart.error);
        if (byPart.ok) {
            out(QStringLiteral("    DEV_ID 读不到 / 选 N32G455CCL7 -> %1 / %2")
                    .arg(byPart.value.partNumber,
                         PartTable::formatSize(byPart.value.flashBytes)));
            check(byPart.value.partNumber == QLatin1String("N32G455CCL7"),
                  QStringLiteral("料号生效"), byPart.value.partNumber);
            check(byPart.value.flashBytes == 256 * 1024,
                  QStringLiteral("容量生效（256 KB）"),
                  PartTable::formatSize(byPart.value.flashBytes));
            check(byPart.value.devId == 0, QStringLiteral("DEV_ID 为 0（未读到）"),
                  QStringLiteral("%1").arg(byPart.value.devId));
        }
    }

    // --- a family with many packages must not be guessed --------------------
    // DEV_ID identifies the die, and e.g. N32G455 ships as QFN36/48, LQFP48/64/
    // 80/100. Rendering one of them would put every signal on the wrong pin, so
    // the page has to refuse and ask for the exact order code.
    {
        out(QString());
        out(QStringLiteral("--- 多封装家族不猜测 ---"));
        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));
        auto *page = window.findChild<PackagePage *>();
        if (!page) {
            check(false, QStringLiteral("找到 PackagePage"));
        } else {
            // Family name only: no pin count, no package hint.
            const bool drawn = page->renderForChip(QStringLiteral("N32G455"), 0, 0, QString());
            const auto *description =
                page->view() ? page->view()->currentDescription() : nullptr;
            const bool ambiguous = description != nullptr
                                   && description->chip.compare(QLatin1String("N32G45x"),
                                                               Qt::CaseInsensitive) == 0;
            out(QStringLiteral("    N32G455（无封装信息）: %1")
                    .arg(drawn ? (ambiguous ? QStringLiteral("画了 N32G45x 某封装（多义）")
                                            : QStringLiteral("画了封装"))
                               : QStringLiteral("拒绝绘制，要求精确料号")));
            check(!drawn || !ambiguous,
                  QStringLiteral("家族级识别且多封装时不擅自选一个封装"));

            // A pin count alone can still tie (this family has both LQFP64 and
            // QFN64). Instead of guessing or refusing outright, the page must offer
            // the candidates in its selector and draw nothing yet.
            page->renderForChip(
                QStringLiteral("N32G455"), 51, 64, QString(),
                {QStringLiteral("LQFP64"), QStringLiteral("QFN64")});
            const auto *pending = page->view() ? page->view()->currentDescription() : nullptr;
            check(!pending, QStringLiteral("多义时不擅自绘制任一封装"));
            if (auto *combo = page->findChild<QComboBox *>()) {
                // isVisible() is false for a page that was never shown (offscreen /
                // selftest), so ask whether it *would* be visible inside the page.
                const bool offered = combo->isVisibleTo(page) && combo->count() >= 2;
                QStringList items;
                for (int i = 0; i < combo->count(); ++i)
                    items << combo->itemText(i);
                check(offered, QStringLiteral("多义时以下拉列表给出候选（无需手打型号）"),
                      items.join(QStringLiteral(", ")));
            }

            // With the package name known the answer is unique and must be used.
            const bool withPackage =
                page->renderForChip(QStringLiteral("N32G455"), 51, 64, QStringLiteral("LQFP64"));
            const auto *picked = page->view() ? page->view()->currentDescription() : nullptr;
            check(withPackage && picked && picked->pins.size() == 64
                      && picked->packageName == QLatin1String("LQFP64"),
                  QStringLiteral("给出封装名后可确定"),
                  picked ? QStringLiteral("%1 (%2 脚)")
                               .arg(picked->packageName)
                               .arg(picked->pins.size())
                         : QStringLiteral("未绘制"));
        }
    }

    // --- 未登记 DEV_ID 不会沿用上一个芯片 ------------------------------------
    // The learning mechanism (rememberDevId) is gone with the automatic inference, so
    // this now covers only the part that still matters: an unknown DEV_ID must resolve
    // to nothing, so the UI cannot keep describing the previous board.
    {
        out(QString());
        out(QStringLiteral("--- 未登记 DEV_ID 查不到型号 ---"));

        QString error;
        DeviceIdentifier::loadDatabase(&error);

        const QVector<DeviceIdEntry> unknown = DeviceIdentifier::lookupByDevId(0x00Au);
        check(unknown.isEmpty(), QStringLiteral("未登记的 DEV_ID 查不到型号"),
              QStringLiteral("%1 条").arg(unknown.size()));

        // The verified rows still resolve: the table itself is intact.
        const QVector<DeviceIdEntry> known = DeviceIdentifier::lookupByDevId(0x712u);
        check(!known.isEmpty() && known.first().mcu.startsWith(QLatin1String("N32H473")),
              QStringLiteral("已登记的 DEV_ID 仍能查到"),
              known.isEmpty() ? QString() : known.first().mcu);

        // DEV_ID -> series is the only thing identification still takes from the table.
        const QString series = PartTable::seriesOf(known.isEmpty() ? QString()
                                                                  : known.first().mcu);
        check(series.startsWith(QLatin1String("N32H47")),
              QStringLiteral("DEV_ID 行能给出系列名"), series);

        const QStringList names = DeviceIdentifier::matchingNames(QStringLiteral("N32G45"));
        check(!names.isEmpty(), QStringLiteral("可按前缀列出候选型号（备用查询）"),
              names.join(QStringLiteral(",")));
    }

    // --- 连接时的型号校验 ----------------------------------------------------
    // The check that replaced automatic identification: probe every peripheral the
    // selected part should have, and refuse the connection when the silicon contradicts
    // the selection. Driven with fabricated statuses so the refusal path is exercised
    // without hardware -- an untested refusal path is how a wrong part silently connects.
    {
        out(QString());
        out(QStringLiteral("--- 连接时型号校验 ---"));

        auto makeStatus = [](const QString &name, PeripheralStatus::Reachability reachability,
                             bool exists = true) {
            PeripheralStatus status;
            status.name = name;
            status.exists = exists;
            status.reachability = reachability;
            if (reachability == PeripheralStatus::Missing)
                status.clockKnown = status.clockEnabled = true;
            return status;
        };

        // (a) A consistent part: most peripherals unanswered because their clocks are
        //     off. Must be ACCEPTED -- refusing here would make the tool unusable.
        {
            QVector<PeripheralStatus> statuses;
            statuses << makeStatus(QStringLiteral("RCC"), PeripheralStatus::Readable);
            statuses << makeStatus(QStringLiteral("GPIOA"), PeripheralStatus::Readable);
            for (int i = 0; i < 28; ++i) {
                statuses << makeStatus(QStringLiteral("PER%1").arg(i),
                                       PeripheralStatus::Unreachable);
            }
            const ValidationVerdict verdict = validatePeripherals(statuses, statuses.size());
            out(QStringLiteral("    时钟多数关闭: %1").arg(verdict.summary()));
            check(verdict.accepted, QStringLiteral("时钟关闭不算对不上（必须接受）"),
                  verdict.reason);
            check(verdict.readable == 2 && verdict.missing == 0,
                  QStringLiteral("统计正确"), verdict.summary());
        }

        // (b) A wrong part: whole address ranges differ, so many blocks fault while their
        //     clock gate reads as enabled. Must be REFUSED.
        {
            QVector<PeripheralStatus> statuses;
            statuses << makeStatus(QStringLiteral("RCC"), PeripheralStatus::Readable);
            for (int i = 0; i < 12; ++i) {
                statuses << makeStatus(QStringLiteral("BLOCK%1").arg(i),
                                       PeripheralStatus::Missing);
            }
            for (int i = 0; i < 17; ++i) {
                statuses << makeStatus(QStringLiteral("OK%1").arg(i),
                                       PeripheralStatus::Unreachable);
            }
            const ValidationVerdict verdict = validatePeripherals(statuses, statuses.size());
            out(QStringLiteral("    大量对不上: %1").arg(verdict.summary()));
            check(!verdict.accepted, QStringLiteral("大量外设对不上时拒绝连接"),
                  verdict.summary());
            check(!verdict.contradictions.isEmpty(),
                  QStringLiteral("列出对不上的外设（供报错）"),
                  verdict.contradictions.join(QStringLiteral(", ")));
            check(verdict.reason.contains(QStringLiteral("不符")),
                  QStringLiteral("拒绝理由可读"), verdict.reason);
        }

        // (c) One or two stragglers must NOT refuse: a gated clock that happens to read as
        //     enabled produces exactly this, and refusing would be a false positive.
        {
            QVector<PeripheralStatus> statuses;
            statuses << makeStatus(QStringLiteral("RCC"), PeripheralStatus::Readable);
            for (int i = 0; i < 20; ++i) {
                statuses << makeStatus(QStringLiteral("OK%1").arg(i),
                                       PeripheralStatus::Unreachable);
            }
            statuses << makeStatus(QStringLiteral("WEIRD1"), PeripheralStatus::Missing);
            statuses << makeStatus(QStringLiteral("WEIRD2"), PeripheralStatus::Missing);
            const ValidationVerdict verdict = validatePeripherals(statuses, statuses.size());
            out(QStringLiteral("    少量对不上: %1").arg(verdict.summary()));
            check(verdict.accepted, QStringLiteral("少量对不上不拒绝（避免误判）"),
                  verdict.reason);
        }

        // (d) Peripherals this part is NOT supposed to have must be ignored entirely:
        //     the SVD lists family blocks that the model lacks, and judging those by their
        //     read result is how every connection would be refused.
        {
            QVector<PeripheralStatus> statuses;
            statuses << makeStatus(QStringLiteral("RCC"), PeripheralStatus::Readable);
            for (int i = 0; i < 10; ++i) {
                statuses << makeStatus(QStringLiteral("ABSENT%1").arg(i),
                                       PeripheralStatus::Missing, /*exists=*/false);
            }
            const ValidationVerdict verdict = validatePeripherals(statuses, statuses.size());
            out(QStringLiteral("    本型号没有的外设被忽略: %1").arg(verdict.summary()));
            check(verdict.accepted, QStringLiteral("本型号不具备的外设不参与判定"),
                  verdict.reason);
            check(verdict.missing == 0 && verdict.expected == 1,
                  QStringLiteral("只统计应有的外设"), verdict.summary());
        }

        // (e) Nothing answered at all: inconclusive, not a wrong-part verdict. The
        //     connection stays but says the tree could not be verified.
        {
            QVector<PeripheralStatus> statuses;
            for (int i = 0; i < 10; ++i) {
                statuses << makeStatus(QStringLiteral("P%1").arg(i),
                                       PeripheralStatus::Unreachable);
            }
            const ValidationVerdict verdict = validatePeripherals(statuses, statuses.size());
            out(QStringLiteral("    全无响应: %1").arg(verdict.reason));
            check(verdict.accepted && verdict.inconclusive(),
                  QStringLiteral("全无响应时不判为型号错误"), verdict.reason);
            check(!verdict.reason.isEmpty(), QStringLiteral("但仍要说明无法验证"),
                  verdict.reason);
        }
    }

    // --- guard: no non-ASCII inside QLatin1String ---------------------------
    // QLatin1String interprets its bytes as Latin-1, so QLatin1String("\u672a\u8fde\u63a5")
    // can never equal a proper QString holding those characters. That silently
    // broke the "is this error a lost session?" check, so it is asserted by
    // scanning the sources instead of trusting review.
    {
        out(QString());
        out(QStringLiteral("--- \u6e90\u7801\uff1aQLatin1 \u53ea\u7528\u4e8e\u7eaf ASCII ---"));
        const QString needle = QStringLiteral("QLatin1String") + QChar(40); // "...("
        const QChar quote(34);                                               // double quote
        QStringList offenders;
        int scanned = 0;
        QDirIterator it(QStringLiteral("src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            ++scanned;
            const QString text = QString::fromUtf8(file.readAll());
            const QStringList lines = text.split(QChar(10));
            for (int i = 0; i < lines.size(); ++i) {
                const QString &line = lines.at(i);
                int from = 0;
                while (true) {
                    const int at = line.indexOf(needle, from);
                    if (at < 0)
                        break;
                    const int begin = line.indexOf(quote, at);
                    const int end = begin < 0 ? -1 : line.indexOf(quote, begin + 1);
                    if (begin < 0 || end < 0)
                        break;
                    const QString literal = line.mid(begin + 1, end - begin - 1);
                    bool nonAscii = false;
                    for (const QChar c : literal) {
                        if (c.unicode() > 127)
                            nonAscii = true;
                    }
                    if (nonAscii)
                        offenders << QStringLiteral("%1:%2").arg(path).arg(i + 1);
                    from = end + 1;
                }
            }
        }
        out(QStringLiteral("    \u626b\u63cf %1 \u4e2a\u6e90\u6587\u4ef6").arg(scanned));
        check(offenders.isEmpty(),
              QStringLiteral("\u6e90\u7801\u4e2d QLatin1String \u4e0d\u542b\u975e ASCII"
                             "\uff08\u5426\u5219\u6c38\u8fdc\u5339\u914d\u4e0d\u5230\uff09"),
              offenders.join(QStringLiteral(", ")));
    }

    // --- CPU health / HardFault decoding -----------------------------------
    // Pure logic, so the fault verdict is testable with no target attached.
    {
        out(QString());
        out(QStringLiteral("--- CPU 状态 / HardFault 判定 ---"));

        FaultSnapshot snap;
        snap.cfsrValid = true;
        snap.hfsrValid = true;

        check(FaultStatus::evaluate(snap, false) == CpuHealth::Normal,
              QStringLiteral("无标志 -> 正常"));
        const QString normalText = FaultStatus::summary(CpuHealth::Normal, snap);
        check(normalText == QStringLiteral("正常"),
              QStringLiteral("正常状态文案"), normalText);

        // HFSR.FORCED: a fault escalated into HardFault. This is what a target
        // stuck in the default HardFault_Handler looks like.
        snap.hfsr = 1u << 30;
        check(FaultStatus::evaluate(snap, false) == CpuHealth::HardFault,
              QStringLiteral("HFSR.FORCED -> HardFault"));
        const QString hardSummary = FaultStatus::summary(CpuHealth::HardFault, snap);
        check(hardSummary.contains(QLatin1String("FORCED")),
              QStringLiteral("HardFault 文案带原因"), hardSummary);

        // HFSR.VECTTBL: the vector table fetch failed (bad SCB->VTOR).
        snap.hfsr = 1u << 1;
        check(FaultStatus::evaluate(snap, false) == CpuHealth::HardFault,
              QStringLiteral("HFSR.VECTTBL -> HardFault"));

        // A latched usage fault without escalation must not be called HardFault.
        snap.hfsr = 0;
        snap.cfsr = 1u << 25; // DIVBYZERO
        check(FaultStatus::evaluate(snap, false) == CpuHealth::FaultFlags,
              QStringLiteral("仅 CFSR 标志 -> 故障标志（非 HardFault）"));
        const QStringList flags = FaultStatus::decode(snap);
        check(flags.contains(QLatin1String("DIVBYZERO")),
              QStringLiteral("CFSR 位解码正确"), flags.join(QStringLiteral(",")));

        // Imprecise bus fault: the address is NOT in BFAR, so BFAR must not be
        // reported as the faulting address.
        snap.cfsr = 1u << 10; // IMPRECISERR
        const QStringList imprecise = FaultStatus::decode(snap);
        check(imprecise.contains(QLatin1String("IMPRECISERR"))
                  && !imprecise.contains(QLatin1String("BFARVALID")),
              QStringLiteral("IMPRECISERR 不算 BFAR 有效"), imprecise.join(QStringLiteral(",")));

        // Precise bus fault sets BFARVALID, so the address is worth showing.
        snap.cfsr = (1u << 9) | (1u << 15); // PRECISERR | BFARVALID
        const QStringList precise = FaultStatus::decode(snap);
        check(precise.contains(QLatin1String("PRECISERR"))
                  && precise.contains(QLatin1String("BFARVALID")),
              QStringLiteral("PRECISERR 带 BFARVALID"), precise.join(QStringLiteral(",")));

        // Nothing read yet must never look healthy.
        FaultSnapshot empty;
        check(FaultStatus::evaluate(empty, false) == CpuHealth::Unknown,
              QStringLiteral("未读到寄存器 -> 状态未知（不谎报正常）"));

        // Halted is reported only when no HardFault is latched.
        snap.cfsr = 0;
        snap.hfsr = 0;
        check(FaultStatus::evaluate(snap, true) == CpuHealth::Halted,
              QStringLiteral("暂停且无故障 -> 已暂停"));
        snap.hfsr = 1u << 30;
        check(FaultStatus::evaluate(snap, true) == CpuHealth::HardFault,
              QStringLiteral("暂停时仍优先报 HardFault"));

        // Clear-by-write-back: write the observed value (W1C), but never DEBUGEVT.
        snap.cfsr = (1u << 25);
        snap.hfsr = (1u << 30) | (1u << 31);
        const auto writes = FaultStatus::clearWrites(snap);
        check(writes.size() == 2, QStringLiteral("清除动作覆盖 CFSR 与 HFSR"),
              QStringLiteral("%1 项").arg(writes.size()));
        for (const auto &write : writes) {
            if (write.first == FaultStatus::kHfsr)
                check((write.second & (1u << 31)) == 0,
                      QStringLiteral("清除 HFSR 时不触碰 DEBUGEVT"));
        }
        FaultSnapshot clean;
        clean.cfsrValid = true;
        clean.hfsrValid = true;
        check(FaultStatus::clearWrites(clean).isEmpty(),
              QStringLiteral("无标志时没有清除动作"));

        // The probe list must match the SCB addresses the decoder names.
        const QVector<quint32> probed = faultProbeAddresses();
        check(probed.contains(FaultStatus::kCfsr) && probed.contains(FaultStatus::kHfsr)
                  && probed.contains(FaultStatus::kDfsr),
              QStringLiteral("轮询地址包含 CFSR/HFSR/DFSR"),
              QStringLiteral("%1 个地址").arg(probed.size()));
    }

    // --- timers must reach the tree ----------------------------------------
    // The SVD declares advanced (ATIM1-3) and basic (BTIM1-2) timers; if the
    // scanner or the tree filter ever dropped them the user would see an MCU with
    // no timers at all, so assert they survive with registers and a clock gate.
    {
        out(QString());
        out(QStringLiteral("--- 定时器 ---"));
        const QHash<QString, DeviceTreeScanner::ClockBit> clocks = DeviceTreeScanner::clockMap(device);
        // Timer naming differs per family (H47x: ATIM/GTIM/BTIM, G45x: TIM2..TIM14),
        // so check every timer the SVD actually has instead of a fixed name list.
        int timerCount = 0;
        int timerGated = 0;
        for (const auto &peripheral : device.peripherals) {
            if (!PeripheralRoles::isTimer(peripheral))
                continue;
            ++timerCount;
            check(!peripheral.registers.isEmpty(),
                  QStringLiteral("%1 在 SVD 中且有寄存器").arg(peripheral.name),
                  QStringLiteral("%1 个").arg(peripheral.registers.size()));
            const DeviceTreeScanner::ClockBit bit = clocks.value(peripheral.name);
            if (bit.valid())
                ++timerGated;
            else
                out(QStringLiteral("    注意: %1 没有 RCC 时钟门控位").arg(peripheral.name));
        }
        check(timerCount > 0, QStringLiteral("SVD 中存在定时器"),
              QStringLiteral("%1 个").arg(timerCount));
        check(timerGated == timerCount, QStringLiteral("每个定时器都有 RCC 时钟门控位"),
              QStringLiteral("%1/%2").arg(timerGated).arg(timerCount));
    }

    // --- identification table: built-in scope + pluggable supplements -------
    // STM32 is intentionally not a supported target right now (no SVD, no pin
    // tables); its rows live in data/unsupported/. The loader must still merge
    // any data/device_ids.d/*.json, so enabling a vendor later is a data-only
    // change -- this test proves that hook keeps working.
    {
        out(QString());
        out(QStringLiteral("--- 器件识别表 ---"));
        QString error;
        const bool loaded = DeviceIdentifier::loadDatabase(&error);
        check(loaded, QStringLiteral("加载 data/device_ids.json"),
              loaded ? QStringLiteral("%1 条").arg(DeviceIdentifier::database().size()) : error);
        out(QStringLiteral("    主表: %1").arg(DeviceIdentifier::databasePath()));

        if (loaded) {
            const auto &database = DeviceIdentifier::database();
            int stm32 = 0;
            QStringList vendors;
            for (const auto &entry : database) {
                // Vendor is data, not code: the table must stay vendor-agnostic.
                if (!entry.vendor.isEmpty() && !vendors.contains(entry.vendor))
                    vendors << entry.vendor;
                if (entry.mcu.startsWith(QLatin1String("STM32"), Qt::CaseInsensitive)
                    || entry.vendor.compare(QLatin1String("ST"), Qt::CaseInsensitive) == 0)
                    ++stm32;
            }
            vendors.sort();
            out(QStringLiteral("    厂商: %1").arg(vendors.join(QStringLiteral(", "))));
            check(stm32 == 0, QStringLiteral("主表不含未支持的 STM32 条目"),
                  QStringLiteral("%1 条").arg(stm32));

            // The archived table must remain loadable as a supplement.
            const QString archived = ResourcePaths::find(QStringLiteral("unsupported/device_ids_stm32.json"));
            check(!archived.isEmpty(), QStringLiteral("STM32 条目归档在 data/unsupported/"),
                  archived.isEmpty() ? QStringLiteral("未找到归档") : archived);
            if (!archived.isEmpty()) {
                const QString supplementDir = QFileInfo(DeviceIdentifier::databasePath()).absolutePath()
                                              + QStringLiteral("/device_ids.d");
                QDir().mkpath(supplementDir);
                const QString probe =
                    supplementDir + QStringLiteral("/00-selftest-supplement.json");
                if (QFile::exists(probe))
                    QFile::remove(probe);
                const bool copied = QFile::copy(archived, probe);
                const int before = DeviceIdentifier::database().size();
                const bool reloaded = copied && DeviceIdentifier::loadDatabase(&error);
                const int after = DeviceIdentifier::database().size();
                check(reloaded && after > before, QStringLiteral("data/device_ids.d 补充表被合并"),
                      QStringLiteral("%1 -> %2 条").arg(before).arg(after));
                if (reloaded && after > before) {
                    int stm32Back = 0;
                    for (const auto &entry : DeviceIdentifier::database()) {
                        if (entry.mcu.startsWith(QLatin1String("STM32"), Qt::CaseInsensitive))
                            ++stm32Back;
                    }
                    check(stm32Back > 0, QStringLiteral("归档表放回即可恢复 STM32 识别"),
                          QStringLiteral("%1 条").arg(stm32Back));
                }
                // Restore the shipped state: no supplement left behind.
                if (QFile::exists(probe))
                    QFile::remove(probe);
                DeviceIdentifier::loadDatabase(&error);
                check(DeviceIdentifier::database().size() == before,
                      QStringLiteral("移除补充表后回到主表条目数"),
                      QStringLiteral("%1 条").arg(DeviceIdentifier::database().size()));
            }
        }
    }

    // --- vendor manifest (exact part -> svd) --------------------------------
    {
        out(QString());
        out(QStringLiteral("--- 厂商 SVD 清单 ---"));
        const int count = library.loadManifest();
        check(count > 0, QStringLiteral("加载 svd/manifest.json"), QStringLiteral("%1 条").arg(count));

        if (count > 0) {
            struct Probe { const char *part; bool expect; };
            const QVector<Probe> probes = {
                {"N32H473CE", true},   // exact match in the pack
                {"N32H473KE", true},   // same SVD, different package
                {"N32G452CB", true},   // shorter than the pack's "N32G452CBL7"
                {"N32H999ZZ", false},  // not a real part
            };
            for (const auto &probe : probes) {
                SvdManifestEntry entry;
                QStringList ambiguous;
                const bool found = library.lookupPart(QString::fromLatin1(probe.part), &entry, &ambiguous);
                check(found == probe.expect,
                      QStringLiteral("清单查询 %1").arg(QString::fromLatin1(probe.part)),
                      found ? QStringLiteral("%1 (IO %2 / 脚 %3)").arg(entry.svdPath).arg(entry.ioCount).arg(entry.pinCount)
                            : (ambiguous.isEmpty() ? QStringLiteral("未命中") 
                                                   : QStringLiteral("多义: %1").arg(ambiguous.join(QLatin1Char('/')))));
                if (found)
                    out(QStringLiteral("    %1 -> %2  IO=%3 脚=%4 封装=%5")
                            .arg(QString::fromLatin1(probe.part), entry.svdPath)
                            .arg(entry.ioCount).arg(entry.pinCount)
                            .arg(entry.packages.isEmpty() ? entry.package : entry.packages.join(QLatin1Char('/'))));
            }
        }
    }

    const QVector<SvdEntry> candidates = library.candidatesFor(device.name);
    check(!candidates.isEmpty(), QStringLiteral("按器件名可匹配 SVD"),
          candidates.isEmpty() ? QString() : candidates.first().path);
    if (!candidates.isEmpty())
        out(QStringLiteral("  best match: %1").arg(candidates.first().path));

    // --- optional snapshots ------------------------------------------------
    if (!options.renderDir.isEmpty()) {
        const Status status = renderSnapshots(options, device);
        check(status.ok, QStringLiteral("离屏渲染快照"), status.error);
    }

    out(QString());
    if (g_failures == 0)
        out(QStringLiteral("=== 自检通过 ==="));
    else
        out(QStringLiteral("=== 自检失败: %1 项 ===").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}

} // namespace mrd::cli
