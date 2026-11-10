#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace mrd {

QString hex8(quint32 value);              ///< "0x1F"
QString hex16(quint32 value);             ///< "0x1234"
QString hex32(quint32 value);             ///< "0x12345678"
QString hexAddr(quint32 value);           ///< "0x40021000"
QString hexValue(quint32 value, int digits); ///< "0x" + zero padded digits
QString binBits(quint32 value, int width);   ///< "1011" (MSB first, width bits)

/// Parse "0x1F", "1F", "0b1011", "31" (decimal fallback). Returns false on garbage.
bool parseHex(const QString &text, quint32 *out);

/// Hex dump of a memory blob, e.g. "0x08000000: 00 01 02 03  ..."
QString hexDumpLine(quint32 address, const QByteArray &data);

/// Number of bytes needed to hold \a bits bits.
int bytesForBits(int bits);

} // namespace mrd
