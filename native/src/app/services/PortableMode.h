#pragma once

// "Make this copy portable" (Settings › Storage).
//
// Portable mode is a file: `portable.txt` next to the single-file exe. With
// it the launcher (src/onefile) unpacks the program into
// `<exe folder>\exFAT Image Builder.data` and the app keeps settings, queue,
// history, logs and temporary files in `<exe folder>\data` (core/Paths).
// The folder edition (the release zip, GitHub #78) has nothing to unpack:
// `portable.txt` goes next to its own exe and only `data` is new.
//
// Everything here takes its paths as arguments, so it can be run against any
// folder (the tests use a temporary one in place of the exe's).

#include "core/Result.h"

#include <QString>
#include <QStringList>

namespace fw::portable {

struct Plan {
    QString exePath;        // the single-file exe
    QString exeDir;         // its folder
    QString markerFile;     // <exeDir>/portable.txt
    QString programDir;     // <exeDir>/exFAT Image Builder.data: the unpacked program from the next start
    QString dataDir;        // <exeDir>/data: settings, queue, history, logs and temporary files from the next start
    QString currentDataDir; // where settings, queue and history are now
    bool unpacks = true;    // false for the folder edition: the program already is in exeDir
};

// What making the copy at `launcherExe` portable means. Nothing is touched.
Plan planFor(const QString& launcherExe, const QString& currentDataDir);

// The same for the folder edition, whose exe is the app itself: programDir is
// the exe's own folder and nothing is unpacked.
Plan planForFolderEdition(const QString& appExe, const QString& currentDataDir);

// True for a folder laid out as the release ships it (the folder edition or
// an unpacked copy): LICENCE.txt and Qt6Core.dll next to the exe. A developer
// build folder has neither.
bool isFolderEdition(const QString& appDir);

// The files "Copy my settings and queue" takes along: the files directly in
// the current data folder (settings, queue, history, the library and payload
// lists), names only, sorted. Not its sub-folders, and not the lock file of
// the running app.
QStringList filesToCopy(const Plan& plan);

struct Outcome {
    QStringList copied; // file names copied into Plan::dataDir
    QStringList kept;   // already there: left as they were, not overwritten
};

// Checks that the exe's folder takes new files, copies the data files when
// `copyData` (never moved, never over a file that is already in the target)
// and creates portable.txt last: when anything before it fails, the copy is
// not portable and the next start is as it was. Fails with AlreadyExists when
// portable.txt is there already and with PermissionDenied when the folder
// cannot be written to.
Result<Outcome> makePortable(const Plan& plan, bool copyData);

} // namespace fw::portable
