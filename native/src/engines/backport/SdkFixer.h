#pragma once

// SDK Fixer (docs/BACKPORT_PARITY.md §2.18, S24, P14): changes only the SDK
// marker of a folder (a full dump or extracted backport files): the PS4/PS5
// SDK values in each executable's process/module param (§2.5), then the file
// is fake-signed again (§2.8). No fakelib is added and the libc import rename
// is off. param.json and param.sfo are never written, and are checked
// unchanged afterwards; fakelib/ and decrypted/ are never processed (S33).
// The work happens in the job's staging folder; a build with any per-file
// failure changes nothing (D4).
//
//  * Copy (default): the folder is copied to "<output>/<name> SDK <N>" through
//    "<copy>.fwpart"; every executable and param file of the copy is checked
//    by SHA-256 and every other file by size, then the copy is renamed. The
//    source is only read (P14: the legacy SDK Fixer patched an ELF-only source
//    in place). A failed or cancelled copy is removed.
//  * In place: the originals of the files that change are written to a backup
//    ZIP in the originals ZIP folder ("<prefix> SDK <N> fix original files.zip",
//    read back before anything changes); the new files are applied through an
//    apply journal (ApplyJournal.h, the same as a backport's Apply: rollback
//    on error or cancel, start-up recovery after a crash, V7). The finished
//    journal is kept with the game's backup records
//    (BackportBackup/<TitleID>/…/sdk-fixes/) and undoes the fix:
//    backport.restore {game, undoSdkFix: true}. When the folder has its own
//    backport record, the fix is added to it (sdk_fixes) so a later restore of
//    the backport originals recognises the fixed files.

#include "core/FileUtil.h"
#include "core/Result.h"
#include "engines/backport/BackportOptions.h"
#include "jobs/Job.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fw::backport {

enum class SdkFixMode { Copy, InPlace };

struct SdkFixOptions {
    QString folder;
    int targetSdk = kDefaultTargetSdk;
    SdkFixMode mode = SdkFixMode::Copy;
    QString outputDir;     // copy: where the copy goes (default: the folder's parent)
    QString originalsDir;  // in place: backup ZIP and records (required); copy: report location
    QStringList excludeDirs; // never entered or copied (the ZIP folders)
    EncryptedPolicy encryptedPolicy = EncryptedPolicy::Refuse;
    bool verifyBuild = true;
    int journalRetentionDays = 30;
    qint64 freeSpaceMarginBytes = 512LL * 1024 * 1024;
    QString journalDir;    // default: <dataDir>/backport/journals
    QString reportDir;     // default: next to the game's backup records (RunReport.h)
    // ---- test seams (as BackportOptions) ----------------------------------------
    std::function<Result<DiskSpace>(const QString&)> diskSpaceProbe;
    std::function<Status(const QString& point, const QString& item)> testHook;
};

struct SdkFixResult {
    QString summary;
    bool nothingToDo = false;
    QString output;        // the copy, or the folder itself (in place)
    QString backupZip;     // in place
    QString journalPath;   // in place
    QString recordPath;    // in place: the kept journal that undoes the fix
    int rewritten = 0;
    int unchanged = 0;
    QStringList leftUnchanged;
    QStringList warnings;
    QString reportJson, reportHtml;
    QJsonObject details;
};

QStringList sdkFixPhases(); // Scan, Patch, Write, Verify

// "<name> SDK <N>": FW tags ("FW 4xx"), earlier SDK tags and "- Copy" / "(N)"
// suffixes are removed from the folder name; the title ID is kept.
QString sdkFixCopyName(const QString& folderName, int targetSdk);
QString sdkFixBackupZipName(const QString& prefix, int targetSdk);
// <records>/sdk-fixes of `game` in `originalsDir` (empty without one).
QString sdkFixRecordDir(const QString& originalsDir, const QString& game);
// The newest finished (not rolled back) SDK fix record of `game`, or empty.
QString latestSdkFixRecord(const QString& originalsDir, const QString& game);

Result<SdkFixResult> runSdkFix(const SdkFixOptions& options, JobContext& ctx);

JobOutcome toOutcome(const SdkFixResult& r);

} // namespace fw::backport
