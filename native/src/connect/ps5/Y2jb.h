#pragma once

// Y2JB (YouTube-to-jailbreak) console tools, legacy PS5 > Y2JB tab.
//
// Regions: USA PPSA01650, EU PPSA01651, JP PPSA01652.
//
// Update block: param.json contentVersion = "99.999.999", versionFileUri =
// "http://127.0.0.2", localVersion = masterVersion (each only when the key is
// there); appinfo.db tbl_appinfo VERSION_FILE_URI / CONTENT_VERSION rows of the
// title; app.db tbl_contentinfo.AppInfoJson (entries of $.field_list[] by key
// when that array exists, else top-level keys). Every patch is verified by
// reading it back. Over FTP each file goes RETR (size checked against SIZE)
// -> local backup -> remote backup (legacy, best effort) -> patch a temporary
// copy (databases: PRAGMA integrity_check must pass) -> upload to a temporary
// name and rename over the live file (never truncated in place) -> RETR again
// -> verify. A failed upload or read-back puts the original back from the
// local backup. 0 matching rows = nothing is uploaded.
//
// param.json is edited in place in its original text (only the targeted
// values change; the legacy app re-serialised the whole file).

#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Log.h"
#include "core/Result.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::ps5 {

enum class Y2jbRegion { USA, EU, JP };
QString y2jbTitleId(Y2jbRegion region);   // "PPSA01650" ...
QString y2jbRegionName(Y2jbRegion region); // "USA", "EU", "JP"
std::optional<Y2jbRegion> y2jbRegionFromName(const QString& name);
QList<Y2jbRegion> allY2jbRegions();

// The YouTube app version Y2JB needs per firmware (Gezine/Y2JB 1.6 README;
// research §8). Install exactly that version, then block its updates.
struct Y2jbYoutubeVersion {
    QString firmwareFrom; // "4.03"
    QString firmwareTo;   // "12.40"
    QString youtube;      // "v01.000.003"
};
const QList<Y2jbYoutubeVersion>& y2jbYoutubeVersions();
// The version for a firmware such as "12.60" or "13.2"; empty outside 4.03–13.60.
QString y2jbYoutubeVersionFor(const QString& firmware);

inline constexpr const char* kBlockedContentVersion = "99.999.999";
inline constexpr const char* kBlockedVersionFileUri = "http://127.0.0.2";

struct PatchCheck {
    bool ok = false;
    QString reason;
};

// ---- the patches (pure / local files) -----------------------------------------------
Result<QByteArray> patchParamJsonText(const QByteArray& original);
PatchCheck verifyParamJsonText(const QByteArray& text);
// Return the number of rows changed. Need Qt's SQLite driver (QSQLITE) with
// JSON functions for app.db.
Result<int> patchAppInfoDb(const QString& dbPath, const QString& titleId);
PatchCheck verifyAppInfoDb(const QString& dbPath, const QString& titleId);
Result<int> patchAppDb(const QString& dbPath, const QString& titleId);
PatchCheck verifyAppDb(const QString& dbPath, const QString& titleId);
// PRAGMA integrity_check must answer exactly "ok".
PatchCheck checkSqliteIntegrity(const QString& dbPath);

struct Y2jbFileResult {
    QString name;        // "param.json", "appinfo.db", "app.db"
    QString path;        // local or remote path
    QString status;      // "✓" verified, "✗" verification failed / 0 rows, "○" not found, "!" error
    QString detail;
    QString backup;      // local backup path (".bak" for local patching)
};

struct Y2jbReport {
    std::vector<Y2jbFileResult> files;
    QStringList verifyFailures; // "✗": written but not confirmed (the original is put back), or 0 rows
    QStringList errors;         // "!": the file could not be patched (nothing of ours is left on the console)
    bool allVerified() const { return verifyFailures.isEmpty(); }
    bool ok() const { return verifyFailures.isEmpty() && errors.isEmpty(); }
    QString summary() const;
};

// "Patch Local Files": a folder holding param.json / appinfo.db / app.db.
// A .bak copy is written next to each file before it changes.
Result<Y2jbReport> patchLocalFolder(const QString& folder, Y2jbRegion region);

// "Auto-Block over FTP" (the legacy confirm text says to stay in the PS5
// Settings menu while it runs).
Result<Y2jbReport> autoBlockOverFtp(FtpClient& client, Y2jbRegion region, const QString& backupDir,
                                    const CancellationToken& token = {}, const Log& log = Log(QStringLiteral("ps5.y2jb")));

// ---- file placement ------------------------------------------------------------------
QString y2jbDownload0Path(Y2jbRegion region); // /user/download/<TID>/download0.dat
Status placeDownload0Dat(FtpClient& client, Y2jbRegion region, const QString& localFile, const QString& backupDir,
                         const CancellationToken& token = {});

// The legacy check: PNG signature and an IHDR of exactly 512x512.
Status validateY2jbIcon(const QString& pngPath);
// STOR /user/appmeta/<TID>/icon0.png per region; returns the remote paths.
Result<QStringList> uploadY2jbIcon(FtpClient& client, const QList<Y2jbRegion>& regions, const QString& pngPath,
                                   const QString& backupDir, const CancellationToken& token = {});

struct ArtSetReport {
    int uploaded = 0;
    int failed = 0;
    QStringList missing; // art names not in the local folder (skipped)
    QStringList lines;   // "✓ pic0.png → /user/appmeta/PPSA01650/pic0.png (1234 bytes)"
    QStringList backups;
};
// /user/appmeta/<TID>/ <- pic0.png pic0.dds pic1.dds pic2.dds icon0.png;
// /user/catalog_downloader/appmeta/<TID>/ <- icon0.png pic0.png. Overwrites
// only, never deletes; missing local files are skipped.
Result<ArtSetReport> uploadY2jbArtSet(FtpClient& client, const QList<Y2jbRegion>& regions, const QString& folder,
                                      const QString& backupDir, const CancellationToken& token = {});

} // namespace fw::ps5
