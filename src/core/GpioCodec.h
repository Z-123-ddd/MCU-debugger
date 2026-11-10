#pragma once

#include "core/HexUtil.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace mrd {

struct SvdPeripheral;

/// Decoded state of one GPIO pin.
struct GpioPin
{
    int index = 0;              ///< 0..15
    bool present = true;
    /// False when the port exists on the die but this package bonds no pin for
    /// this bit. Such bits must not be drawn: a 48-pin part bringing out only
    /// PH0/PH1/PH6 must not show a 16-cell GPIOH matrix.
    bool bonded = true;
    QString portName;           ///< "PA"
    QString name() const { return portName + QString::number(index); }

    QString mode;               ///< "输入" / "输出" / "复用" / "模拟"
    QString subMode;            ///< "浮空/上拉/下拉", "推挽/开漏", "AF7" ...
    QString speed;              ///< "2MHz" / "低" ...
    bool outputHigh = false;    ///< ODR bit
    bool inputHigh = false;     ///< IDR bit
    QString function;           ///< peripheral hint, e.g. "USART1_TX"

    /// Uninitialised = every configuration bit still holds its reset value.
    ///
    /// On N32H47x the PMODE reset value is 0xFFFFFFFF, i.e. every pin reads `11`
    /// (= analog) straight out of reset. Register state alone cannot tell
    /// "firmware configured analog" from "firmware never touched it", so the
    /// caller passes the pins it configured itself (see explicitlyConfigured in
    /// decode()) and everything still at the reset value is reported as
    /// Uninitialized instead of masquerading as an analog pin.
    enum Kind { Uninitialized, Input, Output, Alternate, Analog, Unknown } kind = Unknown;
};

/// One register write needed to (re)configure a pin.
struct GpioWrite
{
    quint32 address = 0;  ///< absolute address
    quint32 value = 0;    ///< full new register value (read-modify-write result)
};

/// Target configuration for a single pin, as chosen in the port matrix menu.
struct GpioPinConfig
{
    enum Mode { AnalogInput, DigitalInput, DigitalOutput, AlternateFunction };
    enum Pull { NoPull, PullUp, PullDown };

    Mode mode = DigitalInput;
    Pull pull = NoPull;
    bool openDrain = false;
    int alternateFunction = 0; ///< only used by AlternateFunction
};

/// Decodes GPIO registers into per-pin state.
///
/// Three register layouts exist in the datasets this tool targets:
///   * STM32F1 style    : CRL/CRH (2 bits MODE + 2 bits CNF per pin), IDR/ODR
///   * STM32F4/G4/L4    : MODER/OTYPER/OSPEEDR/PUPDR/IDR/ODR/AFR[2]
///   * Nations N32H47x  : PMODE/PUPD/POTYPE/AFSEL0..3/DS/PID/POD
/// The layout is chosen from the registers the peripheral actually declares, so
/// no per-family table is needed.
class GpioCodec
{
public:
    /// True when \a peripheral looks like a GPIO port (has IDR/ODR or MODER).
    static bool isGpioPort(const SvdPeripheral &peripheral);

    /// Port letter for "GPIOA"/"PA"/"PORTA" -> "PA"; empty when not a port.
    static QString portName(const SvdPeripheral &peripheral);

    /// Decode every pin. \a values is keyed by absolute address.
    static QVector<GpioPin> decode(const SvdPeripheral &peripheral,
                                   const QHash<quint32, quint32> &values,
                                   bool *valuesValid = nullptr,
                                   const QSet<QString> &explicitlyConfigured = {},
                                   const QVector<int> &bondedBits = {});

    /// Register writes that turn bit into config, read-modify-writing from
    /// current. Returns an empty list when the port layout is not recognised.
    static QVector<GpioWrite> encodePinConfig(const SvdPeripheral &peripheral, int bit,
                                              const GpioPinConfig &config,
                                              const QHash<quint32, quint32> &current);

    /// Human readable label for a configuration choice (menu text / logging).
    static QString describeConfig(const GpioPinConfig &config);

    /// Register write that drives an OUTPUT pin high or low.
    ///
    /// N32H47x / STM32 both expose an atomic set/clear register, which is
    /// preferred over a read-modify-write of the output data register because it
    /// cannot disturb other pins:
    ///   N32H47x   GPIOx_PBSC (0x24): bits[15:0] = set, bits[31:16] = clear
    ///             GPIOx_PBC  (0x28): bits[15:0] = clear
    ///             GPIOx_POD  (0x30): plain output data (fallback)
    ///   STM32F4/G4  BSRR:   bits[15:0] = set, bits[31:16] = reset
    ///   STM32F1     BSRR/BRR
    /// Returns false when the port has no output-data register at all.
    static bool encodePinLevel(const SvdPeripheral &peripheral, int bit, bool high,
                               const QHash<quint32, quint32> &current, GpioWrite *write,
                               QString *registerName = nullptr);

    /// Suggested colour key for the pin ("in"/"out"/"af"/"ana"/"unknown").
    static QString colorKey(const GpioPin &pin);
};

} // namespace mrd
