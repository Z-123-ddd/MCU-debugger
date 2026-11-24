#pragma once

#include "core/SimModel.h"

#include <QByteArray>
#include <QStringList>

class QProcess;

namespace mrd {

/// Runs a Python model in a child interpreter, one `step()` per sample frame.
///
/// Why a subprocess and not an embedded interpreter: the tool must not link CPython (it
/// would pin the build to one interpreter version and drag a licensed runtime into a
/// GPL binary), and a crash in a user's model must not take the debugger down with it.
/// A child process gives both for free.
///
/// Protocol: newline-delimited JSON over stdin/stdout.
///
///   host -> python   {"cmd":"init","sample_time":0.001}
///   python -> host   {"ok":true}
///   host -> python   {"cmd":"step","inputs":{"u":1.5}}
///   python -> host   {"outputs":{"y":0.7}}
///   python -> host   {"error":"Traceback ..."}      (on failure)
///
/// The user's own `print()` output is redirected to stderr inside the harness, because a
/// stray print would otherwise corrupt the stream and look like a protocol error.
class PythonModel : public SimModel
{
    Q_DISABLE_COPY(PythonModel)

public:
    explicit PythonModel(const QString &path);
    ~PythonModel() override;

    Info info() const override;
    Status initialize(double sampleTime) override;
    Status step(double t, double dt, const QHash<QString, double> &inputs,
                QHash<QString, double> *outputs) override;
    void shutdown() override;
    QString lastError() const override { return m_lastError; }

    /// Names the model exposes, discovered by a probe run before initialize().
    /// Filled by inspect(); empty until then.
    const QStringList &probedInputs() const { return m_probedInputs; }
    const QStringList &probedOutputs() const { return m_probedOutputs; }

    /// Locate a Python interpreter ("python", "py -3", ...). Empty when none works.
    static QString findInterpreter();
    /// The harness script this class writes to a temp file. Exposed for the self-test.
    static QString harnessSource();

private:
    bool startProcess();
    /// Send one request line and read one reply line. Blocking, with a timeout: a step
    /// that takes longer than \a timeoutMs is treated as a broken model rather than
    /// being allowed to freeze the UI.
    Status exchange(const QByteArray &request, QByteArray *reply, int timeoutMs);
    void stopProcess();

    QString m_path;
    QString m_lastError;
    QString m_interpreter;
    QString m_harnessPath;
    QProcess *m_process = nullptr;
    double m_sampleTime = 0.001;
    QStringList m_probedInputs;
    QStringList m_probedOutputs;
    bool m_initialized = false;
};

} // namespace mrd
