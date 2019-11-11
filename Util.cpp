#include "App.h"
#include "Logger.h"
#include "Util.h"

#include <chrono>
#include <iostream>

namespace Util {
    QString basename(const QString &s) {
        QRegExp re("[\\/]");
        auto toks = s.split(re);
        return toks.last();
    }

    static const auto t0 = std::chrono::high_resolution_clock::now();

    qint64 getTime() {
        const auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
    }

    qint64 getTimeNS() {
        const auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count();
    }

    double getTimeSecs() {
        return double(getTime()) / 1000.0;
    }

    bool isClockSteady() {
        return std::chrono::high_resolution_clock::is_steady;
    }

    namespace Json {
        QVariant parseString(const QString &str, bool expectMap) {
            QJsonParseError e;
            QJsonDocument d = QJsonDocument::fromJson(str.toUtf8(), &e);
            if (d.isNull())
                throw ParseError(QString("Error parsing Json from string: %1").arg(e.errorString()));
            auto v = d.toVariant();
            if (expectMap && v.type() != QVariant::Map)
                throw Error("Json Error, expected map, got a list instead");
            if (!expectMap && v.type() != QVariant::List)
                throw Error("Json Error, expected list, got a map instead");
            return v;
        }
        QVariant parseFile(const QString &file, bool expectMap) {
            QFile f(file);
            if (!f.open(QFile::ReadOnly))
                throw Error(QString("Could not open file: %1").arg(file));
            QString s(f.readAll());
            return parseString(s, expectMap);
        }
        QString toString(const QVariant &v, bool compact) {
            if (v.isNull() || !v.isValid()) throw Error("Empty or invalid QVariant passed to Json::toString");
            auto d = QJsonDocument::fromVariant(v);
            if (d.isNull())
                throw Error("Bad QVariant pased to Json::toString");
            return d.toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
        }
    } // end namespace Json

    RunInThread::RunInThread(const VoidFunc & work, QObject *receiver, const VoidFunc & completion, const QString & name)
        : QThread(receiver), work(work)
    {
        auto sigReceiver = receiver ? receiver : this;
        if (!name.isNull()) setObjectName(name);
        connect(this, &RunInThread::onCompletion, sigReceiver, [completion, this]{
            if (completion) completion();
            deleteLater();
        });
        if (!blockNew) {
            {
                QMutexLocker ml(&mut);
                extant.insert(this);
            }
            start();
        } else {
            Warning() << "App shutting down, will not start thread " << (name.isNull() ? "(RunInThread)" : name);
        }
    }

    void RunInThread::run()
    {
        if (work)
            work();
        emit onCompletion(); // runs in receiver's thread or main thread if no receiver.
        done();
    }

    /// app exit cleanup handling
    /*static*/ std::atomic_bool RunInThread::blockNew = false;
    /*static*/ QSet<RunInThread *> RunInThread::extant;
    /*static*/ QMutex RunInThread::mut;
    /*static*/ QWaitCondition RunInThread::cond;

    void RunInThread::done()
    {
        QMutexLocker ml(&mut);
        extant.remove(this);
        if (extant.isEmpty()) {
            cond.wakeAll();
        }
    }
    // static
    bool RunInThread::waitForAll(unsigned long timeout, const QString &msg, int *num)
    {
        QMutexLocker ml(&mut);
        if (!extant.isEmpty()) {
            Log() << msg;
            if (num) *num = extant.count();
            auto notTimedOut = cond.wait(&mut, timeout);
            if (!notTimedOut && num) {
                // if we timed out, tell caller how many were still running
                *num = extant.count();
            }
            return notTimedOut;
        } else if (num) *num = 0;
        return true;
    }
    // static
    void RunInThread::test(QObject *receiver)
    {
        auto rit =
        Util::RunInThread::Do([]{
            for (int i = 0; i < 100; ++i) {
                Debug() << "Worker thread...";
                QThread::msleep(100);
                //if (blockNew)
                //    return;
            }
        }, receiver, []{
           Debug() << "COMPLETION!";
        }, "(RunInThread)");
        connect(rit, &QObject::destroyed, receiver ? receiver : qApp, []{
           Debug() << "DESTROYED!!";
        });
    }


    bool LambdaOnObject(const QObject *obj, const VoidFunc & lambda, quint64 timeout_ms)
    {
        if (!lambda) {
            Debug() << __FUNCTION__ << ": Target object: " << obj->objectName() << " lambda is null. FIXME.";
            return true;
        }
        if (QThread::currentThread() == obj->thread()) {
            lambda();
            return true;
        } else {
            if (!obj->thread()->isRunning()) {
                Debug() << __FUNCTION__ << ": Target object: " << obj->objectName() << " thread not running! Will return without calling lambda... FIXME.";
                return false;
            }
            struct SharedState {
                VoidFunc lambda;
                VariantChannel chan;
            };
            auto shared = std::make_shared<SharedState>();
            shared->lambda = lambda;
            decltype(shared)::weak_type weakShared = shared;
            QTimer::singleShot(0, obj, [weakShared] {
                if (auto shared = weakShared.lock(); shared) {
                    shared->lambda();
                    shared->chan.put(true);
                }
            });
            return shared->chan.get<bool>(timeout_ms);
        }
    }

} // end namespace Util

Log::Log() {}

Log::Log(Color c)
{
    setColor(c);
}

Log::Log(const char *fmt...)
    :  s()
{
    va_list ap;
    va_start(ap,fmt);
    str = QString::vasprintf(fmt,ap);
    va_end(ap);
    s.setString(&str, QIODevice::WriteOnly|QIODevice::Append);
}

Log::~Log()
{
    if (doprt) {
        App *ourApp = app();
        s.flush(); // does nothing probably..
        // note: we always want to log the timestamp, even in syslog mode.
        // this is because if logging from a thread, log lines be out-of-order.
        // The timestamp is the only record of the actual order in which things
        // occurred.
        const auto now = Util::getTime();
        const QString tsStr = QString::asprintf("[%lld.%03d] ", now/1000LL, int(now%1000));
        QString thrdStr = "";

        if (QThread *th = QThread::currentThread(); th && ourApp && th != ourApp->thread()) {
            QString thrdName = th->objectName();
            if (thrdName.trimmed().isEmpty()) thrdName = QString::asprintf("%p", reinterpret_cast<void *>(QThread::currentThreadId()));
            thrdStr = QString("<Thread: %1> ").arg(thrdName);
        }

        Logger *logger = ourApp ? ourApp->logger() : nullptr;

        QString theString = tsStr + thrdStr + (logger && logger->isaTTY() ? colorify(str, color) : str);

        if (logger) {
            emit logger->log(level, theString);
        } else {
            // just print to console for now..
            std::cerr << Q2C(theString) << std::endl << std::flush;
        }
    }
}

/* static */
QString Log::colorString(Color c) {
    const char *suffix = "[0m"; // normal
    switch(c) {
    case Black: suffix = "[30m"; break;
    case Red: suffix = "[31m"; break;
    case Green: suffix = "[32m"; break;
    case Yellow: suffix = "[33m"; break;
    case Blue: suffix = "[34m"; break;
    case Magenta: suffix = "[35m"; break;
    case Cyan: suffix = "[36m"; break;
    case White: suffix = "[37m"; break;
    case BrightBlack: suffix = "[30;1m"; break;
    case BrightRed: suffix = "[31;1m"; break;
    case BrightGreen: suffix = "[1,32m"; break;
    case BrightYellow: suffix = "[33;1m"; break;
    case BrightBlue: suffix = "[34;1m"; break;
    case BrightMagenta: suffix = "[35;1m"; break;
    case BrightCyan: suffix = "[36;1m"; break;
    case BrightWhite: suffix = "[37;1m"; break;

    default:
        // will just use normal
        break;
    }
    static const char prefix[2] = { 033, 0 }; // esc 033 in octal
    return QString::asprintf("%s%s", prefix, suffix);
}

QString Log::colorify(const QString &str, Color c) {
    QString colorStr = useColor && c != Normal ? colorString(c) : "";
    QString normalStr = useColor && c != Normal ? colorString(Normal) : "";
    return colorStr + str + normalStr;
}

template <> Log & Log::operator<<(const Color &c) { setColor(c); return *this; }

Debug::~Debug()
{
    level = Logger::Debug;
    doprt = isEnabled();
    if (!doprt) return;
    if (!colorOverridden) color = Cyan;
    str = QString("(Debug) ") + str;
}

bool Debug::isEnabled() {
    auto ourApp = app();
    return !ourApp || ourApp->options.verboseDebug;
}

Trace::~Trace()
{
    level = Logger::Debug;
    doprt = isEnabled();
    if (!doprt) return;
    if (!colorOverridden) color = Green;
    str = QString("(Trace) ") + str;
}

bool Trace::isEnabled() {
    auto ourApp = app();
    return ourApp && ourApp->options.verboseTrace && Debug::isEnabled();
}

Error::~Error()
{
    level = Logger::Critical;
    if (!colorOverridden) color = BrightRed;
}


Warning::~Warning()
{
    level = Logger::Warning;
    if (!colorOverridden) color = Yellow;
}

