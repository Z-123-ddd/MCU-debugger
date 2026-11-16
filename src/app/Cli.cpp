#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceTreeScanner.h"
#include "core/PartTable.h"
#include "core/PeripheralPresence.h"
#include "core/PeripheralRoles.h"
#include "core/ResourcePaths.h"
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
#include <limits>

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
        } else if (is("--autoconnect")) {
            o.autoConnect = true;
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
        } else if (is("--backend")) {
            const QString value = takeValue(args, &i, arg).trimmed().toLower();
            // `jlink-dll` drives the J-Link DLL directly and is the only backend that
            // can do HSS background sampling (live curves). `jlink` keeps using the
            // GDB server, which samples a halted snapshot only.
            if (value == QLatin1String("jlink") || value == QLatin1String("jlink-dll"))
                o.backend = value;
            else
                o.errors << QStringLiteral("--backend 只支持 jlink / jlink-dll");
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
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < kRounds; ++i) {
                // Same register, new value: the common polling case.
                inspector.setRegister(reg, address, quint32(i), true);
            }
            const double perRefresh = double(timer.nsecsElapsed()) / 1000.0 / kRounds;

            // Compare against a full rebuild of the same widget: an absolute
            // threshold would be tuned to one register's field count, while the
            // regression we care about is precisely "we went back to rebuilding".
            inspector.setRegister(nullptr, 0, 0, false);
            timer.restart();
            for (int i = 0; i < kRounds; ++i) {
                inspector.setRegister(nullptr, 0, 0, false);
                inspector.setRegister(reg, address, quint32(i), true);
            }
            const double perRebuild = double(timer.nsecsElapsed()) / 1000.0 / kRounds;
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

    // --- GPIO port set narrows the package ----------------------------------
    // Memory leaves N32G455 with four packages (LQFP48/64/80/100). Their port sets
    // differ (PA..PC / +PD / +PE), so probing which ports exist on the die tells
    // them apart without the user reading a marking.
    {
        out(QString());
        out(QStringLiteral("--- 按 GPIO 端口集合收窄封装 ---"));

        // The candidate packages must really differ in their port sets, otherwise
        // this whole approach would be pointless.
        const auto portsOf = [](const QString &packageName) {
            QStringList result;
            const QString dir = ResourcePaths::packageDirectory();
            for (const QString &name : QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files)) {
                const PackageDescription d = PackageDescription::loadFromFile(dir + QLatin1Char('/') + name);
                if (!d.isValid() || d.packageName != packageName)
                    continue;
                if (!d.chip.startsWith(QLatin1String("N32G45"), Qt::CaseInsensitive))
                    continue;
                const auto gpio = d.gpioPorts();
                result = QStringList(gpio.keyBegin(), gpio.keyEnd());
                result.sort();
                break;
            }
            return result;
        };

        const QStringList lqfp48 = portsOf(QStringLiteral("LQFP48"));
        const QStringList lqfp64 = portsOf(QStringLiteral("LQFP64"));
        const QStringList lqfp80 = portsOf(QStringLiteral("LQFP80"));
        const QStringList lqfp100 = portsOf(QStringLiteral("LQFP100"));
        out(QStringLiteral("    LQFP48: %1").arg(lqfp48.join(QLatin1Char(' '))));
        out(QStringLiteral("    LQFP64: %1").arg(lqfp64.join(QLatin1Char(' '))));
        out(QStringLiteral("    LQFP80: %1").arg(lqfp80.join(QLatin1Char(' '))));
        out(QStringLiteral("    LQFP100: %1").arg(lqfp100.join(QLatin1Char(' '))));

        check(lqfp48.size() < lqfp64.size(), QStringLiteral("LQFP48 端口比 LQFP64 少"),
              QStringLiteral("%1 < %2").arg(lqfp48.size()).arg(lqfp64.size()));
        check(lqfp64.size() < lqfp80.size(), QStringLiteral("LQFP64 端口比 LQFP80 少"),
              QStringLiteral("%1 < %2").arg(lqfp64.size()).arg(lqfp80.size()));
        check(lqfp64.contains(QLatin1String("PD")) && !lqfp64.contains(QLatin1String("PE")),
              QStringLiteral("LQFP64 有 PD 但没有 PE"),
              lqfp64.join(QLatin1Char(' ')));

        // Simulate the die of a 48-pin part: only PA/PB/PC answer. The exclusion rule
        // must then drop every package that needs PD or PE.
        PortProbe die;
        for (const char *port : {"PA", "PB", "PC"})
            die.present.insert(QString::fromLatin1(port));
        for (const char *port : {"PD", "PE"})
            die.absent.insert(QString::fromLatin1(port));

        QStringList survivors;
        QStringList dropped;
        for (const QString &packageName : {QStringLiteral("LQFP48"), QStringLiteral("LQFP64"),
                                          QStringLiteral("LQFP80"), QStringLiteral("LQFP100")}) {
            const QStringList ports = portsOf(packageName);
            bool impossible = false;
            for (const QString &port : ports) {
                if (die.absent.contains(port))
                    impossible = true;
            }
            if (impossible)
                dropped << packageName;
            else
                survivors << packageName;
        }
        out(QStringLiteral("    die 只有 PA/PB/PC -> 保留 %1，排除 %2")
                .arg(survivors.join(QStringLiteral(",")))
                .arg(dropped.join(QStringLiteral(","))));
        check(survivors == QStringList{QStringLiteral("LQFP48")},
              QStringLiteral("只保留能对上的 LQFP48"), survivors.join(QStringLiteral(",")));
        check(dropped.size() == 3, QStringLiteral("排除其余 3 个封装"),
              dropped.join(QStringLiteral(",")));

        // With PE present the 48/64-pin parts are ruled out, leaving 80 or 100.
        PortProbe die80;
        for (const char *port : {"PA", "PB", "PC", "PD", "PE"})
            die80.present.insert(QString::fromLatin1(port));
        die80.absent.insert(QStringLiteral("PF"));
        QStringList survivors80;
        for (const QString &packageName : {QStringLiteral("LQFP48"), QStringLiteral("LQFP64"),
                                          QStringLiteral("LQFP80"), QStringLiteral("LQFP100")}) {
            bool impossible = false;
            for (const QString &port : portsOf(packageName)) {
                if (die80.absent.contains(port))
                    impossible = true;
            }
            if (!impossible)
                survivors80 << packageName;
        }
        out(QStringLiteral("    die 有 PA..PE -> 保留 %1").arg(survivors80.join(QStringLiteral(","))));
        // Measured on real N32G455 silicon: a port the die lacks does NOT fault. Its
        // whole register block reads back as zero (GPIOA/B/C have values,
        // GPIOD..GPIOG are 0x00000000 everywhere) even with the clock enabled. So
        // "all zeros" must count as absent, exactly like a bus error -- relying on
        // faults alone reported all seven ports as present and kept four packages.
        {
            struct FakePort { quint32 value; bool readable; };
            const QVector<FakePort> ports = {
                {0x00080800u, true},  // PA: real config value -> present
                {0x00080800u, true},  // PB
                {0x00080800u, true},  // PC
                {0x00000000u, true},  // PD: readable but all zero -> absent
                {0x00000000u, true},  // PE
                {0x00000000u, false}, // PF: read fails -> absent (clock known open)
            };
            QStringList present;
            QStringList absent;
            for (int i = 0; i < ports.size(); ++i) {
                const QString port = QStringLiteral("P%1").arg(QChar('A' + i));
                if (!ports.at(i).readable) {
                    absent << port;
                } else if (ports.at(i).value != 0) {
                    present << port;
                } else {
                    absent << port;
                }
            }
            out(QStringLiteral("    全零判据: 存在 %1 / 不存在 %2")
                    .arg(present.join(QStringLiteral(",")))
                    .arg(absent.join(QStringLiteral(","))));
            check(present == (QStringList{QStringLiteral("PA"), QStringLiteral("PB"),
                                          QStringLiteral("PC")}),
                  QStringLiteral("非零读数才算端口存在"), present.join(QStringLiteral(",")));
            check(absent.contains(QLatin1String("PD"))
                      && absent.contains(QLatin1String("PE"))
                      && absent.contains(QLatin1String("PF")),
                  QStringLiteral("读回全 0 的端口判为不存在（真机实测行为）"),
                  absent.join(QStringLiteral(",")));
        }

        // A die that implements every port cannot rule anything out: the small
        // packages simply do not use them. This is the honest limit of the method --
        // it only helps when the die itself is cut down for the smaller packages,
        // which has to be measured on real silicon.
        check(survivors80.size() == 4,
              QStringLiteral("die 端口齐全时无法排除任何封装（需实测确认）"),
              survivors80.join(QStringLiteral(",")));
    }

    // --- end to end: a cut-down die resolves the package ---------------------
    // Drives the real identify() with a probe whose GPIOE faults (256K/144K, three
    // ports). N32G455CCL7 (LQFP48) must be the only survivor.
    {
        out(QString());
        out(QStringLiteral("--- 端到端：端口残缺的 die 定封装 ---"));

        struct PortProbeProbe : IProbe
        {
            // 256 KB flash / 144 KB RAM, and only PA/PB/PC implemented.
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
            // RCC_APB2PCLKEN on this family gates the GPIO ports (IOPAEN..IOPEEN).
            quint32 rccApb2 = 0; // reset: every gate closed, like real silicon
            Status readU32(quint32 address, quint32 *out) override
            {
                if (address == 0xE000ED00u) { *out = 0x410FC241u; return Status::success(); }
                if (address == 0xE0042000u) { *out = 0x8F544511u; return Status::success(); }
                const quint32 flashEnd = 0x08000000u + 256u * 1024u;
                if (address >= 0x08000000u && address < flashEnd) { *out = 0x11111111u; return Status::success(); }
                const quint32 ramEnd = 0x20000000u + 144u * 1024u;
                if (address >= 0x20000000u && address < ramEnd) { *out = 0x22222222u; return Status::success(); }
                if (address == 0x40021018u) { *out = rccApb2; return Status::success(); } // RCC_APB2PCLKEN
                // GPIO: PA..PE at 0x40010800 + n*0x400. PA/PB/PC exist; PD/PE do not,
                // and a read also needs that port's clock bit open.
                if (address >= 0x40010800u && address < 0x40011800u) {
                    const quint32 port = (address - 0x40010800u) / 0x400u;
                    if (port > 2)
                        return Status::failure(QStringLiteral("no such port"));
                    // IOPAEN starts at bit 2 on this family.
                    if ((rccApb2 & (1u << (2 + port))) == 0)
                        return Status::failure(QStringLiteral("clock off"));
                    *out = 0x00000001u;
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

        PortProbeProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto identified = DeviceIdentifier::identify(probe, lib, QString());
        if (identified.ok) {
            out(QStringLiteral("    识别结果: %1 (%2 个候选)")
                    .arg(identified.value.mcuName)
                    .arg(identified.value.familyPartCandidates.size()));
            out(QStringLiteral("    端口探测: 存在 %1 / 不存在 %2")
                    .arg(QStringList(identified.value.portProbe.present.constBegin(),
                                     identified.value.portProbe.present.constEnd())
                             .join(QStringLiteral(",")))
                    .arg(QStringList(identified.value.portProbe.absent.constBegin(),
                                     identified.value.portProbe.absent.constEnd())
                             .join(QStringLiteral(","))));
            check(!identified.value.portProbe.absent.isEmpty(),
                  QStringLiteral("探测到缺失的 GPIO 端口"),
                  QStringList(identified.value.portProbe.absent.constBegin(),
                              identified.value.portProbe.absent.constEnd())
                      .join(QStringLiteral(",")));
            check(identified.value.familyPartCandidates.size() < 4,
                  QStringLiteral("候选被端口探测收窄（< 4）"),
                  QStringLiteral("%1 个").arg(identified.value.familyPartCandidates.size()));
            // Only PA/PB/PC exist, so the 48-pin part is the only possibility.
            check(identified.value.mcuName == QLatin1String("N32G455CCL7"),
                  QStringLiteral("定位到 LQFP48 料号 N32G455CCL7"), identified.value.mcuName);
        } else {
            out(QStringLiteral("    识别未成功: %1").arg(identified.error));
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
        check(summaryText.contains(QLatin1String("N32G455")),
              QStringLiteral("摘要含型号"), summaryText);
        check(summaryText.contains(QLatin1String("DEV_ID")),
              QStringLiteral("摘要含 DEV_ID"), summaryText);
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

    // --- chip selector defaults to "auto" -----------------------------------
    // Reported: the toolbar chip selector came up as H473 by default. That value
    // was persisted from the last detection and then used as a manual override, so
    // a different board kept being interpreted as the old one.
    {
        out(QString());
        out(QStringLiteral("--- 芯片选择默认自动 ---"));

        // The setting must survive the migration as "empty = auto".
        AppSettings::migrateChipNameSetting();
        AppSettings::migrateChipNameSetting(); // idempotent
        AppSettings::setChipName(QString());
        check(AppSettings::chipName().isEmpty(),
              QStringLiteral("保存的芯片名为空即表示自动识别"));

        AppSettings::setChipName(QStringLiteral("N32G455REL7"));
        check(AppSettings::chipName() == QLatin1String("N32G455REL7"),
              QStringLiteral("显式选择仍会被记住"));
        AppSettings::setChipName(QString());

        // The selector itself. Item 0 is the auto slot and is identified by its
        // *empty data*, not by its label: the label is deliberately short ("自动",
        // then the detected part number), so asserting on Chinese text would couple
        // the test to wording the user asked to keep short.
        MainWindow window;
        if (!window.openSvdFile(options.svdPath))
            check(false, QStringLiteral("MainWindow 加载 SVD"));

        QComboBox *chipCombo = nullptr;
        for (auto *candidate : window.findChildren<QComboBox *>()) {
            if (candidate->count() > 0 && candidate->itemData(0).toString().isEmpty()
                && candidate->isEditable()) {
                chipCombo = candidate;
                break;
            }
        }
        check(chipCombo != nullptr, QStringLiteral("找到可编辑的芯片下拉框"));
        if (chipCombo) {
            check(chipCombo->currentIndex() == 0,
                  QStringLiteral("默认停在自动项（不是某个具体型号）"),
                  QStringLiteral("index=%1").arg(chipCombo->currentIndex()));
            const QString label = chipCombo->itemText(0);
            out(QStringLiteral("    自动项标签: \"%1\"（%2 字符）")
                    .arg(label)
                    .arg(label.size()));
            // Keep the field short: the user asked for the bare part number, not a
            // sentence. 16 chars covers "N32G455CE" plus any order-code suffix.
            check(label.size() <= 16,
                  QStringLiteral("芯片栏标签简短（不写说明性长句）"),
                  QStringLiteral("%1 字符").arg(label.size()));
            check(!label.contains(QChar(0xFF08)) && !label.contains(QLatin1Char('(')),
                  QStringLiteral("标签里没有括注"), label);

            // After identification the same slot shows the bare part number. Drive
            // the real handler so the display path is covered, not just the layout.
            IdentificationResult identified;
            identified.mcuName = QStringLiteral("N32G455CE");
            identified.partNumber = QStringLiteral("N32G455CE");
            identified.probeRsChip = QStringLiteral("N32G455CE");
            identified.coreName = QStringLiteral("Cortex-M4");
            // A real identification always carries the SVD it matched; without one
            // the handler (correctly) treats the result as unresolved.
            identified.svdPath = options.svdPath;
            QMetaObject::invokeMethod(&window, "onIdentifyFinished", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(QString, QString()),
                                      Q_ARG(IdentificationResult, identified));
            const QString afterLabel = chipCombo->itemText(0);
            out(QStringLiteral("    识别后自动项标签: \"%1\"").arg(afterLabel));
            check(afterLabel == QLatin1String("N32G455CE"),
                  QStringLiteral("识别后只显示完整型号"), afterLabel);
            check(chipCombo->itemData(0).toString().isEmpty(),
                  QStringLiteral("显示型号但仍是自动模式（不变成手动指定）"));
            check(chipCombo->currentIndex() == 0,
                  QStringLiteral("识别后仍停在自动项"),
                  QStringLiteral("index=%1").arg(chipCombo->currentIndex()));
        }
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

    // --- unknown DEV_ID must not dead-end -----------------------------------
    // Reported: an N32H473 board reported DEV_ID 0x711, which was not in the
    // database, and identification simply failed ("请选择具体料号") -- no SVD, no
    // tree, nothing to click. A part number is data, so running out of it must not
    // stop the tool: probe the memory signature and offer what matches.
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
                // 256K flash: last word has code, the 512K boundary is erased.
                if (address == 0x0803FFFCu) { *out = 0x6573223Au; return Status::success(); }
                if (address == 0x0807FFFCu) { *out = 0xFFFFFFFFu; return Status::success(); }
                // 144K RAM: top word is live data, 192K reads back as zero.
                if (address == 0x20023FFCu) { *out = 0x6DF2623Du; return Status::success(); }
                if (address == 0x2002FFFCu) { *out = 0x00000000u; return Status::success(); }
                return Status::failure(QStringLiteral("bus fault"));
            }
            Status writeU32(quint32, quint32) override { return Status::failure(QStringLiteral("ro")); }
            Status exec(const QString &) override { return Status::success(); }
            ProbeInfo info() const override { return ProbeInfo(); }
            QString lastError() const override { return QString(); }
        };

        UnknownIdProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto result = DeviceIdentifier::identify(probe, lib, QString());

        check(result.ok, QStringLiteral("未登记 DEV_ID 也返回结果而不是失败"),
              result.ok ? QString() : result.error);
        if (result.ok) {
            out(QStringLiteral("    内存探测: %1，候选 %2 个")
                    .arg(MemoryProbe::describe(result.value.memoryProbe))
                    .arg(result.value.familyPartCandidates.size()));
            // The memory signature must have been read correctly despite the erased
            // flash and the zero-filled upper RAM window.
            check(result.value.memoryProbe.flashBytes == 256 * 1024,
                  QStringLiteral("擦除的 Flash 上界不被当真（仍判 256KB）"),
                  QStringLiteral("%1 KB").arg(result.value.memoryProbe.flashBytes / 1024));
            check(result.value.memoryProbe.ramBytes == 144 * 1024,
                  QStringLiteral("读回 0 的 RAM 上界不被当真（仍判 144KB）"),
                  QStringLiteral("%1 KB").arg(result.value.memoryProbe.ramBytes / 1024));
            check(!result.value.familyPartCandidates.isEmpty(),
                  QStringLiteral("给出可选的料号列表（供界面下拉）"),
                  QStringLiteral("%1 个").arg(result.value.familyPartCandidates.size()));
            // Placeholder rows in parts.json (e.g. N32H785xIx7) carry no flash/RAM
            // figures at all. "Unknown on both sides" must not count as a match, or
            // they swamp every wildcard scan -- that is exactly how a 256K/144K die
            // once produced six meaningless candidates.
            bool placeholder = false;
            for (const QString &part : result.value.familyPartCandidates) {
                const PartTable::Entry entry = PartTable::lookup(part);
                if (entry.valid && entry.flashBytes <= 0 && entry.ramBytes <= 0)
                    placeholder = true;
            }
            check(!placeholder,
                  QStringLiteral("无容量数据的占位料号不进入候选"),
                  result.value.familyPartCandidates.join(QStringLiteral(", ")));
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
                check(tree->columnCount() == 5,
                      QStringLiteral("变量树有 5 列（变量/类型/地址/大小/游标值）"),
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
                if (adc)
                    adc->setExpanded(true); // a leaf: expanding must add nothing
                check(adc != nullptr && adc->text(1) == QLatin1String("uint16_t[4]")
                          && adc->childCount() == 0,
                      QStringLiteral("标量数组显示为 uint16_t[4] 且不分子节点"),
                      adc ? QStringLiteral("%1，%2 个子节点")
                                .arg(adc->text(1))
                                .arg(adc->childCount())
                          : QStringLiteral("缺失"));
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
        check(!plot.interactive(), QStringLiteral("快照模式不可缩放/拖动"), QString());
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

    // --- local snapshot: table and chart are two views of one capture --------
    // The selector page stopped being an export dialog: it now reads memory itself
    // and shows the same capture as a table or as a curve. These drive the real
    // widgets, so a wrong row count or a flat plot is caught here.
    {
        out(QString());
        out(QStringLiteral("--- 快照表格 / 趋势图表 ---"));

        // An array is the case that motivated this: `error_table[i]` is unreadable as
        // a single number and obvious as a curve.
        SnapshotReader::Snapshot snapshot;
        snapshot.rounds = 1;
        SnapshotReader::Series table;
        // The real armclang shape: the array is wrapped in an anonymous struct, so the
        // selectable leaf is named `__elems_`. The table must show the name the user
        // asked for, not the wrapper member.
        table.name = QStringLiteral("encoders[0].config_.encoder_linearization"
                                    ".error_table.__elems_");
        table.typeLabel = QStringLiteral("float[6]");
        table.address = 0x20001000u;
        table.elementCount = 6;
        table.fromArray = true;
        table.rows.append(QVector<double>{0.0, 1.5, -2.25, 3.0, 4.75, -1.0});
        snapshot.series.append(table);

        // A scalar answers the other question: how did it drift over the capture.
        SnapshotReader::Series drift;
        drift.name = QStringLiteral("pos_abs_");
        drift.typeLabel = QStringLiteral("int32_t");
        drift.address = 0x200098A0u;
        drift.elementCount = 1;
        drift.fromArray = false;
        for (int i = 0; i < 5; ++i)
            drift.rows.append(QVector<double>{double(i * 10 - 20)});
        snapshot.series.append(drift);

        check(snapshot.valueCount() == 11,
              QStringLiteral("快照值总数 = 数组元素 + 标量轮次"),
              QStringLiteral("%1 个").arg(snapshot.valueCount()));
        check(table.flat().size() == 6 && qFuzzyCompare(table.flat().at(2), -2.25),
              QStringLiteral("数组展平保留每个元素"),
              QStringLiteral("%1 个，第 3 个 = %2")
                  .arg(table.flat().size())
                  .arg(table.isEmpty() ? 0.0 : table.flat().at(2)));

        // Drive the page so the check covers the real widget, not a private helper.
        const QString axf2 = ResourcePaths::find(QStringLiteral("test/symbol_test.axf"));
        if (!axf2.isEmpty()) {
            ScopePage page2;
            page2.importSymbolFile(axf2);
            page2.showSnapshotForTest(snapshot);

            GlPlotWidget *plot = page2.plotForTest();
            check(plot != nullptr, QStringLiteral("快照页有图表视图"), QString());
            if (plot) {
                check(plot->xAxisMode() == GlPlotWidget::XAxisMode::Index,
                      QStringLiteral("图表横轴用索引（数组元素 / 采样点）"), QString());
                check(plot->timeWindow() >= 0.0, QStringLiteral("图表缩放窗口不越界"),
                      QStringLiteral("%1").arg(plot->timeWindow()));
            }
        }
    }

    // --- a single surviving candidate must draw, not ask ---------------------
    // Reported: after port probing narrowed N32G455 to one order code, the pinout
    // page still demanded a package choice. Cause: the package/pin facts were
    // resolved in the SVD step, which ran *before* the narrowing, so they stayed
    // empty and the page saw "package unknown".
    {
        out(QString());
        out(QStringLiteral("--- 唯一候选时直接绘图 ---"));

        // End-to-end with the fake probe: one order code survives, and it must come
        // back with package / pin / IO filled in.
        struct OneCandProbe : IProbe
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

        OneCandProbe probe;
        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());
        const auto result = DeviceIdentifier::identify(probe, lib, QString());
        check(result.ok, QStringLiteral("识别成功"));
        if (result.ok) {
            const auto &r = result.value;
            out(QStringLiteral("    %1 / 候选 %2 / 封装 %3 / IO %4 / 脚 %5")
                    .arg(r.mcuName)
                    .arg(r.familyPartCandidates.size())
                    .arg(r.packageName.isEmpty() ? QStringLiteral("?") : r.packageName)
                    .arg(r.ioCount)
                    .arg(r.pinCount));
            check(r.familyPartCandidates.size() == 1,
                  QStringLiteral("只剩一个候选"),
                  QStringLiteral("%1 个").arg(r.familyPartCandidates.size()));
            check(!r.packageName.isEmpty(),
                  QStringLiteral("唯一候选时封装已确定（否则绘图会要求选择）"),
                  r.packageName);
            check(r.packageName == QLatin1String("LQFP48"),
                  QStringLiteral("封装为 LQFP48"), r.packageName);
            check(r.pinCount == 48, QStringLiteral("脚数已补齐"),
                  QStringLiteral("%1").arg(r.pinCount));
            check(r.ioCount == 37, QStringLiteral("IO 数已补齐"),
                  QStringLiteral("%1").arg(r.ioCount));

            // And the page must draw instead of asking. Feed it exactly what the
            // identification produced.
            MainWindow window;
            if (!window.openSvdFile(options.svdPath))
                check(false, QStringLiteral("MainWindow 加载 SVD"));
            if (auto *page = window.findChild<PackagePage *>()) {
                const bool drawn = page->renderForChip(r.mcuName, r.ioCount, r.pinCount,
                                                       r.packageName, {});
                const auto *desc = page->view() ? page->view()->currentDescription() : nullptr;
                out(QStringLiteral("    渲染: %1, 当前封装 %2 (%3 脚)")
                        .arg(drawn ? QStringLiteral("已绘制") : QStringLiteral("未绘制"))
                        .arg(desc ? desc->packageName : QStringLiteral("<无>"))
                        .arg(desc ? desc->pins.size() : 0));
                check(drawn && desc != nullptr,
                      QStringLiteral("唯一候选时引脚图直接画出（不再要求选择）"));
                if (desc)
                    check(desc->pins.size() == 48, QStringLiteral("画出的是 48 脚封装"),
                          QStringLiteral("%1 脚").arg(desc->pins.size()));
            } else {
                check(false, QStringLiteral("找到 PackagePage"));
            }
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
        const QStringList mapped = JLinkDeviceName::candidates(QStringLiteral("N32G455CCL7"));
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

    // --- memory probe narrows a family to concrete order codes --------------
    // The CMSIS-Pack has no DEV_ID table (checked: every <device> only carries
    // description/memory/algorithm/debug), and the datasheets have no ID table
    // either. So flash/RAM is read from the part itself to cut the 15 order codes
    // of one family down to the ones that can actually be attached.
    {
        out(QString());
        out(QStringLiteral("--- 内存探测缩小料号 ---"));

        // Pure matching logic: unknown on either side never rules a part out.
        MemoryProbe::Result known;
        known.flashBytes = 512 * 1024;
        known.ramBytes = 144 * 1024;
        check(MemoryProbe::matches(known, 512 * 1024, 144 * 1024),
              QStringLiteral("容量一致 -> 匹配"));
        check(!MemoryProbe::matches(known, 256 * 1024, 144 * 1024),
              QStringLiteral("Flash 不同 -> 排除"));
        check(!MemoryProbe::matches(known, 512 * 1024, 80 * 1024),
              QStringLiteral("RAM 不同 -> 排除"));
        MemoryProbe::Result unknown;
        check(MemoryProbe::matches(unknown, 128 * 1024, 80 * 1024),
              QStringLiteral("探测失败不排除任何料号（不猜）"));

        // Candidate lists must be ordered large -> small so the first readable
        // boundary is the real size.
        const QVector<int> flash = MemoryProbe::flashCandidates();
        const QVector<int> ram = MemoryProbe::ramCandidates();
        bool sorted = true;
        for (int i = 1; i < flash.size(); ++i)
            sorted = sorted && flash.at(i) < flash.at(i - 1);
        for (int i = 1; i < ram.size(); ++i)
            sorted = sorted && ram.at(i) < ram.at(i - 1);
        check(sorted, QStringLiteral("候选容量按从大到小排列，首个可读即真实大小"),
              QStringLiteral("%1 / %2 项").arg(flash.size()).arg(ram.size()));

        // Family narrowing: N32G455 has 15 order codes; one memory signature must
        // leave only the ones with that signature.
        const QStringList all455 = DeviceIdentifier::familyCandidates(
            QStringLiteral("N32G455"), MemoryProbe::Result());
        check(all455.size() == 15, QStringLiteral("N32G455 共 15 个料号"),
              QStringLiteral("%1 个").arg(all455.size()));

        MemoryProbe::Result r512;
        r512.flashBytes = 512 * 1024;
        r512.ramBytes = 144 * 1024;
        const QStringList narrowed = DeviceIdentifier::familyCandidates(
            QStringLiteral("N32G455"), r512);
        out(QStringLiteral("    N32G455 + Flash512K/RAM144K -> %1 个: %2")
                .arg(narrowed.size())
                .arg(narrowed.join(QStringLiteral(", "))));
        check(narrowed.size() > 0 && narrowed.size() < all455.size(),
              QStringLiteral("内存探测确实缩小了范围"),
              QStringLiteral("%1 -> %2").arg(all455.size()).arg(narrowed.size()));

        // Every remaining candidate must really have that memory.
        bool consistent = true;
        for (const QString &part : narrowed) {
            const PartTable::Entry entry = PartTable::lookup(part);
            consistent = consistent && entry.valid
                         && MemoryProbe::matches(r512, entry.flashBytes, entry.ramBytes);
        }
        check(consistent, QStringLiteral("剩余候选的容量与探测一致"));

        // A nonsense signature must leave nothing rather than everything.
        MemoryProbe::Result bogus;
        bogus.flashBytes = 7 * 1024 * 1024; // no N32 has 7 MB
        bogus.ramBytes = 3 * 1024 * 1024;
        check(DeviceIdentifier::familyCandidates(QStringLiteral("N32G455"), bogus).isEmpty(),
              QStringLiteral("不存在的容量组合 -> 无候选（不静默退回全集）"));
    }

    // --- DEV_ID beats a stale chip name -------------------------------------
    // The toolbar chip name is filled from the last identification, so it names the
    // *previous* board. It used to short-circuit detection, which made a newly
    // plugged G455 keep reporting the old H473 (and the log with it).
    // This drives the real identify() path with a fake probe.
    {
        out(QString());
        out(QStringLiteral("--- DEV_ID 优先于界面芯片名 ---"));

        // Minimal probe: only the IDCODE/CPUID reads succeed.
        struct FakeIdProbe : IProbe
        {
            quint32 idcode = 0xA5384712u; // DEV_ID 0x712 = N32H473CE
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
                if (address == 0xE000ED00u) {
                    *out = 0x410FC241u; // Cortex-M4
                    return Status::success();
                }
                if (address == 0xE0042000u && idcodeReadable) {
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

        SvdLibrary lib;
        lib.setRoots(AppSettings::svdRoots());

        // (a) the chip box says N32G455 (stale/last used) but the silicon is H473.
        FakeIdProbe probe;
        const auto result = DeviceIdentifier::identify(
            probe, lib, QStringLiteral("N32G455"));
        check(result.ok, QStringLiteral("识别流程成功"));
        if (result.ok) {
            const QString name = result.value.mcuName;
            out(QStringLiteral("    界面填 N32G455 / 实际 DEV_ID 0x712 -> 识别为 %1").arg(name));
            check(name.startsWith(QLatin1String("N32H473")),
                  QStringLiteral("DEV_ID 覆盖界面上的旧芯片名"), name);
            check(result.value.devId == 0x712u, QStringLiteral("DEV_ID 读到了"),
                  QStringLiteral("0x%1").arg(result.value.devId, 3, 16, QLatin1Char('0')));
        }

        // (b) the user's real G455 board: IDCODE 0x8F544511 -> DEV_ID 0x511.
        FakeIdProbe g455;
        g455.idcode = 0x8F544511u;
        const auto g455Result = DeviceIdentifier::identify(g455, lib, QString());
        check(g455Result.ok, QStringLiteral("G455 板识别流程成功"));

        if (g455Result.ok) {
            out(QStringLiteral("    DEV_ID 0x511 -> %1 (SVD %2)")
                    .arg(g455Result.value.mcuName,
                         g455Result.value.svdPath.isEmpty() ? QStringLiteral("<无>")
                                                           : QFileInfo(g455Result.value.svdPath).fileName()));
            check(g455Result.value.mcuName.startsWith(QLatin1String("N32G455")),
                  QStringLiteral("DEV_ID 0x511 识别为 N32G455"), g455Result.value.mcuName);
            check(g455Result.value.svdPath.contains(QLatin1String("N32G455.svd")),
                  QStringLiteral("0x511 匹配到 N32G455 的 SVD"), g455Result.value.svdPath);
        }

        // (c) no IDCODE readable: the typed name is the only hint, so it is used.
        FakeIdProbe blind;
        blind.idcodeReadable = false;
        const auto manual = DeviceIdentifier::identify(
            blind, lib, QStringLiteral("N32G455"));
        if (manual.ok) {
            out(QStringLiteral("    DEV_ID 读不到 / 界面填 N32G455 -> 识别为 %1")
                    .arg(manual.value.mcuName));
            check(manual.value.mcuName.startsWith(QLatin1String("N32G455")),
                  QStringLiteral("读不到 DEV_ID 时退回手动指定"), manual.value.mcuName);
            check(manual.value.manuallySelected,
                  QStringLiteral("标记为手动指定（界面需提示）"));
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

    // --- unknown DEV_ID must not inherit the previous chip -------------------
    // Reported symptom: a G455 was connected but the window still showed the old
    // H473, because identification "succeeded" with no part and the stale SVD
    // stayed loaded. These assertions cover the database side of that path.
    {
        out(QString());
        out(QStringLiteral("--- 未知型号 / DEV_ID 学习 ---"));

        QString error;
        DeviceIdentifier::loadDatabase(&error);
        const int before = DeviceIdentifier::database().size();

        // A DEV_ID nobody registered must resolve to nothing: silence here is what
        // used to make the UI keep the previous part.
        const QVector<DeviceIdEntry> unknown = DeviceIdentifier::lookupByDevId(0x00Au);
        check(unknown.isEmpty(), QStringLiteral("未登记的 DEV_ID 查不到型号"),
              QStringLiteral("%1 条").arg(unknown.size()));

        // The verified row still resolves, so learning cannot have broken lookup.
        const QVector<DeviceIdEntry> known = DeviceIdentifier::lookupByDevId(0x712u);
        check(!known.isEmpty() && known.first().mcu.startsWith(QLatin1String("N32H473")),
              QStringLiteral("已登记的 DEV_ID 仍能查到"), known.isEmpty() ? QString() : known.first().mcu);

        // Learning a pairing: writes into the writable supplement, never over an
        // existing row.
        const QString learnedPath = ResourcePaths::learnedDeviceIdPath();
        out(QStringLiteral("    学习文件: %1").arg(learnedPath));
        check(!learnedPath.isEmpty() && learnedPath.contains(QLatin1String("device_ids.d")),
              QStringLiteral("学习文件位于可写的 device_ids.d"));

        // Refuse to steal an existing DEV_ID (that would misidentify real parts).
        QString stealError;
        const bool stole = DeviceIdentifier::rememberDevId(0x712u, QStringLiteral("N32G455"), &stealError);
        check(!stole, QStringLiteral("不会覆盖已登记的 DEV_ID"), stealError);

        // A genuinely new DEV_ID is accepted, then resolvable after reload.
        const quint32 fakeId = 0x7FFu; // not used by any real N32 part in the table
        bool fakePresent = false;
        for (const auto &entry : DeviceIdentifier::database()) {
            if (entry.devId == fakeId)
                fakePresent = true;
        }
        if (fakePresent) {
            out(QStringLiteral("    （0x7FF 已在库中，跳过写入测试）"));
        } else {
            QString learnError;
            const bool learned =
                DeviceIdentifier::rememberDevId(fakeId, QStringLiteral("N32G455"), &learnError);
            check(learned, QStringLiteral("新 DEV_ID 可被记住"), learnError);
            if (learned) {
                const QVector<DeviceIdEntry> resolved = DeviceIdentifier::lookupByDevId(fakeId);
                check(!resolved.isEmpty()
                          && resolved.first().mcu == QLatin1String("N32G455"),
                      QStringLiteral("记住后立即可查到"),
                      resolved.isEmpty() ? QString() : resolved.first().mcu);
                check(DeviceIdentifier::database().size() == before + 1,
                      QStringLiteral("学习后数据库条目 +1"),
                      QStringLiteral("%1 -> %2").arg(before).arg(DeviceIdentifier::database().size()));
            }
            // Leave no trace: the test must not pollute the user's learned table.
            QFile::remove(learnedPath);
            DeviceIdentifier::loadDatabase(&error);
            check(DeviceIdentifier::database().size() == before,
                  QStringLiteral("清理学习文件后条目数恢复"),
                  QStringLiteral("%1").arg(DeviceIdentifier::database().size()));
        }

        // Name suggestions for the toolbar (used when the user has to pick).
        const QStringList g45 = DeviceIdentifier::matchingNames(QStringLiteral("N32G45"));
        check(!g45.isEmpty(), QStringLiteral("可按前缀列出候选型号"),
              g45.join(QStringLiteral(",")));
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
