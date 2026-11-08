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
//   Image builds   image.exfat.build / image.ffpkg.build / image.pfs.build
//                  {source, output (full file path), format ("ffpfsc" or "ffpfs" for PFS)}
//   Image verify   image.<fmt>.verify   {image}  (no source: standalone integrity check)
//   Image extract  image.<fmt>.extract  {image, output, deep (PFS)}
//                  <fmt> from the extension: .exfat → exfat, .ffpkg → ffpkg,
//                  .ffpfs/.ffpfsc → pfs.
//   FPKG build     fpkg.build   {source (folder | .gp5 | image), output?, contentId?, contentVersion?,
//                               title?, basePackage?, baseFolder?, exactSource?, updateOutput?}
//   FPKG add-on    fpkg.addon   {payloadFolder, output?, contentId?, entitlementKey, volumeType?}
//   FPKG verify    fpkg.verify  {package, integrity: bool}
//   FPKG extract   fpkg.extract {package, destination}
//   Library        library.scan     {folders: [..]} → details.games = [LibraryGame::toJson()]
//   PS5            ps5.ftp.upload   {sources: [..], remoteDir?, host?, port?, allowProtectedPaths?}
//                  ps5.ftp.download {sources: [remote..], localDir, overwrite?, host?, port?}
//                  ps5.payload.send {file, target? ("payload"|"shadowmount"|...), host?, port?}
//                  ps5.dpi.install  {package, host?, port?}
//                  ps5.y2jb.autoBlock {region ("USA"|"EU"|"JP"), confirmed: true, backupDir?, host?, port?}
//                  ps5.shadowmount.stop {host?, port?}
//                  ps5.ftp.diagnose {host?, port?, folder?, speedTest} → details = ps5::FtpDiagnosis::toJson()
//   AMPR index     ampr.index.local {folder, force?}           (pfs: writes <folder>/ampr_emu.index)
//                  ampr.index.ftp   {remoteFolder, host?, port?} (integration: build on the console folder, upload with .bak)
//   Image edit     image.exfat.edit {image, operations: [{op: add|replace|delete|mkdir, imagePath, localPath}]}
//                  image.ffpkg.edit {image, operations: [{op: add|replace|delete|rename|mkdir, path, source, newName}]}
//   Convert        image.convert    {image, format ("exfat"|"ffpkg"|"ffpfs"|"ffpfsc"), output (file path),
//                                     method? ("direct"|"repack"), replaceExisting?}
//   Compare        image.compare    {a (image), b (image or folder), depth ("full" (default) | "structure")}
//                  Images: .exfat / .ffpkg / .ffpfs / .ffpfsc (either side may also be a folder).
//                  The job succeeds whatever it finds and leaves outcome.verification NotRun;
//                  summary = one line; details = {
//                    a, b: {path, kind, files, dirs, bytes}, depth, identical: bool,
//                    counts: {missing, extra, missingDirs, extraDirs, sizeDifferent, contentDifferent,
//                             unreadable, differences, filesInBoth, contentCompared, bytesCompared, padded},
//                    missing, extra, missingDirs, extraDirs: [path]   ("missing" = only in A, "extra" = only in B),
//                    sizeDifferent: [{path, a, b}], contentDifferent: [{path, offset (first differing byte)}],
//                    unreadable: [{path, message}], listLimit (500), truncated: bool, warnings: [..] }
//                  Lists hold at most listLimit entries; counts are exact.
//   Hex preview    no job: services/HexPreview reads slices of a file inside an image on a
//                  worker thread (read-only, the image is not kept open between slices).
//   Dump rename    library.rename   {mode: preview|apply|undo, source, preset?, destination?, items: [{source, name, selected}], undoLog}
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
inline const QString kFormatFfpfs = QStringLiteral("ffpfs"); // uncompressed PFS (Build and Convert; image.pfs.build format=ffpfs)

QString imageBuildKind(const QString& format);     // "image.exfat.build", ...
QString imageKindForFile(const QString& path, const QString& op); // op = "verify" | "extract"; "" if unknown
QString outputExtension(const QString& format);    // ".exfat", ".ffpkg", ".ffpfs", ".ffpfsc"

bool isAvailable(const AppContext& ctx, const QString& kind);
QStringList missingKinds(const AppContext& ctx, const QStringList& kinds);
Error unavailable(const QString& kind);

Result<QString> submitImageBuild(AppContext& ctx, const QString& format, const QString& source, const QString& outputDir,
                                 const QString& label, const QVariantMap& overrides, const QJsonObject& extra = {});
Result<QString> submitVerify(AppContext& ctx, const QString& imageOrPackage, const QVariantMap& overrides);
// game.extract: a game (folder or image, an image inside a PFS container
// included) into a new folder, zstd/LZ4 packs unpacked unless `unpackPacks` is false.
Result<QString> submitGameExtract(AppContext& ctx, const QString& source, const QString& destination, bool unpackPacks,
                                  const QVariantMap& overrides = {});
Result<QString> submitExtract(AppContext& ctx, const QString& imageOrPackage, const QString& destination,
                              const QVariantMap& overrides);
Result<QString> submitFpkgBuild(AppContext& ctx, const QString& sourceType, const QString& source, const QString& output,
                                const QJsonObject& details, const QVariantMap& overrides);
// fpkg.backport (engines/fpkg/BackportUpdate.h): backport files as separate
// update packages against a base .pkg, or with buildBase the game's package
// first and the updates against it.
Result<QString> submitFpkgBackport(AppContext& ctx, const QJsonObject& params, const QVariantMap& overrides = {});
// dlc.convert (engines/dlc/DlcConvert.h).
Result<QString> submitDlcConvert(AppContext& ctx, const QJsonObject& params, const QVariantMap& overrides = {});
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

Result<QString> submitFtpDownload(AppContext& ctx, const QString& host, int port, const QStringList& remotePaths,
                                  const QString& localDir, bool overwrite);
Result<QString> submitFtpUploadMany(AppContext& ctx, const QString& host, int port, const QStringList& localPaths,
                                    const QString& remoteDir, bool allowProtectedPaths);
Result<QString> submitDpiInstall(AppContext& ctx, const QString& host, int port, const QString& package);
Result<QString> submitY2jbAutoBlock(AppContext& ctx, const QString& host, int port, const QString& region,
                                    const QString& backupDir);
Result<QString> submitShadowMountStop(AppContext& ctx, const QString& host, int port);
// `folder` empty = the games folder from Settings.
Result<QString> submitFtpDiagnose(AppContext& ctx, const QString& host, int port, const QString& folder, bool speedTest);
// `operations` already in the engine's JSON shape (see the header comment).
Result<QString> submitImageEdit(AppContext& ctx, const QString& image, const QJsonArray& operations,
                                const QVariantMap& overrides);
// `method` "direct" | "repack" | "" (the engine default: direct for .exfat/.ffpkg -> .ffpfsc).
Result<QString> submitConvert(AppContext& ctx, const QString& image, const QString& format, const QString& output,
                              const QVariantMap& overrides, const QString& method = {}, bool replaceExisting = false);
// `depth` = "full" | "structure".
Result<QString> submitCompare(AppContext& ctx, const QString& a, const QString& b, const QString& depth);
Result<QString> submitLibraryRename(AppContext& ctx, const QString& mode, const QString& source, const QString& preset,
                                    const QString& destination, const QJsonArray& items, const QString& undoLog);
Result<QString> submitAmprIndexLocal(AppContext& ctx, const QString& folder, bool force);
Result<QString> submitAmprIndexFtp(AppContext& ctx, const QString& host, int port, const QString& remoteFolder);
QString imageEditKind(const QString& image); // "image.exfat.edit" / "image.ffpkg.edit" / ""

// Kinds the Queue page hides (system jobs: update checks).
bool isSystemKind(const QString& kind);

} // namespace fw::jobs
