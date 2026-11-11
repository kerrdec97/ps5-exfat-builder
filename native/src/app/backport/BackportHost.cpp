#include "app/backport/BackportHost.h"

#include "core/Tr.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
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

bool processAlive(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED; // it exists, it is just not ours to open
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    Q_UNUSED(pid);
    return true; // unknown: keep the file
#endif
}

// A job file is deleted when its worker ends; one that is left behind (the app
// was killed) would stay forever. Files older than a day whose writer is gone
// are removed.
void sweepStaleJobFiles(const QString& jobsDir)
{
    const QDateTime limit = QDateTime::currentDateTime().addDays(-1);
    const QFileInfoList files = QDir(jobsDir).entryInfoList({QStringLiteral("job_*.json")}, QDir::Files);
    for (const QFileInfo& fi : files) {
        if (fi.lastModified() > limit)
            continue;
        bool ok = false;
        const qint64 pid = fi.completeBaseName().section(u'_', 1, 1).toLongLong(&ok);
        if (ok && processAlive(pid))
            continue;
        QFile::remove(fi.absoluteFilePath());
    }
}

} // namespace

bool foreignEngineRunning()
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    QHash<quint32, quint32> parentOf;
    QList<quint32> engines;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        parentOf.insert(pe.th32ProcessID, pe.th32ParentProcessID);
        const QString name = QString::fromWCharArray(pe.szExeFile);
        if (name.startsWith(QLatin1String("PS5BackportGUI"), Qt::CaseInsensitive) && name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
            engines << pe.th32ProcessID;
    }
    CloseHandle(snap);
    const quint32 self = quint32(QCoreApplication::applicationPid());
    for (quint32 pid : engines) {
        // A worker of this app is a descendant of it (the one-file engine's
        // bootloader, then the real worker).
        bool ours = false;
        quint32 p = pid;
        for (int depth = 0; depth < 8 && p != 0; ++depth) {
            if (p == self) {
                ours = true;
                break;
            }
            const auto it = parentOf.constFind(p);
            if (it == parentOf.constEnd() || it.value() == p)
                break;
            p = it.value();
        }
        if (!ours)
            return true;
    }
#endif
    return false;
}

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
                *error = FW_TR("Could not create %1").arg(QDir::toNativeSeparators(d));
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
        m_proc->disconnect(this);
        // End of input is the worker's cancel signal: it stops (and rolls back
        // an apply) at its next safe point.
        m_proc->closeWriteChannel();
        if (killable()) {
            killTree();
            m_proc->waitForFinished(2000);
        } else if (!m_proc->waitForFinished(10000)) {
            // An export, apply or restore that is still writing: destroying the
            // process object would terminate it in the middle of a file, so
            // the object is left to the process for as long as the app lives.
            QProcess* p = m_proc;
            m_proc = nullptr;
            p->setParent(nullptr);
            QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
        }
    }
    if (!m_jobPath.isEmpty())
        QFile::remove(m_jobPath);
}

bool Worker::isRunning() const { return m_proc && m_proc->state() != QProcess::NotRunning && !m_done; }

// Only an engine stage may be killed: an export or apply finishes or rolls
// back its current step by itself, and a restore must run to its end. Before
// the first progress line only the commands that begin with the engine stage
// count as being in it.
bool Worker::killable() const
{
    if (m_cmd == QLatin1String("restore"))
        return false;
    if (m_phase.isEmpty())
        return m_cmd == QLatin1String("backport") || m_cmd == QLatin1String("selftest");
    return m_phase == QLatin1String("engine");
}

bool Worker::start(const QJsonObject& job, QString* error)
{
    const QString exe = engineExecutable();
    if (exe.isEmpty()) {
        if (error)
            *error = FW_TR("The backport engine is missing from this installation (engines\\backport\\%1). Download the app again.")
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
    sweepStaleJobFiles(f.jobsDir);
    m_cmd = job.value(QStringLiteral("cmd")).toString();
    m_jobPath = QDir(f.jobsDir).filePath(QStringLiteral("job_%1_%2.json")
                                             .arg(QCoreApplication::applicationPid())
                                             .arg(QDateTime::currentMSecsSinceEpoch()));
    QFile jf(m_jobPath);
    const QByteArray jobBytes = QJsonDocument(job).toJson(QJsonDocument::Compact);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate) || jf.write(jobBytes) != jobBytes.size() || !jf.flush()) {
        if (error)
            *error = FW_TR("Could not write the job file %1: %2").arg(QDir::toNativeSeparators(m_jobPath), jf.errorString());
        jf.close();
        QFile::remove(m_jobPath);
        m_jobPath.clear();
        return false;
    }
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
                          {QStringLiteral("headline"), FW_TR("Could not start the backport worker: %1").arg(m_proc->errorString())}};
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
        if (self && self->isRunning() && self->killable())
            self->killTree();
    });
}

void Worker::killTree()
{
    if (!m_proc || m_proc->processId() == 0)
        return;
#ifdef Q_OS_WIN
    // One-file PyInstaller: the PID we own is the bootloader; the real worker
    // is its child. taskkill /T ends the whole tree. It is Windows' own
    // taskkill (never one found through the search path), and it runs on its
    // own: nothing here waits for it.
    wchar_t sys[MAX_PATH + 1] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n > MAX_PATH) {
        m_proc->kill();
        return;
    }
    auto* tk = new QProcess;
    tk->setProgram(QDir(QString::fromWCharArray(sys, int(n))).filePath(QStringLiteral("taskkill.exe")));
    tk->setArguments({QStringLiteral("/PID"), QString::number(m_proc->processId()), QStringLiteral("/T"), QStringLiteral("/F")});
    tk->setStandardOutputFile(QProcess::nullDevice());
    tk->setStandardErrorFile(QProcess::nullDevice());
    setNoWindow(tk);
    QObject::connect(tk, &QProcess::finished, tk, &QObject::deleteLater);
    QObject::connect(tk, &QProcess::errorOccurred, tk, [tk, proc = QPointer<QProcess>(m_proc)](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (proc)
            proc->kill(); // at least the process we own
        tk->deleteLater();
    });
    tk->start();
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
        const QString tail = m_errTail.join(u'\n').right(2000);
        if (m_cancelled && killable()) {
            r = {{QStringLiteral("status"), QStringLiteral("CANCELLED")},
                 {QStringLiteral("headline"), FW_TR("Cancelled during the build; no files were written")}};
        } else if (m_cancelled) {
            // Past the engine stage (or a restore): files may have been written
            // or half restored, so this is not a clean cancel.
            const QString headline = m_cmd == QLatin1String("restore")
                ? FW_TR("The restore stopped before it finished; check the game folder and run Restore originals from a backup\u2026 again")
                : m_phase.isEmpty()
                ? FW_TR("The backport worker stopped during its work without finishing; check the output and, if the backport was "
                        "being applied, the game folder (Restore originals from a backup\u2026)")
                : FW_TR("The backport worker stopped during %1 without finishing; check the output and, if the backport was being "
                        "applied, the game folder (Restore originals from a backup\u2026)")
                      .arg(m_phase);
            r = {{QStringLiteral("status"), QStringLiteral("FAILED")}, {QStringLiteral("headline"), headline}, {QStringLiteral("stderr"), tail}};
        } else {
            r = {{QStringLiteral("status"), QStringLiteral("FAILED")},
                 {QStringLiteral("headline"), FW_TR("The backport worker stopped unexpectedly")},
                 {QStringLiteral("stderr"), tail}};
        }
    }
    emit finished(r);
}

} // namespace fw::bp
