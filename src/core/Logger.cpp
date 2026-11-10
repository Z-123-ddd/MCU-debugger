#include "core/Logger.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QTextStream>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mrd {

namespace {
/// True when the process was started from a console (so stderr goes somewhere).
bool haveConsole()
{
#ifdef Q_OS_WIN
    return GetConsoleWindow() != nullptr && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE;
#else
    return true;
#endif
}
} // namespace

Logger::Logger(QObject *parent)
    : QObject(parent)
{
}

Logger *Logger::instance()
{
    static Logger *s_instance = new Logger();
    return s_instance;
}

QString Logger::levelName(Level level)
{
    switch (level) {
    case Debug:
        return QStringLiteral("DEBUG");
    case Info:
        return QStringLiteral("INFO");
    case Warn:
        return QStringLiteral("WARN");
    case Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

void Logger::setLogFile(const QString &path)
{
    QMutexLocker locker(&m_mutex);
    if (m_stream) {
        m_stream->flush();
        delete m_stream;
        m_stream = nullptr;
    }
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    m_logFile.clear();
    if (path.isEmpty())
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());
    auto *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "[WARN] cannot open log file '%s'\n", qPrintable(path));
        delete file;
        return;
    }
    m_file = file;
    m_stream = new QTextStream(file);
    m_stream->setEncoding(QStringConverter::Utf8);
    m_logFile = path;
}

void Logger::log(Level level, const QString &message)
{
    if (level < m_minLevel)
        return;

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString line = QStringLiteral("[%1] %2  %3").arg(stamp, levelName(level), message);

    QMutexLocker locker(&m_mutex);
    if (m_stream) {
        *m_stream << line << '\n';
        m_stream->flush();
    }
    if (haveConsole() && level >= Info)
        std::fprintf(level >= Warn ? stderr : stdout, "%s\n", qPrintable(line));

    emit messageLogged(line, static_cast<int>(level));
}

} // namespace mrd
