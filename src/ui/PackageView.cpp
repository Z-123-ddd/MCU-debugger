#include "ui/PackageView.h"

#include "ui/Theme.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace mrd {

namespace {
// Proportions measured from the LCEDA pinout of this part (body 476 px,
// pitch 39.4 px, pin thickness 22 px, pin length 62 px):
//   thickness / pitch = 0.56 ,  length / pitch = 1.57 ,  body / pitch = 12.08
// They are applied as ratios of the available widget size so the drawing scales
// with the window instead of using fixed pixel constants.
constexpr double kThicknessRatio = 0.56;
constexpr double kLengthRatio = 1.57;
constexpr int kBodySlots = 1;      ///< body spans (slots + 1) * pitch
constexpr int kNumberRoom = 26;    ///< space kept outside the pins for pin numbers

PinMetrics metricsFor(const QSize &widget, int slotCount)
{
    PinMetrics m;
    const int sideSlots = qMax(1, slotCount); // NB: `slots` is a Qt keyword macro

    // Total drawing width = body + one pin on each side:
    //   body   = pitch * (sideSlots + kBodySlots)
    //   pins   = 2 * pitch * kLengthRatio
    // so solving for pitch keeps the whole footprint inside the widget.
    const double denominator = sideSlots + kBodySlots + 2.0 * kLengthRatio;
    const int available = qMin(widget.width(), widget.height()) - 2 * kNumberRoom;
    const int span = qMax(120, available);

    m.pitch = qMax(9, int(span / denominator));
    m.thickness = qMax(6, int(m.pitch * kThicknessRatio));
    m.length = qMax(12, int(m.pitch * kLengthRatio));
    m.bodySpan = m.pitch * (sideSlots + kBodySlots);

    // Guard: never exceed the widget even after the integer rounding above.
    const int footprint = m.bodySpan + 2 * m.length;
    if (footprint > span && m.pitch > 9) {
        const double scale = double(span) / double(footprint);
        m.pitch = qMax(9, int(m.pitch * scale));
        m.thickness = qMax(6, int(m.pitch * kThicknessRatio));
        m.length = qMax(12, int(m.pitch * kLengthRatio));
        m.bodySpan = m.pitch * (sideSlots + kBodySlots);
    }
    return m;
}

} // namespace

const PackagePin *PackageDescription::pinByNumber(int number) const
{
    for (const auto &pin : pins) {
        if (pin.number == number)
            return &pin;
    }
    return nullptr;
}

QHash<QString, QVector<int>> PackageDescription::bondedPorts() const
{
    QHash<QString, QVector<int>> ports;
    for (const auto &pin : pins) {
        if (pin.portName.isEmpty() || pin.portBit < 0)
            continue;
        ports[pin.portName].append(pin.portBit);
    }
    for (auto it = ports.begin(); it != ports.end(); ++it)
        std::sort(it.value().begin(), it.value().end());
    return ports;
}

QHash<QString, QVector<int>> PackageDescription::gpioPorts() const
{
    // A port qualifies only when at least one of its bonded pins is an ordinary
    // GPIO. PH on a 48-pin N32H473 is bound through OSC_IN/OSC_OUT/NRST alone,
    // so it never becomes a group; PB stays a port because PB9..PB15 are GPIO
    // even though PB8 doubles as BOOT0.
    QSet<QString> gpioCapable;
    for (const auto &pin : pins) {
        if (pin.portName.isEmpty() || pin.portBit < 0)
            continue;
        if (pin.type.isEmpty() || pin.type == QLatin1String("io"))
            gpioCapable.insert(pin.portName);
    }

    QHash<QString, QVector<int>> ports;
    for (const auto &pin : pins) {
        if (pin.portName.isEmpty() || pin.portBit < 0)
            continue;
        if (!gpioCapable.contains(pin.portName))
            continue;
        ports[pin.portName].append(pin.portBit);
    }
    for (auto it = ports.begin(); it != ports.end(); ++it)
        std::sort(it.value().begin(), it.value().end());
    return ports;
}

QVector<PackagePin> PackageDescription::dedicatedPins() const
{
    QVector<PackagePin> dedicated;
    for (const auto &pin : pins) {
        const bool ordinaryGpio = pin.type.isEmpty() || pin.type == QLatin1String("io");
        if (ordinaryGpio || pin.displayName.isEmpty())
            continue;
        dedicated << pin;
    }
    return dedicated;
}

int PackageDescription::bondedPinCount(const QString &portName) const
{
    int count = 0;
    for (const auto &pin : pins) {
        if (pin.portName.compare(portName, Qt::CaseInsensitive) == 0 && pin.portBit >= 0)
            ++count;
    }
    return count;
}

const PackagePin *PackageDescription::pinForPort(const QString &portName, int bit) const
{
    for (const auto &pin : pins) {
        if (pin.portName.compare(portName, Qt::CaseInsensitive) == 0 && pin.portBit == bit)
            return &pin;
    }
    return nullptr;
}

/// Structure-only pinout built from counts alone.
///
/// When no `data/packages/*.json` exists for the identified chip we still know the
/// real IO and pin counts (CMSIS-Pack `.pdsc`), which is enough to draw the correct
/// QFN/LQFP grid. Pins are labelled generically (IO1..IOn, PWR1..PWRm) and the
/// description is flagged unverified, so the drawing never invents pin names.
PackageDescription PackageDescription::synthesize(const QString &chip, int ioCount, int pinCount,
                                                  const QString &packageName)
{
    PackageDescription description;
    description.chip = chip;
    description.packageName = packageName.isEmpty() ? QStringLiteral("GENERIC") : packageName;
    description.numberVerified = false;
    description.note = QStringLiteral("引脚布局按 IO 数 %1 与封装脚数 %2 实时生成；"
                                      "尚无该器件的逐脚定义，引脚名为通用编号。")
                           .arg(ioCount)
                           .arg(pinCount);

    if (ioCount <= 0 || pinCount <= 0 || pinCount < ioCount)
        return description; // invalid -> caller keeps the waiting state

    const int powerCount = pinCount - ioCount;
    int number = 1;
    for (int i = 0; i < powerCount; ++i) {
        PackagePin pin;
        pin.number = number++;
        pin.name = QStringLiteral("PWR%1").arg(i + 1);
        pin.type = (i == 0) ? QStringLiteral("power") : QStringLiteral("ground");
        description.pins << pin;
    }
    for (int i = 0; i < ioCount; ++i) {
        PackagePin pin;
        pin.number = number++;
        pin.name = QStringLiteral("IO%1").arg(i + 1);
        pin.type = QStringLiteral("io");
        description.pins << pin;
    }
    return description;
}

PackageDescription PackageDescription::loadFromFile(const QString &path, QString *error)
{
    PackageDescription description;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开封装文件 %1: %2").arg(path, file.errorString());
        return description;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull()) {
        if (error)
            *error = QStringLiteral("%1 解析失败: %2").arg(path, parseError.errorString());
        return description;
    }

    const QJsonObject root = doc.object();
    description.packageName = root.value(QStringLiteral("name")).toString();
    // "numberVerified": false marks a placeholder pin table; absent means verified.
    description.numberVerified = root.value(QStringLiteral("numberVerified")).toBool(true);
    const QString comment = root.value(QStringLiteral("comment")).toString().trimmed();
    const QString countNote = root.value(QStringLiteral("pinCountNote")).toString().trimmed();
    description.note = countNote.isEmpty() ? comment : countNote;
    description.chip = root.value(QStringLiteral("chip")).toString();
    if (description.packageName.isEmpty())
        description.packageName = QFileInfo(path).completeBaseName();

    for (const auto &value : root.value(QStringLiteral("pins")).toArray()) {
        const QJsonObject o = value.toObject();
        PackagePin pin;
        pin.number = o.value(QStringLiteral("number")).toInt();
        pin.name = o.value(QStringLiteral("name")).toString();
        pin.type = o.value(QStringLiteral("type")).toString(QStringLiteral("io"));
        pin.portName = o.value(QStringLiteral("port")).toString();
        pin.portBit = o.contains(QStringLiteral("pin")) ? o.value(QStringLiteral("pin")).toInt() : -1;
        for (const auto &f : o.value(QStringLiteral("functions")).toArray())
            pin.functions << f.toString();

        if (pin.number > 0)
            description.pins << pin;
    }

    // Dedicated pins are what they do, not a GPIO bit: "PH0-OSC_IN" is drawn as
    // "OSC_IN". Only ports that have no ordinary GPIO pin at all are relabelled
    // -- PB8-BOOT0 stays PB8, because PB is a real GPIO port that merely
    // doubles as BOOT0.
    QSet<QString> gpioCapable;
    for (const auto &pin : description.pins) {
        if (pin.portName.isEmpty() || pin.portBit < 0)
            continue;
        if (pin.type.isEmpty() || pin.type == QLatin1String("io"))
            gpioCapable.insert(pin.portName);
    }
    for (auto &pin : description.pins) {
        if (pin.portName.isEmpty() || gpioCapable.contains(pin.portName))
            continue;
        const int dash = pin.name.lastIndexOf(QLatin1Char('-'));
        if (dash >= 0 && dash + 1 < pin.name.size()) {
            pin.displayName = pin.name.mid(dash + 1);
        } else {
            // STM32 writes plain "PD0"/"PD1"; the function list carries OSC_IN.
            for (const QString &function : pin.functions) {
                const bool gpioLike = function.size() >= 2 && function.at(0) == QLatin1Char('P')
                                      && function.at(1).isLetter();
                if (!gpioLike)
                    pin.displayName = function;
            }
        }
        pin.displayName = pin.displayName.trimmed();
    }
    return description;
}

PackageView::PackageView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(420, 420);
}

void PackageView::loadPackages(const QString &directory)
{
    m_packages.clear();
    QDir dir(directory);
    if (dir.exists()) {
        const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString &file : files) {
            QString error;
            const PackageDescription description =
                PackageDescription::loadFromFile(dir.absoluteFilePath(file), &error);
            if (description.isValid())
                m_packages.insert(description.packageName, description);
        }
    }
    // Deliberately no auto-selection: the pinout is drawn only after the target
    // has been identified (see PackagePage::renderForChip).
    update();
}

bool PackageView::setPackage(const QString &packageName)
{
    const auto it = m_packages.constFind(packageName);
    if (it == m_packages.constEnd())
        return false;
    m_current = packageName;
    if (m_chipLabel.isEmpty())
        m_chipLabel = it.value().chip;
    layoutPins();
    update();
    return true;
}

void PackageView::adoptPackage(const PackageDescription &description)
{
    if (!description.isValid())
        return;
    m_packages.insert(description.packageName, description);
    m_current.clear();     // force a fresh layout
    m_chipLabel = description.chip;
    setPackage(description.packageName);
}

void PackageView::showWaiting(const QString &message)
{
    m_waitingMessage = message;
    m_current.clear();
    m_chipLabel.clear();
    m_geometry.clear();
    m_bodyRect = QRect();
    m_offset = QPoint(0, 0);
    m_hoveredPin = -1;
    update();
}

QString PackageView::currentPackage() const
{
    return m_current;
}

const PackageDescription *PackageView::currentDescription() const
{
    const auto it = m_packages.constFind(m_current);
    return it == m_packages.constEnd() ? nullptr : &it.value();
}

void PackageView::setChipLabel(const QString &chip)
{
    m_chipLabel = chip;
    update();
}

void PackageView::setLivePinState(const QString &portName, int bit, const QString &mode, bool high, bool known)
{
    auto it = m_packages.find(m_current);
    if (it == m_packages.end())
        return;
    for (auto &pin : it.value().pins) {
        if (pin.portName.compare(portName, Qt::CaseInsensitive) == 0 && pin.portBit == bit) {
            pin.liveMode = mode;
            pin.liveHigh = high;
            pin.liveKnown = known;
            break;
        }
    }
    update();
}

void PackageView::clearLiveState()
{
    auto it = m_packages.find(m_current);
    if (it == m_packages.end())
        return;
    for (auto &pin : it.value().pins) {
        pin.liveMode.clear();
        pin.liveHigh = false;
        pin.liveKnown = false;
    }
    update();
}

void PackageView::layoutPins()
{
    m_geometry.clear();
    m_bodyRect = QRect();
    m_offset = QPoint(0, 0);
    m_layoutSize = size(); // the body is centred for exactly this widget size

    const auto it = m_packages.constFind(m_current);
    if (it == m_packages.constEnd())
        return;
    const QVector<PackagePin> &pins = it.value().pins;
    const int count = pins.size();
    if (count == 0)
        return;

    // QFN/LQFP ordering: pin 1 at the top-left corner, counter-clockwise,
    // evenly split over the four sides (12 per side for 48 pins).
    int perSide[4] = {count / 4, count / 4, count / 4, count / 4};
    for (int i = 0; i < count % 4; ++i)
        ++perSide[i];

    int slotCount = 1; // NB: `slots` is a Qt keyword macro, do not shadow it
    for (const int sideCount : perSide)
        slotCount = qMax(slotCount, sideCount);

    const PinMetrics m = metricsFor(size(), slotCount);
    const QPoint centre = rect().center();
    m_bodyRect = QRect(centre.x() - m.bodySpan / 2, centre.y() - m.bodySpan / 2, m.bodySpan, m.bodySpan);

    int index = 0;
    for (int side = 0; side < 4; ++side) {
        const int sideCount = perSide[side];
        // Centre the pins of a short side instead of packing them to one corner.
        const int lead = (slotCount - sideCount) / 2;

        for (int slot = 0; slot < sideCount && index < count; ++slot) {
            const int position = lead + slot;
            const int along = m.pitch * (kBodySlots + position);
            const int backwards = m.bodySpan - along;

            PinRect geometry;
            geometry.pinNumber = pins.at(index).number;
            geometry.side = side;

            switch (side) {
            case 0: // left: top -> bottom
                geometry.rect = QRect(m_bodyRect.left() - m.length, m_bodyRect.top() + along - m.thickness / 2,
                                      m.length, m.thickness);
                break;
            case 1: // bottom: left -> right
                geometry.rect = QRect(m_bodyRect.left() + along - m.thickness / 2, m_bodyRect.bottom() + 1,
                                      m.thickness, m.length);
                break;
            case 2: // right: bottom -> top
                geometry.rect = QRect(m_bodyRect.right() + 1, m_bodyRect.top() + backwards - m.thickness / 2,
                                      m.length, m.thickness);
                break;
            default: // top: right -> left
                geometry.rect = QRect(m_bodyRect.left() + backwards - m.thickness / 2,
                                      m_bodyRect.top() - m.length, m.thickness, m.length);
                break;
            }
            m_geometry << geometry;
            ++index;
        }
    }
    m_metrics = m;
}

int PackageView::pinAt(const QPoint &pos) const
{
    for (int i = 0; i < m_geometry.size(); ++i) {
        if (m_geometry.at(i).rect.translated(m_offset).contains(pos))
            return i;
    }
    return -1;
}

QColor PackageView::colorFor(const PackagePin &pin) const
{
    const Palette p = Theme::current();
    if (pin.type == QLatin1String("power"))
        return p.pinPower;
    if (pin.type == QLatin1String("ground"))
        return p.pinGround;
    if (pin.type == QLatin1String("clock"))
        return p.pinClock;
    if (pin.type == QLatin1String("reset"))
        return p.pinReset;
    if (!pin.liveMode.isEmpty()) {
        if (pin.liveMode.contains(QStringLiteral("复用")))
            return p.pinAf;
        if (pin.liveMode.contains(QStringLiteral("输出")))
            return p.pinOut;
        if (pin.liveMode.contains(QStringLiteral("模拟")))
            return p.pinAnalog;
        return p.pinIn;
    }
    return p.border;
}

void PackageView::paintPin(QPainter &painter, const PackagePin &pin, const PinRect &geometry)
{
    const QColor colour = colorFor(pin);
    const QRect r = geometry.rect.translated(m_offset);

    painter.setBrush(colour);
    QPen pen(Theme::current().border, 1);
    if (pin.number == m_hoveredPin)
        pen = QPen(Theme::current().accent, 2);
    painter.setPen(pen);
    painter.drawRect(r);

    const bool vertical = (geometry.side == 1 || geometry.side == 3);

    // Text colour: dark on bright pins, white on dark ones (LCEDA does the same).
    const int luma = (colour.red() * 299 + colour.green() * 587 + colour.blue() * 114) / 1000;
    const QColor textColour = luma > 150 ? Theme::current().text : QColor(Qt::white);

    QFont small = font();
    small.setPointSizeF(qMax(5.5, font().pointSizeF() - 2.5));
    painter.setFont(small);
    const QFontMetricsF metrics(painter.font());

    // Pin name runs along the pin axis, clipped to the pin length.
    // Dedicated pins show what they do ("OSC_IN"), not a GPIO bit they lack.
    const QString label = pin.displayName.isEmpty() ? pin.name : pin.displayName;
    const QString name = metrics.elidedText(label, Qt::ElideRight, (vertical ? r.height() : r.width()) - 4);
    painter.setPen(textColour);
    if (!vertical) {
        painter.drawText(r.adjusted(2, 0, -2, 0), Qt::AlignCenter, name);
    } else {
        painter.save();
        painter.translate(r.center());
        painter.rotate(-90);
        painter.drawText(QRectF(-r.height() / 2.0 + 2, -r.width() / 2.0, r.height() - 4, r.width()),
                         Qt::AlignCenter, name);
        painter.restore();
    }

    // Pin number sits just outside the pin, dimmed.
    painter.setPen(Theme::current().textDim);
    QFont numberFont = small;
    numberFont.setPointSizeF(qMax(5.0, small.pointSizeF() - 0.5));
    painter.setFont(numberFont);
    const QString number = QString::number(pin.number);
    switch (geometry.side) {
    case 0: // left: number to the left of the pin
        painter.drawText(QRect(r.left() - kNumberRoom + 2, r.top() - 1, kNumberRoom - 4, r.height() + 2),
                         Qt::AlignRight | Qt::AlignVCenter, number);
        break;
    case 2: // right
        painter.drawText(QRect(r.right() + 2, r.top() - 1, kNumberRoom - 4, r.height() + 2),
                         Qt::AlignLeft | Qt::AlignVCenter, number);
        break;
    case 1: // bottom
        painter.drawText(QRect(r.left() - 8, r.bottom() + 1, r.width() + 16, 10), Qt::AlignHCenter | Qt::AlignTop,
                         number);
        break;
    default: // top
        painter.drawText(QRect(r.left() - 8, r.top() - 11, r.width() + 16, 10),
                         Qt::AlignHCenter | Qt::AlignBottom, number);
        break;
    }
}

void PackageView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const Palette p = Theme::current();
    painter.fillRect(rect(), p.background);

    const auto it = m_packages.constFind(m_current);
    if (it == m_packages.constEnd()) {
        // Nothing to draw yet: say what we are waiting for instead of showing a
        // stale or arbitrary package.
        painter.setPen(p.textDim);
        QFont hint = font();
        hint.setPointSizeF(font().pointSizeF() + 0.5);
        painter.setFont(hint);
        painter.drawText(rect().adjusted(40, 0, -40, 0), Qt::AlignCenter | Qt::TextWordWrap, m_waitingMessage);
        return;
    }

    // The body is centred in the widget, so a resize invalidates the geometry.
    if (m_geometry.size() != it.value().pins.size() || m_bodyRect.isNull() || m_layoutSize != size())
        layoutPins();

    const QRect body = m_bodyRect.translated(m_offset);

    // The body must read as a distinct chip against the canvas (LCEDA uses a
    // light grey body on white); surfaceAlt alone is too close to the backdrop.
    painter.setBrush(p.surfaceAlt.darker(112));
    painter.setPen(QPen(p.border, 2));
    painter.drawRoundedRect(body, 10, 10);

    // Pin-1 marker
    painter.setBrush(p.accent);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(body.topLeft() + QPoint(16, 16), 5, 5);

    // Labels
    QFont title = font();
    title.setBold(true);
    title.setPointSizeF(title.pointSizeF() + 3.0);
    painter.setFont(title);
    painter.setPen(p.text);
    painter.drawText(body.adjusted(0, body.height() / 3, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     m_chipLabel.isEmpty() ? it.value().chip : m_chipLabel);

    QFont sub = font();
    sub.setPointSizeF(font().pointSizeF() - 1.0);
    painter.setFont(sub);
    painter.setPen(p.textDim);
    painter.drawText(body.adjusted(0, body.height() / 3 + 28, 0, 0), Qt::AlignHCenter | Qt::AlignTop,
                     it.value().packageName);

    // Pins
    for (int i = 0; i < m_geometry.size() && i < it.value().pins.size(); ++i)
        paintPin(painter, it.value().pins.at(i), m_geometry.at(i));
}

void PackageView::mousePressEvent(QMouseEvent *event)
{
    const auto it = m_packages.constFind(m_current);
    if (it == m_packages.constEnd())
        return;
    const int index = pinAt(event->pos());
    if (index >= 0 && index < it.value().pins.size()) {
        const PackagePin &pin = it.value().pins.at(index);
        if (!pin.portName.isEmpty() && pin.portBit >= 0)
            emit pinActivated(pin.portName, pin.portBit);
    }
    QWidget::mousePressEvent(event);
}

void PackageView::mouseMoveEvent(QMouseEvent *event)
{
    const auto it = m_packages.constFind(m_current);
    if (it == m_packages.constEnd())
        return;
    int hovered = -1;
    const int hit = pinAt(event->pos());
    for (int i = 0; i < m_geometry.size() && i < it.value().pins.size(); ++i) {
        if (i == hit) {
            hovered = it.value().pins.at(i).number;
            const PackagePin &pin = it.value().pins.at(i);
            QString shown = pin.displayName.isEmpty() ? pin.name : pin.displayName;
            QString text = QStringLiteral("<b>%1</b>  (引脚 %2)<br/>类型: %3").arg(shown).arg(pin.number).arg(pin.type);
            if (!pin.displayName.isEmpty() && pin.displayName != pin.name)
                text += QStringLiteral("<br/>数据手册引脚名: %1").arg(pin.name);
            if (pin.liveKnown)
                text += QStringLiteral("<br/>当前: %1 %2").arg(pin.liveMode,
                                                               pin.liveHigh ? QStringLiteral("高") : QStringLiteral("低"));
            if (!pin.functions.isEmpty())
                text += QStringLiteral("<br/>功能: %1").arg(pin.functions.join(QStringLiteral(", ")).toHtmlEscaped());
            if (!it.value().numberVerified)
                text += QStringLiteral("<br/><i>引脚号为占位编号，待数据手册核对</i>");
            if (!pin.portName.isEmpty())
                text += QStringLiteral("<br/><i>点击跳到 %1%2</i>").arg(pin.portName).arg(pin.portBit);
            QToolTip::showText(event->globalPosition().toPoint(), text, this);
            break;
        }
    }
    if (hovered != m_hoveredPin) {
        m_hoveredPin = hovered;
        if (hovered < 0)
            QToolTip::hideText();
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void PackageView::leaveEvent(QEvent *event)
{
    m_hoveredPin = -1;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// PackagePage
// ---------------------------------------------------------------------------

PackagePage::PackagePage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *top = new QHBoxLayout();
    m_packageLabel = new QLabel(QStringLiteral("封装"), this);
    top->addWidget(m_packageLabel);
    m_box = new QComboBox(this);
    m_box->setMinimumWidth(160);
    top->addWidget(m_box);
    // Nothing to choose before identification: the selector stays hidden until a
    // package has to be picked by hand.
    m_packageLabel->setVisible(false);
    m_box->setVisible(false);
    m_hint = new QLabel(QStringLiteral("引脚布局将在识别到 MCU 型号后生成"), this);
    m_hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    top->addWidget(m_hint, 1);
    layout->addLayout(top);

    m_view = new PackageView(this);
    layout->addWidget(m_view, 1);

    connect(m_box, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString name = m_box->itemData(index).toString();
        if (name.isEmpty())
            return; // placeholder row: keep the current/empty drawing
        m_view->setPackage(name);
        updateHint(name);
    });
}

void PackagePage::setPackages(const QStringList &names)
{
    {
        QSignalBlocker blocker(m_box);
        m_box->clear();
        // Placeholder entry: nothing is selected until the chip is identified.
        m_box->addItem(QStringLiteral("（连接后自动选择）"), QString());
        for (const QString &name : names)
            m_box->addItem(name, name);
        m_box->setCurrentIndex(0);
    }
    m_view->showWaiting(QStringLiteral("连接目标后自动识别并渲染引脚布局"));
    updateHint(QString());
}

bool PackagePage::renderForChip(const QString &chip, int ioCount, int pinCount, const QString &packageHint)
{
    PackageView *view = m_view;
    // The package follows from the identified part, so there is no choice to offer.
    const bool autoSelected = true;

    // 1) Prefer a real per-pin description whose chip matches the identified part.
    QString chosen;
    if (!chip.isEmpty()) {
        for (const auto &description : view->descriptions()) {
            if (description.chip.isEmpty())
                continue;
            if (description.chip.startsWith(chip, Qt::CaseInsensitive)
                || chip.startsWith(description.chip, Qt::CaseInsensitive)) {
                chosen = description.packageName;
                break;
            }
        }
        if (chosen.isEmpty() && !packageHint.isEmpty()) {
            for (const auto &description : view->descriptions()) {
                if (description.packageName.compare(packageHint, Qt::CaseInsensitive) == 0
                    && (description.chip.isEmpty()
                        || description.chip.startsWith(chip, Qt::CaseInsensitive))) {
                    chosen = description.packageName;
                    break;
                }
            }
        }
    }

    if (!chosen.isEmpty() && view->setPackage(chosen)) {
        view->setChipLabel(chip);
    } else {
        // 2) No per-pin data: draw the grid live from the counts we do know.
        const PackageDescription synthesized =
            PackageDescription::synthesize(chip, ioCount, pinCount, packageHint);
        if (!synthesized.isValid()) {
            showWaiting(QStringLiteral("已识别 %1，但缺少 IO 数/封装脚数，无法生成引脚布局")
                            .arg(chip.isEmpty() ? QStringLiteral("未知器件") : chip));
            return false;
        }
        view->adoptPackage(synthesized);
        chosen = synthesized.packageName;
    }

    // When the package follows from the identified part there is nothing to pick,
    // so the selector is hidden instead of offering options that would contradict
    // the silicon. Manual switching stays available through
    // "文件 -> 打开封装目录" only when no package could be determined.
    if (!autoSelected) {
        QSignalBlocker blocker(m_box);
        int index = m_box->findData(chosen);
        if (index < 0) {
            m_box->addItem(chosen, chosen);
            index = m_box->count() - 1;
        }
        m_box->setCurrentIndex(index);
        m_packageLabel->setVisible(true);
        m_box->setVisible(true);
    } else {
        m_packageLabel->setVisible(false);
        m_box->setVisible(false);
    }
    updateHint(chosen);
    return true;
}

void PackagePage::showWaiting(const QString &reason)
{
    m_view->showWaiting(reason);
    m_packageLabel->setVisible(false);
    m_box->setVisible(false);
    QSignalBlocker blocker(m_box);
    m_box->setCurrentIndex(0);
    m_hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    m_hint->setToolTip(QString());
    m_hint->setText(reason);
}

/// Surface placeholder provenance instead of silently trusting it.
void PackagePage::updateHint(const QString &packageName)
{
    if (packageName.isEmpty()) {
        m_hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
        m_hint->setToolTip(QString());
        m_hint->setText(QStringLiteral("引脚布局将在识别到 MCU 型号后生成"));
        return;
    }
    bool verified = true;
    QString note;
    for (const auto &description : m_view->descriptions()) {
        if (description.packageName == packageName) {
            verified = description.numberVerified;
            note = description.note;
            break;
        }
    }

    if (verified) {
        m_hint->setStyleSheet(QString());
        m_hint->setToolTip(QString());
        m_hint->setText(QStringLiteral("点击 GPIO 引脚可跳转到对应端口寄存器"));
    } else {
        m_hint->setStyleSheet(QStringLiteral("color: #B45309;"));
        m_hint->setText(note.isEmpty() ? QStringLiteral("占位引脚表：引脚号待数据手册核对")
                                       : QStringLiteral("占位引脚表：%1").arg(note));
        m_hint->setToolTip(note);
    }
}

} // namespace mrd
