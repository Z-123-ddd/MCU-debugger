#pragma once

#include "core/SvdTypes.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QVector>

#include <memory>

namespace mrd {

class ProbeController;

/// One decoded register value plus its provenance.
struct RegisterValue
{
    quint32 value = 0;
    bool valid = false;      ///< a read actually happened
    bool offline = false;    ///< shown from resetValue because nothing was read
    QString error;
    qint64 timestampMs = 0;

    bool differsFromReset = false;
};

/// Keeps the loaded SVD device, the register snapshot cache and the write
/// history; talks to the probe through ProbeController.
class RegisterSession : public QObject
{
    Q_OBJECT

public:
    explicit RegisterSession(ProbeController *probe, QObject *parent = nullptr);
    ~RegisterSession() override;

    // --- device ------------------------------------------------------------
    void setDevice(const SvdDevice &device);
    void clearDevice();
    const SvdDevice &device() const { return m_device; }
    bool hasDevice() const { return !m_device.peripherals.isEmpty(); }

    // --- values ------------------------------------------------------------
    RegisterValue valueFor(quint32 address) const;
    quint32 effectiveValue(quint32 address) const; ///< cached value or resetValue
    QHash<quint32, quint32> snapshot(const SvdPeripheral &peripheral) const;
    QHash<quint32, quint32> allValues() const;

    void applyRead(quint32 address, const QByteArray &data, bool ok, const QString &error);
    void setValueLocally(quint32 address, quint32 value);

    // --- access ------------------------------------------------------------
    /// Read the whole window of \a peripheral (one probe-rs call per poll).
    void requestPeripheralRead(const SvdPeripheral &peripheral);
    void requestRegisterRead(const SvdPeripheral &peripheral, const SvdRegister &reg);

    /// Write a register value (4/2/1 byte depending on size).
    void writeRegister(const SvdPeripheral &peripheral, const SvdRegister &reg, quint32 value);
    /// Read-modify-write a single field.
    void writeField(const SvdPeripheral &peripheral, const SvdRegister &reg, const SvdField &field,
                    quint32 fieldValue);

    /// Raw write to an absolute address with an already encoded payload
    /// (used by the pin-configuration path, which read-modify-writes several
    /// registers at once).
    void applyRegisterWrite(quint32 address, const QByteArray &data);

    bool canUndo() const { return !m_history.isEmpty(); }
    void undoLastWrite();

    bool connected() const { return m_connected; }
    void setConnected(bool connected);

signals:
    void deviceChanged();
    void valuesChanged(quint32 firstAddress, quint32 lastAddress);
    void readFailed(quint32 address, const QString &error);
    void writeFinished(quint32 address, bool ok, const QString &error);
    void historyChanged();

private slots:
    void onReadFinished(quint64 requestId, quint32 address, const QByteArray &data, bool ok, const QString &error);
    void onWriteFinished(quint64 requestId, quint32 address, bool ok, const QString &error);

private:
    quint32 resetValueFor(const SvdRegister &reg) const;
    void rememberWrite(quint32 address, quint32 oldValue, bool oldValid);

    struct PendingRead
    {
        quint32 address = 0;
        int length = 0;
        qint64 startMs = 0;
        bool poll = false;
    };

    struct WriteRecord
    {
        quint32 address = 0;
        quint32 oldValue = 0;
        bool oldValid = false;
        QString label;
    };

    ProbeController *m_probe = nullptr;
    SvdDevice m_device;
    QHash<quint32, RegisterValue> m_values;
    QHash<quint64, PendingRead> m_pendingReads;
    QVector<WriteRecord> m_history;
    bool m_connected = false;
};

} // namespace mrd
