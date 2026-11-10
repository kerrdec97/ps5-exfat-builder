#pragma once

// Shared helpers for every test executable.

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "jobs/Job.h"

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace fwtest {

// Points Paths at a fresh temporary root so tests never touch real app data.
// Call from initTestCase(). It also keeps the test off the network and away
// from the files the bundled PS5 Backport engine shares with its standalone
// GUI: FW_OFFLINE is set (unless the caller set it), and D97BETA_CONFIG_DIR,
// D97BETA_DATA_DIR and D97BETA_ENGINE_HOME point into `dir`. A test that needs
// other values sets them after this call. A temporary folder that could not
// be created ends the program: nothing may run without the isolation.
void isolateAppData(QTemporaryDir& dir);

// True while a process with this id exists (false for an id that was never
// given out or whose process has ended).
bool processIsRunning(qint64 pid);
// Ends a process the test itself started (a stand-in left sleeping). Never
// call it with an id that did not come from such a process.
void endProcess(qint64 pid);

// Takes away (or gives back) the right to list `dir` (icacls on Windows, the
// mode bits elsewhere). False when this machine cannot do that. A test must
// give the right back before its temporary folder is removed, and skip when
// the account can list the folder all the same (administrator, root).
bool denyListing(const QString& dir, bool deny);

// Writes `size` deterministic pseudo-random bytes (seeded) to `path`.
bool writeRandomFile(const QString& path, qint64 size, quint32 seed);
bool writeTextFile(const QString& path, const QByteArray& text);

QString helperPath();
QString dataDir();

} // namespace fwtest
