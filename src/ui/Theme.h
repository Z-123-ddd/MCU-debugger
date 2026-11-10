#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace mrd {

/// Colour keys shared by the views (register blocks, bit strips, pin maps).
struct Palette
{
    QColor background;
    QColor surface;
    QColor surfaceAlt;
    QColor border;
    QColor text;
    QColor textDim;
    QColor accent;

    QColor blockNormal;    ///< register block, value equal to reset
    QColor blockChanged;   ///< register block, value differs from reset
    QColor blockOffline;   ///< no value read yet
    QColor blockSelected;

    QColor fieldReadWrite;
    QColor fieldReadOnly;
    QColor fieldReserved;
    QColor fieldSelected;

    QColor pinIn;
    QColor pinOut;
    QColor pinAf;
    QColor pinAnalog;
    QColor pinPower;
    QColor pinGround;
    QColor pinClock;
    QColor pinReset;
};

class Theme
{
public:
    static Palette light();
    static Palette dark();
    static Palette current();
    static QString currentName();

    /// Apply globally: palette + stylesheet. \a name is "light" or "dark".
    static void apply(const QString &name);
    static void apply(QApplication *app, const QString &name);
};

} // namespace mrd
