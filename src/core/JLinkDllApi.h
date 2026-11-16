#pragma once

#include "core/Result.h"

#include <QString>

#include <cstdint>

namespace mrd {

/// Runtime binding to the SEGGER J-Link DLL.
///
/// Loaded with QLibrary rather than linked, for three reasons:
///   - the DLL is a separate install the user may update or move;
///   - a 64-bit host needs `JLink_x64.dll` while a 32-bit one needs `JLinkARM.dll`,
///     and only one of them may exist;
///   - the HSS entry points are not documented outside the paid SDK, so they are
///     resolved by name at runtime and their absence must be a clean failure.
///
/// Every signature below was recovered from SEGGER's own visualizer binary and
/// verified against a live probe: JLINK_HSS_Start
/// returns 0 on success, and JLINK_HSS_Read returns a *sample count* whose stride is
/// 4-byte timestamp followed by the sum of the configured block sizes.
class JLinkDllApi
{
public:
    /// One HSS block: exactly 16 bytes, which is what the DLL walks over.
    struct HssBlock
    {
        quint32 address = 0;
        quint32 numBytes = 0;
        quint32 reserved0 = 0;
        quint32 reserved1 = 0;
    };

    JLinkDllApi();
    ~JLinkDllApi();
    JLinkDllApi(const JLinkDllApi &) = delete;
    JLinkDllApi &operator=(const JLinkDllApi &) = delete;

    /// Load the DLL and resolve every symbol. Safe to call repeatedly.
    Status load();
    void unload();
    bool isLoaded() const { return m_loaded; }
    QString libraryPath() const { return m_path; }

    /// Cheap existence probe, so the UI can refuse to start with a clear message
    /// instead of failing at the first API call.
    static bool libraryAvailable();

    /// True when the HSS entry points were all found (older DLLs lack them).
    bool hasHss() const { return m_hssStart && m_hssRead && m_hssStop; }

    // --- session ------------------------------------------------------------
    /// OpenEx returns 0 on success; the message buffer receives the version banner.
    Status open(QString *banner = nullptr);
    void close();
    /// 0 = JTAG, 1 = SWD (matches pylink's JLinkInterfaces enum).
    Status selectInterface(const QString &name);
    Status setSpeed(int speedKHz);
    Status execCommand(const QString &command, QString *output = nullptr);

    // --- target -------------------------------------------------------------
    Status connectTarget();
    void disconnectTarget();
    quint32 idCode() const { return m_idCode; }
    Status halt();
    Status go();
    Status reset();
    bool isHalted() const;

    // --- memory -------------------------------------------------------------
    /// JLINK_ReadMem returns 0 on success (not a byte count), which is easy to get
    /// wrong: a "returned < requested" test would reject every good read.
    Status readMemory(quint32 address, int length, QByteArray *out);
    Status writeMemory(quint32 address, const QByteArray &data);
    /// Core register by J-Link index: 0..15 = R0..R15, 16 = xPSR.
    Status readRegister(int index, quint32 *value);

    // --- high-speed sampling ------------------------------------------------
    /// Start background sampling of \a blocks every \a periodUs microseconds.
    ///
    /// The array must carry one extra zeroed entry: the DLL walks until it sees a
    /// block with a zero address, so a missing terminator reads past the end.
    Status hssStart(const QVector<HssBlock> &blocks, int periodUs);
    /// Pull whatever samples are buffered. Returns the sample count and fills
    /// \a bytes with the raw records (each = 4-byte timestamp + block payloads).
    Status hssRead(int maxSamples, int sampleBytes, QByteArray *bytes, int *samples);
    Status hssStop();
    bool hssRunning() const { return m_hssRunning; }

    QString lastError() const { return m_lastError; }

private:
    using OpenExFn = quint32 (*)(const char *, char *, int);
    using VoidFn = void (*)();
    using IntFn = int (*)();
    using U32Fn = quint32 (*)();
    using IntIntFn = int (*)(int);
    using IntU32Fn = int (*)(quint32);
    using ReadMemFn = int (*)(quint32, quint32, void *);
    using WriteMemFn = int (*)(quint32, quint32, const void *);
    using ReadRegFn = int (*)(quint32, quint32 *);
    using ExecFn = int (*)(const char *, char *);
    using HssStartFn = int (*)(HssBlock *, quint32, quint32, quint32);
    using HssReadFn = int (*)(void *, quint32);
    using HssStopFn = int (*)();

    template <typename Fn>
    Fn resolve(const char *name);

    void *m_library = nullptr;
    bool m_loaded = false;
    QString m_path;
    QString m_lastError;
    quint32 m_idCode = 0;
    bool m_hssRunning = false;

    OpenExFn m_openEx = nullptr;
    VoidFn m_close = nullptr;
    IntIntFn m_tifSelect = nullptr;
    IntU32Fn m_setSpeed = nullptr;
    ExecFn m_exec = nullptr;
    IntFn m_connect = nullptr;
    VoidFn m_disconnect = nullptr;
    IntFn m_halt = nullptr;
    IntFn m_go = nullptr;
    IntFn m_reset = nullptr;
    U32Fn m_isHalted = nullptr;
    U32Fn m_getId = nullptr;
    ReadMemFn m_readMem = nullptr;
    WriteMemFn m_writeMem = nullptr;
    ReadRegFn m_readReg = nullptr;
    HssStartFn m_hssStart = nullptr;
    HssReadFn m_hssRead = nullptr;
    HssStopFn m_hssStop = nullptr;
};

} // namespace mrd
