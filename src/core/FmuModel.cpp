#include "core/FmuModel.h"

#include "core/ZipArchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QXmlStreamReader>

#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace mrd {
namespace {

// ---------------------------------------------------------------------------
// FMI 2.0 ABI. Copied from fmi2FunctionTypes.h / fmi2TypesPlatform.h and kept
// local on purpose: the tool must not depend on the FMU author's headers, and
// every one of these layouts is fixed by the standard.
// ---------------------------------------------------------------------------

using fmi2Component = void *;
using fmi2ComponentEnvironment = void *;
using fmi2String = const char *;
using fmi2Real = double;
using fmi2ValueReference = unsigned int;
using fmi2Boolean = int;
using fmi2Status = int;
using fmi2Type = int;

constexpr fmi2Type fmi2CoSimulation = 1;
constexpr fmi2Boolean fmi2True = 1;

/// Status codes: only "ok" and "warning" mean the step produced usable output.
constexpr fmi2Status fmi2OK = 0;
constexpr fmi2Status fmi2Warning = 1;

const char *statusText(fmi2Status status)
{
    switch (status) {
    case 0:
        return "OK";
    case 1:
        return "Warning";
    case 2:
        return "Discard";
    case 3:
        return "Error";
    case 4:
        return "Fatal";
    case 5:
        return "Pending";
    default:
        return "Unknown";
    }
}

using fmi2CallbackLogger = void (*)(fmi2ComponentEnvironment, fmi2String, fmi2Status,
                                    fmi2String, fmi2String, ...);
using fmi2CallbackAllocateMemory = void *(*)(size_t, size_t);
using fmi2CallbackFreeMemory = void (*)(void *);
using fmi2StepFinished = void (*)(fmi2ComponentEnvironment, fmi2Status);

/// Field order is part of the ABI; a swapped field makes the FMU call our free() with a
/// logger pointer (or worse), so it is spelled out here rather than guessed.
struct fmi2CallbackFunctions
{
    fmi2CallbackLogger logger;
    fmi2CallbackAllocateMemory allocateMemory;
    fmi2CallbackFreeMemory freeMemory;
    fmi2StepFinished stepFinished;
    fmi2ComponentEnvironment componentEnvironment;
};

using GetTypesPlatformFn = fmi2String (*)();
using GetVersionFn = fmi2String (*)();
using InstantiateFn = fmi2Component (*)(fmi2String, fmi2Type, fmi2String, fmi2String,
                                        const fmi2CallbackFunctions *, fmi2Boolean, fmi2Boolean);
using FreeInstanceFn = void (*)(fmi2Component);
using SetupExperimentFn = fmi2Status (*)(fmi2Component, fmi2Boolean, fmi2Real, fmi2Real,
                                         fmi2Boolean, fmi2Real);
using EnterExitFn = fmi2Status (*)(fmi2Component);
using SetRealFn = fmi2Status (*)(fmi2Component, const fmi2ValueReference[], size_t,
                                 const fmi2Real[]);
using GetRealFn = fmi2Status (*)(fmi2Component, const fmi2ValueReference[], size_t,
                                 fmi2Real[]);
using DoStepFn = fmi2Status (*)(fmi2Component, fmi2Real, fmi2Real, fmi2Boolean);
using TerminateFn = fmi2Status (*)(fmi2Component);

/// Collects logger output so a failing instantiate can be explained rather than just
/// reported as "returned null".
struct LoggerSink
{
    QStringList messages;
};

void fmi2Logger(fmi2ComponentEnvironment environment, fmi2String instanceName,
                fmi2Status status, fmi2String category, fmi2String message, ...)
{
    auto *sink = static_cast<LoggerSink *>(environment);
    if (!sink)
        return;

    // The message is printf-style; format it before storing.
    char buffer[1024] = {0};
    va_list args;
    va_start(args, message);
    std::vsnprintf(buffer, sizeof(buffer), message ? message : "", args);
    va_end(args);

    const QString line = QStringLiteral("[%1] %2: %3")
                             .arg(statusText(status),
                                  QString::fromUtf8(category ? category : ""),
                                  QString::fromUtf8(buffer).trimmed());
    sink->messages << line;
    if (sink->messages.size() > 40)
        sink->messages.removeFirst();
    Q_UNUSED(instanceName);
}

void *fmi2Allocate(size_t count, size_t size)
{
    return std::calloc(count, size);
}

void fmi2Free(void *pointer)
{
    std::free(pointer);
}

void fmi2StepFinishedCallback(fmi2ComponentEnvironment, fmi2Status)
{
    // Co-simulation only: the FMU reports when an asynchronous step completes, which this
    // synchronous stepper never does.
}

} // namespace

struct FmuModel::Private
{
    ZipArchive archive;
    QLibrary library;
    QString extractDir;
    quint32 component = 0;
    fmi2Component instance = nullptr;
    LoggerSink logger;
    fmi2CallbackFunctions callbacks{};

    InstantiateFn instantiate = nullptr;
    FreeInstanceFn freeInstance = nullptr;
    SetupExperimentFn setupExperiment = nullptr;
    EnterExitFn enterInitialization = nullptr;
    EnterExitFn exitInitialization = nullptr;
    EnterExitFn terminate = nullptr;
    SetRealFn setReal = nullptr;
    GetRealFn getReal = nullptr;
    DoStepFn doStep = nullptr;

    bool initialised = false;
    double currentTime = 0.0;

    /// Release paths the FMU may have opened (DLL file handle blocks deletion on Windows).
    void unload()
    {
        if (instance && freeInstance) {
            if (terminate)
                terminate(instance);
            freeInstance(instance);
        }
        instance = nullptr;
        initialised = false;
        if (library.isLoaded())
            library.unload();
        if (!extractDir.isEmpty()) {
            QDir(extractDir).removeRecursively();
            extractDir.clear();
        }
    }
};

FmuModel::FmuModel(const QString &path)
    : d(new Private)
    , m_path(QFileInfo(path).absoluteFilePath())
{
    m_description = readDescription(m_path);
}

FmuModel::~FmuModel()
{
    shutdown();
}

QString FmuModel::libraryNameFor(const QString &modelIdentifier)
{
    return modelIdentifier.isEmpty() ? QStringLiteral("<modelIdentifier>") : modelIdentifier;
}

FmuModel::Description FmuModel::readDescription(const QString &fmuPath)
{
    Description description;

    const auto archive = ZipArchive::open(fmuPath);
    if (!archive) {
        description.error = archive.error;
        return description;
    }

    // @modelDescription.xml is at the archive root by definition.
    const QByteArray xml = archive.value.read(QStringLiteral("modelDescription.xml"));
    if (xml.isEmpty()) {
        description.error = QStringLiteral("包内没有 modelDescription.xml，不是有效的 FMU");
        return description;
    }

    QXmlStreamReader reader(xml);
    bool inModelVariables = false;
    QString currentType;

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;

        const QStringView name = reader.name();

        if (name == QLatin1String("fmiModelDescription")) {
            const QXmlStreamAttributes attrs = reader.attributes();
            description.fmiVersion = attrs.value(QLatin1String("fmiVersion")).toString();
            description.modelName = attrs.value(QLatin1String("modelName")).toString();
            description.guid = attrs.value(QLatin1String("guid")).toString();
        } else if (name == QLatin1String("CoSimulation")) {
            description.hasCoSimulation = true;
            // Some packages give a different CS identifier than the ME one.
            const QString id =
                reader.attributes().value(QLatin1String("modelIdentifier")).toString();
            if (!id.isEmpty())
                description.modelIdentifier = id;
        } else if (name == QLatin1String("ModelExchange")) {
            description.hasModelExchange = true;
            if (description.modelIdentifier.isEmpty()) {
                description.modelIdentifier =
                    reader.attributes().value(QLatin1String("modelIdentifier")).toString();
            }
        } else if (name == QLatin1String("ModelVariables")) {
            inModelVariables = true;
        } else if (name == QLatin1String("ScalarVariable") && inModelVariables) {
            const QXmlStreamAttributes attrs = reader.attributes();
            const QString varName = attrs.value(QLatin1String("name")).toString();
            const quint32 vr = attrs.value(QLatin1String("valueReference")).toUInt();
            const QString causality = attrs.value(QLatin1String("causality")).toString();

            // The type is the child element; peek so a non-Real variable is reported
            // instead of being silently dropped from the mapping table.
            currentType.clear();
            const auto child = reader.readNext();
            Q_UNUSED(child);
            while (!reader.isEndElement() || reader.name() != QLatin1String("ScalarVariable")) {
                if (reader.isStartElement()) {
                    currentType = reader.name().toString();
                    if (currentType != QLatin1String("Real")) {
                        description.nonRealVariables
                            << QStringLiteral("%1 (%2)").arg(varName, currentType);
                        break;
                    }
                    break;
                }
                if (reader.atEnd())
                    break;
                reader.readNext();
            }

            if (currentType == QLatin1String("Real")) {
                description.valueReferences.insert(varName, vr);
                if (causality == QLatin1String("input"))
                    description.inputNames << varName;
                else if (causality == QLatin1String("output"))
                    description.outputNames << varName;
            }
        } else if (name == QLatin1String("ModelStructure")) {
            inModelVariables = false;
        }
    }

    if (reader.hasError()) {
        description.error = QStringLiteral("modelDescription.xml 解析失败：%1")
                                .arg(reader.errorString());
        return description;
    }
    if (description.modelName.isEmpty()) {
        description.error = QStringLiteral("modelDescription.xml 里没有 modelName");
        return description;
    }
    if (description.fmiVersion != QLatin1String("2.0")) {
        // 3.0 renamed the whole API; pretending it is 2.0 would call the wrong symbols.
        description.error =
            QStringLiteral("该 FMU 是 FMI %1，本工具只支持 FMI 2.0（请用 2.0 重新导出）")
                .arg(description.fmiVersion.isEmpty() ? QStringLiteral("?")
                                                      : description.fmiVersion);
        return description;
    }
    return description;
}

SimModel::Info FmuModel::info() const
{
    Info out;
    out.displayName = m_description.modelName.isEmpty() ? QFileInfo(m_path).fileName()
                                                        : m_description.modelName;
    out.kind = m_description.hasCoSimulation ? QStringLiteral("FMI 2.0 Co-Simulation")
                                             : QStringLiteral("FMI 2.0");
    out.inputs = m_description.inputNames;
    out.outputs = m_description.outputNames;
    out.detail = QStringLiteral("guid %1%2")
                     .arg(m_description.guid.left(8),
                          m_description.hasCoSimulation ? QString()
                                                        : QStringLiteral("（无 CoSimulation）"));
    return out;
}

Status FmuModel::initialize(double sampleTime)
{
    Q_UNUSED(sampleTime);
    shutdown();

    if (!m_description.ok())
        return Status::failure(m_description.error);
    if (!m_description.hasCoSimulation) {
        return Status::failure(
            QStringLiteral("该 FMU 只提供 Model Exchange，没有 Co-Simulation。\n"
                           "Model Exchange 需要本工具自己积分，请改用 Co-Simulation 重新导出。"));
    }

    const auto archive = ZipArchive::open(m_path);
    if (!archive)
        return Status::failure(archive.error);
    d->archive = archive.value;

    // --- extract the platform binary -------------------------------------
    // A DLL cannot be loaded from memory, so it goes to a per-process temp dir. The whole
    // binaries/win64 folder is extracted, because an FMU may ship its own dependencies
    // next to the model DLL.
    const QString suffix = QStringLiteral(".dll");
    QString matchedPath;
    const QByteArray libraryBytes =
        d->archive.readFirstMatching(QStringLiteral("binaries/"), suffix, &matchedPath);
    if (libraryBytes.isEmpty()) {
        return Status::failure(
            QStringLiteral("包里没有 Windows 64 位二进制（binaries/win64/*.dll）。\n"
                           "在 Linux/macOS 上导出的 FMU 不能在这里运行。"));
    }

    d->extractDir = QDir(QDir::tempPath())
                        .filePath(QStringLiteral("mrd_fmu_%1_%2")
                                      .arg(QCoreApplication::applicationPid())
                                      .arg(QFileInfo(m_path).completeBaseName()));
    QDir().mkpath(d->extractDir);

    // Preserve the relative layout so the DLL finds sibling libraries and resources/.
    for (const QString &entryPath : d->archive.entriesUnder(QStringLiteral("binaries/"))) {
        const QByteArray data = d->archive.read(entryPath);
        if (entryPath.endsWith(QLatin1Char('/')))
            continue;
        const QString target = QDir(d->extractDir).filePath(entryPath);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        file.write(data);
        file.close();
    }

    // FMI wants a directory (with trailing separator) for fmuResourceLocation.
    const QString resourcesDir = QDir(d->extractDir).filePath(QStringLiteral("resources"));
    QDir().mkpath(resourcesDir);
    for (const QString &entryPath : d->archive.entriesUnder(QStringLiteral("resources/"))) {
        if (entryPath.endsWith(QLatin1Char('/')))
            continue;
        const QByteArray data = d->archive.read(entryPath);
        const QString target = QDir(d->extractDir).filePath(entryPath);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        file.write(data);
        file.close();
    }

    const QString libraryPath = QDir(d->extractDir).filePath(matchedPath);
    d->library.setFileName(libraryPath);
    if (!d->library.load()) {
        const QString reason = d->library.errorString();
        d->unload();
        return Status::failure(
            QStringLiteral("无法加载 FMU 的 DLL：%1\n%2").arg(libraryPath, reason));
    }

    // --- resolve the FMI 2.0 entry points --------------------------------
    d->instantiate = reinterpret_cast<InstantiateFn>(d->library.resolve("fmi2Instantiate"));
    d->freeInstance = reinterpret_cast<FreeInstanceFn>(d->library.resolve("fmi2FreeInstance"));
    d->setupExperiment =
        reinterpret_cast<SetupExperimentFn>(d->library.resolve("fmi2SetupExperiment"));
    d->enterInitialization =
        reinterpret_cast<EnterExitFn>(d->library.resolve("fmi2EnterInitializationMode"));
    d->exitInitialization =
        reinterpret_cast<EnterExitFn>(d->library.resolve("fmi2ExitInitializationMode"));
    d->terminate = reinterpret_cast<TerminateFn>(d->library.resolve("fmi2Terminate"));
    d->setReal = reinterpret_cast<SetRealFn>(d->library.resolve("fmi2SetReal"));
    d->getReal = reinterpret_cast<GetRealFn>(d->library.resolve("fmi2GetReal"));
    d->doStep = reinterpret_cast<DoStepFn>(d->library.resolve("fmi2DoStep"));

    const QVector<QPair<QString, void *>> required{
        {QStringLiteral("fmi2Instantiate"), reinterpret_cast<void *>(d->instantiate)},
        {QStringLiteral("fmi2FreeInstance"), reinterpret_cast<void *>(d->freeInstance)},
        {QStringLiteral("fmi2SetupExperiment"), reinterpret_cast<void *>(d->setupExperiment)},
        {QStringLiteral("fmi2EnterInitializationMode"),
         reinterpret_cast<void *>(d->enterInitialization)},
        {QStringLiteral("fmi2ExitInitializationMode"),
         reinterpret_cast<void *>(d->exitInitialization)},
        {QStringLiteral("fmi2SetReal"), reinterpret_cast<void *>(d->setReal)},
        {QStringLiteral("fmi2GetReal"), reinterpret_cast<void *>(d->getReal)},
        {QStringLiteral("fmi2DoStep"), reinterpret_cast<void *>(d->doStep)},
    };
    QStringList missing;
    for (const auto &entry : required) {
        if (!entry.second)
            missing << entry.first;
    }
    if (!missing.isEmpty()) {
        d->unload();
        return Status::failure(QStringLiteral("DLL 缺少 FMI 2.0 函数：%1")
                                   .arg(missing.join(QStringLiteral(", "))));
    }

    // Optional sanity check: a 3.0 binary renamed to fmi2* would be caught here.
    if (auto versionFn = reinterpret_cast<GetVersionFn>(d->library.resolve("fmi2GetVersion"))) {
        const QString version = QString::fromLatin1(versionFn());
        if (version != QLatin1String("2.0")) {
            d->unload();
            return Status::failure(
                QStringLiteral("DLL 报告的 FMI 版本是 %1，不是 2.0").arg(version));
        }
    }

    // --- instantiate ------------------------------------------------------
    d->callbacks.logger = &fmi2Logger;
    d->callbacks.allocateMemory = &fmi2Allocate;
    d->callbacks.freeMemory = &fmi2Free;
    d->callbacks.stepFinished = &fmi2StepFinishedCallback;
    d->callbacks.componentEnvironment = &d->logger;

    const QByteArray instanceName = "mcuRegDebugger";

    // fmuResourceLocation must be a URI. Qt's fromPercentEncoding/toString handles the
    // common case (no spaces in %TEMP%, but do not assume).
    QString resourceLocation = QDir::toNativeSeparators(resourcesDir);
    resourceLocation.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!resourceLocation.endsWith(QLatin1Char('/')))
        resourceLocation += QLatin1Char('/');
    const QByteArray resourceBytes = "file:///" + resourceLocation.toUtf8();

    d->instance = d->instantiate(instanceName.constData(), fmi2CoSimulation,
                                m_description.guid.toUtf8().constData(),
                                resourceBytes.constData(), &d->callbacks, fmi2True, fmi2True);
    if (!d->instance) {
        const QString log = d->logger.messages.join(QLatin1Char('\n'));
        d->unload();
        return Status::failure(
            QStringLiteral("fmi2Instantiate 失败。%1")
                .arg(log.isEmpty() ? QStringLiteral("（模型没有给出原因）") : log));
    }

    // --- start the experiment --------------------------------------------
    fmi2Status status = d->setupExperiment(d->instance, 0, 0.0, 0.0, 0, 0.0);
    if (status != fmi2OK && status != fmi2Warning) {
        const QString log = d->logger.messages.join(QLatin1Char('\n'));
        d->unload();
        return Status::failure(QStringLiteral("fmi2SetupExperiment 失败（%1）\n%2")
                                   .arg(QString::fromLatin1(statusText(status)), log));
    }

    status = d->enterInitialization(d->instance);
    if (status == fmi2OK || status == fmi2Warning)
        status = d->exitInitialization(d->instance);
    if (status != fmi2OK && status != fmi2Warning) {
        const QString log = d->logger.messages.join(QLatin1Char('\n'));
        d->unload();
        return Status::failure(QStringLiteral("初始化模式切换失败（%1）\n%2")
                                   .arg(QString::fromLatin1(statusText(status)), log));
    }

    d->initialised = true;
    d->currentTime = 0.0;
    return Status::success();
}

Status FmuModel::step(double t, double dt, const QHash<QString, double> &inputs,
                      QHash<QString, double> *outputs)
{
    if (!d->initialised || !d->instance)
        return Status::failure(QStringLiteral("FMU 未初始化"));

    // --- inputs -----------------------------------------------------------
    // Only names the model actually declares are sent; a mapping entry that points at a
    // variable the FMU does not have is the user's mistake, reported by the page rather
    // than being pushed into an unrelated valueReference.
    QVector<fmi2ValueReference> inputRefs;
    QVector<fmi2Real> inputValues;
    for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it) {
        const auto ref = m_description.valueReferences.constFind(it.key());
        if (ref == m_description.valueReferences.constEnd())
            continue;
        if (!m_description.inputNames.contains(it.key()))
            continue;
        inputRefs.append(ref.value());
        inputValues.append(it.value());
    }
    if (!inputRefs.isEmpty()) {
        const fmi2Status status =
            d->setReal(d->instance, inputRefs.constData(), size_t(inputRefs.size()),
                       inputValues.constData());
        if (status != fmi2OK && status != fmi2Warning) {
            return Status::failure(QStringLiteral("fmi2SetReal 失败（%1）")
                                       .arg(QString::fromLatin1(statusText(status))));
        }
    }

    // --- advance ----------------------------------------------------------
    const fmi2Status stepStatus = d->doStep(d->instance, t, dt, fmi2True);
    if (stepStatus != fmi2OK && stepStatus != fmi2Warning) {
        const QString log = d->logger.messages.join(QLatin1Char('\n'));
        return Status::failure(QStringLiteral("fmi2DoStep 失败（%1）%2")
                                   .arg(QString::fromLatin1(statusText(stepStatus)),
                                        log.isEmpty() ? QString() : QStringLiteral("\n") + log));
    }
    d->currentTime = t + dt;

    // --- outputs ----------------------------------------------------------
    if (outputs) {
        outputs->clear();
        QVector<fmi2ValueReference> refs;
        refs.reserve(m_description.outputNames.size());
        for (const QString &name : m_description.outputNames)
            refs.append(m_description.valueReferences.value(name));

        if (!refs.isEmpty()) {
            QVector<fmi2Real> values(refs.size(), 0.0);
            const fmi2Status status =
                d->getReal(d->instance, refs.constData(), size_t(refs.size()), values.data());
            if (status != fmi2OK && status != fmi2Warning) {
                return Status::failure(QStringLiteral("fmi2GetReal 失败（%1）")
                                           .arg(QString::fromLatin1(statusText(status))));
            }
            for (int i = 0; i < m_description.outputNames.size(); ++i)
                outputs->insert(m_description.outputNames.at(i), values.at(i));
        }
    }
    return Status::success();
}

void FmuModel::shutdown()
{
    if (!d)
        return;
    d->unload();
}

} // namespace mrd
