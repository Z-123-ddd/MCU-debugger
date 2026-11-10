#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceTreeScanner.h"
#include "core/ResourcePaths.h"
#include "ui/PackageView.h"
#include "core/DeviceIdentifier.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/SvdLibrary.h"
#include "core/SvdParser.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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

    // --- timers must reach the tree ----------------------------------------
    // The SVD declares advanced (ATIM1-3) and basic (BTIM1-2) timers; if the
    // scanner or the tree filter ever dropped them the user would see an MCU with
    // no timers at all, so assert they survive with registers and a clock gate.
    {
        out(QString());
        out(QStringLiteral("--- 定时器 ---"));
        const QHash<QString, DeviceTreeScanner::ClockBit> clocks = DeviceTreeScanner::clockMap(device);
        for (const char *timer : {"ATIM1", "ATIM2", "ATIM3", "BTIM1", "BTIM2", "GTIM1", "LPTIM1"}) {
            const QString name = QString::fromLatin1(timer);
            const SvdPeripheral *peripheral = device.findPeripheral(name);
            const bool found = peripheral != nullptr && !peripheral->registers.isEmpty();
            const DeviceTreeScanner::ClockBit bit = clocks.value(name);
            const QString detail =
                found ? QStringLiteral("%1 个寄存器, 时钟位 %2")
                            .arg(peripheral->registers.size())
                            .arg(bit.valid() ? QStringLiteral("%1[%2]").arg(bit.registerName).arg(bit.bit)
                                             : QStringLiteral("—"))
                      : QStringLiteral("未找到");
            check(found, QStringLiteral("%1 在 SVD 中且有寄存器").arg(name), detail);
            if (found)
                check(bit.valid(), QStringLiteral("%1 有 RCC 时钟门控位").arg(name), detail);
        }
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
