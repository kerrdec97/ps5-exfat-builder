#pragma once

// Where the application keeps its own files.
//
// Default: settings and history in the roaming app-data folder, logs, temp and
// caches in the local app-data folder. Portable mode (a `portable.txt` next to
// the executable) keeps everything in `<exe dir>/data`. The FW_DATA_DIR
// environment variable overrides both (tests, CI).

#include <QString>

namespace fw::Paths {

// Call once after QCoreApplication exists (sets org/app names, resolves mode).
void initialize();
// Tests: force every location under `root`.
void overrideRoot(const QString& root);

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

} // namespace fw::Paths
