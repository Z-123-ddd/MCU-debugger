#include "ui/PortGridView.h"

#include "ui/Theme.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QVBoxLayout>

namespace mrd {

namespace {
constexpr int kCellWidth = 92;
constexpr int kCellHeight = 64;
constexpr int kSpacing = 8;
constexpr int kColumns = 4;
constexpr int kRows = 4;
} // namespace

QColor colorForPin(const GpioPin &pin)
{
    const Palette p = Theme::current();
    switch (pin.kind) {
    case GpioPin::Uninitialized:
        // Reset value: draw it as "not configured" rather than pretending the
        // firmware selected analog mode.
        return p.blockOffline;
    case GpioPin::Input:
        return p.pinIn;
    case GpioPin::Output:
        return p.pinOut;
    case GpioPin::Alternate:
        return p.pinAf;
    case GpioPin::Analog:
        return p.pinAnalog;
    default:
        return p.border;
    }
}

PortGridWidget::PortGridWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(minimumSizeHint());
}

void PortGridWidget::setPins(const QVector<GpioPin> &pins, bool valuesValid)
{
    m_pins = pins;
    m_valid = valuesValid;
    if (m_selected >= m_pins.size())
        m_selected = -1;
    update();
}

void PortGridWidget::setSelectedPin(int index)
{
    m_selected = index;
    update();
}

QSize PortGridWidget::sizeHint() const
{
    const int count = visibleBits().size();
    const int rows = qMax(1, (count + kColumns - 1) / kColumns);
    return QSize(kColumns * (kCellWidth + kSpacing) + kSpacing,
                 rows * (kCellHeight + kSpacing) + kSpacing + 24);
}

QVector<int> PortGridWidget::visibleBits() const
{
    QVector<int> bits;
    for (const auto &pin : m_pins) {
        if (pin.bonded)
            bits << pin.index;
    }
    return bits;
}

QRect PortGridWidget::pinRect(int index) const
{
    // Lay out only the bonded pins: a 48-pin part brings out 3 of GPIOH's 16 bits,
    // so reserving 16 cells would draw 13 phantom pins.
    const QVector<int> bits = visibleBits();
    const int slot = bits.indexOf(index);
    if (slot < 0)
        return QRect();
    const int row = slot / kColumns;
    const int column = slot % kColumns;
    return QRect(kSpacing + column * (kCellWidth + kSpacing), kSpacing + row * (kCellHeight + kSpacing), kCellWidth,
                 kCellHeight);
}

int PortGridWidget::pinAt(const QPoint &pos) const
{
    for (int i = 0; i < m_pins.size(); ++i) {
        const QRect rect = pinRect(i);
        if (rect.isValid() && rect.contains(pos))
            return i;
    }
    return -1;
}

void PortGridWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const Palette p = Theme::current();
    painter.fillRect(rect(), p.background);

    if (m_pins.isEmpty()) {
        painter.setPen(p.textDim);
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("未连接或该端口没有 GPIO 寄存器数据"));
        return;
    }

    QFont small = font();
    small.setPointSizeF(font().pointSizeF() - 1.5);
    QFont bold = small;
    bold.setBold(true);

    for (int i = 0; i < m_pins.size(); ++i) {
        const GpioPin &pin = m_pins.at(i);
        if (!pin.bonded)
            continue;
        const QRect r = pinRect(i);
        if (!r.isValid())
            continue;
        const QColor base = colorForPin(pin);
        QColor fill = base;
        fill.setAlpha(m_valid ? 235 : 140);

        painter.setBrush(fill);
        QPen border(pin.index == m_selected ? p.accent : p.border);
        border.setWidthF(pin.index == m_selected ? 2.0 : 1.0);
        painter.setPen(border);
        painter.drawRoundedRect(r, 6, 6);

        // Light fills (e.g. the grey "uninitialised" cell) need dark text.
        const int luma = (fill.red() * 299 + fill.green() * 587 + fill.blue() * 114) / 1000;
        const QColor onFill = luma > 150 ? Theme::current().text : QColor(Qt::white);

        painter.setFont(bold);
        painter.setPen(onFill);
        painter.drawText(r.adjusted(6, 4, -6, -6), Qt::AlignLeft | Qt::AlignTop, pin.name());

        painter.setFont(small);
        painter.setPen(onFill);
        const QString level = pin.kind == GpioPin::Output
                                  ? (pin.outputHigh ? QStringLiteral("H") : QStringLiteral("L"))
                                  : (pin.inputHigh ? QStringLiteral("H") : QStringLiteral("L"));
        painter.drawText(r.adjusted(6, 4, -6, -6), Qt::AlignRight | Qt::AlignTop, level);
        painter.setPen(onFill);
        painter.drawText(r.adjusted(6, 18, -6, -4), Qt::AlignLeft | Qt::AlignTop, pin.mode);
        const QString sub = QStringLiteral("%1 %2").arg(pin.subMode, pin.speed).trimmed();
        painter.drawText(r.adjusted(6, 34, -6, -4), Qt::AlignLeft | Qt::AlignTop,
                         painter.fontMetrics().elidedText(sub, Qt::ElideRight, r.width() - 12));
    }

    const int legendTop = kRows * (kCellHeight + kSpacing) + 6;
    const QVector<QPair<QString, QColor>> legend = {
        {QStringLiteral("输入"), p.pinIn},
        {QStringLiteral("输出"), p.pinOut},
        {QStringLiteral("复用"), p.pinAf},
        {QStringLiteral("模拟"), p.pinAnalog},
    };
    int x = kSpacing;
    painter.setFont(small);
    for (const auto &entry : legend) {
        painter.setBrush(entry.second);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRect(x, legendTop, 12, 12), 3, 3);
        painter.setPen(p.text);
        painter.drawText(QRect(x + 16, legendTop - 2, 44, 16), Qt::AlignLeft | Qt::AlignVCenter, entry.first);
        x += 62;
    }
}

void PortGridWidget::mousePressEvent(QMouseEvent *event)
{
    const int index = pinAt(event->pos());
    if (index >= 0) {
        m_selected = index;
        update();
        emit pinClicked(index);
    }
    QWidget::mousePressEvent(event);
}

void PortGridWidget::contextMenuEvent(QContextMenuEvent *event)
{
    const int index = pinAt(event->pos());
    if (index < 0 || index >= m_pins.size()) {
        QWidget::contextMenuEvent(event);
        return;
    }
    m_selected = index;
    update();
    showPinMenu(index, event->globalPos());
}

void PortGridWidget::showPinMenu(int index, const QPoint &globalPos)
{
    const GpioPin &pin = m_pins.at(index);

    QMenu menu(this);
    QAction *header = menu.addAction(QStringLiteral("%1  ·  %2").arg(pin.name(), pin.mode));
    header->setEnabled(false);
    if (pin.kind == GpioPin::Uninitialized) {
        QAction *note = menu.addAction(QStringLiteral("配置寄存器均为复位值，固件未初始化"));
        note->setEnabled(false);
    }
    menu.addSeparator();

    // Emit the chosen configuration through the page-level signal.
    const auto addItem = [this](QMenu *target, int bit, const QString &text, GpioPinConfig config) {
        QAction *action = target->addAction(text);
        connect(action, &QAction::triggered, this, [this, bit, config] {
            emit pinConfigureRequested(bit, config);
        });
    };

    GpioPinConfig config;

    config = {};
    config.mode = GpioPinConfig::AnalogInput;
    addItem(&menu, index, QStringLiteral("模拟量输入"), config);

    QMenu *inputMenu = menu.addMenu(QStringLiteral("数字输入"));
    config = {};
    config.mode = GpioPinConfig::DigitalInput;
    config.pull = GpioPinConfig::NoPull;
    addItem(inputMenu, index, QStringLiteral("浮空"), config);
    config.pull = GpioPinConfig::PullUp;
    addItem(inputMenu, index, QStringLiteral("上拉"), config);
    config.pull = GpioPinConfig::PullDown;
    addItem(inputMenu, index, QStringLiteral("下拉"), config);

    QMenu *outputMenu = menu.addMenu(QStringLiteral("数字输出"));
    config = {};
    config.mode = GpioPinConfig::DigitalOutput;
    config.pull = GpioPinConfig::NoPull;
    config.openDrain = false;
    addItem(outputMenu, index, QStringLiteral("推挽"), config);
    config.openDrain = true;
    addItem(outputMenu, index, QStringLiteral("开漏"), config);
    config.openDrain = false;
    config.pull = GpioPinConfig::PullUp;
    addItem(outputMenu, index, QStringLiteral("推挽 + 上拉"), config);
    config.pull = GpioPinConfig::PullDown;
    addItem(outputMenu, index, QStringLiteral("推挽 + 下拉"), config);

    // --- output level (only for pins driven as outputs) --------------------
    QMenu *levelMenu = menu.addMenu(QStringLiteral("输出电平"));
    const bool isOutput = (pin.kind == GpioPin::Output);
    levelMenu->setEnabled(isOutput);
    if (!isOutput) {
        QAction *note = levelMenu->addAction(QStringLiteral("（仅输出模式下可用）"));
        note->setEnabled(false);
    } else {
        QAction *high = levelMenu->addAction(QStringLiteral("置高 (PBS1=1)"));
        connect(high, &QAction::triggered, this, [this, index] {
            emit pinLevelRequested(index, true);
        });
        QAction *low = levelMenu->addAction(QStringLiteral("置低 (PBC1=1)"));
        connect(low, &QAction::triggered, this, [this, index] {
            emit pinLevelRequested(index, false);
        });
        QAction *toggle = levelMenu->addAction(pin.outputHigh ? QStringLiteral("翻转 -> 低")
                                                              : QStringLiteral("翻转 -> 高"));
        connect(toggle, &QAction::triggered, this, [this, index, pin] {
            emit pinLevelRequested(index, !pin.outputHigh);
        });
    }

    menu.addSeparator();
    QAction *details = menu.addAction(QStringLiteral("当前: %1 / %2 / %3")
                                          .arg(pin.mode, pin.subMode.isEmpty() ? QStringLiteral("-") : pin.subMode,
                                               pin.speed.isEmpty() ? QStringLiteral("-") : pin.speed));
    details->setEnabled(false);

    menu.exec(globalPos);
}

void PortGridWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int index = pinAt(event->pos());
    if (index >= 0)
        emit pinActivated(index);
    QWidget::mouseDoubleClickEvent(event);
}

void PortGridWidget::mouseMoveEvent(QMouseEvent *event)
{
    const int index = pinAt(event->pos());
    if (index != m_hovered) {
        m_hovered = index;
        if (index >= 0 && index < m_pins.size()) {
            const GpioPin &pin = m_pins.at(index);
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("<b>%1</b><br/>模式: %2<br/>%3<br/>速度: %4<br/>"
                                              "ODR: %5  IDR: %6")
                                   .arg(pin.name(), pin.mode, pin.subMode, pin.speed,
                                        pin.outputHigh ? QStringLiteral("1") : QStringLiteral("0"),
                                        pin.inputHigh ? QStringLiteral("1") : QStringLiteral("0")),
                               this);
        } else {
            QToolTip::hideText();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void PortGridWidget::leaveEvent(QEvent *event)
{
    m_hovered = -1;
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// PortGridView
// ---------------------------------------------------------------------------

PortGridView::PortGridView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("端口"), this));
    m_portBox = new QComboBox(this);
    m_portBox->setMinimumWidth(120);
    top->addWidget(m_portBox);
    m_summary = new QLabel(this);
    m_summary->setStyleSheet(QStringLiteral("color: palette(mid);"));
    top->addWidget(m_summary, 1);
    layout->addLayout(top);

    m_grid = new PortGridWidget(this);
    layout->addWidget(m_grid, 1);

    connect(m_portBox, &QComboBox::currentTextChanged, this, [this](const QString &name) {
        if (m_updating || name.isEmpty())
            return;
        const auto it = m_cache.constFind(name);
        m_grid->setPins(it == m_cache.constEnd() ? QVector<GpioPin>() : it.value(), it != m_cache.constEnd());
        m_summary->setText(it == m_cache.constEnd() ? QStringLiteral("无数据（需连接目标）")
                                                    : QStringLiteral("%1 个引脚").arg(it.value().size()));
        emit portChanged(name);
    });
    connect(m_grid, &PortGridWidget::pinClicked, this, [this](int index) {
        emit pinClicked(m_portBox->currentText(), index);
    });
    connect(m_grid, &PortGridWidget::pinConfigureRequested, this,
            [this](int index, const GpioPinConfig &config) {
                emit pinConfigureRequested(m_portBox->currentText(), index, config);
            });
    connect(m_grid, &PortGridWidget::pinLevelRequested, this, [this](int index, bool high) {
        emit pinLevelRequested(m_portBox->currentText(), index, high);
    });
}

void PortGridView::setPorts(const QStringList &portNames)
{
    m_updating = true;
    const QString current = m_portBox->currentText();
    m_portBox->clear();
    m_portBox->addItems(portNames);
    const int idx = m_portBox->findText(current);
    if (idx >= 0)
        m_portBox->setCurrentIndex(idx);
    m_updating = false;
    if (idx < 0 && !portNames.isEmpty()) {
        m_updating = true;
        m_portBox->setCurrentIndex(0);
        m_updating = false;
    }
}

void PortGridView::setActivePort(const QString &portName)
{
    const int idx = m_portBox->findText(portName);
    if (idx >= 0)
        m_portBox->setCurrentIndex(idx);
}

QString PortGridView::activePort() const
{
    return m_portBox->currentText();
}

void PortGridView::setPins(const QString &portName, const QVector<GpioPin> &pins, bool valuesValid)
{
    m_cache.insert(portName, pins);
    if (m_portBox->currentText().compare(portName, Qt::CaseInsensitive) == 0) {
        m_grid->setPins(pins, valuesValid);
        m_summary->setText(QStringLiteral("%1 个引脚").arg(pins.size()));
    }
}

} // namespace mrd
