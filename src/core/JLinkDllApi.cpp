#include "core/JLinkDllApi.h"

#include "core/AppSettings.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStringList>

#include <cstring>

namespace mrd {

namespace {

/// Candidate DLL paths, best first.
///
/// `JLink_x64.dll` is the 64-bit build; `JLinkARM.dll` is 32-bit only, so in a
/// 64-bit process loading it fails with Windows error 193 (ERROR_BAD_EXE_FORMAT) and
/// the next candidate has to be tried instead of reported as "not installed".
QStringList dllCandidates()
{
    QStringList out;

    const QString settingsPath = AppSettings::jlinkDllPath();
    if (!settingsPath.isEmpty())
        out << settingsPath;

    // Next to a configured JLink.exe, newest SEGGER install first.
    const QString jlink = AppSettings::jlinkPath();
    if (!jlink.isEmpty()) {
        const QDir dir = QFileInfo(jlink).absoluteDir();
        for (const QString &name : {QStringLiteral("JLink_x64.dll"),
                                    QStringLiteral("JLinkARM.dll")}) {
            const QString path = dir.filePath(name);
            if (QFileInfo::exists(path))
                out << path;
        }
    }

    const QStringList roots = {
        QStringLiteral("C:/Program Files/SEGGER"),
        QStringLiteral("C:/Program Files (x86)/SEGGER"),
    };
    for (const QString &root : roots) {
        const QDir base(root);
        if (!base.exists())
            continue;
        QStringList dirs = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        // Newest version directory first: V826 before V796.
        std::reverse(dirs.begin(), dirs.end());
        for (const QString &sub : dirs) {
            for (const QString &name : {QStringLiteral("JLink_x64.dll"),
                                        QStringLiteral("JLinkARM.dll")}) {
                const QString path = base.filePath(sub + QLatin1Char('/') + name);
                if (QFileInfo::exists(path))
                    out << path;
            }
        }
    }

    // Bare names last, to let the OS search path have a go.
    out << QStringLiteral("JLink_x64.dll") << QStringLiteral("JLinkARM.dll");
    return out;
}

} // namespace

bool JLinkDllApi::libraryAvailable()
{
    for (const QString &candidate : dllCandidates()) {
        if (QFileInfo::exists(candidate))
            return true;
    }
    return false;
}

JLinkDllApi::JLinkDllApi() = default;

JLinkDllApi::~JLinkDllApi()
{
    close();
    unload();
}

Status JLinkDllApi::load()
{
    if (m_loaded)
        return Status::success();

    for (const QString &candidate : dllCandidates()) {
        auto *library = new QLibrary(candidate);
        if (!library->load()) {
            delete library;
            continue;
        }
        m_library = library;
        m_path = candidate;

        m_openEx = resolve<OpenExFn>("JLINK_OpenEx");
        m_close = resolve<VoidFn>("JLINK_Close");
        m_tifSelect = resolve<IntIntFn>("JLINK_TIF_Select");
        m_setSpeed = resolve<IntU32Fn>("JLINK_SetSpeed");
        m_exec = resolve<ExecFn>("JLINK_ExecCommand");
        m_connect = resolve<IntFn>("JLINK_Connect");
        m_disconnect = resolve<VoidFn>("JLINK_Disconnect");
        m_halt = resolve<IntFn>("JLINK_Halt");
        m_go = resolve<IntFn>("JLINK_Go");
        m_reset = resolve<IntFn>("JLINK_Reset");
        m_isHalted = resolve<U32Fn>("JLINK_IsHalted");
        m_getId = resolve<U32Fn>("JLINK_GetId");
        m_readMem = resolve<ReadMemFn>("JLINK_ReadMem");
        m_writeMem = resolve<WriteMemFn>("JLINK_WriteMem");
        m_readReg = resolve<ReadRegFn>("JLINK_ReadReg");
        m_hssStart = resolve<HssStartFn>("JLINK_HSS_Start");
        m_hssRead = resolve<HssReadFn>("JLINK_HSS_Read");
        m_hssStop = resolve<HssStopFn>("JLINK_HSS_Stop");

        // The core set must exist; HSS may legitimately be absent on old DLLs.
        if (!m_openEx || !m_close || !m_connect) {
            unload();
            continue;
        }
        m_loaded = true;
        Logger::instance()->debug(QStringLiteral("已加载 J-Link DLL: %1（HSS %2）")
                                     .arg(m_path, hasHss() ? QStringLiteral("可用")
                                                           : QStringLiteral("不可用")));
        return Status::success();
    }

    m_lastError = QStringLiteral("未找到可用的 J-Link DLL（JLink_x64.dll / JLinkARM.dll）。"
                                 "请安装 SEGGER J-Link 软件包。");
    return Status::failure(m_lastError);
}

void JLinkDllApi::unload()
{
    if (m_library) {
        auto *library = static_cast<QLibrary *>(m_library);
        library->unload();
        delete library;
        m_library = nullptr;
    }
    m_loaded = false;
    m_hssRunning = false;
}

template <typename Fn>
Fn JLinkDllApi::resolve(const char *name)
{
    auto *library = static_cast<QLibrary *>(m_library);
    return library ? reinterpret_cast<Fn>(library->resolve(name)) : nullptr;
}

Status JLinkDllApi::open(QString *banner)
{
    if (!m_loaded)
        return Status::failure(QStringLiteral("J-Link DLL 未加载"));
    char buffer[512] = {0};
    // A null "connect info" makes the DLL use its defaults (first probe found).
    const quint32 result = m_openEx(nullptr, buffer, int(sizeof(buffer)));
    if (banner)
        *banner = QString::fromLocal8Bit(buffer).trimmed();
    if (result != 0) {
        m_lastError = QStringLiteral("JLINK_OpenEx 失败（%1）").arg(result);
        return Status::failure(m_lastError);
    }
    return Status::success();
}

void JLinkDllApi::close()
{
    // Stop sampling first: stopping after Close() would talk to a dead session.
    if (m_hssRunning)
        hssStop();
    if (m_loaded && m_disconnect)
        m_disconnect();
    if (m_loaded && m_close)
        m_close();
}

Status JLinkDllApi::selectInterface(const QString &name)
{
    if (!m_tifSelect)
        return Status::failure(QStringLiteral("JLINK_TIF_Select 不可用"));
    const QString upper = name.trimmed().toUpper();
    const int tif = upper.startsWith(QLatin1String("JTAG")) ? 0 : 1;
    const int rc = m_tifSelect(tif);
    if (rc < 0) {
        m_lastError = QStringLiteral("选择接口失败（%1）").arg(rc);
        return Status::failure(m_lastError);
    }
    return Status::success();
}

Status JLinkDllApi::setSpeed(int speedKHz)
{
    if (!m_setSpeed)
        return Status::failure(QStringLiteral("JLINK_SetSpeed 不可用"));
    if (speedKHz > 0)
        m_setSpeed(quint32(speedKHz));
    return Status::success();
}

Status JLinkDllApi::execCommand(const QString &command, QString *output)
{
    if (!m_exec)
        return Status::failure(QStringLiteral("JLINK_ExecCommand 不可用"));
    char buffer[1024] = {0};
    const int rc = m_exec(qPrintable(command), buffer);
    if (output)
        *output = QString::fromLocal8Bit(buffer).trimmed();
    if (rc < 0) {
        m_lastError = QStringLiteral("命令失败: %1").arg(command);
        return Status::failure(m_lastError);
    }
    return Status::success();
}

Status JLinkDllApi::connectTarget()
{
    if (!m_connect)
        return Status::failure(QStringLiteral("JLINK_Connect 不可用"));
    const int rc = m_connect();
    if (rc != 0) {
        m_lastError = QStringLiteral("连接目标失败（%1）").arg(rc);
        return Status::failure(m_lastError);
    }
    m_idCode = (m_getId ? m_getId() : 0);
    return Status::success();
}

void JLinkDllApi::disconnectTarget()
{
    if (m_disconnect)
        m_disconnect();
}

Status JLinkDllApi::halt()
{
    if (!m_halt)
        return Status::failure(QStringLiteral("JLINK_Halt 不可用"));
    // JLINK_Halt/JLINK_Go are state transitions, not commands: calling Go on a core
    // that already runs crashes inside the DLL (reproduced on V8.26). Guarding here
    // means callers can ask for the state they want without tracking it themselves.
    if (isHalted())
        return Status::success();
    const int rc = m_halt();
    return rc < 0 ? Status::failure(QStringLiteral("暂停失败（%1）").arg(rc))
                  : Status::success();
}

Status JLinkDllApi::go()
{
    if (!m_go)
        return Status::failure(QStringLiteral("JLINK_Go 不可用"));
    if (!isHalted())
        return Status::success(); // already running; a second Go crashes the DLL
    const int rc = m_go();
    return rc < 0 ? Status::failure(QStringLiteral("运行失败（%1）").arg(rc))
                  : Status::success();
}

Status JLinkDllApi::reset()
{
    if (!m_reset)
        return Status::failure(QStringLiteral("JLINK_Reset 不可用"));
    const int rc = m_reset();
    return rc < 0 ? Status::failure(QStringLiteral("复位失败（%1）").arg(rc))
                  : Status::success();
}

bool JLinkDllApi::isHalted() const
{
    return m_isHalted && m_isHalted() != 0;
}

Status JLinkDllApi::readMemory(quint32 address, int length, QByteArray *out)
{
    if (!out)
        return Status::failure(QStringLiteral("内部错误：空输出缓冲区"));
    if (!m_readMem)
        return Status::failure(QStringLiteral("JLINK_ReadMem 不可用"));
    out->resize(length);
    // Zero the buffer before the call. JLINK_ReadMem reports failure for unmapped
    // addresses but writes NOTHING, leaving whatever the caller's memory contained --
    // measured as 0xAAAAAAAA. Callers that only check the return code are fine, but
    // anything that inspects bytes after a "readable" probe would see that garbage as
    // data (this is how a 256 KB part was reported as having 2048 KB of flash).
    out->fill(char(0));
    // SUCCESS IS EXACTLY ZERO. The DLL answers an unmapped address with a *positive*
    // error code (measured: 1) and fills the buffer with 0xAAAAAAAA, so testing
    // `rc < 0` accepted the failure and handed those bytes back as data. That is how a
    // 256 KB part was measured as having 2048 KB of flash, which then failed to match
    // any order code. Real J-Link reads return 0, never a byte count.
    const int rc = m_readMem(address, quint32(length),
                             const_cast<char *>(out->data()));
    if (rc != 0) {
        out->clear();
        m_lastError = QStringLiteral("读取 0x%1 失败（%2）")
                          .arg(address, 8, 16, QLatin1Char('0'))
                          .arg(rc);
        return Status::failure(m_lastError);
    }
    return Status::success();
}

Status JLinkDllApi::writeMemory(quint32 address, const QByteArray &data)
{
    if (!m_writeMem)
        return Status::failure(QStringLiteral("JLINK_WriteMem 不可用"));
    const int rc = m_writeMem(address, quint32(data.size()), data.constData());
    if (rc != 0) {
        m_lastError = QStringLiteral("写入 0x%1 失败（%2）")
                          .arg(address, 8, 16, QLatin1Char('0'))
                          .arg(rc);
        return Status::failure(m_lastError);
    }
    return Status::success();
}

Status JLinkDllApi::readRegister(int index, quint32 *value)
{
    if (!m_readReg)
        return Status::failure(QStringLiteral("JLINK_ReadReg 不可用"));
    if (!value)
        return Status::failure(QStringLiteral("内部错误：空寄存器指针"));
    const int rc = m_readReg(quint32(index), value);
    return rc < 0 ? Status::failure(QStringLiteral("读寄存器 %1 失败").arg(index))
                  : Status::success();
}

Status JLinkDllApi::hssStart(const QVector<HssBlock> &blocks, int periodUs)
{
    if (!hasHss())
        return Status::failure(QStringLiteral("该 J-Link DLL 不支持 HSS"));
    if (!m_hssStart || !m_hssRead || !m_hssStop)
        return Status::failure(QStringLiteral("HSS 接口不可用"));
    if (blocks.isEmpty())
        return Status::failure(QStringLiteral("没有要采样的变量"));

    // The DLL walks the array until it meets a zeroed block, so the terminator is
    // not optional: without it the last entry is followed by whatever memory holds.
    QVector<HssBlock> terminated = blocks;
    terminated.append(HssBlock());

    // flags = 1 is what SEGGER's own visualizer passes; it means "sampling enabled".
    const int rc = m_hssStart(terminated.data(), quint32(blocks.size()),
                              quint32(qMax(1, periodUs)), 1);
    if (rc < 0) {
        m_lastError = QStringLiteral("启动 HSS 失败（%1）").arg(rc);
        return Status::failure(m_lastError);
    }
    m_hssRunning = true;
    return Status::success();
}

Status JLinkDllApi::hssRead(int maxSamples, int sampleBytes, QByteArray *bytes, int *samples)
{
    if (!m_hssRunning || !m_hssRead)
        return Status::failure(QStringLiteral("HSS 未在运行"));
    if (!bytes || !samples)
        return Status::failure(QStringLiteral("内部错误：空缓冲区"));
    if (sampleBytes <= 0 || maxSamples <= 0)
        return Status::failure(QStringLiteral("内部错误：采样尺寸无效"));

    // The DLL ignores the size it is given: it writes *every* record buffered since the
    // last read, wherever that lands. Measured on V8.26 / J-Link PLUS, one 700 ms gap
    // produced 5552 records = 44 KB through a 16 KB buffer -- the overwrite lands on the
    // heap, which is what crashed before. So the buffer is sized for the backlog, not
    // for the request, and the full size is what gets passed in.
    // Sized for the backlog, and scaled by the record width: eight 8-byte blocks give a
    // 68-byte record, so a fixed 1 MiB would only cover ~15k records. 32768 records is
    // far beyond anything the probe can accumulate between two UI ticks.
    constexpr int kMinBufferBytes = 1 << 20;
    const int kBufferBytes = qMax(kMinBufferBytes, sampleBytes * 32768);
    Q_UNUSED(maxSamples);

    bytes->resize(kBufferBytes);
    // Zero it every time: a zeroed tail reads as "timestamp 0, not increasing", which is
    // how the end of real data is detected. Leftover bytes from a previous drain would
    // look like samples.
    std::memset(bytes->data(), 0, size_t(kBufferBytes));
    const int got = m_hssRead(bytes->data(), quint32(kBufferBytes));
    if (got < 0) {
        bytes->clear();
        *samples = 0;
        m_lastError = QStringLiteral("读取 HSS 数据失败（%1）").arg(got);
        return Status::failure(m_lastError);
    }

    // Never trust the count past what the buffer can physically hold.
    const int capacity = kBufferBytes / qMax(1, sampleBytes);
    *samples = qMin(got, capacity);
    bytes->resize(*samples * sampleBytes);
    return Status::success();
}

Status JLinkDllApi::hssStop()
{
    if (!m_hssRunning)
        return Status::success();
    m_hssRunning = false;
    if (!m_hssStop)
        return Status::success();
    const int rc = m_hssStop();
    return rc < 0 ? Status::failure(QStringLiteral("停止 HSS 失败（%1）").arg(rc))
                  : Status::success();
}

} // namespace mrd
