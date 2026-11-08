#include "app/backport/BackportHost.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::bp {

namespace {

constexpr const char* kAppId = "Deckerr97BackportGuiBeta";

QString envDir(const char* name)
{
    const QString v = qEnvironmentVariable(name).trimmed();
    return v.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(v));
}

QString roamingBase()
{
#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty())
        return QDir::fromNativeSeparators(appData);
#endif
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

QString localBase()
{
#ifdef Q_OS_WIN
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        return QDir::fromNativeSeparators(local);
#endif
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

void setNoWindow(QProcess* p)
{
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(p);
#endif
}

} // namespace

Folders folders()
{
    Folders f;
    f.configDir = envDir("D97BETA_CONFIG_DIR");
    if (f.configDir.isEmpty())
        f.configDir = QDir(roamingBase()).filePath(QString::fromLatin1(kAppId));
    f.dataDir = envDir("D97BETA_DATA_DIR");
    if (f.dataDir.isEmpty())
        f.dataDir = QDir(localBase()).filePath(QString::fromLatin1(kAppId));
    f.jobsDir = QDir(f.dataDir).filePath(QStringLiteral("jobs"));
    f.logDir = QDir(f.dataDir).filePath(QStringLiteral("logs"));
    f.runsDir = QDir(f.logDir).filePath(QStringLiteral("runs"));
    f.settingsPath = QDir(f.configDir).filePath(QStringLiteral("settings.json"));
    f.ledgerPath = QDir(f.configDir).filePath(QStringLiteral("applied_backports.json"));
    return f;
}

bool ensureFolders(QString* error)
{
    const Folders f = folders();
    for (const QString& d : {f.configDir, f.dataDir, f.jobsDir, f.logDir}) {
        if (!QDir().mkpath(d)) {
            if (error)
                *error = QStringLiteral("Could not create %1").arg(QDir::toNativeSeparators(d));
            return false;
        }
    }
    return true;
}

QString engineHome()
{
    const QString env = envDir("D97BETA_ENGINE_HOME");
    if (!env.isEmpty())
        return env;
#ifdef Q_OS_WIN
    QSettings fs(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem"), QSettings::NativeFormat);
    const bool longPaths = fs.value(QStringLiteral("LongPathsEnabled"), 0).toInt() == 1;
    if (!longPaths) {
        QString drive = qEnvironmentVariable("SystemDrive");
        if (drive.isEmpty())
            drive = QStringLiteral("C:");
        return QDir::fromNativeSeparators(drive + QStringLiteral("\\D97B"));
    }
#endif
    return QDir(localBase()).filePath(QStringLiteral("D97B"));
}

QString engineExecutable()
{
    const QString env = qEnvironmentVariable("FW_BACKPORT_ENGINE").trimmed();
    if (!env.isEmpty() && QFileInfo(env).isFile())
        return QFileInfo(env).absoluteFilePath();
    QDir dir(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 4; ++up) {
        const QString candidate = dir.filePath(QStringLiteral("engines/backport/%1").arg(QLatin1String(kEngineExeName)));
        if (QFileInfo(candidate).isFile())
            return QFileInfo(candidate).absoluteFilePath();
        if (!dir.cdUp())
            break;
    }
    return {};
}

QProcessEnvironment workerEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString& k : env.keys())
        if (k.startsWith(QLatin1String("PYTHON"), Qt::CaseInsensitive))
            env.remove(k);
    env.remove(QStringLiteral("D97BETA_TEST_FAULT"));
    return env;
}

// ---- Worker ----------------------------------------------------------------------------

Worker::Worker(QObject* parent) : QObject(parent) {}

Worker::~Worker()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        // The stdin pipe closes with us: the worker cancels itself (and rolls
        // back an apply) at its next safe point. Kill only an engine stage.
        if (m_phase == QLatin1String("engine") && m_cmd != QLatin1String("restore"))
            killTree();
        m_proc->waitForFinished(2000);
    }
    if (!m_jobPath.isEmpty())
        QFile::remove(m_jobPath);
}

bool Worker::isRunning() const { return m_proc && m_proc->state() != QProcess::NotRunning && !m_done; }

bool Worker::start(const QJsonObject& job, QString* error)
{
    const QString exe = engineExecutable();
    if (exe.isEmpty()) {
        if (error)
            *error = QStringLiteral("The backport engine is missing from this installation (engines\\backport\\%1). Download the app again.")
                         .arg(QLatin1String(kEngineExeName));
        return false;
    }
    QString err;
    if (!ensureFolders(&err)) {
        if (error)
            *error = err;
        return false;
    }
    const Folders f = folders();
    m_cmd = job.value(QStringLiteral("cmd")).toString();
    m_jobPath = QDir(f.jobsDir).filePath(QStringLiteral("job_%1_%2.json")
                                             .arg(QCoreApplication::applicationPid())
                                             .arg(QDateTime::currentMSecsSinceEpoch()));
    QFile jf(m_jobPath);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write the job file %1: %2").arg(QDir::toNativeSeparators(m_jobPath), jf.errorString());
        return false;
    }
    jf.write(QJsonDocument(job).toJson(QJsonDocument::Compact));
    jf.close();

    m_proc = new QProcess(this);
    m_proc->setProgram(exe);
    m_proc->setArguments({QStringLiteral("--worker"), QDir::toNativeSeparators(m_jobPath)});
    m_proc->setWorkingDirectory(f.dataDir);
    m_proc->setProcessEnvironment(workerEnvironment());
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    setNoWindow(m_proc);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &Worker::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &Worker::onStderr);
    connect(m_proc, &QProcess::finished, this, &Worker::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && !m_done) {
            m_done = true;
            QJsonObject r{{QStringLiteral("status"), QStringLiteral("FAILED")},
                          {QStringLiteral("headline"), QStringLiteral("Could not start the backport worker: %1").arg(m_proc->errorString())}};
            QFile::remove(m_jobPath);
            emit finished(r);
        }
    });
    m_clock.start();
    m_proc->start(QIODevice::ReadWrite); // stdin stays open: EOF would mean cancel
    return true;
}

void Worker::cancel()
{
    if (!m_proc || m_done || m_cancelled)
        return;
    m_cancelled = true;
    if (m_proc->state() == QProcess::Running) {
        m_proc->write("cancel\n");
    } else {
        // Still starting: send it as soon as the process is up.
        connect(m_proc, &QProcess::started, this, [this] { m_proc->write("cancel\n"); }, Qt::SingleShotConnection);
    }
    if (m_cmd == QLatin1String("restore"))
        return; // restores ignore cancel and are never killed
    QPointer<Worker> self(this);
    QTimer::singleShot(1500, this, [self] {
        if (self && self->isRunning() && self->m_phase == QLatin1String("engine"))
            self->killTree();
    });
}

void Worker::killTree()
{
    if (!m_proc || m_proc->processId() == 0)
        return;
#ifdef Q_OS_WIN
    // One-file PyInstaller: the PID we own is the bootloader; the real worker
    // is its child. taskkill /T ends the whole tree.
    QProcess tk;
    tk.setProgram(QStringLiteral("taskkill"));
    tk.setArguments({QStringLiteral("/PID"), QString::number(m_proc->processId()), QStringLiteral("/T"), QStringLiteral("/F")});
    setNoWindow(&tk);
    tk.start();
    tk.waitForFinished(5000);
#else
    m_proc->kill();
#endif
}

void Worker::onStdout()
{
    m_outBuf += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = int(m_outBuf.indexOf('\n'))) >= 0) {
        const QByteArray line = m_outBuf.left(nl).trimmed();
        m_outBuf.remove(0, nl + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
}

void Worker::handleLine(const QByteArray& line)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return; // not a protocol line
    QJsonObject ev = doc.object();
    const QString kind = ev.value(QStringLiteral("event")).toString();
    if (kind == QLatin1String("progress")) {
        const QString ph = ev.value(QStringLiteral("phase")).toString();
        if (!ph.isEmpty())
            m_phase = ph;
        emit progress(ev);
    } else if (kind == QLatin1String("result")) {
        ev.remove(QStringLiteral("event"));
        m_result = ev;
        m_haveResult = true;
    }
}

void Worker::onStderr()
{
    m_errBuf += m_proc->readAllStandardError();
    int nl;
    while ((nl = int(m_errBuf.indexOf('\n'))) >= 0) {
        m_errTail << QString::fromUtf8(m_errBuf.left(nl)).trimmed();
        m_errBuf.remove(0, nl + 1);
        while (m_errTail.size() > 200)
            m_errTail.removeFirst();
    }
}

void Worker::onFinished()
{
    if (m_done)
        return;
    onStdout();
    if (!m_outBuf.trimmed().isEmpty()) {
        handleLine(m_outBuf.trimmed());
        m_outBuf.clear();
    }
    onStderr();
    if (!m_errBuf.isEmpty())
        m_errTail << QString::fromUtf8(m_errBuf).trimmed();
    m_done = true;
    QFile::remove(m_jobPath);
    QJsonObject r = m_result;
    if (!m_haveResult) {
        if (m_cancelled) {
            r = {{QStringLiteral("status"), QStringLiteral("CANCELLED")},
                 {QStringLiteral("headline"), QStringLiteral("Cancelled during the build; no files were written")}};
        } else {
            const QString tail = m_errTail.join(u'\n').right(2000);
            r = {{QStringLiteral("status"), QStringLiteral("FAILED")},
                 {QStringLiteral("headline"), QStringLiteral("The backport worker stopped unexpectedly")},
                 {QStringLiteral("stderr"), tail}};
        }
    }
    emit finished(r);
}

} // namespace fw::bp
