#pragma once

// Where the application keeps its own files.
//
// Default: settings and history in the roaming app-data folder, logs, temp and
// caches in the local app-data folder. Portable mode (a `portable.txt` in the
// app folder, see appDir()) keeps everything in `<app dir>/data`. The
// FW_DATA_DIR environment variable overrides both (tests, CI).

#include <QString>
#include <QStringList>

namespace fw::Paths {

// Call once after QCoreApplication exists (sets org/app names, resolves mode).
void initialize();
// Tests: force every location under `root`.
void overrideRoot(const QString& root);
// Tests: forget the resolved locations; the next call resolves them again.
void resetForTests();

// The folder the user keeps the program in. Started through the single-file
// launcher (src/onefile) the program runs from an unpacked copy, so this is
// the launcher's folder (FW_LAUNCHER_DIR, which the launcher sets); otherwise
// the executable's own folder.
QString appDir();

// The single-file exe the app was started through (FW_LAUNCHER_EXE, which the
// launcher sets); "" when the program was started directly (a developer
// build, a test).
QString launcherExe();
// Where the program's own files are: the unpacked copy when it was started
// through the launcher, otherwise the executable's folder.
QString programDir();
// `portable.txt` in appDir(): the file that turns portable mode on. It is
// looked at once, when the locations are resolved (isPortable()).
QString portableMarkerFile();
// True when FW_DATA_DIR or a test chose the locations; portable.txt is not
// looked at then.
bool isOverridden();

bool isPortable();
QString dataDir();     // settings, history, queue
QString localDir();    // logs, temp, caches
QString settingsFile();
QString historyFile();
QString queueFile();
QString logsDir();
QString cacheDir();
QString tempRoot();    // private per-user temp root for jobs
QString toolsDir();    // where user-chosen tool paths are remembered / extracted helpers

// Removes the "fw-" folders in the app's own temp root that are older than
// `maxAgeHours` (startup sweep, audit M9). Returns how many were removed.
// A folder whose marker names a process that still runs (TempDir::isInUse)
// stays in both sweeps, whatever its age.
int sweepStaleTemp(int maxAgeHours = 24);
// The same sweep in another folder (the temporary folder chosen in Settings).
// That folder is the user's, so only what the app can prove it created goes:
// a folder with the queue's job-folder name that holds TempDir's marker file.
// Anything else stays, whatever its name. Links are removed, never followed.
int sweepStaleTempIn(const QString& folder, int maxAgeHours = 24);
// Both sweeps on a background thread (the app's own temp root, then each of
// `otherFolders`): deleting what a crashed job left can take a while and must
// not hold up the start of the app.
void sweepStaleTempInBackground(const QStringList& otherFolders = {}, int maxAgeHours = 24);

// Bytes used by the app's own temp root (Settings > Storage). Links and
// junctions are not followed.
qint64 tempUsageBytes();
// The same for another folder (the temporary folder chosen in Settings): only
// the job folders sweepStaleTempIn() would remove are counted.
qint64 tempUsageBytesIn(const QString& folder);

} // namespace fw::Paths
