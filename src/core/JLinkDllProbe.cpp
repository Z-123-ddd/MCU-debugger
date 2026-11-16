#include "core/JLinkDllProbe.h"

#include "core/JLinkDeviceName.h"
#include "core/Logger.h"

#include <cstring>

namespace mrd {

JLinkDllProbe::JLinkDllProbe()
    : m_api(std::make_unique<JLinkDllApi>())
{
    m_info.backend = QStringLiteral("jlink-dll");
    m_info.interfaceName = m_interface;
    m_info.speedKHz = m_speedKHz;
    m_info.chipName = m_device;
}

JLinkDllProbe::~JLinkDllProbe()
{
    close();
}

Status JLinkDllProbe::open()
{
    if (m_open)
        return Status::success();
    if (const Status st = m_api->load(); !st)
        return st;

    QString banner;
    if (const Status st = m_api->open(&banner); !st)
        return st;

    m_api->selectInterface(m_interface);
    m_api->setSpeed(m_speedKHz);
    m_info.binaryPath = m_api->libraryPath();
    m_info.version = banner;
    m_info.open = true;
    m_open = true;
    Logger::instance()->debug(QStringLiteral("J-Link DLL 会话已建立：%1").arg(banner));
    return Status::success();
}

void JLinkDllProbe::close()
{
    if (!m_open)
        return;
    m_api->close();
    m_api->unload();
    m_open = false;
    m_connected = false;
    m_info.open = false;
    m_info.connected = false;
}

bool JLinkDllProbe::isOpen() const
{
    return m_open;
}

Status JLinkDllProbe::setInterface(const QString &interfaceName)
{
    m_interface = interfaceName.isEmpty() ? QStringLiteral("SWD") : interfaceName;
    m_info.interfaceName = m_interface;
    if (m_open)
        return m_api->selectInterface(m_interface);
    return Status::success();
}

Status JLinkDllProbe::setSpeed(int speedKHz)
{
    if (speedKHz > 0)
        m_speedKHz = speedKHz;
    m_info.speedKHz = m_speedKHz;
    if (m_open)
        return m_api->setSpeed(m_speedKHz);
    return Status::success();
}

Status JLinkDllProbe::setDevice(const QString &deviceName)
{
    const QString name = deviceName.trimmed();
    if (name.isEmpty())
        return Status::success();
    if (name == m_device)
        return Status::success();

    m_device = name;
    m_info.chipName = m_device;

    // `device = X` is only honoured before JLINK_Connect, and the DLL refuses a second
    // Connect on a live session. So a device change means a NEW session, exactly like
    // the GDB backend's stopServer(). Without this the identification re-attach silently
    // did nothing: the session kept the generic Cortex-M4, so the part was never
    // recognised (reported as "G455 又识别不出来了").
    if (m_connected)
        m_needsReattach = true;
    return Status::success();
}

QString JLinkDllProbe::deviceName() const
{
    return m_device;
}

Status JLinkDllProbe::connectTarget()
{
    if (!m_open) {
        if (const Status st = open(); !st)
            return st;
    }

    // Tear the old session down before switching device: Close releases both the target
    // and the probe, then OpenEx gives a fresh context where `device =` takes effect.
    if (m_needsReattach) {
        m_api->close();
        QString banner;
        if (const Status st = m_api->open(&banner); !st)
            return st;
        m_needsReattach = false;
        m_connected = false;
        m_info.connected = false;
    } else if (m_connected) {
        // Already attached with this device: re-running JLINK_Connect on a live session
        // fails with -1 and used to be mistaken for "the device name is wrong".
        return Status::success();
    }

    // Only names the DLL actually knows. Measured on V8.26: `device = N32G455` (a
    // family name that is not in the catalogue) crashes the process outright, while
    // `device = N32G455CC` connects. A typed or remembered name can be anything, so
    // validate before handing it over and fall back to the generic core.
    QString deviceForDll = m_device;
    if (JLinkDeviceName::hasCatalogue() && !JLinkDeviceName::isKnown(deviceForDll)) {
        Logger::instance()->warn(
            QStringLiteral("J-Link 目录中没有该器件名，改用通用 Cortex-M4 以免崩溃：%1")
                .arg(deviceForDll));
        deviceForDll = QStringLiteral("Cortex-M4");
    }
    if (const Status st = m_api->execCommand(QStringLiteral("device = %1").arg(deviceForDll));
        !st)
        Logger::instance()->warn(st.error);
    if (const Status st = m_api->selectInterface(m_interface); !st)
        return st;
    m_api->setSpeed(m_speedKHz);

    if (const Status st = m_api->connectTarget(); !st)
        return st;

    m_connected = true;
    m_info.connected = true;
    m_info.halted = m_api->isHalted();
    m_info.detectedChip = m_device;
    Logger::instance()->debug(QStringLiteral("J-Link DLL 已连接目标（%1，IDCODE 0x%2）")
                                 .arg(m_device)
                                 .arg(m_api->idCode(), 8, 16, QLatin1Char('0')));
    return Status::success();
}

void JLinkDllProbe::disconnectTarget()
{
    if (!m_connected)
        return;
    m_api->disconnectTarget();
    m_connected = false;
    m_info.connected = false;
    m_needsReattach = false;
}

bool JLinkDllProbe::isConnected() const
{
    return m_connected;
}

Status JLinkDllProbe::halt()
{
    if (const Status st = m_api->halt(); !st)
        return st;
    m_info.halted = true;
    return Status::success();
}

Status JLinkDllProbe::resume()
{
    if (const Status st = m_api->go(); !st)
        return st;
    m_info.halted = false;
    return Status::success();
}

Status JLinkDllProbe::resetTarget()
{
    if (const Status st = m_api->reset(); !st)
        return st;
    m_info.halted = m_api->isHalted();
    return Status::success();
}

bool JLinkDllProbe::isHalted() const
{
    return m_api->isHalted();
}

Status JLinkDllProbe::readMemory(quint32 address, int length, QByteArray *out)
{
    return m_api->readMemory(address, length, out);
}

Status JLinkDllProbe::writeMemory(quint32 address, const QByteArray &data)
{
    return m_api->writeMemory(address, data);
}

Status JLinkDllProbe::readU32(quint32 address, quint32 *out)
{
    if (!out)
        return Status::failure(QStringLiteral("内部错误：空指针"));
    QByteArray bytes;
    if (const Status st = m_api->readMemory(address, 4, &bytes); !st)
        return st;
    if (bytes.size() < 4)
        return Status::failure(QStringLiteral("读取长度不足"));
    std::memcpy(out, bytes.constData(), 4);
    return Status::success();
}

Status JLinkDllProbe::writeU32(quint32 address, quint32 value)
{
    QByteArray data(4, char(0));
    std::memcpy(data.data(), &value, 4);
    return m_api->writeMemory(address, data);
}

Status JLinkDllProbe::readCoreRegisters(QVector<quint32> *out)
{
    if (!out)
        return Status::failure(QStringLiteral("内部错误：空指针"));
    // J-Link's Cortex-M register indices: 0..15 are R0..R15, 16 is xPSR. The UI
    // expects SP at 13 and PC at 15, which this ordering already provides.
    out->clear();
    out->reserve(17);
    for (int i = 0; i < 17; ++i) {
        quint32 value = 0;
        if (const Status st = m_api->readRegister(i, &value); !st)
            return st;
        out->append(value);
    }
    return Status::success();
}

Status JLinkDllProbe::exec(const QString &command)
{
    return m_api->execCommand(command);
}

ProbeInfo JLinkDllProbe::info() const
{
    return m_info;
}

QString JLinkDllProbe::lastError() const
{
    return m_api->lastError();
}

} // namespace mrd
