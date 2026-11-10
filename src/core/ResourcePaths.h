#pragma once

#include <QString>
#include <QStringList>

namespace mrd {

/// Where the app looks for its data files (`data/` in the source tree next to the
/// binary, or `<AppData>/data` once installed).
class ResourcePaths
{
public:
    static QStringList dataRoots();
    static QString find(const QString &relativePath);
    static QString deviceIdDatabase(); ///< data/device_ids.json
    /// Extra identification tables: every data/device_ids.d/*.json.
    ///
    /// Keeps the built-in table small while leaving the door open for other
    /// vendors (an archived STM32 table drops back in here, no code change).
    static QStringList deviceIdSupplements();
    static QString packageDirectory(); ///< data/packages
};

} // namespace mrd
