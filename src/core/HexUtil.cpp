#include "core/HexUtil.h"

#include <QChar>

namespace mrd {

namespace {
QString padded(quint32 value, int digits)
{
    return QStringLiteral("0x") + QString::number(value, 16).toUpper().rightJustified(digits, QLatin1Char('0'));
}
} // namespace

QString hex8(quint32 value) { return padded(value, 2); }
QString hex16(quint32 value) { return padded(value, 4); }
QString hex32(quint32 value) { return padded(value, 8); }
QString hexAddr(quint32 value) { return padded(value, 8); }

QString hexValue(quint32 value, int digits)
{
    if (digits < 1)
        digits = 1;
    return padded(value, digits);
}

QString binBits(quint32 value, int width)
{
    if (width < 1)
        return QString();
    if (width > 32)
        width = 32;
    QString out(width, QLatin1Char('0'));
    for (int i = 0; i < width; ++i) {
        const bool bit = (value >> (width - 1 - i)) & 0x1u;
        out[i] = bit ? QLatin1Char('1') : QLatin1Char('0');
    }
    return out;
}

bool parseHex(const QString &text, quint32 *out)
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return false;

    int base = 16;
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        t = t.mid(2);
        base = 16;
    } else if (t.startsWith(QLatin1String("0b"), Qt::CaseInsensitive)) {
        t = t.mid(2);
        base = 2;
    } else {
        // Bare digits are interpreted as hex (register-centric UI); fall back to
        // decimal only when the text contains no hex-only characters.
        bool isDecimalCandidate = true;
        for (const QChar c : t) {
            if (c.isLetter()) {
                isDecimalCandidate = false;
                break;
            }
        }
        if (isDecimalCandidate && t.size() > 8)
            base = 10;
    }

    t.remove(QLatin1Char('_'));
    t.remove(QLatin1Char(' '));
    if (t.isEmpty())
        return false;

    bool ok = false;
    const qulonglong value = t.toULongLong(&ok, base);
    if (!ok || value > 0xFFFFFFFFull)
        return false;
    if (out)
        *out = static_cast<quint32>(value);
    return true;
}

QString hexDumpLine(quint32 address, const QByteArray &data)
{
    QString bytes;
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            bytes += QLatin1Char(' ');
        bytes += QString::number(static_cast<quint8>(data.at(i)), 16).toUpper().rightJustified(2, QLatin1Char('0'));
    }
    return hexAddr(address) + QStringLiteral(": ") + bytes;
}

int bytesForBits(int bits)
{
    if (bits <= 0)
        return 1;
    return (bits + 7) / 8;
}

} // namespace mrd
