#pragma once

// Tools: Language Stripper, the dump check and the problem-report bundle.
//
// Job kinds and parameters:
//   tools.languages.scan    {folder, batch: bool}            → details.games = [LanguageScan JSON]
//   tools.languages.strip   {gameRoot, remove: [relative], backupFolder?}  (backupFolder defaults to
//                           tools.languageBackupFolder)       → outputs = [backup zip]; when some items
//                           could not be removed completely the outcome is kept (verification
//                           Inconclusive, a warning per item and per file left in place)
//   tools.languages.restore {backupZip, gameRoot}            → details.restored = n, details.skippedExisting
//                           = files already in the game (never replaced; listed as warnings)
//   tools.dump.check        {folder, title?}                 → read-only check of a game dump (DumpCheck.h):
//                           details = {folder, findings: [{severity: error|warning|info, code, path?, message,
//                           count?, paths?}], counts: {files, folders, bytes, executables: {total, <class>: n},
//                           errors, warnings, infos, listed, truncated, byCode: {code: n}}, facts: {...}};
//                           it succeeds whenever the walk completed: verification Passed when nothing is
//                           wrong, Inconclusive with problems or warnings; at most 500 findings are listed,
//                           the counts are exact. `title` is only the name the job shows.
//   tools.report            {destinationFolder, logsFolder, extraFiles: [], diagnostics: {}}
//                                                             → outputs = [report zip]; also takes the newest
//                           crash reports (<logsFolder>/crashes/crash-*.txt)

#include "jobs/Module.h"

namespace fw::tools {

void registerModule(ModuleContext& ctx);

namespace jobkinds {
inline const QString kDumpCheck = QStringLiteral("tools.dump.check");
} // namespace jobkinds

namespace keys {
inline const QString kLanguageBackupFolder = QStringLiteral("tools.languageBackupFolder");
inline const QString kRedactReports = QStringLiteral("tools.redactProblemReports");
} // namespace keys

} // namespace fw::tools
