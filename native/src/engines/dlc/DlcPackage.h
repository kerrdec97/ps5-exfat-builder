#pragma once

// DLC .pkg inspection for the DLC page (the ppr-pkg Companion's DlcPackage /
// Ps4Pkg). PS5 packages are read through "fpkg-cli pkg-info" (LibProsperoPkg);
// PS4 packages are read directly (header and param.sfo, inspection only).
//
// A package can be rebuilt for another target when it is a PS5 additional-
// content package with data, debug (not official/retail), with a readable
// param.json whose identity agrees with the header. Entitlement-only DLC (no
// data) and PS4 packages are shown but not rebuilt.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/dlc/FpkgCli.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace fw::dlc {

struct DlcPackageInfo {
    QString path;
    qint64 size = 0;
    QDateTime modifiedUtc;
    bool ps4 = false;
    QString type;             // "PS5 PKG (FIH Debug)", "PS4 PKG", ...
    QString imageMode;        // "plaintext" / "native" / ""
    QString contentId;
    QString titleId;
    QString title;
    QString version;          // contentVersion (PS4: VERSION)
    QString masterVersion;
    QString firmware;         // requiredSystemSoftwareVersion (PS4: SYSTEM_VER)
    QString drm;              // "Entitlement / additional content (CNT header 0x10)", ...
    int contentType = -1;     // CNT header content type (0x21 = AC with data)
    int headerDrm = -1;       // 0x10 entitlement, 0x0 free
    bool isDlc = false;
    bool entitlementOnly = false;
    bool official = false;
    bool hasParam = false;
    bool canRebuild = false;
    QString rebuildLimitation;
    QStringList warnings;
    std::vector<std::pair<QString, QString>> metadata; // param.json / param.sfo fields
};

// Reads either kind; errors when the file is not a package at all.
Result<DlcPackageInfo> inspectDlcPackage(const FpkgCli& cli, const QString& path, const QString& passcode,
                                         const CancellationToken& token);

// Parsing the "fpkg-cli pkg-info" text (exposed for tests).
DlcPackageInfo parsePkgInfoOutput(const QString& output);
// PS4: big-endian header, param.sfo (0x1000) and icon0.png (0x1200) entries.
Result<DlcPackageInfo> inspectPs4Package(const QString& path, QByteArray* icon = nullptr);
bool looksLikePs4Package(const QString& path); // 7F 'C' 'N' 'T' and a CUSA content ID

// Extracts the whole package into `destination` (empty) with fpkg-cli;
// returns the folder holding sce_sys. The Companion's checks: the source is
// unchanged since `info` was read and sce_sys/param.json exists afterwards.
Result<QString> extractDlcPackage(const FpkgCli& cli, const DlcPackageInfo& info, const QString& destination,
                                  const QString& passcode, const QString& tempDir, int priorityMode,
                                  const CancellationToken& token, const std::function<void(const QString&)>& onLine = {});

// The package's icon0.png (for the list), or empty.
QByteArray extractDlcIcon(const FpkgCli& cli, const QString& path, const QString& passcode, const QString& tempDir,
                          const CancellationToken& token);

} // namespace fw::dlc
