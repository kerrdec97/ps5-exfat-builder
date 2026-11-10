#pragma once

// Update checker.
//
// Looks at the repository's latest GitHub release. The version is the release
// tag ("v5.0.2"); the download is the one-file program ("exFAT Image Builder.exe",
// which GitHub serves as "exFAT.Image.Builder.exe") and its hash comes from the
// release's SHA256SUMS.txt. A newer release without a usable download is still
// reported, with the release page to open instead.
// Downloading talks to GitHub's own hosts only, never fetches an older or equal
// version, checks size and SHA-256 of the file as it is on disk and fails closed
// when the checksum is missing. The verified file stays in the cache folder:
// this module never starts it and never replaces the running program.

#include "jobs/Module.h"

#include <QJsonObject>
#include <QString>

namespace fw::update {

void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kCheckOnStart = QStringLiteral("general.checkForUpdatesOnStart");
}

inline const QString kDefaultApiUrl = QStringLiteral("https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/latest");

struct ReleaseInfo {
    QString version;     // "5.0.1", from the tag
    QString tag;         // "v5.0.1"
    QString name;
    QString notes;       // markdown body
    QString pageUrl;     // html_url: the release page
    QString assetName;   // the download as GitHub lists it ("exFAT.Image.Builder.exe"); empty without one
    QString installerName; // same as assetName (older field name, still read by the UI)
    QString installerUrl;  // the download's address; empty when the release has none this app accepts
    qint64 installerSize = 0;
    QString sumsUrl;     // SHA256SUMS.txt
    bool newerThanRunning = false;

    // True when update.download can fetch and verify this release: a program
    // file and a checksum list, both at GitHub release addresses.
    bool canDownload() const;

    QJsonObject toJson() const;
    static ReleaseInfo fromJson(const QJsonObject& o);
};

// Compares dotted versions numerically ("5.10.0" > "5.9.3"). <0, 0, >0.
int compareVersions(const QString& a, const QString& b);

// Reads a GitHub "latest release" JSON document. Fails with NotFound for a
// draft, a pre-release or a tag that is not a version; a release without a
// usable download still parses (installerUrl stays empty).
Result<ReleaseInfo> parseRelease(const QByteArray& githubJson, const QString& runningVersion);

// Finds `fileName`'s hash in a SHA256SUMS.txt body ("<hex>  <name>" lines).
// Spaces and dots in names count as the same character, because GitHub turns
// the spaces of an uploaded file name into dots.
QString findChecksum(const QByteArray& sums, const QString& fileName);

// Tests only: also accept http://127.0.0.1 addresses, so a local server can
// stand in for GitHub. Off by default.
void setLoopbackAllowedForTests(bool allowed);

// Job kinds:
//   update.check     params {apiUrl?}                 → outcome.details = ReleaseInfo JSON
//                                                        (+ "upToDate", "canDownload", "note")
//   update.download  params {release: ReleaseInfo JSON} → outcome.outputs = [verified file in the cache folder]
inline const QString kCheckJob = QStringLiteral("update.check");
inline const QString kDownloadJob = QStringLiteral("update.download");

} // namespace fw::update
