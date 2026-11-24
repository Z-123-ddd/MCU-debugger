#pragma once

#include "core/Result.h"
#include "core/SimModel.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QLibrary;

namespace mrd {

/// Runs an FMU (FMI 2.0 Co-Simulation) by loading its platform binary.
///
/// Why this and not a `.slx` parser: Simulink's "Export to FMU" produces a ZIP with
/// `modelDescription.xml` and a DLL exporting the standard FMI C API. Calling that API
/// runs the *actual* model -- the same equations the simulation used -- whereas
/// re-interpreting a block diagram would only ever be an approximation of it.
///
/// FMI 2.0 only. FMI 3.0 renamed every entry point (`fmi3*`) and changed the type system;
/// mixing them up silently would produce garbage, so a 3.0 FMU is refused with a clear
/// message instead.
class FmuModel : public SimModel
{
    Q_DISABLE_COPY(FmuModel)

public:
    explicit FmuModel(const QString &path);
    ~FmuModel() override;

    Info info() const override;
    Status initialize(double sampleTime) override;
    Status step(double t, double dt, const QHash<QString, double> &inputs,
                QHash<QString, double> *outputs) override;
    void shutdown() override;
    QString lastError() const override { return m_lastError; }

    /// Parsed `modelDescription.xml`, kept for the UI and the self-test.
    struct Description
    {
        QString fmiVersion;
        QString modelName;
        QString guid;
        QString modelIdentifier;  ///< DLL base name
        bool hasCoSimulation = false;
        bool hasModelExchange = false;
        QStringList inputNames;
        QStringList outputNames;
        QHash<QString, quint32> valueReferences;  ///< name -> vr
        /// Variables that exist but are not Real; reported so the gap is visible.
        QStringList nonRealVariables;
        QString error;
        bool ok() const { return error.isEmpty() && !modelName.isEmpty(); }
    };

    /// Parse an FMU's modelDescription without instantiating anything (no DLL load).
    /// Used by open() and by the self-test, so a bad package is diagnosed before any
    /// native code runs.
    static Description readDescription(const QString &fmuPath);
    /// Default FMI library so a bare FMU reports one clear message.
    static QString libraryNameFor(const QString &modelIdentifier);

private:
    struct Private;
    std::unique_ptr<Private> d;
    QString m_path;
    QString m_lastError;
    Description m_description;
};

} // namespace mrd
