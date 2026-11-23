#include "app/AppContext.h"

#include "app/models/LogModel.h"
#include "core/BuildInfo.h"
#include "core/Paths.h"
#include "engines/fpkg/BackportUpdate.h"
#include "engines/general/Module.h"
#include "engines/integration/Module.h"
#include "registration/Registration.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QThreadPool>

#include <exception>
#include <QtConcurrent/QtConcurrentRun>

namespace fw {

static AppContext* g_instance = nullptr;

AppContext* AppContext::instance()
{
    return g_instance;
}

bool AppContext::usePortableWindowState()
{
    if (!Paths::isPortable() && qEnvironmentVariable("FW_DATA_DIR").isEmpty())
        return false;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, Paths::dataDir());
    return true;
}

AppContext::AppContext(QObject* parent) : QObject(parent), m_schema(std::make_shared<SettingsSchema>())
{
    g_instance = this;
    qRegisterMetaType<LogRecord>();
    qRegisterMetaType<DependencyInfo>();
    ModuleContext ctx{*m_schema, m_deps, m_factory};
    registerAllModules(ctx);
    m_settings = std::make_unique<SettingsStore>(m_schema, Paths::settingsFile(), this);
    m_history = std::make_unique<JobHistory>(Paths::historyFile());
    m_queue = std::make_unique<JobQueue>(*m_settings, m_factory, this);
    m_postBuild = new integration::PostBuildActions(*m_queue, *m_settings, this);
    m_queue->setHistory(m_history.get());
    m_queue->setPersistencePath(Paths::queueFile());
}

AppContext::~AppContext()
{
    // A dependency check still running uses m_deps: wait for it first. A
    // destructor must not throw: QFuture::waitForFinished() rethrows what a
    // task threw, and that would end the app with std::terminate.
    for (QFuture<void>& f : m_depChecks) {
        try {
            f.waitForFinished();
        } catch (const std::exception& e) {
            Log(QStringLiteral("app")).warn(QStringLiteral("A dependency check ended with an error: %1").arg(QString::fromUtf8(e.what())));
        } catch (...) {
            Log(QStringLiteral("app")).warn(QStringLiteral("A dependency check ended with an error."));
        }
    }
    shutdownQueue();
    flushSettingsSave();
    // The logger is process-wide and outlives this object: none of its sinks
    // stays registered (the file sink would keep the log file open).
    Logger::instance().flush();
    for (const std::shared_ptr<LogSink>& sink : {std::shared_ptr<LogSink>(m_uiSink), std::shared_ptr<LogSink>(m_file), std::shared_ptr<LogSink>(m_ring)})
        if (sink)
            Logger::instance().removeSink(sink);
    if (g_instance == this)
        g_instance = nullptr;
}

void AppContext::shutdownQueue(int waitMs)
{
    if (m_queueShutDown || !m_queue)
        return;
    m_queueShutDown = true;
    if (!m_queue->shutdown(waitMs)) {
        // A worker that does not reach a checkpoint keeps the closed app in
        // memory: the log (its sinks are still here) says which jobs were running.
        Log(QStringLiteral("queue")).warn(QStringLiteral("Still waiting for a job to stop after %1 s. Running when the app closed: %2")
                                              .arg(waitMs / 1000)
                                              .arg(m_queue->activeTitles().join(QStringLiteral("; "))));
    }
    flushSettingsSave();
    Logger::instance().flush();
}

void AppContext::flushSettingsSave()
{
    if (!m_savePending)
        return;
    m_saveTimer.stop();
    saveSettingsNow();
}

void AppContext::saveSettingsNow()
{
    m_savePending = false;
    ++m_settingsSaves;
    const Status saved = m_settings->save();
    if (!saved.ok() && !m_saveFailureLogged) {
        m_saveFailureLogged = true; // once: every further change would say the same
        Log(QStringLiteral("app")).warn(QStringLiteral("Settings could not be saved: %1").arg(saved.error().toLogString()));
    } else if (saved.ok()) {
        m_saveFailureLogged = false;
    }
}

LogModel& AppContext::logModel()
{
    if (!m_logModel)
        m_logModel = new LogModel(*this, this);
    return *m_logModel;
}

Status AppContext::initialize(Mode mode)
{
    if (mode == Mode::ReadOnly)
        m_queue->setPersistencePath({}); // never written, also not at shutdown
    m_ring = std::make_shared<RingBufferSink>(20000);
    Logger::instance().addSink(m_ring);

    // Read-only: a damaged settings file of the running app stays where it is.
    FW_TRY(m_settings->load(/*setAsideDamaged=*/mode != Mode::ReadOnly));
    const auto snap = m_settings->snapshot();
    // Read-only: the log stays in memory. The running app's log files are
    // neither appended to nor rotated by a second process.
    if (mode != Mode::ReadOnly) {
        const QString customLogs = snap.string(general::keys::kLogFolder).trimmed();
        const QString logsDir = (!customLogs.isEmpty() && QFileInfo(customLogs).isDir()) ? customLogs : Paths::logsDir();
        m_file = std::make_shared<FileLogSink>(logsDir, QStringLiteral("farewell"),
                                               snap.integer(general::keys::kLogMaxMiB) * 1024 * 1024,
                                               int(snap.integer(general::keys::kLogKeepFiles)));
        Logger::instance().addSink(m_file);
    }
    applyLogLevel();
    // One save for all the changes of one turn of the event loop: Reset all
    // and the v4 import change dozens of keys, and each save is a synced
    // rewrite of the file. Closing the app saves what is still pending.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(0);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_savePending)
            saveSettingsNow();
    });
    connect(m_settings.get(), &SettingsStore::changed, this, [this, mode](const QString& key, const QVariant&) {
        if (key == general::keys::kLogLevel)
            applyLogLevel();
        if (mode == Mode::ReadOnly)
            return; // never writes the running app's settings file
        m_savePending = true;
        m_saveTimer.start();
    });

    // Called on whichever thread logs: the record is posted to the application
    // object and this context is looked at on the UI thread only.
    QPointer<AppContext> self(this);
    m_uiSink = std::make_shared<CallbackSink>([self](const LogRecord& r) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, r] {
            if (self)
                emit self->logRecordAdded(r);
        }, Qt::QueuedConnection);
    });
    Logger::instance().addSink(m_uiSink);

    Log log(QStringLiteral("app"));
    log.info(QStringLiteral("%1 %2 starting").arg(QLatin1String(build::kProductName), QLatin1String(build::kVersion)),
             QJsonObject{{QStringLiteral("edition"), QLatin1String(build::kEditionName)},
                         {QStringLiteral("dataDir"), Paths::dataDir()},
                         {QStringLiteral("portable"), Paths::isPortable()}});
    if (mode == Mode::ReadOnly)
        return success();
    // What a crash or power loss left behind, removed on background threads
    // (it can take a while and must not hold up the start): job folders in the
    // app's own temp root and in the temporary folder chosen in Settings (jobs
    // run there when there is one), and the work folders of backport builds.
    const QString customTemp = snap.string(general::keys::kTempFolder).trimmed();
    Paths::sweepStaleTempInBackground(customTemp.isEmpty() ? QStringList() : QStringList{customTemp}, 24);
    (void)QtConcurrent::run([] { fpkg::sweepStaleBackportWork(); });

    if (snap.boolean(general::keys::kRestoreQueue)) {
        auto st = m_queue->restoreQueue();
        if (!st.ok())
            log.warn(st.error().toLogString());
        if (m_queue->pendingCount() > 0)
            m_queue->holdQueue(true); // restored jobs wait for the user
    }
    return success();
}

QString AppContext::logFilePath() const
{
    return m_file ? m_file->textPath() : QString();
}

void AppContext::applyLogLevel()
{
    const QString lvl = m_settings->value(general::keys::kLogLevel).toString();
    Logger::instance().setMinimumLevel(logLevelFromName(lvl, LogLevel::Info));
}

void AppContext::checkDependenciesAsync()
{
    QPointer<AppContext> self(this);
    const SettingsValues snap = m_settings->snapshot();
    auto* deps = &m_deps;
    auto* mutex = &m_depMutex;
    m_depChecks.removeIf([](const QFuture<void>& f) { return f.isFinished(); });
    m_depChecks << QtConcurrent::run([self, snap, deps, mutex] {
        const QMutexLocker lock(mutex); // one probe run at a time
        deps->probeAll(snap);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self] {
            if (self)
                emit self->dependenciesChecked();
        }, Qt::QueuedConnection);
    });
}

} // namespace fw
