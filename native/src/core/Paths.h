#pragma once

// Where the application keeps its own files.
//
// Default: settings and history in the roaming app-data folder, logs, temp and
// caches in the local app-data folder. Portable mode (a `portable.txt` in the
// app folder, see appDir()) keeps everything in `<app dir>/data`. The
// FW_DATA_DIR environment variable overrides both (tests, CI).

#include <QString>

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

// Removes this app's own temp folders older than `maxAgeHours` (startup sweep,
// audit M9). Returns how many were removed.
int sweepStaleTemp(int maxAgeHours = 24);
// The same sweep in another folder (the temporary folder chosen in Settings):
// only "fw-" folders older than maxAgeHours go.
int sweepStaleTempIn(const QString& folder, int maxAgeHours = 24);

// Bytes used by this app's temp folders (Settings > Storage).
qint64 tempUsageBytes();

} // namespace fw::Paths
