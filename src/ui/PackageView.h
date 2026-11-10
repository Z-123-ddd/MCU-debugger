#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;

namespace mrd {

/// One package pin, as described by `data/packages/*.json`.
struct PackagePin
{
    int number = 0;
    QString name;
    QString type;       ///< power / ground / clock / reset / boot / io
    QString portName;   ///< "PA" when the pin is a GPIO
    int portBit = -1;
    QStringList functions;
    /// Live state from the port decoder (colours the pin).
    QString liveMode;
    bool liveHigh = false;
    bool liveKnown = false;
};

struct PackageDescription
{
    QString packageName; ///< "LQFP48"
    QString chip;        ///< "STM32F103C8"
    /// False when `number` values are placeholders (provenance not yet verified
    /// against the vendor datasheet). The UI says so instead of pretending.
    bool numberVerified = true;
    QString note;        ///< shown in the UI hint / tooltip
    QVector<PackagePin> pins;

    bool isValid() const { return !pins.isEmpty(); }
    const PackagePin *pinByNumber(int number) const;
    const PackagePin *pinForPort(const QString &portName, int bit) const;
    static PackageDescription loadFromFile(const QString &path, QString *error = nullptr);

    /// Build a structure-only pinout from counts alone (no per-pin names).
    ///
    /// Used when the identified chip has no data/packages JSON yet: the real IO
    /// and pin counts from the CMSIS-Pack pdsc still let us draw the correct
    /// grid, and the pins are labelled generically so nothing is fabricated.
    static PackageDescription synthesize(const QString &chip, int ioCount, int pinCount,
                                         const QString &packageName);
};

/// CubeMX-like package view: chip body with pins on all four sides, coloured by
/// pin type / live GPIO mode; clicking a GPIO pin jumps to its port.
/// Geometry of one pin column/row, derived from the widget size.
struct PinMetrics
{
    int pitch = 20;
    int thickness = 11;
    int length = 31;
    int bodySpan = 260;
};

class PackageView : public QWidget
{
    Q_OBJECT

public:
    explicit PackageView(QWidget *parent = nullptr);

    /// Load every `*.json` in \a directory; falls back to an embedded map.
    void loadPackages(const QString &directory);
    bool setPackage(const QString &packageName);

    /// Inject a package that was not loaded from disk (e.g. synthesized).
    void adoptPackage(const PackageDescription &description);
    /// Drop the drawing and show a message instead (pre-identification state).
    void showWaiting(const QString &message);
    bool hasPackage() const { return !m_current.isEmpty(); }

    QString currentPackage() const;
    QStringList packages() const { return m_packages.keys(); }
    QVector<PackageDescription> descriptions() const { return m_packages.values().toVector(); }

    void setLivePinState(const QString &portName, int bit, const QString &mode, bool high, bool known);
    void clearLiveState();

    /// Chip name shown in the package body.
    void setChipLabel(const QString &chip);

    QSize sizeHint() const override { return QSize(560, 560); }

signals:
    void pinActivated(const QString &portName, int bit);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct PinRect
    {
        QRect rect;
        int pinNumber = 0;
        int side = 0; ///< 0 = left, 1 = bottom, 2 = right, 3 = top
    };
    void layoutPins();
    void paintPin(QPainter &painter, const PackagePin &pin, const PinRect &geometry);
    QColor colorFor(const PackagePin &pin) const;

    QHash<QString, PackageDescription> m_packages;
    QString m_current;
    QString m_waitingMessage = QStringLiteral("连接目标后自动识别并渲染引脚布局");
    QString m_chipLabel;
    QVector<PinRect> m_geometry;
    QRect m_bodyRect;        ///< chip body in widget coordinates (centred)
    QSize m_layoutSize;      ///< widget size the geometry was computed for
    PinMetrics m_metrics;    ///< pitch/thickness/length used for the last layout
    QPoint m_offset;         ///< keeps the whole drawing centred in the widget
    int m_hoveredPin = -1;
    int pinAt(const QPoint &pos) const;
};

class PackagePage : public QWidget
{
    Q_OBJECT

public:
    explicit PackagePage(QWidget *parent = nullptr);

    PackageView *view() const { return m_view; }
    void setPackages(const QStringList &names);

    /// Draw the pinout for the chip J-Link just identified.
    /// Prefers a real data/packages JSON; otherwise synthesizes the grid from the
    /// given IO / pin counts. Returns false when there is nothing to draw.
    bool renderForChip(const QString &chip, int ioCount, int pinCount,
                       const QString &packageHint = QString());

    /// Pre-identification / disconnected state.
    void showWaiting(const QString &reason);

private:
    void updateHint(const QString &packageName);

    QComboBox *m_box = nullptr;
    QLabel *m_hint = nullptr;
    PackageView *m_view = nullptr;
};

} // namespace mrd
