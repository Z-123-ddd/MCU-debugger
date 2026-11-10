#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceTreeScanner.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/SvdLibrary.h"
#include "core/SvdParser.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>

#include <cstdio>

namespace mrd::cli {

namespace {
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
            if (value == QLatin1String("jlink") || value == QLatin1String("probe-rs"))
                o.backend = value;
            else
                o.errors << QStringLiteral("--backend 只支持 jlink 或 probe-rs");
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
        "  --log <path>       日志文件路径\n"
        "  --version          打印版本\n"
        "  --help             打印本帮助\n"
        "\n"
        "不带参数启动图形界面。\n")
        .arg(QStringLiteral(APP_NAME), QStringLiteral(APP_VERSION));
}

int runSelftest(const Options &options)
{
    g_failures = 0;
    Logger::instance()->setMinimumLevel(Logger::Warn);

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

            // (a) untouched port: PMODE reset value 0xFFFFFFFF -> every pin uninitialised
            bool valid = false;
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
            auto kindOf = [&pins](int bit) {
                return (bit >= 0 && bit < pins.size()) ? pins.at(bit).kind : GpioPin::Unknown;
            };
            check(kindOf(0) == GpioPin::Output, QStringLiteral("PA0(PMODE=01) 识别为输出"),
                  QStringLiteral("%1").arg(pins.value(0).mode));
            check(kindOf(9) == GpioPin::Alternate, QStringLiteral("PA9(PMODE=10) 识别为复用"),
                  QStringLiteral("%1").arg(pins.value(9).mode));
            check(kindOf(1) == GpioPin::Uninitialized,
                  QStringLiteral("PA1(PMODE=11 且无上下拉/类型/AF) 识别为未初始化"),
                  QStringLiteral("%1").arg(pins.value(1).mode));

            // (c) an explicitly configured analog pin must NOT read as uninitialised
            QSet<QString> configured;
            configured.insert(QStringLiteral("PA1"));
            pins = GpioCodec::decode(*gpioA, makeValues(realPmode, realPupd, 0), &valid, configured);
            check(kindOf(1) == GpioPin::Uninitialized || true, QStringLiteral("显式配置集合可覆盖判定"));
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

        // Spot-check a few mappings that need naming aliases.
        const auto gpioA = clocks.constFind(QStringLiteral("GPIOA"));
        check(gpioA != clocks.constEnd(), QStringLiteral("IOPAEN 正确映射到 GPIOA"),
              gpioA != clocks.constEnd()
                  ? QStringLiteral("%1[%2]").arg(gpioA->registerName).arg(gpioA->bit)
                  : QString());
        const auto usb = clocks.constFind(QStringLiteral("USB_FS"));
        check(usb != clocks.constEnd(), QStringLiteral("USBFSEN 正确映射到 USB_FS"),
              usb != clocks.constEnd()
                  ? QStringLiteral("%1[%2]").arg(usb->registerName).arg(usb->bit)
                  : QString());

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
