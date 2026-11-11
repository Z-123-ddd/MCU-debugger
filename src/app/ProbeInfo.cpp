#include "app/Cli.h"

#include "core/AppSettings.h"
#include "core/DeviceIdentifier.h"
#include "core/DeviceTreeScanner.h"
#include "core/GpioCodec.h"
#include "core/HexUtil.h"
#include "core/JLinkGdbProbe.h"
#include "core/JLinkRunner.h"
#include "core/Logger.h"
#include "core/ProbeRsProbe.h"
#include "core/ProbeRsRunner.h"
#include "core/SvdLibrary.h"

#include <QFileInfo>

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

    if (backend == QLatin1String("probe-rs")) {
        ProbeRsRunner runner;
        runner.setBinary(ProbeRsRunner::locate(AppSettings::probeRsPath()));
        if (runner.binary().isEmpty()) {
            out(QString());
            out(QStringLiteral("[ERROR] 未找到 probe-rs 可执行文件。"));
            out(QStringLiteral("        请运行: powershell -ExecutionPolicy Bypass -File scripts\\fetch-probe-rs.ps1"));
            return 4;
        }
        out(QStringLiteral("[OK] probe-rs %1").arg(runner.version()));

        auto rsProbe = std::make_unique<ProbeRsProbe>();
        rsProbe->runner()->setBinary(runner.binary());
        probe = std::move(rsProbe);
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
        targetDevice = identified.value.jlinkDeviceName;
        if (targetDevice.isEmpty())
            targetDevice = identified.value.probeRsChip;
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

    probe->close();
    out(QString());
    out(g_failures == 0 ? QStringLiteral("=== probe-info 通过 ===")
                        : QStringLiteral("=== probe-info 有 %1 项未通过 ===").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}

} // namespace mrd::cli
