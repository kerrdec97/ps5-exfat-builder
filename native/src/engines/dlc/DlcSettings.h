#pragma once

// Settings of the DLC page (the ppr-pkg Companion's DLC Converter) and the
// snapshot a dlc.convert job reads. The Companion took the engine options
// (Kraken level, PFS format, image mode, backend, SDK, deterministic) from
// ppr-pkg's Create tab; here they are DLC settings of their own.

#include "core/Settings.h"

#include <QString>

namespace fw::dlc {

namespace keys {
inline const QString kFpkgCliPath = QStringLiteral("dlc.fpkgCliPath");
inline const QString kOutputFolder = QStringLiteral("dlc.outputFolder");
inline const QString kEntitlementKey = QStringLiteral("dlc.entitlementKey");
inline const QString kSourcePasscode = QStringLiteral("dlc.sourcePasscode");
inline const QString kImageMode = QStringLiteral("dlc.imageMode");
inline const QString kCompressionLevel = QStringLiteral("dlc.compressionLevel");
inline const QString kBackend = QStringLiteral("dlc.krakenBackend");
inline const QString kPfsFormat = QStringLiteral("dlc.pfsFormat");
inline const QString kSdkOverride = QStringLiteral("dlc.sdkOverride");
inline const QString kDeterministic = QStringLiteral("dlc.deterministic");
inline const QString kFullVerify = QStringLiteral("dlc.fullVerify");
inline const QString kOnlineLookup = QStringLiteral("dlc.onlineLookup");
inline const QString kSaveReport = QStringLiteral("dlc.saveReport");
} // namespace keys

inline const QString kZeroKey = QString(32, QLatin1Char('0'));

// "" when acceptable, else why not (exactly 32 hexadecimal characters).
QString hex32Problem(const QString& value);

struct DlcSettings {
    QString fpkgCliPath;
    QString outputFolder;              // "" = "DLC packages" next to the first input
    QString entitlementKey = kZeroKey; // 32 hex
    QString sourcePasscode = kZeroKey; // 32 hex: extracting imported packages and the rebuilt package
    QString imageMode = QStringLiteral("plaintext"); // "plaintext" | "native"
    int compressionLevel = 7;          // Kraken -4..9
    QString backend = QStringLiteral("auto");        // auto | builtin | pubtools | none
    QString pfsFormat = QStringLiteral("v2");         // v2 | v3
    int sdkOverride = 0;               // 0 = none, else 1..11
    bool deterministic = true;
    bool fullVerify = true;
    bool onlineLookup = true;
    bool saveReport = true;
    int threads = 0;                   // effective workers (0 = every core)
    int priorityMode = 2;              // performance.mode
    qint64 freeSpaceMarginBytes = 512LL * 1024 * 1024;

    static DlcSettings from(const SettingsValues& values);
};

void registerDlcSettings(SettingsSchema& schema);

} // namespace fw::dlc
