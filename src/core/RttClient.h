#pragma once

#include "core/IProbe.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace mrd {

/// One SEGGER RTT up-channel as described by the target's control block.
///
/// Layout is the public `SEGGER_RTT_BUFFER_UP` struct (32-bit ARM):
///   0x00 sName      pointer to the channel name
///   0x04 pBuffer    pointer to the ring buffer
///   0x08 SizeOfBuffer
///   0x0C WrOff      written by the target
///   0x10 RdOff      written by the reader (us)
///   0x14 Flags
struct RttBuffer
{
    QString name;
    quint32 descriptorAddress = 0;
    quint32 bufferAddress = 0;
    quint32 size = 0;
    quint32 writeOffsetAddress = 0;
    quint32 readOffsetAddress = 0;
    quint32 flags = 0;

    bool valid() const { return bufferAddress != 0 && size > 0; }
};

/// The target's RTT control block (`SEGGER_RTT_CB`).
struct RttControlBlock
{
    quint32 address = 0;
    int maxUpBuffers = 0;
    int maxDownBuffers = 0;
    QVector<RttBuffer> upBuffers;

    bool valid() const { return address != 0 && !upBuffers.isEmpty(); }
    const RttBuffer *buffer(int index) const;
};

/// Read SEGGER RTT data out of a running target over the probe.
///
/// This is the standard way RTT is consumed, and the reason it is *not* polling in
/// the sense that matters: the target writes into a ring buffer on its own
/// schedule, and the reader only drains what is already there. Nothing here
/// suspends the core, single-steps it, or triggers any target-side action -- a read
/// of the write offset is a plain memory peek, and the data itself is one bulk
/// `readMemory` per burst.
///
/// The ring buffer is drained by advancing `RdOff`; that write is required by the
/// RTT protocol (without it the target sees a full buffer and starts dropping
/// data), and it touches only the reader-owned field of the descriptor.
class RttClient
{
public:
    /// Locate the control block.
    ///
    /// \a hintAddress comes from the `_SEGGER_RTT` symbol when the ELF has one --
    /// that is the reliable route, because the control block is a normal global.
    /// Without a hint the RAM window is scanned for the `SEGGER RTT` id, which is
    /// what tools do when they have no symbol file.
    static Result<RttControlBlock> find(IProbe &probe, quint32 hintAddress,
                                        quint32 ramStart, quint32 ramSize);

    /// Typed struct size constants, exposed for the self-test.
    static constexpr int kIdOffset = 0x00;
    static constexpr int kIdSize = 16;
    static constexpr int kMaxUpOffset = 0x10;
    static constexpr int kMaxDownOffset = 0x14;
    static constexpr int kUpArrayOffset = 0x18;
    static constexpr int kUpStride = 24;

    /// Bytes that appear at the start of a control block.
    static QByteArray expectedId();

    /// Drain everything written since the last call.
    ///
    /// \a readOffset is updated to the new read position so the next call resumes
    /// where this one stopped. Returns the raw bytes exactly as the target wrote
    /// them -- interpreting them is the caller's job, since the payload format is
    /// whatever the firmware chose.
    static Result<QByteArray> drain(IProbe &probe, const RttBuffer &buffer, quint32 *readOffset);
};

} // namespace mrd
