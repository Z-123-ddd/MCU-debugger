#pragma once

#include "core/GpioCodec.h"

#include <QPoint>

#include <QHash>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;

namespace mrd {

/// 4x4 matrix of one GPIO port, coloured by the decoded pin mode.
class PortGridWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PortGridWidget(QWidget *parent = nullptr);

    void setPins(const QVector<GpioPin> &pins, bool valuesValid);
    const QVector<GpioPin> &pins() const { return m_pins; }
    void setSelectedPin(int index);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(360, 300); }

signals:
    void pinClicked(int index);
    void pinActivated(int index);
    /// Reconfiguration requested from the context menu on pin  index.
    void pinConfigureRequested(int index, const mrd::GpioPinConfig &config);
    /// Drive the pin high/low without touching its mode (only meaningful for
    /// output pins); the page encodes it as a PBSC/POD write.
    void pinLevelRequested(int index, bool high);

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect pinRect(int index) const;
    int pinAt(const QPoint &pos) const;
    /// Bits this package actually bonds out, in ascending order.
    QVector<int> visibleBits() const;

    void showPinMenu(int index, const QPoint &globalPos);

    QVector<GpioPin> m_pins;
    bool m_valid = false;
    int m_selected = -1;
    int m_hovered = -1;
};

/// Page wrapping the pin matrix with a port selector and a legend.
class PortGridView : public QWidget
{
    Q_OBJECT

public:
    explicit PortGridView(QWidget *parent = nullptr);

    void setPorts(const QStringList &portNames);
    void setActivePort(const QString &portName);
    QString activePort() const;

    void setPins(const QString &portName, const QVector<GpioPin> &pins, bool valuesValid);

signals:
    void portChanged(const QString &portName);
    void pinClicked(const QString &portName, int index);
    void pinLevelRequested(const QString &portName, int index, bool high);
    /// Pin reconfigured from the matrix menu (port name + bit + target config).
    void pinConfigureRequested(const QString &portName, int index, const mrd::GpioPinConfig &config);

private:
    QComboBox *m_portBox = nullptr;
    QLabel *m_summary = nullptr;
    PortGridWidget *m_grid = nullptr;
    QHash<QString, QVector<GpioPin>> m_cache;
    bool m_updating = false;
};

} // namespace mrd
