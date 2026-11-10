#pragma once

#include "core/Result.h"
#include "core/SvdTypes.h"

#include <QByteArray>
#include <QString>

namespace mrd {

/// Parse a CMSIS-SVD file (schema 1.0/1.1/1.3).
///
/// Handles the parts that matter for a register viewer:
///   * default-value inheritance (device -> peripheral -> register -> field)
///   * `derivedFrom` on peripherals, registers and fields (with loop detection)
///   * `dim`/`dimIncrement`/`dimIndex` array expansion
///   * `<cluster>` nesting
///   * `bitOffset`+`bitWidth`, `lsb`+`msb` and `bitRange="[msb:lsb]"` field syntax
///   * `enumeratedValues` (incl. `isDefault` and `#1x` don't-care patterns)
class SvdParser
{
public:
    static Result<SvdDevice> parseFile(const QString &path);
    static Result<SvdDevice> parseData(const QByteArray &data, const QString &label);
};

} // namespace mrd
