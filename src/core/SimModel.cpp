#include "core/SimModel.h"

#include "core/FmuModel.h"
#include "core/PythonModel.h"

#include <QFileInfo>

namespace mrd {

QStringList SimModel::fileFilters()
{
    return {
        QStringLiteral("全部支持的模型 (*.fmu *.py)"),
        QStringLiteral("FMU 模型 (*.fmu)"),
        QStringLiteral("Python 模型 (*.py)"),
        QStringLiteral("所有文件 (*)"),
    };
}

bool SimModel::isSupported(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("fmu") || suffix == QLatin1String("py");
}

Result<std::unique_ptr<SimModel>> SimModel::open(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return Result<std::unique_ptr<SimModel>>::failure(
            QStringLiteral("模型文件不存在：%1").arg(path));
    }

    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("fmu")) {
        auto model = std::make_unique<FmuModel>(path);
        // Refuse at open time rather than at "start", so a bad package is reported while
        // the user is still looking at the file selector.
        const FmuModel::Description description = FmuModel::readDescription(path);
        if (!description.ok()) {
            return Result<std::unique_ptr<SimModel>>::failure(description.error);
        }
        return Result<std::unique_ptr<SimModel>>::success(std::move(model));
    }
    if (suffix == QLatin1String("py")) {
        auto model = std::make_unique<PythonModel>(path);
        if (PythonModel::findInterpreter().isEmpty()) {
            return Result<std::unique_ptr<SimModel>>::failure(
                QStringLiteral("未找到 python3 解释器：PATH 里没有可用的 python/python3/py"));
        }
        return Result<std::unique_ptr<SimModel>>::success(std::move(model));
    }

    return Result<std::unique_ptr<SimModel>>::failure(
        QStringLiteral("不支持的模型格式“.%1”。支持 .fmu（FMI 2.0）与 .py").arg(suffix));
}

} // namespace mrd
