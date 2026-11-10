#pragma once

#include "core/IProbe.h"

#include <QStringList>

namespace mrd {

class ProbeRsRunner;

/// IProbe implementation that drives the official `probe-rs` CLI.
///
/// Mapping notes (see `src/core/ProbeRsProbe.cpp`):
///   * every operation is one `probe-rs <subcommand>` process, so calls must go
///     through ProbeController's worker thread;
///   * `read`   -> halts while reading, then resumes the core;
///   * `write`  -> leaves the core halted (probe-rs never resumes after write);
///   * `halt`   -> `info`, which attaches/halts and leaves the core halted;
///   * `resume` -> a 1-word read, which resumes the core without resetting;
///   * `reset`  -> `reset`, which resets and runs.
class ProbeRsProbe : public IProbe
{
public:
    ProbeRsProbe();
    ~ProbeRsProbe() override;

    Status open() override;
    void close() override;
    bool isOpen() const override { return m_open; }

    Status setInterface(const QString &interfaceName) override;
    Status setSpeed(int speedKHz) override;
    Status setDevice(const QString &deviceName) override;
    QString deviceName() const override { return m_chip; }

    Status connectTarget() override;
    void disconnectTarget() override;
    bool isConnected() const override { return m_connected; }

    Status halt() override;
    Status resume() override;
    Status resetTarget() override;
    bool isHalted() const override { return m_halted; }

    Status readMemory(quint32 address, int length, QByteArray *out) override;
    Status writeMemory(quint32 address, const QByteArray &data) override;
    Status readU32(quint32 address, quint32 *out) override;
    Status writeU32(quint32 address, quint32 value) override;

    /// Runs raw CLI arguments, e.g. exec("chip info STM32F103C8").
    Status exec(const QString &command) override;

    ProbeInfo info() const override;
    QString lastError() const override { return m_lastError; }

    /// Detected chip name reported by `probe-rs info` (may differ from the one
    /// configured).
    QString detectedChip() const { return m_detectedChip; }

    /// Verify a chip name through `probe-rs chip info`; returns probe-rs' own
    /// message when the name is unknown or ambiguous.
    Status validateChipName(const QString &chip, QString *message = nullptr) override;

    ProbeRsRunner *runner() const { return m_runner; }

private:
    QStringList probeOptions() const;
    Status ensureChip();

    ProbeRsRunner *m_runner = nullptr;
    QString m_lastError;
    QString m_chip;
    QString m_detectedChip;
    QString m_interfaceName = QStringLiteral("SWD");
    int m_speedKHz = 4000;
    bool m_open = false;
    bool m_connected = false;
    bool m_halted = false;
    QStringList m_probeSerials;
    QString m_version;
};

} // namespace mrd
