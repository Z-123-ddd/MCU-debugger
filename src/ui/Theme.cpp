#include "ui/Theme.h"

#include <QApplication>
#include <QPalette>

namespace mrd {

namespace {
QString g_currentName = QStringLiteral("light");
Palette g_current = Palette{};
bool g_initialised = false;

void ensureInit()
{
    if (!g_initialised) {
        g_current = Theme::light();
        g_initialised = true;
    }
}
} // namespace

Palette Theme::light()
{
    Palette p;
    p.background = QColor(0xF5, 0xF6, 0xF8);
    p.surface = QColor(0xFF, 0xFF, 0xFF);
    p.surfaceAlt = QColor(0xEE, 0xF1, 0xF5);
    p.border = QColor(0xC8, 0xCF, 0xD8);
    p.text = QColor(0x1F, 0x29, 0x37);
    p.textDim = QColor(0x6B, 0x72, 0x80);
    p.accent = QColor(0x1D, 0x6F, 0xE0);

    p.blockNormal = QColor(0xE8, 0xF0, 0xFE);
    p.blockChanged = QColor(0xFD, 0xE7, 0xC8);
    p.blockOffline = QColor(0xE9, 0xEC, 0xF0);
    p.blockSelected = QColor(0x1D, 0x6F, 0xE0);

    p.fieldReadWrite = QColor(0x8A, 0xB8, 0xF5);
    p.fieldReadOnly = QColor(0xC3, 0xC9, 0xD2);
    p.fieldReserved = QColor(0xDF, 0xE3, 0xE9);
    p.fieldSelected = QColor(0x1D, 0x6F, 0xE0);

    p.pinIn = QColor(0x7F, 0xB2, 0x69);
    p.pinOut = QColor(0xE0, 0x8B, 0x3C);
    p.pinAf = QColor(0x8A, 0x6F, 0xD0);
    p.pinAnalog = QColor(0x4E, 0xA8, 0xA8);
    p.pinPower = QColor(0xC0, 0x3A, 0x3A);
    p.pinGround = QColor(0x33, 0x33, 0x33);
    p.pinClock = QColor(0x2C, 0x7B, 0xD1);
    p.pinReset = QColor(0xB0, 0x54, 0xC8);
    return p;
}

Palette Theme::dark()
{
    Palette p;
    p.background = QColor(0x1B, 0x1F, 0x26);
    p.surface = QColor(0x24, 0x2A, 0x33);
    p.surfaceAlt = QColor(0x2E, 0x35, 0x40);
    p.border = QColor(0x3C, 0x45, 0x52);
    p.text = QColor(0xE6, 0xEA, 0xF1);
    p.textDim = QColor(0x9A, 0xA4, 0xB2);
    p.accent = QColor(0x5A, 0xA9, 0xFF);

    p.blockNormal = QColor(0x2B, 0x3B, 0x55);
    p.blockChanged = QColor(0x4A, 0x3D, 0x22);
    p.blockOffline = QColor(0x2A, 0x2F, 0x38);
    p.blockSelected = QColor(0x5A, 0xA9, 0xFF);

    p.fieldReadWrite = QColor(0x3F, 0x6F, 0xB5);
    p.fieldReadOnly = QColor(0x47, 0x4F, 0x5C);
    p.fieldReserved = QColor(0x30, 0x36, 0x3F);
    p.fieldSelected = QColor(0x5A, 0xA9, 0xFF);

    p.pinIn = QColor(0x5F, 0x8F, 0x4F);
    p.pinOut = QColor(0xB8, 0x71, 0x2E);
    p.pinAf = QColor(0x6F, 0x58, 0xA8);
    p.pinAnalog = QColor(0x3E, 0x88, 0x88);
    p.pinPower = QColor(0x9A, 0x2E, 0x2E);
    p.pinGround = QColor(0x60, 0x60, 0x60);
    p.pinClock = QColor(0x2C, 0x6B, 0xB1);
    p.pinReset = QColor(0x8E, 0x44, 0xA0);
    return p;
}

Palette Theme::current()
{
    ensureInit();
    return g_current;
}

QString Theme::currentName()
{
    return g_currentName;
}

void Theme::apply(const QString &name)
{
    ensureInit();
    g_currentName = (name.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0)
                        ? QStringLiteral("dark")
                        : QStringLiteral("light");
    g_current = (g_currentName == QLatin1String("dark")) ? dark() : light();
    apply(qobject_cast<QApplication *>(QApplication::instance()), g_currentName);
}

void Theme::apply(QApplication *app, const QString &name)
{
    if (!app)
        return;
    const Palette p = (name == QLatin1String("dark")) ? dark() : light();
    app->setStyle(QStringLiteral("Fusion"));

    QPalette pal = app->palette();
    pal.setColor(QPalette::Window, p.background);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.surface);
    pal.setColor(QPalette::AlternateBase, p.surfaceAlt);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.surfaceAlt);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, QColor(Qt::white));
    pal.setColor(QPalette::ToolTipBase, p.surface);
    pal.setColor(QPalette::ToolTipText, p.text);
    app->setPalette(pal);

    const QString qss = QStringLiteral(
                            "QMainWindow, QDockWidget, QWidget { background: %1; color: %2; }"
                            "QLineEdit, QPlainTextEdit, QTreeWidget, QTableWidget, QComboBox, QSpinBox {"
                            "  background: %3; border: 1px solid %4; border-radius: 4px; padding: 2px 4px; }"
                            "QHeaderView::section { background: %5; color: %6; border: none; padding: 4px; }"
                            "QTabBar::tab { background: %5; color: %6; padding: 6px 14px; border-top-left-radius: 4px;"
                            "  border-top-right-radius: 4px; margin-right: 2px; }"
                            "QTabBar::tab:selected { background: %3; color: %2; font-weight: 600; }"
                            "QToolBar { spacing: 4px; border-bottom: 1px solid %4; }"
                            "QPushButton { background: %5; border: 1px solid %4; border-radius: 4px; padding: 4px 10px; }"
                            "QPushButton:hover { background: %3; }"
                            "QPushButton:disabled { color: %7; }"
                            "QStatusBar { border-top: 1px solid %4; }"
                            "QGroupBox { border: 1px solid %4; border-radius: 4px; margin-top: 8px; padding-top: 6px; }"
                            "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; color: %7; }")
                            .arg(p.background.name(), p.text.name(), p.surface.name(), p.border.name(),
                                 p.surfaceAlt.name(), p.text.name(), p.textDim.name());
    app->setStyleSheet(qss);
}

} // namespace mrd
