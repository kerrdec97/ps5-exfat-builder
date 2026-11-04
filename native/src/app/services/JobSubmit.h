#pragma once

// The only place where pages hand work to the queue.
//
// Every function checks that the job kind is registered (AppContext::
// jobFactory().contains(kind)); when it is not, it returns an Error with
// code Unsupported and the UI shows the designed "not available in this
// build" state instead of enqueuing. Parameters are plain JSON, so the queue
// can persist, restore and retry them.
//
// Kinds and parameter names used by the UI (to reconcile with the engines):
//
//   Image builds   image.exfat.build / image.ufs2.build / image.pfs.build
//                  {source, output, label?, title?, titleId?, version?, format}
//                  `format` is "exfat" | "ffpkg" | "ffpfsc"; `output` is the
//                  destination folder; `label` the chosen file name (no path).
//   Image verify   image.<fmt>.verify   {image}
//   Image extract  image.<fmt>.extract  {image, destination}
//                  <fmt> comes from the file extension: .exfat → exfat,
//                  .ffpkg → ufs2, .ffpfs/.ffpfsc → pfs.
//   FPKG build     fpkg.build   {source, sourceType ("folder"|"exfat"|"ffpfsc"|"ffpkg"|"gp5"),
//                               output, contentId?, title?, contentVersion?,
//                               packageType ("app"|"addon"|"update"), basePackage?}
//   FPKG verify    fpkg.verify  {package, integrity: bool}
//   FPKG extract   fpkg.extract {package, destination}
//   Backport       backport.run     {gameFolders: [..]}   (+ per-job overrides of backport.* settings)
//                  backport.restore {originalsZip, gameFolder?}
//                  backport.scan    {folders: [..], depth?} → details.games (see LibraryGame)
//   Library        library.scan     {folders: [..]} → details.games = [{folder, title, titleId,
//                                   version, sdk, requiredFw, sizeBytes, files, cover?}]
//   PS5            ps5.ftp.upload   {host, port, localPath, remotePath}
//                  ps5.payload.send {host, port, file}
//   Updates        update.check     {apiUrl?}            → details = ReleaseInfo (+ newer, upToDate)
//                  update.download  {release: <details>} → outputs[0] = verified installer
//   Tools          tools.languages.scan    {folder, batch}
//                  tools.languages.strip   {gameRoot, remove: [..], backupFolder?}
//                  tools.languages.restore {backupZip, gameRoot}
//                  tools.report            {destinationFolder, logsFolder, extraFiles: [..], diagnostics: {}}

#include "core/Result.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace fw {
class AppContext;
}

namespace fw::jobs {

// Format identifiers as the UI names them.
inline const QString kFormatExfat = QStringLiteral("exfat");
inline const QString kFormatFfpkg = QStringLiteral("ffpkg");
inline const QString kFormatFfpfsc = QStringLiteral("ffpfsc");

QString imageBuildKind(const QString& format);     // "image.exfat.build", ...
QString imageKindForFile(const QString& path, const QString& op); // op = "verify" | "extract"; "" if unknown
QString outputExtension(const QString& format);    // ".exfat", ".ffpkg", ".ffpfsc"

bool isAvailable(const AppContext& ctx, const QString& kind);
QStringList missingKinds(const AppContext& ctx, const QStringList& kinds);
Error unavailable(const QString& kind);

Result<QString> submitImageBuild(AppContext& ctx, const QString& format, const QString& source, const QString& outputDir,
                                 const QString& label, const QVariantMap& overrides, const QJsonObject& extra = {});
Result<QString> submitVerify(AppContext& ctx, const QString& imageOrPackage, const QVariantMap& overrides);
Result<QString> submitExtract(AppContext& ctx, const QString& imageOrPackage, const QString& destination,
                              const QVariantMap& overrides);
Result<QString> submitFpkgBuild(AppContext& ctx, const QString& sourceType, const QString& source, const QString& output,
                                const QJsonObject& details, const QVariantMap& overrides);
Result<QString> submitBackport(AppContext& ctx, const QStringList& gameFolders, const QVariantMap& overrides);
Result<QString> submitBackportRestore(AppContext& ctx, const QString& originalsZip, const QString& gameFolder);
Result<QString> scanLibrary(AppContext& ctx, const QStringList& folders);
Result<QString> submitFtpUpload(AppContext& ctx, const QString& host, int port, const QString& localPath,
                                const QString& remotePath);
Result<QString> submitPayload(AppContext& ctx, const QString& host, int port, const QString& file);
Result<QString> submitUpdateCheck(AppContext& ctx);
Result<QString> submitUpdateDownload(AppContext& ctx, const QJsonObject& release);
Result<QString> submitLanguageScan(AppContext& ctx, const QString& folder, bool batch);
Result<QString> submitLanguageStrip(AppContext& ctx, const QString& gameRoot, const QStringList& remove,
                                    const QString& backupFolder);
Result<QString> submitLanguageRestore(AppContext& ctx, const QString& backupZip, const QString& gameRoot);
Result<QString> submitProblemReport(AppContext& ctx, const QString& destinationFolder, const QJsonObject& diagnostics);

// Kinds the Queue page hides (system jobs: update checks).
bool isSystemKind(const QString& kind);

} // namespace fw::jobs
