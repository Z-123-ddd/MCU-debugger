#include "core/FirmwareBurner.h"

#include "core/JLinkRunner.h"

#include <QElapsedTimer>
#include <memory>
#include <QFileInfo>

namespace mrd {

FirmwareBurner::FirmwareBurner(const QString &commanderExecutable)
    : m_commander(commanderExecutable)
{
}

QStringList FirmwareBurner::buildCommands(const QString &device, const QString &imagePath,
                                          quint32 startAddress, quint32 endAddress, int speedKHz,
                                          bool forceErase)
{
    // J-Link Commander wants forward slashes and no spaces unquoted; the caller passes an
    // absolute path. `loadbin` keeps the same address convention as the rest of the tool.
    QString normalized = QFileInfo(imagePath).absoluteFilePath();
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString address = QStringLiteral("0x%1").arg(startAddress, 8, 16, QLatin1Char('0'));
    const QString endAddressText =
        QStringLiteral("0x%1").arg(endAddress, 8, 16, QLatin1Char('0'));

    QStringList commands{
        QStringLiteral("device %1").arg(device),
        QStringLiteral("si SWD"),
        QStringLiteral("speed %1").arg(qBound(100, speedKHz, 50000)),
        QStringLiteral("connect"),
        // A core that is running while Flash is programmed is the usual cause of a
        // flaky burn, so halt first and reset after.
        QStringLiteral("h"),
    };
    if (forceErase) {
        // Erase exactly the sectors the image covers. `erase` with a range keeps a
        // config area that starts past the image intact, unlike `erase` with no range
        // which would wipe the whole chip.
        commands << QStringLiteral("erase %1 %2").arg(address, endAddressText);
    }
    commands
        << QStringLiteral("loadbin \"%1\", %2").arg(normalized, address)
        << QStringLiteral("verifybin \"%1\", %2").arg(normalized, address)
        << QStringLiteral("r")
        << QStringLiteral("g")
        << QStringLiteral("qc");
    return commands;
}

QString FirmwareBurner::describeCommands(const QString &device, const QString &imagePath,
                                         quint32 startAddress, quint32 endAddress, int speedKHz,
                                         bool forceErase)
{
    QStringList lines;
    for (const QString &command :
         buildCommands(device, imagePath, startAddress, endAddress, speedKHz, forceErase))
        lines << QStringLiteral("  %1").arg(command);
    return lines.join(QLatin1Char('\n'));
}

BurnResult FirmwareBurner::burn(const QString &device, const FirmwareImage &image, int speedKHz,
                                bool forceErase, int timeoutMs) const
{
    BurnResult result;
    if (!image.isValid()) {
        result.error = QStringLiteral("固件镜像无效（未识别出起止地址）");
        return result;
    }
    if (m_commander.isEmpty()) {
        result.error = QStringLiteral("未找到 JLink.exe（J-Link Commander），无法烧录");
        return result;
    }

    // burnPath() is the merged temp file when a bootloader was combined in; for a plain
    // burn it is the original .bin.
    const QStringList commands = buildCommands(device, image.burnPath(), image.startAddress,
                                              image.endAddress, speedKHz, forceErase);

    QElapsedTimer timer;
    timer.start();
    const Result<QString> run = JLinkRunner::runScript(m_commander, commands, timeoutMs);
    result.elapsedMs = timer.elapsed();

    if (!run) {
        result.error = run.error;
        return result;
    }
    return interpret(0, run.value, false, result.elapsedMs);
}

BurnResult FirmwareBurner::interpret(int exitCode, const QString &output, bool timedOut,
                                     qint64 elapsedMs)
{
    BurnResult result;
    result.output = output;
    result.elapsedMs = elapsedMs;

    if (timedOut) {
        result.error = QStringLiteral("烧录超时，已被终止");
        return result;
    }

    // Commander returns 0 even for some failures, so the transcript is what decides.
    // These are the strings it prints when a step did not happen.
    const bool loadFailed = output.contains(QLatin1String("Failed to load"))
        || output.contains(QLatin1String("Could not open file"))
        || output.contains(QLatin1String("ERROR: Failed"))
        || output.contains(QLatin1String("Cannot connect"))
        || output.contains(QLatin1String("Could not connect to the target"));
    const bool verifyFailed = output.contains(QLatin1String("Verify failed"))
        || output.contains(QLatin1String("verification failed"))
        || output.contains(QLatin1String("Failed to verify"));

    result.verified = !verifyFailed && !loadFailed && output.contains(QLatin1String("O.K."));
    result.ok = !loadFailed && !verifyFailed && exitCode == 0;

    if (loadFailed) {
        result.error = QStringLiteral("烧录失败，详见输出");
    } else if (verifyFailed) {
        result.error = QStringLiteral("写入完成但校验失败");
    } else if (exitCode != 0) {
        result.error = QStringLiteral("J-Link Commander 退出码 %1").arg(exitCode);
    }
    return result;
}

void FirmwareBurner::burnAsync(const QString &device, const FirmwareImage &image, int speedKHz,
                               bool forceErase, int timeoutMs,
                               std::function<void(const BurnResult &)> onFinished) const
{
    if (!image.isValid()) {
        BurnResult result;
        result.error = QStringLiteral("固件镜像无效（未识别出起止地址）");
        if (onFinished)
            onFinished(result);
        return;
    }
    if (m_commander.isEmpty()) {
        BurnResult result;
        result.error = QStringLiteral("未找到 JLink.exe（J-Link Commander），无法烧录");
        if (onFinished)
            onFinished(result);
        return;
    }

    const QStringList commands = buildCommands(device, image.burnPath(), image.startAddress,
                                              image.endAddress, speedKHz, forceErase);

    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    JLinkRunner::runScriptAsync(
        m_commander, commands, timeoutMs,
        [clock, onFinished](int exitCode, const QString &output, bool timedOut) {
            const BurnResult result =
                interpret(exitCode, output, timedOut, clock->elapsed());
            if (onFinished)
                onFinished(result);
        });
}

} // namespace mrd
