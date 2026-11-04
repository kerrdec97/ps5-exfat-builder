#pragma once

// Update checker for the native Farewell Edition.
//
// Looks at the repository's latest GitHub release for a native installer
// (exFAT-Image-Builder-Farewell-<x.y.z>-win64-setup.exe) and its SHA256SUMS.txt.
// Downloading verifies size and SHA-256 and fails closed when the checksum is
// missing. Installing is the UI's job (launch the verified installer, quit).
// Releases of the legacy Python app are ignored.

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
    QString version;     // "5.0.1"
    QString tag;         // "v5.0.1"
    QString name;
    QString notes;       // markdown body
    QString pageUrl;     // html_url
    QString installerName;
    QString installerUrl;
    qint64 installerSize = 0;
    QString sumsUrl;     // SHA256SUMS.txt
    bool newerThanRunning = false;

    QJsonObject toJson() const;
    static ReleaseInfo fromJson(const QJsonObject& o);
};

// Compares dotted versions numerically ("5.10.0" > "5.9.3"). <0, 0, >0.
int compareVersions(const QString& a, const QString& b);

// Picks the native installer from a GitHub "latest release" JSON document.
Result<ReleaseInfo> parseRelease(const QByteArray& githubJson, const QString& runningVersion);

// Finds `fileName`'s hash in a SHA256SUMS.txt body ("<hex>  <name>" lines).
QString findChecksum(const QByteArray& sums, const QString& fileName);

// Job kinds:
//   update.check     params {apiUrl?}                 → outcome.details = ReleaseInfo JSON (+ "upToDate")
//   update.download  params {release: ReleaseInfo JSON} → outcome.outputs = [verified installer path]
inline const QString kCheckJob = QStringLiteral("update.check");
inline const QString kDownloadJob = QStringLiteral("update.download");

} // namespace fw::update
