#include "core/RttClient.h"

#include "core/HexUtil.h"
#include "core/Logger.h"

namespace mrd {

const RttBuffer *RttControlBlock::buffer(int index) const
{
    if (index < 0 || index >= upBuffers.size())
        return nullptr;
    return &upBuffers.at(index);
}

QByteArray RttClient::expectedId()
{
    // "SEGGER RTT" padded with NULs to 16 bytes.
    QByteArray id = QByteArrayLiteral("SEGGER RTT");
    id.append(QByteArray(kIdSize - id.size(), '\0'));
    return id;
}

namespace {

quint32 readU32At(IProbe &probe, quint32 address, bool *ok)
{
    quint32 value = 0;
    const Status status = probe.readU32(address, &value);
    if (ok)
        *ok = static_cast<bool>(status);
    return static_cast<bool>(status) ? value : 0;
}

/// A channel name is optional; an unreadable or empty one becomes "通道 N".
QString readChannelName(IProbe &probe, quint32 pointer)
{
    if (pointer == 0)
        return {};
    // RTT names are short; 32 bytes is plenty and keeps this to one round-trip.
    QByteArray bytes;
    if (!probe.readMemory(pointer, 32, &bytes) || bytes.isEmpty())
        return {};
    const int nul = bytes.indexOf('\0');
    const QByteArray trimmed = nul >= 0 ? bytes.left(nul) : bytes;
    return QString::fromLatin1(trimmed).trimmed();
}

} // namespace

Result<RttControlBlock> RttClient::find(IProbe &probe, quint32 hintAddress, quint32 ramStart,
                                        quint32 ramSize)
{
    if (!probe.isConnected())
        return Result<RttControlBlock>::failure(QStringLiteral("未连接目标"));

    const QByteArray id = expectedId();

    auto parseAt = [&](quint32 address, RttControlBlock *out) -> bool {
        QByteArray header;
        if (!probe.readMemory(address, kIdSize, &header) || header.size() < kIdSize)
            return false;
        if (header.left(10) != id.left(10))
            return false; // not "SEGGER RTT"

        bool ok = false;
        const quint32 maxUp = readU32At(probe, address + kMaxUpOffset, &ok);
        if (!ok || maxUp == 0 || maxUp > 32)
            return false;

        out->address = address;
        out->maxUpBuffers = static_cast<int>(maxUp);
        out->maxDownBuffers = static_cast<int>(readU32At(probe, address + kMaxDownOffset, &ok));

        for (int i = 0; i < out->maxUpBuffers; ++i) {
            const quint32 descriptor = address + kUpArrayOffset + i * kUpStride;
            QByteArray raw;
            if (!probe.readMemory(descriptor, kUpStride, &raw) || raw.size() < kUpStride)
                break;
            const uchar *b = reinterpret_cast<const uchar *>(raw.constData());
            auto le32 = [&](int offset) -> quint32 {
                return static_cast<quint32>(b[offset]) | (static_cast<quint32>(b[offset + 1]) << 8)
                    | (static_cast<quint32>(b[offset + 2]) << 16)
                    | (static_cast<quint32>(b[offset + 3]) << 24);
            };
            RttBuffer buffer;
            buffer.descriptorAddress = descriptor;
            const quint32 namePointer = le32(0x00);
            buffer.bufferAddress = le32(0x04);
            buffer.size = le32(0x08);
            buffer.writeOffsetAddress = descriptor + 0x0C;
            buffer.readOffsetAddress = descriptor + 0x10;
            buffer.flags = le32(0x14);
            buffer.name = readChannelName(probe, namePointer);
            if (buffer.name.isEmpty())
                buffer.name = QStringLiteral("通道 %1").arg(i);
            if (!buffer.valid())
                break; // upstream channels are allocated contiguously
            out->upBuffers.append(buffer);
        }
        return out->valid();
    };

    RttControlBlock block;

    // Preferred: the `_SEGGER_RTT` symbol from the ELF is the exact address.
    if (hintAddress != 0 && parseAt(hintAddress, &block)) {
        Logger::instance()->debug(QStringLiteral("RTT: 控制块在符号地址 %1，%2 个上行通道")
                                      .arg(hexAddr(hintAddress))
                                      .arg(block.upBuffers.size()));
        return Result<RttControlBlock>::success(block);
    }

    // Fallback: scan the RAM window for the id string. Step 4 bytes at a time --
    // the control block is pointer-aligned, so a byte-wise scan would be 4x the
    // traffic for no benefit.
    if (ramStart == 0 || ramSize == 0)
        return Result<RttControlBlock>::failure(
            QStringLiteral("未找到 RTT 控制块：ELF 里没有 _SEGGER_RTT 符号，"
                           "也没有 RAM 范围可供扫描"));

    const quint32 chunk = 1024;
    for (quint32 offset = 0; offset + kIdSize <= ramSize; offset += chunk) {
        const quint32 address = ramStart + offset;
        QByteArray bytes;
        if (!probe.readMemory(address, static_cast<int>(qMin(chunk + kIdSize,
                                                             ramSize - offset)),
                              &bytes)
            || bytes.isEmpty()) {
            continue; // unmapped hole: keep going
        }
        int at = bytes.indexOf(id.left(10));
        while (at >= 0) {
            const quint32 candidate = address + static_cast<quint32>(at);
            if (parseAt(candidate, &block)) {
                Logger::instance()->debug(
                    QStringLiteral("RTT: 扫描到控制块 %1，%2 个上行通道")
                        .arg(hexAddr(candidate))
                        .arg(block.upBuffers.size()));
                return Result<RttControlBlock>::success(block);
            }
            at = bytes.indexOf(id.left(10), at + 1);
        }
    }

    return Result<RttControlBlock>::failure(
        QStringLiteral("未找到 RTT 控制块。目标固件需要链接 SEGGER RTT（SEGGER_RTT.c），"
                       "并且已经调用过 SEGGER_RTT_Init / 写过至少一次数据"));
}

Result<QByteArray> RttClient::drain(IProbe &probe, const RttBuffer &buffer, quint32 *readOffset)
{
    if (!buffer.valid() || !readOffset)
        return Result<QByteArray>::failure(QStringLiteral("RTT 通道无效"));

    bool ok = false;
    const quint32 writeOffset = readU32At(probe, buffer.writeOffsetAddress, &ok);
    if (!ok)
        return Result<QByteArray>::failure(QStringLiteral("无法读取 RTT 写指针"));

    quint32 read = *readOffset;
    if (read >= buffer.size) {
        // The descriptor was re-initialised (or we are seeing it for the first
        // time after a target reset): resynchronise to the writer's position.
        read = writeOffset < buffer.size ? writeOffset : 0;
        *readOffset = read;
        return Result<QByteArray>::success(QByteArray());
    }

    if (writeOffset == read)
        return Result<QByteArray>::success(QByteArray()); // nothing new

    // Data may wrap around the end of the ring.
    QByteArray collected;
    if (writeOffset > read) {
        const int length = static_cast<int>(writeOffset - read);
        QByteArray chunk;
        if (!probe.readMemory(buffer.bufferAddress + read, length, &chunk))
            return Result<QByteArray>::failure(QStringLiteral("读取 RTT 缓冲区失败"));
        collected = chunk;
    } else {
        const int tailLength = static_cast<int>(buffer.size - read);
        QByteArray tail;
        if (tailLength > 0) {
            if (!probe.readMemory(buffer.bufferAddress + read, tailLength, &tail))
                return Result<QByteArray>::failure(QStringLiteral("读取 RTT 缓冲区尾部失败"));
        }
        QByteArray head;
        if (writeOffset > 0) {
            if (!probe.readMemory(buffer.bufferAddress, static_cast<int>(writeOffset), &head))
                return Result<QByteArray>::failure(QStringLiteral("读取 RTT 缓冲区头部失败"));
        }
        collected = tail + head;
    }

    // Advance RdOff so the target can keep writing. This is the only write the
    // reader performs, and it only touches the reader-owned field.
    const quint32 nextRead = writeOffset;
    if (!probe.writeU32(buffer.readOffsetAddress, nextRead))
        return Result<QByteArray>::failure(QStringLiteral("无法更新 RTT 读指针"));
    *readOffset = nextRead;

    return Result<QByteArray>::success(collected);
}

} // namespace mrd
