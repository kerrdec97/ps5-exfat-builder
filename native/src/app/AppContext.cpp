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
    // A dependency check still running uses m_deps: wait for it first.
    for (QFuture<void>& f : m_depChecks)
        f.waitForFinished();
    if (m_queue)
        m_queue->shutdown(15000);
    // The logger is process-wide and outlives this object: none of its sinks
    // stays registered (the file sink would keep the log file open).
    Logger::instance().flush();
    for (const std::shared_ptr<LogSink>& sink : {std::shared_ptr<LogSink>(m_uiSink), std::shared_ptr<LogSink>(m_file), std::shared_ptr<LogSink>(m_ring)})
        if (sink)
            Logger::instance().removeSink(sink);
    if (g_instance == this)
        g_instance = nullptr;
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

    FW_TRY(m_settings->load());
    const auto snap = m_settings->snapshot();
    const QString customLogs = snap.string(general::keys::kLogFolder).trimmed();
    const QString logsDir = (!customLogs.isEmpty() && QFileInfo(customLogs).isDir()) ? customLogs : Paths::logsDir();
    m_file = std::make_shared<FileLogSink>(logsDir, QStringLiteral("farewell"),
                                           snap.integer(general::keys::kLogMaxMiB) * 1024 * 1024,
                                           int(snap.integer(general::keys::kLogKeepFiles)));
    Logger::instance().addSink(m_file);
    applyLogLevel();
    connect(m_settings.get(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == general::keys::kLogLevel)
            applyLogLevel();
        const Status saved = m_settings->save();
        if (!saved.ok() && !m_saveFailureLogged) {
            m_saveFailureLogged = true; // once: every further change would say the same
            Log(QStringLiteral("app")).warn(QStringLiteral("Settings could not be saved: %1").arg(saved.error().toLogString()));
        } else if (saved.ok()) {
            m_saveFailureLogged = false;
        }
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
