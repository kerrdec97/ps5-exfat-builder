#pragma once

// Everything one backport run needs, built from the settings snapshot plus
// the job's parameters (BackportSettings.cpp: optionsFromSettings()).

#include "core/FileUtil.h"
#include "core/Result.h"
#include "engines/backport/NidPatch.h"
#include "engines/backport/SdkTable.h"

#include <QString>

#include <functional>

namespace fw::backport {

enum class EncryptedPolicy { Refuse, LeaveUnchanged };

struct BackportOptions {
    QString game;
    int targetSdk = kDefaultTargetSdk;
    QString fakelibDir;
    QString originalsDir;
    QString backportDir;      // empty: no backport ZIP
    bool applyToGame = true;  // D5 (owner decision; Backport GUI defaults to off)
    NidMode nidMode = NidMode::Auto;
    EncryptedPolicy encryptedPolicy = EncryptedPolicy::Refuse;
    bool verifyBuild = true;
    bool keepStagingOnFailure = true;
    int journalRetentionDays = 30;
    QString notes;
    bool confirmDiffers = false; // the user confirmed S7 "differs"
    qint64 freeSpaceMarginBytes = 512LL * 1024 * 1024;

    // Where the private staging folder will be (S11/S12). Default: the job's
    // temporary folder.
    QString stagingHint;
    // Apply journals (default: <dataDir>/backport/journals).
    QString journalDir;

    // ---- test seams -----------------------------------------------------------
    // Free-space probe (default fw::diskSpace).
    std::function<Result<DiskSpace>(const QString&)> diskSpaceProbe;
    // Fault injection: called at "build.file", "zip.member", "zip.commit" and
    // "apply.file" (an error aborts at that point) and "apply.crash" (an error
    // makes Apply stop as if the process died: no rollback, journal pending).
    std::function<Status(const QString& point, const QString& item)> testHook;
};

} // namespace fw::backport
