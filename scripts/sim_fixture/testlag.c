/*
 * A minimal FMI 2.0 Co-Simulation slave, used as a test fixture.
 *
 * Why this exists: the FMU backend has to load a real DLL and call the real FMI entry
 * points in the right ABI order. A mock in C++ would test our code against itself, so
 * this is a genuine FMI 2.0 shared library (built by scripts/build-sim-fixture.py) packed
 * into a genuine .fmu. If our loader can step this, it can step any conformant FMU.
 *
 * Model: a first-order lag,  y' = (u - y) / tau  with tau = 0.1 s.
 * That is enough to verify a real signal path: hold u = 1 and y must rise monotonically
 * towards 1, which no constant or mis-wired implementation would do.
 */

#include <stdlib.h>
#include <string.h>

typedef void *fmi2Component;
typedef unsigned int fmi2ValueReference;
typedef double fmi2Real;
typedef int fmi2Status;
typedef int fmi2Boolean;
typedef const char *fmi2String;
typedef int fmi2Type;

typedef void (*fmi2CallbackLogger)(void *, fmi2String, fmi2Status, fmi2String, fmi2String,
                                   ...);
typedef void *(*fmi2CallbackAllocateMemory)(size_t, size_t);
typedef void (*fmi2CallbackFreeMemory)(void *);
typedef void (*fmi2StepFinished)(void *, fmi2Status);

typedef struct
{
    fmi2CallbackLogger logger;
    fmi2CallbackAllocateMemory allocateMemory;
    fmi2CallbackFreeMemory freeMemory;
    fmi2StepFinished stepFinished;
    void *componentEnvironment;
} fmi2CallbackFunctions;

#define FMI2_EXPORT __declspec(dllexport)

typedef struct
{
    double u;
    double y;
    double stepCount;
} LagModel;

/* valueReferences, matching modelDescription.xml */
#define VR_U 1u
#define VR_Y 2u
#define VR_STEPS 3u

FMI2_EXPORT const char *fmi2GetTypesPlatform(void)
{
    return "default";
}

FMI2_EXPORT const char *fmi2GetVersion(void)
{
    return "2.0";
}

FMI2_EXPORT fmi2Status fmi2SetDebugLogging(fmi2Component c, fmi2Boolean loggingOn,
                                           size_t n, const fmi2String categories[])
{
    (void)c;
    (void)loggingOn;
    (void)n;
    (void)categories;
    return 0;
}

FMI2_EXPORT fmi2Component fmi2Instantiate(fmi2String instanceName, fmi2Type fmuType,
                                          fmi2String guid, fmi2String resourceLocation,
                                          const fmi2CallbackFunctions *functions,
                                          fmi2Boolean visible, fmi2Boolean loggingOn)
{
    (void)instanceName;
    (void)fmuType;
    (void)resourceLocation;
    (void)functions;
    (void)visible;
    (void)loggingOn;

    /* The guid in the XML and in the caller must match; getting this wrong is a classic
     * integration mistake, so the fixture refuses instead of silently starting. */
    if (guid == NULL || strcmp(guid, "6b1f2f7a1c3d4e5f8a9b0c1d2e3f4a5b") != 0)
        return NULL;

    LagModel *model = (LagModel *)calloc(1, sizeof(LagModel));
    return model;
}

FMI2_EXPORT void fmi2FreeInstance(fmi2Component component)
{
    free(component);
}

FMI2_EXPORT fmi2Status fmi2SetupExperiment(fmi2Component c, fmi2Boolean toleranceDefined,
                                           fmi2Real tolerance, fmi2Real startTime,
                                           fmi2Boolean stopTimeDefined, fmi2Real stopTime)
{
    (void)c;
    (void)toleranceDefined;
    (void)tolerance;
    (void)startTime;
    (void)stopTimeDefined;
    (void)stopTime;
    return 0;
}

FMI2_EXPORT fmi2Status fmi2EnterInitializationMode(fmi2Component c)
{
    (void)c;
    return 0;
}

FMI2_EXPORT fmi2Status fmi2ExitInitializationMode(fmi2Component c)
{
    (void)c;
    return 0;
}

FMI2_EXPORT fmi2Status fmi2Terminate(fmi2Component c)
{
    (void)c;
    return 0;
}

FMI2_EXPORT fmi2Status fmi2Reset(fmi2Component c)
{
    LagModel *model = (LagModel *)c;
    if (model)
    {
        model->u = 0.0;
        model->y = 0.0;
        model->stepCount = 0.0;
    }
    return 0;
}

FMI2_EXPORT fmi2Status fmi2SetReal(fmi2Component c, const fmi2ValueReference vr[], size_t nvr,
                                   const fmi2Real value[])
{
    LagModel *model = (LagModel *)c;
    if (!model)
        return 3; /* fmi2Error */
    for (size_t i = 0; i < nvr; ++i)
    {
        if (vr[i] == VR_U)
            model->u = value[i];
    }
    return 0;
}

FMI2_EXPORT fmi2Status fmi2GetReal(fmi2Component c, const fmi2ValueReference vr[], size_t nvr,
                                   fmi2Real value[])
{
    LagModel *model = (LagModel *)c;
    if (!model)
        return 3;
    for (size_t i = 0; i < nvr; ++i)
    {
        switch (vr[i])
        {
        case VR_U:
            value[i] = model->u;
            break;
        case VR_Y:
            value[i] = model->y;
            break;
        case VR_STEPS:
            value[i] = model->stepCount;
            break;
        default:
            value[i] = 0.0;
            break;
        }
    }
    return 0;
}

FMI2_EXPORT fmi2Status fmi2DoStep(fmi2Component c, fmi2Real currentCommunicationPoint,
                                  fmi2Real communicationStepSize,
                                  fmi2Boolean noSetFMUStatePriorToCurrentPoint)
{
    (void)currentCommunicationPoint;
    (void)noSetFMUStatePriorToCurrentPoint;

    LagModel *model = (LagModel *)c;
    if (!model)
        return 3;
    if (communicationStepSize <= 0.0)
        return 1; /* fmi2Warning */

    const double tau = 0.1;
    const double alpha = communicationStepSize / (tau + communicationStepSize);
    model->y += alpha * (model->u - model->y);
    model->stepCount += 1.0;
    return 0;
}
