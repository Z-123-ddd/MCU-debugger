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
    static QString packageDirectory(); ///< data/packages
};

} // namespace mrd
