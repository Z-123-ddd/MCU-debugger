#include "core/ProbeRsProbe.h"

#include "core/HexUtil.h"
#include "core/Logger.h"
#include "core/ProbeRsRunner.h"

#include <QRegularExpression>
#include <QtEndian>

#include <cstring>

namespace mrd {

namespace {

/// Width selectors accepted by `probe-rs read/write`.
QString widthName(int bits)
{
    switch (bits) {
    case 8:
        return QStringLiteral("b8");
    case 16:
        return QStringLiteral("b16");
    case 64:
        return QStringLiteral("b64");
    case 32:
    default:
        return QStringLiteral("b32");
    }
}

/// `probe-rs read` prints, per word, the stored bytes in reverse order
/// (little-endian storage shown big-endian). Rebuild the byte array.
QByteArray parseReadOutput(const QString &text, int wordBytes, int expectedBytes)
{
    QByteArray out;
    out.reserve(expectedBytes);

    const QRegularExpression lineRe(QStringLiteral("^\\s*([0-9a-fA-F]+):\\s*(.*)$"));
    for (const QString &rawLine : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        QString payload = rawLine.trimmed();
        const auto match = lineRe.match(payload);
        if (match.hasMatch())
            payload = match.captured(2).trimmed();

        const QStringList tokens = payload.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (const QString &token : tokens) {
            if (token.startsWith(QLatin1Char('#')) || token.isEmpty())
                continue;
            bool numeric = false;
            token.toULongLong(&numeric, 16);
            if (!numeric)
                continue;

            QString hex = token;
            if (hex.size() % 2 != 0)
                hex.prepend(QLatin1Char('0'));
            const QByteArray word = QByteArray::fromHex(hex.toLatin1());
            if (word.isEmpty())
                continue;

            if (wordBytes <= 1) {
                out.append(word);
            } else {
                // Stored bytes are shown reversed for multi-byte words.
                for (int i = word.size() - 1; i >= 0; --i)
                    out.append(word.at(i));
            }
            if (expectedBytes > 0 && out.size() >= expectedBytes)
                return out.left(expectedBytes);
        }
    }
    return out;
}

/// `probe-rs list` lines look like:
///   [0]: J-Link (vid:1366 pid:0101):000123456789 (JLink)
QStringList parseProbeList(const QString &text)
{
    QStringList serials;
    const QRegularExpression re(QStringLiteral("^\\[\\d+\\]:\\s*(.+)$"));
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const auto match = re.match(line.trimmed());
        if (match.hasMatch())
            serials << match.captured(1).trimmed();
    }
    return serials;
}

QString parseDetectedChip(const QString &text)
{
    const QRegularExpression re(QStringLiteral("Detected chip:\\s*(\\S+)"));
    const auto match = re.match(text);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

} // namespace

ProbeRsProbe::ProbeRsProbe()
    : m_runner(new ProbeRsRunner())
{
}

ProbeRsProbe::~ProbeRsProbe()
{
    delete m_runner;
}

QStringList ProbeRsProbe::probeOptions() const
{
    QStringList args;
    args << QStringLiteral("--protocol") << m_interfaceName.toLower();
    if (m_speedKHz > 0)
        args << QStringLiteral("--speed") << QString::number(m_speedKHz);
    if (!m_chip.isEmpty())
        args << QStringLiteral("--chip") << m_chip;
    return args;
}

Status ProbeRsProbe::open()
{
    if (m_open)
        return Status::success();

    QString error = m_runner->ensureBinary();
    if (!error.isEmpty()) {
        m_lastError = error;
        Logger::instance()->error(error);
        return Status::failure(error);
    }

    const ProbeRsRun versionRun = m_runner->run({QStringLiteral("--version")}, 20000);
    if (!versionRun.ok()) {
        m_lastError = QStringLiteral("probe-rs 无法运行: %1").arg(versionRun.errorText());
        Logger::instance()->error(m_lastError);
        return Status::failure(m_lastError);
    }
    m_version = versionRun.stdOut.trimmed();

    const ProbeRsRun listRun = m_runner->run({QStringLiteral("list")}, 20000);
    if (!listRun.ok()) {
        m_lastError = QStringLiteral("probe-rs list 失败: %1").arg(listRun.errorText());
        Logger::instance()->error(m_lastError);
        return Status::failure(m_lastError);
    }
    m_probeSerials = parseProbeList(listRun.stdOut);
    if (m_probeSerials.isEmpty()) {
        m_lastError = QStringLiteral("未发现调试器（probe-rs list 为空）。请检查 USB 连接与驱动。");
        Logger::instance()->warn(m_lastError);
        return Status::failure(m_lastError);
    }

    m_open = true;
    Logger::instance()->info(QStringLiteral("probe-rs %1 就绪，发现 %2 个调试器")
                                 .arg(m_version, QString::number(m_probeSerials.size())));
    return Status::success();
}

void ProbeRsProbe::close()
{
    m_connected = false;
    m_halted = false;
    m_open = false;
}

Status ProbeRsProbe::setInterface(const QString &interfaceName)
{
    const QString upper = interfaceName.trimmed().toUpper();
    m_interfaceName = (upper == QLatin1String("JTAG")) ? QStringLiteral("JTAG") : QStringLiteral("SWD");
    return Status::success();
}

Status ProbeRsProbe::setSpeed(int speedKHz)
{
    m_speedKHz = speedKHz > 0 ? speedKHz : 4000;
    return Status::success();
}

Status ProbeRsProbe::validateChipName(const QString &chip, QString *message)
{
    if (m_runner->binary().isEmpty()) {
        if (QString error = m_runner->ensureBinary(); !error.isEmpty())
            return Status::failure(error);
    }

    const QString name = chip.trimmed();
    if (name.isEmpty())
        return Status::failure(QStringLiteral("芯片名为空"));

    const ProbeRsRun run = m_runner->run({QStringLiteral("chip"), QStringLiteral("info"), name}, 30000);
    if (message)
        *message = run.ok() ? run.stdOut : run.errorText();
    if (!run.ok())
        return Status::failure(QStringLiteral("probe-rs 不认识的芯片名 '%1': %2").arg(name, run.errorText().trimmed()));
    return Status::success();
}

Status ProbeRsProbe::setDevice(const QString &deviceName)
{
    const QString name = deviceName.trimmed();
    if (name.isEmpty())
        return Status::failure(QStringLiteral("芯片名为空"));
    m_chip = name;
    m_connected = false;
    return Status::success();
}

Status ProbeRsProbe::ensureChip()
{
    if (!m_chip.isEmpty())
        return Status::success();
    if (m_detectedChip.isEmpty())
        return Status::failure(QStringLiteral("未指定芯片名，且未能自动探测。请用 --chip 指定（probe-rs chip list 可列出）"));
    m_chip = m_detectedChip;
    return Status::success();
}

Status ProbeRsProbe::connectTarget()
{
    if (!m_open) {
        if (const Status st = open(); !st)
            return st;
    }

    // `probe-rs info` attaches (halts) and reports the detected chip.
    QStringList args{QStringLiteral("info")};
    args << probeOptions();
    const ProbeRsRun run = m_runner->run(args, 60000);
    const QString detected = parseDetectedChip(run.stdOut);
    if (!detected.isEmpty())
        m_detectedChip = detected;

    if (!run.ok()) {
        m_connected = false;
        m_lastError = QStringLiteral("连接目标失败: %1").arg(run.errorText().trimmed());
        Logger::instance()->error(m_lastError);
        return Status::failure(m_lastError);
    }

    if (m_chip.isEmpty() && !m_detectedChip.isEmpty())
        m_chip = m_detectedChip;

    m_connected = true;
    m_halted = true; // info attaches with resume_target=false
    Logger::instance()->info(QStringLiteral("目标已连接: chip=%1 (%2)")
                                 .arg(m_chip.isEmpty() ? QStringLiteral("<未知>") : m_chip,
                                      m_detectedChip.isEmpty() ? QStringLiteral("未探测") : m_detectedChip));
    return Status::success();
}

void ProbeRsProbe::disconnectTarget()
{
    // probe-rs has no persistent session: each CLI call attaches and detaches.
    m_connected = false;
    m_halted = false;
}

Status ProbeRsProbe::halt()
{
    if (!m_connected)
        return Status::failure(QStringLiteral("目标未连接"));
    if (const Status st = ensureChip(); !st)
        return st;

    QStringList args{QStringLiteral("info")};
    args << probeOptions();
    const ProbeRsRun run = m_runner->run(args, 60000);
    if (!run.ok()) {
        m_lastError = QStringLiteral("暂停失败: %1").arg(run.errorText().trimmed());
        return Status::failure(m_lastError);
    }
    m_halted = true;
    Logger::instance()->info(QStringLiteral("CPU 已暂停 (probe-rs info attach)"));
    return Status::success();
}

Status ProbeRsProbe::resume()
{
    if (!m_connected)
        return Status::failure(QStringLiteral("目标未连接"));
    if (const Status st = ensureChip(); !st)
        return st;

    // `probe-rs read` resumes all cores when it finishes, without resetting.
    QStringList args{QStringLiteral("read")};
    args << probeOptions();
    args << QStringLiteral("b32") << QStringLiteral("0xE000ED00") << QStringLiteral("1");
    const ProbeRsRun run = m_runner->run(args, 60000);
    if (!run.ok()) {
        m_lastError = QStringLiteral("恢复运行失败: %1").arg(run.errorText().trimmed());
        return Status::failure(m_lastError);
    }
    m_halted = false;
    Logger::instance()->info(QStringLiteral("CPU 已运行"));
    return Status::success();
}

Status ProbeRsProbe::resetTarget()
{
    if (!m_connected)
        return Status::failure(QStringLiteral("目标未连接"));
    if (const Status st = ensureChip(); !st)
        return st;

    QStringList args{QStringLiteral("reset")};
    args << probeOptions();
    const ProbeRsRun run = m_runner->run(args, 60000);
    if (!run.ok()) {
        m_lastError = QStringLiteral("复位失败: %1").arg(run.errorText().trimmed());
        return Status::failure(m_lastError);
    }
    m_halted = false; // `probe-rs reset` resets and runs
    Logger::instance()->info(QStringLiteral("目标已复位并运行"));
    return Status::success();
}

Status ProbeRsProbe::readMemory(quint32 address, int length, QByteArray *out)
{
    if (!out)
        return Status::failure(QStringLiteral("内部错误: 空输出缓冲区"));
    if (length <= 0)
        return Status::failure(QStringLiteral("读取长度非法: %1").arg(length));
    if (!m_connected)
        return Status::failure(QStringLiteral("目标未连接"));
    if (const Status st = ensureChip(); !st)
        return st;

    // Prefer 32-bit words; fall back to byte reads for unaligned/short spans.
    int width = 32;
    if (address % 4 != 0 || length % 4 != 0)
        width = 8;
    const int wordBytes = width / 8;
    const int wordCount = length / wordBytes;

    QStringList args{QStringLiteral("read")};
    args << probeOptions();
    args << widthName(width) << hexAddr(address) << QString::number(wordCount);

    const ProbeRsRun run = m_runner->run(args, 60000);
    if (!run.ok()) {
        m_lastError = QStringLiteral("读取 0x%1 (%2 字节) 失败: %3")
                          .arg(QString::number(address, 16), QString::number(length), run.errorText().trimmed());
        return Status::failure(m_lastError);
    }

    QByteArray data = parseReadOutput(run.stdOut, wordBytes, length);
    if (data.size() < length) {
        m_lastError = QStringLiteral("读取 0x%1 返回 %2/%3 字节，无法解析输出")
                          .arg(QString::number(address, 16), QString::number(data.size()), QString::number(length));
        Logger::instance()->warn(QStringLiteral("%1 (stdout: %2)").arg(m_lastError, run.stdOut.trimmed()));
        return Status::failure(m_lastError);
    }
    *out = data.left(length);
    return Status::success();
}

Status ProbeRsProbe::writeMemory(quint32 address, const QByteArray &data)
{
    if (data.isEmpty())
        return Status::failure(QStringLiteral("写入数据为空"));
    if (!m_connected)
        return Status::failure(QStringLiteral("目标未连接"));
    if (const Status st = ensureChip(); !st)
        return st;

    int width = 32;
    if (address % 4 != 0 || (data.size() % 4) != 0)
        width = 8;
    const int wordBytes = width / 8;

    QStringList values;
    values.reserve(data.size() / wordBytes);
    for (int offset = 0; offset + wordBytes <= data.size(); offset += wordBytes) {
        quint64 value = 0;
        std::memcpy(&value, data.constData() + offset, static_cast<size_t>(wordBytes));
        values << QStringLiteral("0x%1").arg(value, wordBytes * 2, 16, QLatin1Char('0'));
    }

    QStringList args{QStringLiteral("write")};
    args << probeOptions();
    args << widthName(width) << hexAddr(address) << values;

    const ProbeRsRun run = m_runner->run(args, 60000);
    if (!run.ok()) {
        m_lastError = QStringLiteral("写入 0x%1 (%2 字节) 失败: %3")
                          .arg(QString::number(address, 16), QString::number(data.size()), run.errorText().trimmed());
        return Status::failure(m_lastError);
    }
    m_halted = true; // probe-rs leaves the core halted after a write
    return Status::success();
}

Status ProbeRsProbe::readU32(quint32 address, quint32 *out)
{
    QByteArray data;
    if (const Status st = readMemory(address, 4, &data); !st)
        return st;
    quint32 value = 0;
    std::memcpy(&value, data.constData(), 4);
    if (out)
        *out = value;
    return Status::success();
}

Status ProbeRsProbe::writeU32(quint32 address, quint32 value)
{
    QByteArray data(4, Qt::Uninitialized);
    std::memcpy(data.data(), &value, 4);
    return writeMemory(address, data);
}

Status ProbeRsProbe::exec(const QString &command)
{
    if (m_runner->binary().isEmpty()) {
        if (QString error = m_runner->ensureBinary(); !error.isEmpty())
            return Status::failure(error);
    }
    const QStringList args = command.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (args.isEmpty())
        return Status::failure(QStringLiteral("命令为空"));
    const ProbeRsRun run = m_runner->run(args, 120000);
    if (!run.ok())
        return Status::failure(run.errorText().trimmed());
    return Status::success();
}

ProbeInfo ProbeRsProbe::info() const
{
    ProbeInfo info;
    info.backend = QStringLiteral("probe-rs");
    info.binaryPath = m_runner ? m_runner->binary() : QString();
    info.version = m_version;
    info.probeSerials = m_probeSerials;
    info.serialNumber = m_probeSerials.isEmpty() ? QString() : m_probeSerials.first();
    info.interfaceName = m_interfaceName;
    info.speedKHz = m_speedKHz;
    info.open = m_open;
    info.connected = m_connected;
    info.chipName = m_chip;
    info.detectedChip = m_detectedChip;
    info.halted = m_halted;
    return info;
}

} // namespace mrd
