#pragma once

// Tools: Language Stripper and the problem-report bundle.
//
// Job kinds and parameters:
//   tools.languages.scan    {folder, batch: bool}            → details.games = [LanguageScan JSON]
//   tools.languages.strip   {gameRoot, remove: [relative], backupFolder?}  (backupFolder defaults to
//                           tools.languageBackupFolder)       → outputs = [backup zip]
//   tools.languages.restore {backupZip, gameRoot}            → details.restored = n
//   tools.report            {destinationFolder, logsFolder, extraFiles: [], diagnostics: {}}
//                                                             → outputs = [report zip]

#include "jobs/Module.h"

namespace fw::tools {

void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kLanguageBackupFolder = QStringLiteral("tools.languageBackupFolder");
inline const QString kRedactReports = QStringLiteral("tools.redactProblemReports");
} // namespace keys

} // namespace fw::tools
