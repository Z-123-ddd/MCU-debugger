#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

class QFile;
class QTextStream;

namespace mrd {

/// Central log sink: forwards to the UI (signal) and optionally to a file.
class Logger : public QObject
{
    Q_OBJECT

public:
    enum Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

    static Logger *instance();

    /// Redirect log output to \a path (append). Empty path disables file logging.
    void setLogFile(const QString &path);
    QString logFile() const { return m_logFile; }

    /// Everything at or above \a level is forwarded; default Info.
    void setMinimumLevel(Level level) { m_minLevel = level; }
    Level minimumLevel() const { return m_minLevel; }

    void log(Level level, const QString &message);
    void debug(const QString &message) { log(Debug, message); }
    void info(const QString &message) { log(Info, message); }
    void warn(const QString &message) { log(Warn, message); }
    void error(const QString &message) { log(Error, message); }

    static QString levelName(Level level);

signals:
    void messageLogged(const QString &text, int level);

private:
    explicit Logger(QObject *parent = nullptr);

    mutable QMutex m_mutex;
    QString m_logFile;
    QFile *m_file = nullptr;
    QTextStream *m_stream = nullptr;
    Level m_minLevel = Info;
};

} // namespace mrd
