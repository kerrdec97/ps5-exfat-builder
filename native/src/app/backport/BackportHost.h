#pragma once

// Host side of the bundled PS5 Backport engine (the owner's PS5 Backport GUI,
// run as `PS5BackportGUI.exe --worker job.json`). Which release is bundled
// is chosen in cmake/BackportEngine.cmake: the 0.3.5-test4 testing build by
// default, 0.3.4 with -DFW_BACKPORT_ENGINE_VERSION=0.3.4.
//
// The Backport page replaces that program's window; every build, rebuild,
// switch and restore runs in the owner's worker exactly as in the original
// program, so results are identical. This file is the protocol driver:
//
//  * job file in <DATA_DIR>\jobs, written UTF-8 without BOM, deleted after;
//  * cwd = DATA_DIR, every PYTHON* variable removed, CREATE_NO_WINDOW;
//  * stdout: one JSON object per line; `progress` and `result` are used,
//    anything else (non-JSON, unknown events) is ignored;
//  * stdin stays open for the worker's whole life (EOF means cancel);
//    cancel = "cancel\n", then after 1.5 s the process TREE is killed, but
//    only while the last progress phase is "engine" (export and apply finish
//    or roll back their current step themselves; a restore is never killed);
//  * stderr is drained continuously (last 200 lines kept);
//  * no result line: CANCELLED when cancel was asked during the engine stage,
//    FAILED "stopped during <phase>" when it was asked later (files may have
//    been written), otherwise FAILED "The backport worker stopped
//    unexpectedly"; the last two carry the stderr tail.
//
// Per-user folders are the original program's (so a backport applied by the
// standalone PS5 Backport GUI is recognised here and vice versa):
//   CONFIG_DIR %APPDATA%\Deckerr97BackportGuiBeta   (settings.json, applied_backports.json)
//   DATA_DIR   %LOCALAPPDATA%\Deckerr97BackportGuiBeta (logs, logs\runs, jobs)
// D97BETA_CONFIG_DIR / D97BETA_DATA_DIR override them, as in the original.

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace fw::bp {

// One source for the engine version: cmake/BackportEngine.cmake
// (FW_BACKPORT_ENGINE_VERSION; the fallbacks are its default).
#ifndef FW_BACKPORT_ENGINE_VERSION_STR
#define FW_BACKPORT_ENGINE_VERSION_STR "0.3.5-test4"
#endif
#ifndef FW_BACKPORT_ENGINE_REVISION_STR
#define FW_BACKPORT_ENGINE_REVISION_STR "3e08eda5-19422409a6"
#endif
#ifndef FW_BACKPORT_ENGINE_TESTING
#define FW_BACKPORT_ENGINE_TESTING 1
#endif
inline constexpr const char* kEngineVersion = FW_BACKPORT_ENGINE_VERSION_STR;
// The engine revision the pinned exe reports in its selftest.
inline constexpr const char* kEngineRevision = FW_BACKPORT_ENGINE_REVISION_STR;
// True when the bundled engine is a testing build, not a regular release.
inline constexpr bool kEngineIsTestingBuild = FW_BACKPORT_ENGINE_TESTING != 0;
inline constexpr const char* kEngineExeName = "PS5BackportGUI.exe";
inline constexpr const char* kIssuesUrl = "https://github.com/kerrdec97/PS5-Backport-GUI/issues/new";

struct Folders {
    QString configDir;   // settings.json, applied_backports.json
    QString dataDir;     // worker cwd
    QString jobsDir;
    QString logDir;      // app_YYYYMMDD.log
    QString runsDir;     // worker run logs
    QString settingsPath;
    QString ledgerPath;
};
Folders folders();
bool ensureFolders(QString* error = nullptr);

// Where the engine keeps its unpacked copy (for the working-space check):
// %LOCALAPPDATA%\D97B, or %SystemDrive%\D97B while Windows long paths are off.
QString engineHome();

// The bundled engine executable. Order: FW_BACKPORT_ENGINE (env), then
// <app>\engines\backport\PS5BackportGUI.exe, then the same under the app's
// parent folders (developer builds). Empty when not found.
QString engineExecutable();

// Removes every PYTHON* variable (the engine must not see a local Python).
QProcessEnvironment workerEnvironment();

// True while a PS5 Backport GUI process runs that is not a worker of this app
// (the standalone program, which shares the settings and the ledger).
bool foreignEngineRunning();
// Tests only: replaces the process-list check (an empty function restores it).
void setForeignEngineCheckForTests(std::function<bool()> check);

// One worker process (one job). Not reusable: create one per job.
class Worker : public QObject {
    Q_OBJECT
public:
    explicit Worker(QObject* parent = nullptr);
    ~Worker() override;

    // Starts `job`. On failure emits nothing and returns false with `error`.
    bool start(const QJsonObject& job, QString* error);
    void cancel();               // idempotent; restores ignore the kill rule
    bool isRunning() const;
    QString phase() const { return m_phase; }
    QString command() const { return m_cmd; }
    qint64 elapsedMs() const { return m_clock.isValid() ? m_clock.elapsed() : 0; }

signals:
    void progress(const QJsonObject& event); // {stage, label, detail, pct, phase}
    void finished(const QJsonObject& result); // always exactly once after a successful start()

private:
    void onStdout();
    void onStderr();
    void onFinished();
    void handleLine(const QByteArray& line);
    void killTree();
    bool killable() const;

    QProcess* m_proc = nullptr;
    QByteArray m_outBuf, m_errBuf;
    QStringList m_errTail;
    QJsonObject m_result;
    bool m_haveResult = false;
    bool m_cancelled = false;
    bool m_done = false;
    QString m_phase; // the last progress phase; empty until the worker reports one
    QString m_cmd;
    QString m_jobPath;
    QElapsedTimer m_clock;
};

// The engine's `selftest` job: revision, engine root, module checks.
struct EngineInfo {
    bool ok = false;
    QString revision;     // "3e08eda5-914d90065d"
    QString engineRoot;
    QString error;        // user-facing
};

} // namespace fw::bp
