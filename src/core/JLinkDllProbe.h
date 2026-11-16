#pragma once

#include "core/IProbe.h"
#include "core/JLinkDllApi.h"

#include <memory>

namespace mrd {

/// Probe backend that talks to the J-Link DLL directly instead of driving
/// `JLinkGDBServerCL.exe` over the GDB RSP protocol.
///
/// The reason is HSS. Background sampling exists only in the DLL, and it is what
/// makes live variable curves possible at all: the GDB path samples a *halted*
/// snapshot, so every update costs the target its execution. Everything the register
/// views need (halt/run/reset, memory, registers) the DLL provides too, so the whole
/// session lives in one place and the GDB server no longer has to run alongside.
class JLinkDllProbe : public IProbe
{
public:
    JLinkDllProbe();
    ~JLinkDllProbe() override;

    Status open() override;
    void close() override;
    bool isOpen() const override;

    Status setInterface(const QString &interfaceName) override;
    Status setSpeed(int speedKHz) override;
    Status setDevice(const QString &deviceName) override;
    QString deviceName() const override;

    Status connectTarget() override;
    void disconnectTarget() override;
    bool isConnected() const override;

    Status halt() override;
    Status resume() override;
    Status resetTarget() override;
    bool isHalted() const override;

    Status readMemory(quint32 address, int length, QByteArray *out) override;
    Status writeMemory(quint32 address, const QByteArray &data) override;
    Status readU32(quint32 address, quint32 *out) override;
    Status writeU32(quint32 address, quint32 value) override;

    Status readCoreRegisters(QVector<quint32> *out) override;

    Status exec(const QString &command) override;

    ProbeInfo info() const override;
    QString lastError() const override;

    /// Direct access for the HSS sampler, which needs the DLL object rather than the
    /// `IProbe` surface (sampling is not a per-operation request/reply).
    JLinkDllApi *api() { return m_api.get(); }

    // --- test hooks, no hardware needed -------------------------------------
    /// Pretend a session is attached, so the re-attach state machine is testable.
    void markConnectedForTest()
    {
        m_open = true;
        m_connected = true;
    }
    /// True when the next connectTarget() must rebuild the DLL session.
    bool needsReattachForTest() const { return m_needsReattach; }
    bool connectedForTest() const { return m_connected; }

private:
    std::unique_ptr<JLinkDllApi> m_api;
    ProbeInfo m_info;
    QString m_device = QStringLiteral("Cortex-M4");
    QString m_interface = QStringLiteral("SWD");
    int m_speedKHz = 4000;
    bool m_open = false;
    bool m_connected = false;
    /// setDevice() asked for a different part: the next connectTarget() must rebuild the
    /// session, because `device =` is only read before Connect.
    bool m_needsReattach = false;
};

} // namespace mrd
