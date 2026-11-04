#pragma once

// Backport settings (docs/BACKPORT_PARITY.md §5). Every key is read by this
// module: optionsFromSettings() (backport.run, backport.scan, backport.sdkFix,
// backport.pinProviders), the scan job (library folders and depth) and the
// restore job (originals folder). Blocked
// features (PS5 Pro fix / Pro build, default fakelib set, right.sprx copy,
// import-table audit, audio check, save-data fixes, SDK 11-13) have no
// settings on purpose.

#include "core/Settings.h"
#include "engines/backport/BackportOptions.h"

namespace fw::backport {

namespace keys {
inline const QString kOriginalsZipFolder = QStringLiteral("backport.originalsZipFolder");
inline const QString kBackportZipFolder = QStringLiteral("backport.backportZipFolder");
inline const QString kTargetSdk = QStringLiteral("backport.targetSdk");
inline const QString kFakelibFolder = QStringLiteral("backport.fakelibFolder");
inline const QString kAmprPlaygoFolder = QStringLiteral("backport.amprPlaygoFolder");
inline const QString kApplyToGameFolder = QStringLiteral("backport.applyToGameFolder");
inline const QString kNidPatchMode = QStringLiteral("backport.nidPatchMode");
inline const QString kEncryptedSelfPolicy = QStringLiteral("backport.encryptedSelfPolicy");
inline const QString kVerifyBuild = QStringLiteral("backport.verifyBuild");
inline const QString kScanFolders = QStringLiteral("backport.scanFolders");
inline const QString kScanDepth = QStringLiteral("backport.scanDepth");
inline const QString kKeepStagingOnFailure = QStringLiteral("backport.keepStagingOnFailure");
inline const QString kJournalRetentionDays = QStringLiteral("backport.journalRetentionDays");
inline const QString kNotes = QStringLiteral("backport.notes");
inline const QString kProFix = QStringLiteral("backport.proFix"); // #32 shader-assert part (ProFix.h)
inline const QString kStandardRightSprx = QStringLiteral("backport.standardRightSprx"); // #35 (RightSprx.h)
// Read from the general module's schema when it is registered.
inline const QString kFreeSpaceMarginMiB = QStringLiteral("storage.freeSpaceMarginMiB");
} // namespace keys

inline constexpr int kMaxNotesLength = 500;

void registerBackportSettings(SettingsSchema& schema);

// The run options for one game from a settings snapshot (global values plus
// the job's overrides).
BackportOptions optionsFromSettings(const SettingsValues& settings, const QString& game);

} // namespace fw::backport
