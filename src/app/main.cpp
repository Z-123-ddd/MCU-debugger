#include "app/Cli.h"
#include "core/Logger.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

/// A GUI (WIN32) process has no console; attach to the parent console when the
/// tool is launched from cmd.exe so that CLI modes can print diagnostics.
void attachParentConsole()
{
#ifdef Q_OS_WIN
    static bool done = false;
    if (done)
        return;
    done = true;

    // When launched from cmd.exe (or with redirection) the standard handles are
    // inherited and already usable - re-opening CONOUT$ would break piping.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        // Make sure the console renders the UTF-8 text we print below.
        SetConsoleOutputCP(CP_UTF8);
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
#endif
}

QString defaultLogPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/logs/mcuRegDebugger-%1.log")
                     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
}

} // namespace

int main(int argc, char *argv[])
{
    const mrd::cli::Options options = mrd::cli::parse(argc, argv);

    if (options.printHelp || options.printVersion || options.selftest || options.probeInfo)
        attachParentConsole();

    if (options.printVersion) {
        std::printf("%s %s\n", APP_NAME, APP_VERSION);
        return 0;
    }
    if (options.printHelp) {
        std::printf("%s", qPrintable(mrd::cli::helpText()));
        return 0;
    }

    // Offscreen rendering keeps --selftest usable on headless sessions.
    if (!options.renderDir.isEmpty())
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MCUTools"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("mcutools.local"));
    QCoreApplication::setApplicationName(QStringLiteral("McuRegDebugger"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

    mrd::Logger::instance()->setLogFile(options.logPath.isEmpty() ? defaultLogPath() : options.logPath);

    if (!options.errors.isEmpty()) {
        for (const QString &e : options.errors)
            std::fprintf(stderr, "[ERROR] %s\n", qPrintable(e));
        std::fprintf(stderr, "\n%s", qPrintable(mrd::cli::helpText()));
        return 1;
    }

    if (options.selftest)
        return mrd::cli::runSelftest(options);
    if (options.probeInfo)
        return mrd::cli::runProbeInfo(options);

    mrd::MainWindow window;
    window.show();
    if (!options.svdPath.isEmpty())
        window.openSvdFile(options.svdPath);
    if (options.autoConnect)
        window.scheduleAutoConnect();

    return app.exec();
}
