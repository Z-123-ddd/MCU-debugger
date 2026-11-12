#pragma once

#include "core/Result.h"

#include <QtGlobal>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QStringList>

namespace mrd {

/// Snapshot of the probe (what is physically attached) - independent of the
/// target MCU.
struct ProbeInfo
{
    QString backend;        ///< "probe-rs"
    QString binaryPath;     ///< probe-rs executable
    QString version;        ///< probe-rs version string
    QString serialNumber;   ///< first probe serial reported by `probe-rs list`
    QStringList probeSerials;
    QString interfaceName;  ///< "SWD" / "JTAG"
    int speedKHz = 0;
    bool open = false;
    bool connected = false;
    QString chipName;       ///< probe-rs chip name currently selected
    QString detectedChip;   ///< chip name detected by `probe-rs info`
    bool halted = false;
};

/// Debug-probe abstraction. The current implementation drives the official
/// `probe-rs` CLI (see ProbeRsProbe); keeping the interface separate keeps the
/// UI testable and leaves room for other transports later.
class IProbe
{
public:
    virtual ~IProbe() = default;

    virtual Status open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual Status setInterface(const QString &interfaceName) = 0;
    virtual Status setSpeed(int speedKHz) = 0;
    virtual Status setDevice(const QString &deviceName) = 0;
    virtual QString deviceName() const = 0;

    virtual Status connectTarget() = 0;
    virtual void disconnectTarget() = 0;
    virtual bool isConnected() const = 0;

    virtual Status halt() = 0;
    virtual Status resume() = 0;
    virtual Status resetTarget() = 0;
    virtual bool isHalted() const = 0;

    virtual Status readMemory(quint32 address, int length, QByteArray *out) = 0;
    virtual Status writeMemory(quint32 address, const QByteArray &data) = 0;
    virtual Status readU32(quint32 address, quint32 *out) = 0;
    virtual Status writeU32(quint32 address, quint32 value) = 0;

    /// Read the core registers, in the backend's own order.
    ///
    /// Local variables live at "frame pointer + offset", so showing them needs at
    /// least SP and PC -- and both are only stable while the core is halted. The
    /// default reports "unsupported" so backends and test doubles that cannot do it
    /// stay valid.
    virtual Status readCoreRegisters(QVector<quint32> *out)
    {
        Q_UNUSED(out)
        return Status::failure(QStringLiteral("该后端不支持读取寄存器"));
    }

    /// Raw J-Link command (e.g. "device = STM32F103C8", "R0", "W4 0x40021000 0x1").
    virtual Status exec(const QString &command) = 0;

    virtual ProbeInfo info() const = 0;
    virtual QString lastError() const = 0;

    /// Check that the backend knows this chip name. Backends that cannot answer
    /// return success (the connection attempt itself will surface any problem).
    virtual Status validateChipName(const QString &chip, QString *message = nullptr)
    {
        Q_UNUSED(chip)
        Q_UNUSED(message)
        return Status::success();
    }
};

} // namespace mrd

Q_DECLARE_METATYPE(mrd::ProbeInfo)
