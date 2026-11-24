#include "core/PythonModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>

namespace mrd {

QString PythonModel::harnessSource()
{
    // Kept as one string so it can be written next to nothing else and inspected by the
    // self-test. Notes on the shape:
    //   * the protocol handle is a dup of fd 1 taken before sys.stdout is replaced, so a
    //     user model that prints cannot corrupt the reply stream;
    //   * Model(sample_time=...) is tried first, then Model(dt), then Model(), because
    //     all three styles exist in the wild;
    //   * every reply is one line of JSON, flushed, so the host can block on a line.
    return QStringLiteral(R"PY(# mrd simulation harness - generated, do not edit
import sys, os, json, importlib.util, traceback

_proto = os.fdopen(os.dup(1), "w", encoding="utf-8", buffering=1)
sys.stdout = sys.stderr


def _reply(obj):
    _proto.write(json.dumps(obj, allow_nan=False) + "\n")
    _proto.flush()


def _load(path):
    spec = importlib.util.spec_from_file_location("mrd_user_model", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["mrd_user_model"] = mod
    spec.loader.exec_module(mod)
    return mod


def _make(mod, dt):
    cls = getattr(mod, "Model", None)
    if cls is None:
        if not hasattr(mod, "step"):
            raise RuntimeError(
                "model file must define a class Model with step(inputs) -> dict, "
                "or a module-level step(inputs) -> dict")
        return mod
    for attempt in (lambda: cls(sample_time=dt), lambda: cls(dt), lambda: cls()):
        try:
            return attempt()
        except TypeError:
            continue
    return cls()


def main():
    path = sys.argv[1]
    model = None
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except Exception as exc:
            _reply({"error": "bad request: %s" % exc})
            continue
        cmd = msg.get("cmd")
        if cmd == "quit":
            break
        try:
            if cmd == "init":
                dt = float(msg.get("sample_time", 0.001))
                mod = _load(path)
                model = _make(mod, dt)

                # Declared names, if the model provides them. Never probed by stepping:
                # that would advance a model whose state matters (an integrator would
                # start from the wrong place).
                def _names(attr):
                    for holder in (model, mod):
                        value = getattr(holder, attr, None)
                        if value:
                            return [str(v) for v in value]
                    return []

                _reply({"ok": True,
                        "inputs": _names("INPUTS"),
                        "outputs": _names("OUTPUTS")})
            elif cmd == "step":
                result = model.step(dict(msg.get("inputs") or {}))
                if result is None:
                    result = {}
                clean = {}
                for key, value in dict(result).items():
                    try:
                        clean[str(key)] = float(value)
                    except (TypeError, ValueError):
                        raise TypeError("output %r is not a number: %r" % (key, value))
                _reply({"outputs": clean})
            else:
                _reply({"error": "unknown cmd %r" % (cmd,)})
        except Exception:
            _reply({"error": traceback.format_exc().replace("\n", " | ")[-1200:]})


main()
)PY");
}

QString PythonModel::findInterpreter()
{
    // A bare "python" on Windows may be the Microsoft Store stub, which prints a notice
    // and exits without running anything -- so the candidate is verified by actually
    // running it, not by finding an executable.
    const QStringList candidates{
        QStringLiteral("python"),
        QStringLiteral("python3"),
        QStringLiteral("py"),
    };
    for (const QString &candidate : candidates) {
        QProcess probe;
        probe.setProgram(candidate);
        probe.setArguments({QStringLiteral("-c"), QStringLiteral("import sys;print(sys.version_info[0])")});
        probe.start();
        if (!probe.waitForFinished(5000))
            continue;
        if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0)
            continue;
        if (probe.readAllStandardOutput().trimmed() != "3")
            continue;
        return candidate;
    }
    return QString();
}

PythonModel::PythonModel(const QString &path)
    : m_path(QFileInfo(path).absoluteFilePath())
    , m_interpreter(findInterpreter())
{
}

PythonModel::~PythonModel()
{
    shutdown();
}

SimModel::Info PythonModel::info() const
{
    Info out;
    out.displayName = QFileInfo(m_path).fileName();
    out.kind = QStringLiteral("Python");
    out.inputs = m_probedInputs;
    out.outputs = m_probedOutputs;
    out.detail = m_interpreter.isEmpty()
        ? QStringLiteral("未找到 python3")
        : QStringLiteral("解释器 %1%2")
              .arg(m_interpreter,
                   m_probedInputs.isEmpty()
                       ? QStringLiteral("；模型未声明 INPUTS，输入名需在模型里补上 "
                                        "INPUTS = [\"u\"] 才能映射")
                       : QString());
    return out;
}

bool PythonModel::startProcess()
{
    if (m_process)
        return true;
    if (m_interpreter.isEmpty()) {
        m_lastError = QStringLiteral("未找到可用的 python3 解释器（PATH 里没有 python/python3/py）");
        return false;
    }
    if (m_harnessPath.isEmpty()) {
        const QString path = QDir(QDir::tempPath())
                                 .filePath(QStringLiteral("mrd_sim_harness_%1.py")
                                               .arg(QCoreApplication::applicationPid()));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_lastError = QStringLiteral("无法写 harness：%1").arg(file.errorString());
            return false;
        }
        file.write(harnessSource().toUtf8());
        file.close();
        m_harnessPath = path;
    }

    m_process = new QProcess();
    // Keep the model's own output separate, so a crash traceback can be shown.
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);
    m_process->setProgram(m_interpreter);
    m_process->setArguments({m_harnessPath, m_path});
    m_process->start();

    if (!m_process->waitForStarted(8000)) {
        m_lastError = QStringLiteral("启动 %1 失败：%2").arg(m_interpreter, m_process->errorString());
        stopProcess();
        return false;
    }
    return true;
}

Status PythonModel::exchange(const QByteArray &request, QByteArray *reply, int timeoutMs)
{
    if (!m_process) {
        return Status::failure(QStringLiteral("Python 进程未启动"));
    }

    m_process->write(request);
    m_process->write("\n");
    if (!m_process->waitForBytesWritten(2000))
        return Status::failure(QStringLiteral("写入模型超时"));

    // Read exactly one line. waitForReadyRead can return with a partial line, so the
    // deadline is tracked manually rather than trusting a single wait.
    QByteArray buffer;
    QElapsedTimer clock;
    clock.start();
    while (!buffer.contains('\n')) {
        const int remaining = timeoutMs - int(clock.elapsed());
        if (remaining <= 0)
            return Status::failure(QStringLiteral("模型响应超时（%1 ms）").arg(timeoutMs));
        if (!m_process->waitForReadyRead(remaining)) {
            const QByteArray err = m_process->readAllStandardError();
            const QString hint = QString::fromUtf8(err).trimmed().right(600);
            return Status::failure(
                hint.isEmpty() ? QStringLiteral("模型无响应") : QStringLiteral("模型无响应：%1").arg(hint));
        }
        buffer += m_process->readAllStandardOutput();
    }

    const int newline = buffer.indexOf('\n');
    if (reply)
        *reply = buffer.left(newline);
    return Status::success();
}

Status PythonModel::initialize(double sampleTime)
{
    shutdown();
    m_sampleTime = sampleTime > 0.0 ? sampleTime : 0.001;
    if (!startProcess())
        return Status::failure(m_lastError);

    QJsonObject request;
    request.insert(QStringLiteral("cmd"), QStringLiteral("init"));
    request.insert(QStringLiteral("sample_time"), m_sampleTime);

    QByteArray reply;
    const Status status =
        exchange(QJsonDocument(request).toJson(QJsonDocument::Compact), &reply, 20000);
    if (!status) {
        m_lastError = status.error;
        stopProcess();
        return status;
    }

    const QJsonObject answer = QJsonDocument::fromJson(reply).object();
    if (answer.contains(QStringLiteral("error"))) {
        m_lastError = answer.value(QStringLiteral("error")).toString();
        stopProcess();
        return Status::failure(m_lastError);
    }

    // Names come from the model's own INPUTS / OUTPUTS declarations. Nothing is stepped
    // here: a probe step would advance a stateful model before the first real sample.
    // When nothing is declared the names are learned from the first real step instead
    // (see step()), and info().detail says so.
    m_probedInputs.clear();
    m_probedOutputs.clear();
    const QJsonArray inputs = answer.value(QStringLiteral("inputs")).toArray();
    for (const QJsonValue &value : inputs)
        m_probedInputs << value.toString();
    const QJsonArray outputs = answer.value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue &value : outputs)
        m_probedOutputs << value.toString();
    if (m_probedInputs.isEmpty()) {
        m_lastError.clear();
        // Not an error: the model may only report names once it has run. The page shows a
        // hint instead of pretending the mapping is complete.
    }

    m_initialized = true;
    return Status::success();
}

Status PythonModel::step(double t, double dt, const QHash<QString, double> &inputs,
                         QHash<QString, double> *outputs)
{
    Q_UNUSED(t);
    Q_UNUSED(dt);
    if (!m_initialized || !m_process)
        return Status::failure(QStringLiteral("模型未初始化"));

    QJsonObject inputObject;
    for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it)
        inputObject.insert(it.key(), it.value());

    QJsonObject request;
    request.insert(QStringLiteral("cmd"), QStringLiteral("step"));
    request.insert(QStringLiteral("inputs"), inputObject);

    QByteArray reply;
    const Status status =
        exchange(QJsonDocument(request).toJson(QJsonDocument::Compact), &reply, 10000);
    if (!status) {
        m_lastError = status.error;
        return status;
    }

    const QJsonObject answer = QJsonDocument::fromJson(reply).object();
    if (answer.contains(QStringLiteral("error"))) {
        m_lastError = answer.value(QStringLiteral("error")).toString();
        return Status::failure(m_lastError);
    }

    if (outputs) {
        outputs->clear();
        const QJsonObject out = answer.value(QStringLiteral("outputs")).toObject();
        for (auto it = out.constBegin(); it != out.constEnd(); ++it)
            outputs->insert(it.key(), it.value().toDouble());
        if (m_probedOutputs.isEmpty())
            m_probedOutputs = out.keys();
    }
    return Status::success();
}

void PythonModel::stopProcess()
{
    if (!m_process)
        return;
    if (m_process->state() == QProcess::Running) {
        m_process->write("\n{\"cmd\":\"quit\"}\n");
        m_process->waitForBytesWritten(500);
        if (!m_process->waitForFinished(1500)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    delete m_process;
    m_process = nullptr;
}

void PythonModel::shutdown()
{
    stopProcess();
    m_initialized = false;
    if (!m_harnessPath.isEmpty()) {
        QFile::remove(m_harnessPath);
        m_harnessPath.clear();
    }
}

} // namespace mrd
