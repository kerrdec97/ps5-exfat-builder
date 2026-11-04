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
    // #34: the user's own libSceAmpr / libScePlayGo (ProviderSet.h); empty: none.
    QString providerDir;
    // Where the provider pins are kept (default: <dataDir>/backport/provider-pins.json).
    QString providerPinsPath;
    QString originalsDir;
    QString backportDir;      // empty: no backport ZIP
    bool applyToGame = true;  // D5 (owner decision; Backport GUI defaults to off)
    NidMode nidMode = NidMode::Auto;
    EncryptedPolicy encryptedPolicy = EncryptedPolicy::Refuse;
    bool verifyBuild = true;
    // #32: PS5 Pro fix, shader-assert part (ProFix.h): the root eboot.bin's
    // guarded "s_trap 1" become "s_nop 0". Opt-in, never automatic.
    bool proFix = false;
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
    // Run reports (#24). Empty: next to the game's backup records
    // (RunReport.h), or <dataDir>/backport/reports without an originals folder.
    QString reportDir;

    // ---- test seams -----------------------------------------------------------
    // Free-space probe (default fw::diskSpace).
    std::function<Result<DiskSpace>(const QString&)> diskSpaceProbe;
    // Fault injection: called at "build.file", "zip.member", "zip.commit" and
    // "apply.file" (an error aborts at that point) and "apply.crash" (an error
    // makes Apply stop as if the process died: no rollback, journal pending).
    std::function<Status(const QString& point, const QString& item)> testHook;
};

} // namespace fw::backport
