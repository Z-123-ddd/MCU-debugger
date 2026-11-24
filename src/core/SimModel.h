#pragma once

#include "core/Result.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>

namespace mrd {

/// A model that can be stepped with real measurements.
///
/// Deliberately narrow: one `step()` taking a named input map and returning a named output
/// map. That is enough for both supported kinds and keeps the data path honest -- the
/// values going in are the exact numbers the probe read from the addresses in the .axf
/// variable table, not a synthesised waveform.
///
/// Two kinds are supported, both because they are formats a real model can be exported
/// to rather than something we re-interpret:
///
///   * **FMU** (`.fmu`, FMI 2.0 Co-Simulation) -- what Simulink's "Export to FMU" emits.
///     A ZIP holding `modelDescription.xml` plus a platform DLL with the standard
///     `fmi2Instantiate`/`fmi2DoStep` C API, so the model itself runs, unmodified.
///   * **Python** (`.py`) -- a file defining `class Model` with `step(inputs) -> outputs`,
///     run in a child interpreter. Used for hand-written models and for re-implementing a
///     block diagram when exporting an FMU is not an option.
///
/// Deliberately *not* supported: parsing `.slx`/`.mdl` block diagrams. Simulating those
/// correctly means solvers, algebraic loops, Stateflow and library blocks, i.e. Simulink
/// itself; a partial re-implementation would produce confidently wrong numbers.
class SimModel
{
public:
    /// What the model exposes, for the UI's mapping table.
    struct Info
    {
        QString displayName;   ///< "Plant (fmu)" -- shown in the page title
        QString kind;          ///< "FMI 2.0 Co-Simulation" / "Python"
        QStringList inputs;    ///< model input names, in declaration order
        QStringList outputs;   ///< model output names
        QString detail;        ///< version/guid/interpreter, for the log
    };

    virtual ~SimModel() = default;

    virtual Info info() const = 0;

    /// Prepare for stepping. \a sampleTime is the expected communication step in seconds;
    /// both backends use it as their nominal step.
    virtual Status initialize(double sampleTime) = 0;

    /// Advance one step. \a t is the elapsed simulated time, \a dt the step length.
    ///
    /// Inputs are looked up by name; a missing name is passed as 0 (and reported once),
    /// so a mapping mistake shows up as a flat line instead of a hard failure.
    virtual Status step(double t, double dt, const QHash<QString, double> &inputs,
                        QHash<QString, double> *outputs) = 0;

    /// Release the instance. Safe to call more than once.
    virtual void shutdown() = 0;

    virtual QString lastError() const = 0;

    /// Open a model by file extension: `.fmu` or `.py`.
    static Result<std::unique_ptr<SimModel>> open(const QString &path);

    /// Extensions the file dialog offers.
    static QStringList fileFilters();
    /// True when this path looks like something open() understands.
    static bool isSupported(const QString &path);
};

} // namespace mrd
