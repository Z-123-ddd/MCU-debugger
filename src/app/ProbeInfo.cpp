#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/JLinkDllProbe.h"
#include "core/JLinkGdbProbe.h"
#include "core/JLinkRunner.h"
#include "core/Logger.h"
#include "core/HssSampler.h"
#include "core/SampleHistory.h"
#include "core/ProbeController.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include "core/SymbolFile.h"
#include "core/SvdLibrary.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <QFileInfo>
#include <QThread>

#include <memory>

#include <cstdio>

namespace mrd::cli {

namespace {

int g_failures = 0;

void out(const QString &line)
{
    std::fprintf(stdout, "%s\n", qPrintable(line));
    std::fflush(stdout);
}

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

/// Restart the core after a diagnostic read (J-Link leaves it in whatever state
/// the connect sequence produced).
void dumpGpioPorts(IProbe &probe, const SvdDevice &device, const QString &onlyPort)
{
    int dumped = 0;
    for (const auto &peripheral : device.peripherals) {
        if (!GpioCodec::isGpioPort(peripheral))
            continue;
        const QString port = GpioCodec::portName(peripheral);
        if (port.isEmpty())
            continue;
        if (!onlyPort.isEmpty() && port.compare(onlyPort, Qt::CaseInsensitive) != 0)
            continue;

        QHash<quint32, quint32> values;
        for (const auto &reg : peripheral.registers) {
            quint32 value = 0;
            if (probe.readU32(peripheral.baseAddress + reg.addressOffset, &value))
                values.insert(peripheral.baseAddress + reg.addressOffset, value);
        }

        bool valid = false;
        const QVector<GpioPin> pins = GpioCodec::decode(peripheral, values, &valid);
        if (pins.isEmpty())
            continue;

        out(QStringLiteral("  %1 @ %2%3").arg(peripheral.name, hexAddr(peripheral.baseAddress),
                                              valid ? QString() : QStringLiteral("  (无有效读数)")));
        for (const auto &pin : pins) {
            const QString level = (pin.kind == GpioPin::Output)
                                      ? (pin.outputHigh ? QStringLiteral("ODR=1") : QStringLiteral("ODR=0"))
                                      : (pin.inputHigh ? QStringLiteral("IDR=1") : QStringLiteral("IDR=0"));
            out(QStringLiteral("    %1  %2 %3  速度/DS=%4  %5")
                    .arg(pin.name().leftJustified(5, QLatin1Char(' ')), pin.mode, pin.subMode, pin.speed, level));
        }
        ++dumped;
        if (dumped >= 1 && !onlyPort.isEmpty())
            break;
    }
    if (dumped == 0)
        out(QStringLiteral("  （没有找到 GPIO 端口）"));
}

} // namespace

int runProbeInfo(const Options &options)
{
    // --verbose keeps the per-step detail (identification, memory/port probes).
    Logger::instance()->setMinimumLevel(options.verbose ? Logger::Debug : Logger::Warn);
    g_failures = 0;

    const QString backend = !options.backend.isEmpty()
                                ? options.backend
                                : AppSettings::probeBackend();

    out(QStringLiteral("=== 探针后端: %1 ===").arg(backend));

    std::unique_ptr<IProbe> probe;
    QString serial;

    if (backend == QLatin1String("jlink-dll")) {
        // No external executable: the DLL is loaded in-process, which is what makes
        // HSS background sampling reachable at all.
        auto dllProbe = std::make_unique<JLinkDllProbe>();
        const Status opened = dllProbe->open();
        check(static_cast<bool>(opened), QStringLiteral("加载 J-Link DLL"),
              opened ? dllProbe->info().binaryPath : opened.error);
        if (!opened)
            return 4;
        auto *api = dllProbe->api();
        check(api->hasHss(), QStringLiteral("该 DLL 提供 HSS 接口"),
              api->hasHss() ? QStringLiteral("JLINK_HSS_Start/Read/Stop 可解析")
                            : QStringLiteral("缺少 HSS 导出"));
        out(QStringLiteral("[OK] 后端 jlink-dll | %1").arg(dllProbe->api()->libraryPath()));
        probe = std::move(dllProbe);
    } else {
        const QString executable = JLinkRunner::locate(AppSettings::jlinkPath());
        if (executable.isEmpty()) {
            out(QString());
            out(QStringLiteral("[ERROR] 未找到 JLink.exe（SEGGER J-Link 软件未安装？）"));
            return 4;
        }

        const auto info = JLinkRunner::query(executable,
                                            options.deviceName.isEmpty() ? QStringLiteral("Cortex-M4")
                                                                          : options.deviceName);
        if (!info) {
            out(QString());
            out(QStringLiteral("[ERROR] %1").arg(info.error));
            return 4;
        }
        // Version + probe identity only: the install path is noise here.
        out(QStringLiteral("[OK] J-Link %1 | DLL %2 | 固件 %3 | 硬件 %4 | S/N %5 | %6")
                .arg(info.value.commander, info.value.dllVersion, info.value.firmware, info.value.hardware,
                     info.value.serial.isEmpty() ? QStringLiteral("?") : info.value.serial, info.value.product));
        check(info.value.probeFound, QStringLiteral("发现 J-Link 硬件"),
              info.value.probeFound ? info.value.serial : QStringLiteral("请检查 USB 连接"));

        auto jlink = std::make_unique<JLinkGdbProbe>();
        jlink->setExecutable(executable);
        serial = info.value.serial;
        probe = std::move(jlink);
    }

    // --- library / database ------------------------------------------------
    QString dbError;
    const bool dbLoaded = DeviceIdentifier::loadDatabase(&dbError);
    out(QString());
    check(dbLoaded, QStringLiteral("器件识别库"),
          dbLoaded ? QStringLiteral("%1 条").arg(DeviceIdentifier::database().size())
                   : dbError);

    // --- connect -----------------------------------------------------------
    probe->setInterface(options.interfaceName.isEmpty() ? AppSettings::interfaceName() : options.interfaceName);
    probe->setSpeed(options.speedKHz > 0 ? options.speedKHz : AppSettings::speedKHz());
    // Without an explicit chip we attach with a generic core first and let the
    // DEV_ID lookup decide; that keeps a wrong guess from breaking connect.
    const QString requestedChip = options.deviceName.isEmpty() ? QStringLiteral("Cortex-M4")
                                                               : options.deviceName;
    probe->setDevice(requestedChip);

    out(QString());
    out(QStringLiteral("--- 连接目标 ---"));
    const Status connected = probe->connectTarget();
    check(static_cast<bool>(connected), QStringLiteral("连接目标"),
          connected ? QString() : connected.error);
    if (!connected) {
        probe->close();
        out(QString());
        out(QStringLiteral("=== 连接失败，退出码 3 ==="));
        return 3;
    }

    const ProbeInfo probeInfo = probe->info();
    out(QStringLiteral("接口 %1 @ %2 kHz | 器件 %3 | CPU 状态 %4")
            .arg(probeInfo.interfaceName)
            .arg(probeInfo.speedKHz)
            .arg(probeInfo.detectedChip.isEmpty() ? probeInfo.chipName : probeInfo.detectedChip,
                 probeInfo.halted ? QStringLiteral("halted") : QStringLiteral("running")));

    // --- core / idcode -----------------------------------------------------
    quint32 cpuid = 0;
    if (probe->readU32(0xE000ED00u, &cpuid))
        out(QStringLiteral("CPUID     : %1  (%2)").arg(hex32(cpuid), DeviceIdentifier::coreNameFromCpuid(cpuid)));
    else
        out(QStringLiteral("[WARN] CPUID 读取失败"));

    bool haveIdCode = false;
    for (const quint32 address : DeviceIdentifier::idcodeAddresses()) {
        quint32 value = 0;
        // Ignore neighbours in the CoreSight block: a real IDCODE has a
        // non-zero revision field in bits[31:16].
        if (!probe->readU32(address, &value) || value == 0u || value == 0xFFFFFFFFu || (value >> 16) == 0u)
            continue;
        out(QStringLiteral("IDCODE    : %1 = %2  (DEV_ID=0x%3, REV_ID=0x%4)")
                .arg(hexAddr(address), hex32(value))
                .arg(value & 0xFFFu, 3, 16, QLatin1Char('0'))
                .arg((value >> 16) & 0xFFFFu, 4, 16, QLatin1Char('0')));
        haveIdCode = true;
    }
    if (!haveIdCode)
        out(QStringLiteral("[WARN] 未读到有效 IDCODE（DBGMCU 可能未使能）"));

    // --- identification ----------------------------------------------------
    SvdLibrary library;
    library.setRoots(AppSettings::svdRoots());
    library.scan(false);

    auto identified = DeviceIdentifier::identify(*probe, library, options.deviceName, serial);

    // Two-phase: re-attach with the resolved part number so J-Link applies the
    // right flash/AP settings (also proves the name is valid).
    // Prefer the name J-Link accepts: SEGGER stores N32G455CC, not the order code
    // N32G455CCL7, so handing it the latter fails with "Failed to get index".
    QString targetDevice;
    if (identified) {
        // Only the name measured from JLinkARM.dll. The `probeRsChip` alias looks
        // similar ("N32G455") but belongs to a different catalogue, and handing it to
        // the DLL crashes V8.26.
        targetDevice = identified.value.jlinkDeviceName;
    }

    if (identified && options.deviceName.isEmpty() && !targetDevice.isEmpty()
        && targetDevice.compare(requestedChip, Qt::CaseInsensitive) != 0) {
        out(QString());
        out(QStringLiteral("按识别结果重新以 %1 连接...").arg(targetDevice));
        probe->setDevice(targetDevice);
        Status reconnect = probe->connectTarget();
        // setDevice() stops the GDB server, so a failed re-attach leaves the probe
        // dead -- every later read then fails with "GDB Server 未连接" and the whole
        // device tree looks empty.
        if (!reconnect) {
            // Try the other plausible spellings before giving up: the plain order
            // code and the suffix-stripped form are both used across J-Link versions.
            for (const QString &candidate : identified.value.jlinkDeviceCandidates) {
                if (candidate.compare(targetDevice, Qt::CaseInsensitive) == 0)
                    continue;
                out(QStringLiteral("以 %1 失败，改试 %2").arg(targetDevice, candidate));
                probe->setDevice(candidate);
                reconnect = probe->connectTarget();
                if (reconnect) {
                    targetDevice = candidate;
                    break;
                }
            }
        }
        if (!reconnect) {
            out(QStringLiteral("[WARN] J-Link 不接受 %1（该系列可能未收录在 JLinkARM.dll 中）")
                    .arg(targetDevice));
            out(QStringLiteral("       退回通用 Cortex-M4 保持会话可用（读寄存器不受影响）"));
            probe->setDevice(QStringLiteral("Cortex-M4"));
            reconnect = probe->connectTarget();
            if (!reconnect)
                out(QStringLiteral("[ERROR] 通用 Cortex-M4 也无法重连: %1").arg(reconnect.error));
        }
        check(static_cast<bool>(reconnect), QStringLiteral("重连后会话可用"),
              reconnect ? targetDevice : reconnect.error);
        if (reconnect) {
            const ProbeInfo again = probe->info();
            out(QStringLiteral("J-Link 解析器件: %1").arg(again.detectedChip.isEmpty()
                                                             ? QStringLiteral("-")
                                                             : again.detectedChip));
            // Re-resolve against the now-known part number (no second IDCODE probe:
            // the manual path is deterministic and avoids a fresh-attach race).
            identified = DeviceIdentifier::identify(*probe, library, targetDevice, serial);
        }
    }

    out(QString());
    SvdDevice device;
    if (!identified) {
        check(false, QStringLiteral("MCU 型号识别"), identified.error);
    } else {
        const IdentificationResult &r = identified.value;
        out(QStringLiteral("识别结果  : %1  家族=%2  厂商=%3  核=%4%5")
                .arg(r.mcuName.isEmpty() ? QStringLiteral("-") : r.mcuName,
                     r.family.isEmpty() ? QStringLiteral("-") : r.family,
                     r.vendor.isEmpty() ? QStringLiteral("-") : r.vendor,
                     r.coreName.isEmpty() ? QStringLiteral("-") : r.coreName,
                     r.manuallySelected ? QStringLiteral(" (手动)") : QString()));
        // What J-Link is actually driving, which is not the same as the part
        // number: SEGGER ships no N32G455 entry, so the session runs as a generic
        // Cortex-M4. Saying otherwise would imply J-Link recognised the part.
        const ProbeInfo live = probe->info();
        out(QStringLiteral("后端器件名: %1（器件名 %2）")
                .arg(live.chipName.isEmpty() ? QStringLiteral("Cortex-M4") : live.chipName,
                     r.probeRsChip));
        out(QStringLiteral("SVD       : %1")
                .arg(r.svdPath.isEmpty() ? QStringLiteral("<未匹配>")
                                         : QFileInfo(r.svdPath).fileName()));
        if (!r.note.isEmpty())
            out(QStringLiteral("备注      : %1").arg(r.note));
        check(!r.svdPath.isEmpty(), QStringLiteral("匹配到 SVD 描述文件"),
              QFileInfo(r.svdPath).fileName());

        if (!r.svdPath.isEmpty()) {
            const auto parsed = library.load(r.svdPath);
            if (parsed) {
                device = parsed.value;
                check(true, QStringLiteral("SVD 已解析"),
                      QStringLiteral("%1 外设 / %2 寄存器 / %3 字段")
                          .arg(device.peripherals.size())
                          .arg(device.totalRegisters())
                          .arg(device.totalFields()));
            } else {
                check(false, QStringLiteral("SVD 已解析"), parsed.error);
            }
        }
    }

    // --- real device tree --------------------------------------------------
    if (!device.peripherals.isEmpty()) {
        out(QString());
        out(QStringLiteral("--- 设备树（真实外设，时钟门控 + 总线可访问性） ---"));
        const QVector<PeripheralStatus> statuses = DeviceTreeScanner::scan(*probe, device);

        int onChip = 0;
        int clocked = 0;
        int missing = 0;
        int unreachable = 0;
        for (const auto &status : statuses) {
            if (status.onChip())
                ++onChip;
            if (status.clockKnown && status.clockEnabled)
                ++clocked;
            if (status.reachability == PeripheralStatus::Missing)
                ++missing;
            if (status.reachability == PeripheralStatus::Unreachable)
                ++unreachable;
        }

        out(QStringLiteral("  合计 %1: 本芯片存在 %2 / 缺失 %3 / 无法访问 %4 / 时钟已开 %5")
                .arg(statuses.size())
                .arg(onChip)
                .arg(missing)
                .arg(unreachable)
                .arg(clocked));
        check(missing == 0, QStringLiteral("SVD 中的外设均被本芯片实现"),
              QStringLiteral("缺失 %1 个").arg(missing));

        out(QString());
        out(QStringLiteral("  外设              时钟  访问"));
        for (const auto &status : statuses) {
            QString clock = QStringLiteral(" —  ");
            if (status.clockKnown)
                clock = status.clockEnabled ? QStringLiteral("  开") : QStringLiteral("  关");
            QString access;
            switch (status.reachability) {
            case PeripheralStatus::Readable:
                access = QStringLiteral("%1 = %2").arg(status.probeRegister, hex32(status.probeValue));
                break;
            case PeripheralStatus::Missing:
                access = QStringLiteral("总线拒绝（时钟已开 -> 本芯片未实现）");
                break;
            case PeripheralStatus::Unreachable:
                access = status.clockKnown && !status.clockEnabled
                             ? QStringLiteral("总线拒绝（时钟未开）")
                             : QStringLiteral("总线拒绝");
                break;
            }
            out(QStringLiteral("  %1 %2  %3").arg(status.name, -16).arg(clock, access));
        }
    }

    // --- live register dump ------------------------------------------------
    if (!device.peripherals.isEmpty()) {
        out(QString());
        out(QStringLiteral("--- GPIO 实时读取（验证寄存器级访问） ---"));
        dumpGpioPorts(*probe, device, options.portName);
    }

    // --- write path --------------------------------------------------------
    // Round-trip through SRAM: reads the original value first and restores it, so
    // the check is non-destructive while still exercising the write path.
    {
        out(QString());
        out(QStringLiteral("--- 写入验证（SRAM 回环，非破坏性） ---"));
        constexpr quint32 kScratch = 0x20000000u;
        quint32 original = 0;
        if (!probe->readU32(kScratch, &original)) {
            out(QStringLiteral("[WARN] 无法读取 %1，跳过写入验证").arg(hexAddr(kScratch)));
        } else {
            const quint32 pattern = 0xA5C30000u | (original & 0x0000FFFFu);
            const Status wroteStatus = probe->writeU32(kScratch, pattern);
            const bool wrote = static_cast<bool>(wroteStatus);
            quint32 readBack = 0;
            const bool readOk = static_cast<bool>(probe->readU32(kScratch, &readBack));
            const Status restored = probe->writeU32(kScratch, original);

            out(QStringLiteral("原始值 %1 -> 写入 %2 -> 读回 %3 -> 恢复 %4")
                    .arg(hex32(original), hex32(pattern), readOk ? hex32(readBack) : QStringLiteral("-"),
                         static_cast<bool>(restored) ? QStringLiteral("OK") : restored.error));
            check(wrote && readOk && readBack == pattern, QStringLiteral("寄存器写入/回读一致"),
                  wrote ? QString() : wroteStatus.error);
        }
    }

    // --- one full read of every static variable (DLL backend only) -----------
    // Answers "how long does a realtime refresh cost, and does it stop the MCU?".
    // The DLL's JLINK_ReadMem goes through the same AHB-AP path HSS uses, so this is
    // the honest number for a snapshot-style refresh -- not a GDB all-stop figure.
    if (backend == QLatin1String("jlink-dll") && options.benchRead
        && !options.symbolPath.isEmpty()) {
        out(QString());
        out(QStringLiteral("--- 一次性读取全部静态变量 ---"));

        auto dll = dynamic_cast<JLinkDllProbe *>(probe.get());
        const auto symbols = SymbolFile::load(options.symbolPath);
        if (!dll || !symbols) {
            check(false, QStringLiteral("需要 DLL 后端与符号文件"), QString());
        } else {
            // Only addressable leaves, which is what "update every variable" means.
            QVector<WatchVariable> leaves;
            for (const WatchVariable &variable : symbols.value.variables) {
                if (variable.usable())
                    leaves.append(variable);
            }
            check(!leaves.isEmpty(), QStringLiteral("得到可读变量"),
                  QStringLiteral("%1 个").arg(leaves.size()));

            // Merge neighbours into contiguous spans: one request per *block* instead of
            // one per variable is what makes this tractable at all.
            QVector<QPair<quint32, int>> blocks;
            {
                QVector<QPair<quint32, int>> spans;
                spans.reserve(leaves.size());
                for (const WatchVariable &variable : leaves)
                    spans.append(qMakePair(variable.address, variable.span()));
                std::sort(spans.begin(), spans.end());
                for (const auto &span : spans) {
                    const quint32 begin = span.first;
                    const quint32 end = begin + quint32(span.second);
                    if (!blocks.isEmpty()
                        && begin <= blocks.last().first + quint32(blocks.last().second)) {
                        const quint32 mergedEnd =
                            qMax(blocks.last().first + quint32(blocks.last().second), end);
                        blocks.last().second = int(mergedEnd - blocks.last().first);
                    } else {
                        blocks.append(qMakePair(begin, int(end - begin)));
                    }
                }
            }
            int totalBytes = 0;
            for (const auto &block : blocks)
                totalBytes += block.second;
            out(QStringLiteral("    %1 个变量 → %2 个连续区段 / %3 字节")
                    .arg(leaves.size()).arg(blocks.size()).arg(totalBytes));

            // Find the target's own tick counter: it advances only while the core runs,
            // which is a direct answer to "did reading pause the MCU?".
            const WatchVariable *tick = nullptr;
            for (const WatchVariable &variable : symbols.value.variables) {
                if (variable.name.endsWith(QLatin1String("xTickCount"))) {
                    tick = &variable;
                    break;
                }
            }
            auto readU32 = [&](quint32 address, quint32 *out) -> bool {
                return static_cast<bool>(probe->readU32(address, out));
            };
            auto halted = [&]() -> int {
                quint32 dhcsr = 0;
                if (!readU32(0xE000EDF0u, &dhcsr))
                    return -1;
                return (dhcsr & 0x2u) ? 1 : 0;
            };

            // Run the core: a halted target would make every number meaningless.
            probe->resume();
            QThread::msleep(300);
            const int haltedBefore = halted();

            // Baseline: how far the tick counter gets in the same wall time, untouched.
            qint64 tickBefore = -1;
            if (tick) {
                quint32 raw = 0;
                if (readU32(tick->address, &raw))
                    tickBefore = qint64(raw);
            }

            // Warm up once so the first run does not pay for lazy buffers.
            for (const auto &block : blocks) {
                QByteArray scratch;
                probe->readMemory(block.first, block.second, &scratch);
            }

            // Average over several sweeps: one sample of a 2 ms operation is noise.
            constexpr int kSweeps = 5;
            QElapsedTimer timer;
            timer.start();
            int readErrors = 0;
            for (int sweep = 0; sweep < kSweeps; ++sweep) {
                for (const auto &block : blocks) {
                    QByteArray scratch;
                    if (!probe->readMemory(block.first, block.second, &scratch))
                        ++readErrors;
                }
            }
            const qint64 elapsedMs = timer.elapsed();
            const double perSweepMs = double(elapsedMs) / double(kSweeps);
            const int haltedDuring = halted();

            qint64 tickAfter = -1;
            if (tick) {
                quint32 raw = 0;
                if (readU32(tick->address, &raw))
                    tickAfter = qint64(raw);
            }

            out(QStringLiteral("    单次全量读取: %1 ms（%2 次平均，共 %3 ms）")
                    .arg(perSweepMs, 0, 'f', 1).arg(kSweeps).arg(elapsedMs));
            out(QStringLiteral("    等效刷新率  : %1 Hz")
                    .arg(perSweepMs > 0.0 ? 1000.0 / perSweepMs : 0.0, 0, 'f', 1));
            out(QStringLiteral("    DHCSR.C_HALT: 读取前 %1，读取后 %2（0=运行）")
                    .arg(haltedBefore).arg(haltedDuring));
            if (tick) {
                out(QStringLiteral("    %1: %2 -> %3（增量 %4）")
                        .arg(tick->name).arg(tickBefore).arg(tickAfter)
                        .arg(tickAfter - tickBefore));
            }

            check(readErrors == 0, QStringLiteral("全量读取无失败"),
                  QStringLiteral("%1 次失败").arg(readErrors));
            check(haltedDuring == 0, QStringLiteral("读取后内核仍在运行（未暂停 MCU）"),
                  haltedDuring == 0 ? QStringLiteral("DHCSR.C_HALT=0")
                                    : QStringLiteral("DHCSR.C_HALT=1，被暂停了"));
            if (tick && tickBefore >= 0 && tickAfter >= 0) {
                check(tickAfter > tickBefore,
                      QStringLiteral("读取期间目标节拍仍在推进"),
                      QStringLiteral("+%1 tick").arg(tickAfter - tickBefore));
            }
        }
    }

    // --- value-column polling budget (DLL backend only) ---------------------
    // The 值 column refreshes on a 200 ms timer. Whether that is affordable depends
    // entirely on how many variables are in the set, so measure it instead of assuming:
    // a full-image sweep is far too slow, a screenful is not.
    if (backend == QLatin1String("jlink-dll") && options.benchPoll > 0
        && !options.symbolPath.isEmpty()) {
        out(QString());
        out(QStringLiteral("--- 数值列轮询预算（%1 行） ---").arg(options.benchPoll));

        auto dll = dynamic_cast<JLinkDllProbe *>(probe.get());
        const auto symbols = SymbolFile::load(options.symbolPath);
        if (!dll || !symbols) {
            check(false, QStringLiteral("需要 DLL 后端与符号文件"), QString());
        } else {
            QVector<WatchVariable> leaves;
            for (const WatchVariable &variable : symbols.value.variables) {
                if (variable.usable())
                    leaves.append(variable);
            }
            check(!leaves.isEmpty(), QStringLiteral("得到可读变量"),
                  QStringLiteral("%1 个").arg(leaves.size()));

            // A screenful is not 4000 variables; take a deterministic spread so repeated
            // runs measure the same thing.
            QVector<WatchVariable> chosen;
            const int wanted = qMin(options.benchPoll, leaves.size());
            for (int i = 0; i < wanted; ++i) {
                const int index = leaves.size() > 1
                    ? int(qint64(i) * (leaves.size() - 1) / qMax(1, wanted - 1))
                    : 0;
                chosen.append(leaves.at(qMin(index, leaves.size() - 1)));
            }

            // Same coalescing rule as ValuePoller: adjacent addresses share one request.
            QVector<QPair<quint32, int>> spans;
            {
                QVector<QPair<quint32, int>> raw;
                for (const WatchVariable &variable : chosen)
                    raw.append(qMakePair(variable.address, variable.span()));
                std::sort(raw.begin(), raw.end());
                for (const auto &span : raw) {
                    const quint32 begin = span.first;
                    const quint32 end = begin + quint32(span.second);
                    if (!spans.isEmpty()
                        && begin <= spans.last().first + quint32(spans.last().second)) {
                        const quint32 mergedEnd =
                            qMax(spans.last().first + quint32(spans.last().second), end);
                        spans.last().second = int(mergedEnd - spans.last().first);
                    } else {
                        spans.append(qMakePair(begin, int(end - begin)));
                    }
                }
            }
            out(QStringLiteral("    %1 个变量 → %2 个连续区段")
                    .arg(chosen.size()).arg(spans.size()));

            auto readDhcsr = [&]() -> int {
                quint32 dhcsr = 0;
                if (!probe->readU32(0xE000EDF0u, &dhcsr))
                    return -1;
                return (dhcsr & 0x2u) ? 1 : 0;
            };
            probe->resume();
            QThread::msleep(200);
            const int haltedBefore = readDhcsr();

            // Warm up, then measure how many sweeps fit in one second.
            for (const auto &span : spans) {
                QByteArray scratch;
                probe->readMemory(span.first, span.second, &scratch);
            }
            QElapsedTimer timer;
            timer.start();
            int sweeps = 0;
            int readErrors = 0;
            while (timer.elapsed() < 1000) {
                for (const auto &span : spans) {
                    QByteArray scratch;
                    if (!probe->readMemory(span.first, span.second, &scratch))
                        ++readErrors;
                }
                ++sweeps;
            }
            const qint64 elapsedMs = timer.elapsed();
            const double perSweepMs = sweeps > 0 ? double(elapsedMs) / double(sweeps) : 0.0;
            const int haltedDuring = readDhcsr();

            out(QStringLiteral("    单次刷新: %1 ms（1 秒内完成 %2 次）")
                    .arg(perSweepMs, 0, 'f', 1).arg(sweeps));
            out(QStringLiteral("    200 ms 目标: %1")
                    .arg(perSweepMs <= 200.0 ? QStringLiteral("可达")
                                             : QStringLiteral("不可达（需要 %1 ms）")
                                                   .arg(perSweepMs, 0, 'f', 0)));
            out(QStringLiteral("    DHCSR.C_HALT: 轮询前 %1，轮询后 %2（0=运行）")
                    .arg(haltedBefore).arg(haltedDuring));

            // The cost that matters is per *request*, not per byte: on a real 3777-leaf
            // image a merged sweep is ~700 requests and ~692 ms. Measure the constant on
            // this hardware so "200 ms buys ~N requests" is a number, not a guess.
            {
                const quint32 scratchAddress = chosen.isEmpty() ? 0x20000000u
                                                                : chosen.first().address;
                constexpr int kRequests = 200;
                QElapsedTimer requestTimer;
                requestTimer.start();
                for (int i = 0; i < kRequests; ++i) {
                    QByteArray scratch;
                    probe->readMemory(scratchAddress, 4, &scratch);
                }
                const double perRequestMs = double(requestTimer.elapsed()) / double(kRequests);
                out(QStringLiteral("    单次小请求: %1 ms → 200 ms 预算约 %2 次请求")
                        .arg(perRequestMs, 0, 'f', 3)
                        .arg(perRequestMs > 0.0 ? int(200.0 / perRequestMs) : 0));
            }

            check(readErrors == 0, QStringLiteral("轮询读取无失败"),
                  QStringLiteral("%1 次失败").arg(readErrors));
            check(haltedDuring == 0, QStringLiteral("轮询期间内核仍在运行（不停机）"),
                  QStringLiteral("DHCSR.C_HALT=%1").arg(haltedDuring));
        }
    }

    // --- HSS live sampling (DLL backend only) ------------------------------
    // This is the capability the GDB path cannot provide at all: samples arrive while
    // the core keeps executing. A cycle counter is the proof -- if it advances between
    // records the target really was running during the capture.
    if (backend == QLatin1String("jlink-dll")) {
        out(QString());
        out(QStringLiteral("--- HSS 实时采样 ---"));

        // One probe, one session. The direct probe above still holds the J-Link, so it
        // has to be released before a controller-based session can open -- otherwise
        // the second JLINK_OpenEx blocks until the timeout.
        probe->close();
        probe.reset();

        // Go through a controller so the sampling runs on the same worker thread the
        // GUI uses; the probe object is not reachable directly from here either.
        ProbeController controller;
        controller.requestOpen(backend, QString(), options.interfaceName.isEmpty()
                                   ? QStringLiteral("SWD") : options.interfaceName,
                               options.speedKHz > 0 ? options.speedKHz : 4000);
        QEventLoop openLoop;
        bool opened = false;
        QMetaObject::Connection openConn = QObject::connect(
            &controller, &ProbeController::opened, &openLoop,
            [&](bool ok, const QString &err, const ProbeInfo &) {
                opened = ok;
                if (!ok)
                    out(QStringLiteral("[WARN] 打开探针: %1").arg(err));
                openLoop.quit();
            });
        QTimer::singleShot(20000, &openLoop, &QEventLoop::quit);
        openLoop.exec();
        QObject::disconnect(openConn);

        if (opened) {
            controller.requestConnect(QString());
            QEventLoop connectLoop;
            bool connected = false;
            QMetaObject::Connection conn = QObject::connect(
                &controller, &ProbeController::connectFinished, &connectLoop,
                [&](bool ok, const QString &err, const ProbeInfo &) {
                    connected = ok;
                    if (!ok)
                        out(QStringLiteral("[WARN] 连接: %1").arg(err));
                    connectLoop.quit();
                });
            QTimer::singleShot(30000, &connectLoop, &QEventLoop::quit);
            connectLoop.exec();
            QObject::disconnect(conn);

            if (connected) {
                check(controller.supportsHss(), QStringLiteral("后端支持 HSS"),
                      controller.supportsHss() ? QString() : QStringLiteral("不支持"));

                HssSampler sampler;
                sampler.setProbe(&controller);

                WatchVariable cyccnt;
                cyccnt.name = QStringLiteral("DWT_CYCCNT");
                cyccnt.typeLabel = QStringLiteral("uint32_t");
                cyccnt.kind = ScalarKind::Unsigned;
                cyccnt.address = 0xE0001004u;
                cyccnt.elementSize = 4;
                cyccnt.elementCount = 1;

                controller.requestRun(); // HSS produces nothing while halted

                // start() is asynchronous (the DLL call runs on the worker), so wait for
                // the verdict instead of assuming success and then draining nothing.
                bool hssOk = false;
                QString hssError;
                QEventLoop startLoop;
                QMetaObject::Connection sc = QObject::connect(
                    &sampler, &HssSampler::started, &startLoop,
                    [&](bool ok, const QString &err) {
                        hssOk = ok;
                        hssError = err;
                        startLoop.quit();
                    });
                const Status started = sampler.start({cyccnt}, 1000);
                if (started) {
                    QTimer::singleShot(15000, &startLoop, &QEventLoop::quit);
                    startLoop.exec();
                }
                QObject::disconnect(sc);
                check(static_cast<bool>(started) && hssOk,
                      QStringLiteral("启动 HSS 采样"),
                      started ? hssError : started.error);

                if (started) {
                    SampleHistory history(8192);
                    history.setTraceNames({QStringLiteral("DWT_CYCCNT")});

                    HssSampler::Result drained;
                    QEventLoop drainLoop;
                    QMetaObject::Connection dc = QObject::connect(
                        &sampler, &HssSampler::drained, &drainLoop,
                        [&](const HssSampler::Result &r) {
                            drained = r;
                            drainLoop.quit();
                        });
                    // Real sampling needs wall time: the probe fills its buffer on its
                    // own timebase, so there is nothing to do but wait for it.
                    QThread::msleep(700);
                    sampler.setDrainTarget(&history);
                    if (sampler.requestDrain()) {
                        QTimer::singleShot(15000, &drainLoop, &QEventLoop::quit);
                        drainLoop.exec();
                    }
                    QObject::disconnect(dc);

                    check(drained.samples > 0, QStringLiteral("取到实时样本"),
                          QStringLiteral("%1 个（%2 字节）")
                              .arg(drained.samples).arg(drained.bytes));
                    check(history.traceCount() == 1, QStringLiteral("曲线通道已登记"),
                          history.traceNames().join(QLatin1Char(',')));

                    if (drained.samples > 1) {
                        const double first = history.valueAt(0, 0);
                        const double last = history.valueAt(0, history.count() - 1);
                        check(std::isfinite(first) && std::isfinite(last) && first != last,
                              QStringLiteral("样本在推进（目标确实在运行）"),
                              QStringLiteral("%1 -> %2").arg(first).arg(last));
                    }
                    // Continuity is what the user sees: report the gap histogram, so
                    // "the curve is torn" becomes a number instead of an impression.
                    {
                        int gaps = 0;
                        int longestGapMs = 0;
                        int previousMs = INT_MIN;
                        for (int i = 0; i < history.count(); ++i) {
                            const int us = int(history.timeAt(i));
                            if (previousMs != INT_MIN) {
                                const int delta = us - previousMs;
                                if (delta > int(sampler.measuredIntervalUs() * 2.5)) {
                                    ++gaps;
                                    longestGapMs = qMax(longestGapMs, delta / 1000);
                                }
                            }
                            previousMs = us;
                        }
                        out(QStringLiteral("    连续性: %1 个样本，%2 处缺口，最长 %3 ms")
                                .arg(history.count())
                                .arg(gaps)
                                .arg(longestGapMs));
                        check(gaps <= history.count() / 20,
                              QStringLiteral("缺口不超过样本数的 5%（曲线连续）"),
                              QStringLiteral("%1 / %2").arg(gaps).arg(history.count()));
                    }
                    out(QStringLiteral("    共 %1 个样本，请求周期 %2 µs，实测间隔 %3 µs")
                            .arg(sampler.totalSamples())
                            .arg(sampler.periodUs())
                            .arg(sampler.measuredIntervalUs(), 0, 'f', 1));
                    for (const HssSampler::Channel &channel : sampler.channels()) {
                        out(QStringLiteral("    %1: 最新 %2  范围 %3..%4  均值 %5")
                                .arg(channel.name)
                                .arg(channel.latest)
                                .arg(channel.min)
                                .arg(channel.max)
                                .arg(channel.average()));
                    }
                    sampler.stop();
                }
            }
            controller.requestClose();
        }
    }

    // --- on-target variable read -------------------------------------------
    // `--read-var` proves the snapshot path end to end: same symbol lookup, same
    // address, same decoder the table and the chart use. Without it the only evidence
    // that reading `error_table` works would be that the code compiles.
    if (!options.readVar.isEmpty()) {
        out(QString());
        out(QStringLiteral("--- 变量快照: %1 ---").arg(options.readVar));

        const auto table = SymbolFile::load(options.symbolPath);
        if (!table) {
            check(false, QStringLiteral("加载符号文件"), table.error);
        } else {
            const WatchVariable *variable = table.value.find(options.readVar);
            if (!variable) {
                // Fall back to a suffix match so `error_table` finds
                // `...error_table.__elems_` without the caller spelling the whole path.
                for (const WatchVariable &candidate : table.value.variables) {
                    if (candidate.name.endsWith(options.readVar)) {
                        variable = &candidate;
                        break;
                    }
                }
            }
            check(variable != nullptr, QStringLiteral("符号文件里找到该变量"),
                  variable ? variable->name : QStringLiteral("未找到：%1").arg(options.readVar));

            if (variable) {
                const int elements = qMax(1, variable->elementCount);
                const int bytes = elements * variable->elementSize;
                QByteArray memory;
                const Status read = probe->readMemory(variable->address, bytes, &memory);
                check(static_cast<bool>(read) && memory.size() == bytes,
                      QStringLiteral("从目标读到 %1 字节").arg(bytes),
                      read ? QStringLiteral("实际 %1").arg(memory.size()) : read.error);

                if (read && memory.size() == bytes) {
                    const QVector<double> values =
                        SymbolFile::decode(*variable, memory, elements);
                    check(values.size() == elements,
                          QStringLiteral("解码出 %1 个值").arg(elements),
                          QStringLiteral("%1 个").arg(values.size()));

                    // Print in a pitch the table view uses, so comparing the two is
                    // mechanical: index, decimal, hex.
                    const int limit = qMin(values.size(), 24);
                    for (int i = 0; i < limit; ++i) {
                        const double v = values.at(i);
                        out(QStringLiteral("    [%1]%2  %3  %4")
                                .arg(i, 3)
                                .arg(QString())
                                .arg(v, 12, 'g', 9)
                                .arg(QStringLiteral("0x%1")
                                         .arg(quint32(qint32(v)), 8, 16, QLatin1Char('0'))));
                    }
                    if (values.size() > limit)
                        out(QStringLiteral("    ... 其余 %1 个省略")
                                .arg(values.size() - limit));
                }
            }
        }
    }

    // --- bus contention: does polling all static variables slow the target? ---
    // The question only has an empirical answer, so it is measured: read the target's
    // own FreeRTOS tick counter twice with the bus idle, then keep the bus busy for
    // the same wall time and read it again. A drop in ticks/second is the target
    // actually losing execution cycles to AHB-AP accesses.
    if (options.benchBus > 0) {
        out(QString());
        out(QStringLiteral("--- 总线占用实测（%1 秒/阶段） ---").arg(options.benchBus));

        const auto symTable = SymbolFile::load(options.symbolPath);
        const WatchVariable *tickVar = nullptr;
        if (symTable) {
            tickVar = symTable.value.find(QStringLiteral("xTickCount"));
            if (!tickVar) {
                for (const WatchVariable &candidate : symTable.value.variables) {
                    if (candidate.name.endsWith(QLatin1String("xTickCount"))) {
                        tickVar = &candidate;
                        break;
                    }
                }
            }
        }

        auto readTick = [&]() -> qint64 {
            if (!tickVar)
                return -1;
            quint32 value = 0;
            if (!probe->readU32(tickVar->address, &value))
                return -1;
            return qint64(value);
        };

        // Every addressable leaf, so a sweep is exactly "update all static variables".
        QVector<WatchVariable> leaves;
        if (symTable) {
            for (const WatchVariable &variable : symTable.value.variables) {
                if (variable.usable())
                    leaves.append(variable);
            }
        }
        int sweepBytes = 0;
        for (const WatchVariable &v : leaves)
            sweepBytes += v.span();
        out(QStringLiteral("    待读取叶子 %1 个，一次全量约 %2 字节")
                .arg(leaves.size())
                .arg(sweepBytes));

        if (!tickVar) {
            check(false, QStringLiteral("找到目标侧节拍计数器（否则无法判断是否变慢）"),
                  QStringLiteral("未找到 xTickCount"));
        } else {
            const QString tickName = tickVar->name;
            const int stageMs = qBound(1, options.benchBus, 60) * 1000;

            // DHCSR.C_HALT (bit 1) is the only trustworthy "is it running?" answer:
            // a read succeeding proves nothing, because a halted core answers memory
            // reads happily and the numbers look identical.
            auto coreRunning = [&]() -> bool {
                quint32 dhcsr = 0;
                if (!probe->readU32(0xE000EDF0u, &dhcsr))
                    return false;
                return (dhcsr & 0x2u) == 0;
            };

            // Prove whether values change at all while "running": a frozen counter
            // can mean a halted core OR a GDB server that returns stale memory, and
            // those two demand different conclusions.
            // Decisive test: halt, read, run 1 s, halt again, read. If the counter
            // advanced across the run, the core really executes; then a *frozen*
            // value during "running" reads proves the GDB server hands back stale
            // memory instead of a live view.
            probe->halt();
            const qint64 tickHaltedA = readTick();
            probe->resume();
            QThread::msleep(1000);
            probe->halt();
            const qint64 tickHaltedB = readTick();
            out(QStringLiteral("    停机->跑 1s->停机: %1 -> %2（增量 %3 tick）")
                    .arg(tickHaltedA).arg(tickHaltedB).arg(tickHaltedB - tickHaltedA));

            probe->resume();
            QThread::msleep(200);
            QStringList samples;
            for (int i = 0; i < 4; ++i) {
                samples << QString::number(readTick());
                QThread::msleep(300);
            }
            out(QStringLiteral("    运行中连读 xTickCount: %1")
                    .arg(samples.join(QLatin1String(" "))));

            // --- phase 1: bus idle -------------------------------------------------
            const Status resumed = probe->resume();
            QThread::msleep(300); // let it settle after the resume
            check(static_cast<bool>(resumed), QStringLiteral("恢复目标运行"),
                  resumed ? QString() : resumed.error);
            const bool running = coreRunning();
            out(QStringLiteral("    DHCSR.C_HALT=%1（%2）")
                    .arg(running ? 0 : 1)
                    .arg(running ? QStringLiteral("报称运行") : QStringLiteral("报称停机")));
            const qint64 idleStart = readTick();
            QThread::msleep(stageMs);
            const qint64 idleEnd = readTick();
            const double idleHz = (idleEnd - idleStart) * 1000.0 / stageMs;

            // --- phase 2: bus busy -------------------------------------------------
            // Read every leaf back to back for the same wall time. This is the
            // workload "keep all variables fresh" implies.
            qint64 busyStart = readTick();
            QElapsedTimer busyTimer;
            busyTimer.start();
            int sweeps = 0;
            int readErrors = 0;
            qint64 busyEnd = busyStart;
            while (busyTimer.elapsed() < stageMs) {
                for (const WatchVariable &v : leaves) {
                    QByteArray scratch;
                    if (!probe->readMemory(v.address, v.span(), &scratch))
                        ++readErrors;
                }
                ++sweeps;
                busyEnd = readTick();
                if (busyEnd < 0)
                    break;
            }
            const bool stillRunning = coreRunning();
            const qint64 busyMs = qMax<qint64>(1, busyTimer.elapsed());
            const double busyHz = (busyEnd - busyStart) * 1000.0 / double(busyMs);
            const double sweepMs = sweeps > 0 ? double(busyMs) / sweeps : 0.0;

            probe->resume(); // leave it running as we found it

            out(QStringLiteral("    节拍计数器 %1 @ %2").arg(tickName, hexAddr(tickVar->address)));
            out(QStringLiteral("    空载   : %1 tick/s").arg(idleHz, 0, 'f', 1));
            out(QStringLiteral("    满载   : %1 tick/s（%2 次全量扫描，每次 %3 ms，读失败 %4）")
                    .arg(busyHz, 0, 'f', 1)
                    .arg(sweeps)
                    .arg(sweepMs, 0, 'f', 1)
                    .arg(readErrors));
            const double slowdown = idleHz > 0.0 ? (1.0 - busyHz / idleHz) * 100.0 : 0.0;
            out(QStringLiteral("    目标变慢: %1%").arg(slowdown, 0, 'f', 1));
            check(readErrors == 0, QStringLiteral("全量读取无失败"),
                  readErrors == 0 ? QStringLiteral("全部成功")
                                  : QStringLiteral("%1 次失败").arg(readErrors));
            Q_UNUSED(stillRunning);

            // The measurement above is the answer to "will polling slow the MCU?" for
            // *this* architecture, and it is a stronger statement than a percentage:
            // the target's own counter does not advance at all between reads, so the
            // GDB-RSP path is serving a halted snapshot rather than a live view.
            // Live background polling needs the J-Link DLL's HSS engine
            // (JLINK_HSS_Start/Read/Stop), which this program deliberately does not
            // call -- that engine belongs to the DLL, not to GDB RSP.
            check(true, QStringLiteral("单次全量读取耗时"),
                  QStringLiteral("%1 ms / %2 字节").arg(sweepMs, 0, 'f', 0).arg(sweepBytes));

            // Per-request round trip dominates, not bytes: 3777 tiny reads cost far
            // more than the same bytes in a few big ones. Merging neighbouring
            // variables into contiguous blocks is the single biggest lever, so the
            // saving is measured rather than assumed.
            QVector<QPair<quint32, int>> blocks;
            {
                QVector<QPair<quint32, int>> spans;
                spans.reserve(leaves.size());
                for (const WatchVariable &v : leaves)
                    spans.append(qMakePair(v.address, v.span()));
                std::sort(spans.begin(), spans.end());
                for (const auto &span : spans) {
                    const quint32 begin = span.first;
                    const quint32 end = begin + quint32(span.second);
                    if (!blocks.isEmpty()
                        && begin <= blocks.last().first + quint32(blocks.last().second)) {
                        const quint32 mergedEnd =
                            qMax(blocks.last().first + quint32(blocks.last().second), end);
                        blocks.last().second = int(mergedEnd - blocks.last().first);
                    } else {
                        blocks.append(qMakePair(begin, int(end - begin)));
                    }
                }
            }
            if (!blocks.isEmpty()) {
                QElapsedTimer blockTimer;
                blockTimer.start();
                int blockErrors = 0;
                int blockBytes = 0;
                for (const auto &block : blocks) {
                    QByteArray scratch;
                    if (!probe->readMemory(block.first, block.second, &scratch))
                        ++blockErrors;
                    else
                        blockBytes += scratch.size();
                }
                const qint64 blockMs = blockTimer.elapsed();
                out(QStringLiteral("    合并读取: %1 个连续区段 / %2 字节耗时 %3 ms"
                                   "（%4 次请求降到 %1 次）")
                        .arg(blocks.size())
                        .arg(blockBytes)
                        .arg(blockMs)
                        .arg(leaves.size()));
                check(blockErrors == 0, QStringLiteral("合并区段读取无失败"),
                      QStringLiteral("%1 个区段，%2 次失败").arg(blocks.size()).arg(blockErrors));
            }
            if (idleHz > 0.0 && busyHz > 0.0) {
                out(QStringLiteral("    结论   : 目标变慢 %1%（节拍可比）")
                        .arg(slowdown, 0, 'f', 1));
            } else {
                out(QStringLiteral("    结论   : 目标节拍在读取期间未推进 —— 该通路是"
                                   "「停机读取」，不是后台实时读取"));
                out(QStringLiteral("             要达到真正的实时轮询，需要 J-Link DLL 的 "
                                   "HSS（JLINK_HSS_*），不是 GDB RSP"));
            }
        }
    }

    // probe may already be gone: the HSS section releases it so a controller session can
    // take the probe over (one J-Link, one session at a time).
    if (probe)
        probe->close();
    out(QString());
    out(g_failures == 0 ? QStringLiteral("=== probe-info 通过 ===")
                        : QStringLiteral("=== probe-info 有 %1 项未通过 ===").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}

} // namespace mrd::cli
