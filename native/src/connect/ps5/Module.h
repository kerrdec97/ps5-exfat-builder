#pragma once

#include "jobs/Module.h"

#include <QString>

namespace fw {
class SettingsStore;
}

namespace fw::ps5 {

// Registers the "PS5 connection" settings and the job kinds below.
void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kConsoleIp = QStringLiteral("ps5.consoleIp");               // legacy ftp_ip / ps5_ip
inline const QString kFtpPort = QStringLiteral("ps5.ftpPort");                   // legacy ftp_port (2121)
inline const QString kGamesFolder = QStringLiteral("ps5.gamesFolder");           // legacy ftp_path
inline const QString kPayloadPort = QStringLiteral("ps5.payloadPort");           // legacy pl_port (was 9090; now 9021)
inline const QString kShadowMountPort = QStringLiteral("ps5.shadowMountPort");   // legacy smp_payload_port (9021)
inline const QString kMicroMountPort = QStringLiteral("ps5.microMountPort");     // legacy mm_payload_port (9021)
inline const QString kKlogPort = QStringLiteral("ps5.klogPort");                 // legacy klog_port (3232)
inline const QString kDpiPort = QStringLiteral("ps5.dpiPort");                   // legacy y2jb_dpi_port (12800)
inline const QString kSmpApiPort = QStringLiteral("ps5.shadowMountApiPort");     // ShadowMount+ 1.7 HTTP API (10101)
inline const QString kConnectTimeout = QStringLiteral("ps5.connectTimeoutSeconds");
inline const QString kIdleTimeout = QStringLiteral("ps5.idleTimeoutSeconds");
inline const QString kParallelTransfers = QStringLiteral("ps5.parallelTransfers");
inline const QString kPassiveMode = QStringLiteral("ps5.passiveMode");
inline const QString kAutoloaderFolder = QStringLiteral("ps5.autoloaderFolder");   // legacy al_dir (local folder)
} // namespace keys

// Ports the console tools use (research §4-§7), for hints and presets.
namespace ports {
inline constexpr quint16 kFtpsrv = 2121;     // ps5-payload-dev ftpsrv
inline constexpr quint16 kEtaHenFtp = 1337;  // etaHEN's FTP server
inline constexpr quint16 kZftpd = 2120;      // zftpd >= 1.5.0
inline constexpr quint16 kElfLoader = 9021;  // elfldr (ps5-payload-dev, etaHEN 2.4B+)
inline constexpr quint16 kExploitLoader = 9020; // the exploit's built-in loader, before elfldr runs
inline constexpr quint16 kY2jbJs = 50000;    // Y2JB remote JS server (.js only)
inline constexpr quint16 kEtaHenDpiV1 = 9090; // etaHEN DPI v1: NOT a payload loader
inline constexpr quint16 kKlogsrv = 3232;
inline constexpr quint16 kEtaHenKlog = 9081; // off by default (Klog=1)
inline constexpr quint16 kDpiV2 = 12800;     // off by default (DPI_v2=1)
inline constexpr quint16 kSmpApi = 10101;
} // namespace ports

// One-time migration of the old payload-port default: a stored 9090 (etaHEN's
// DPI v1 service, which discards an ELF) becomes 9021 (elfldr). Runs once per
// data folder (`markerPath` records it), so a 9090 the user sets again later
// stays. Returns true when the setting was changed.
bool migrateLegacyPayloadPort(SettingsStore& settings, const QString& markerPath);

namespace jobkinds {
inline const QString kUpload = QStringLiteral("ps5.ftp.upload");
inline const QString kDownload = QStringLiteral("ps5.ftp.download");
inline const QString kPayloadSend = QStringLiteral("ps5.payload.send");
inline const QString kDpiInstall = QStringLiteral("ps5.dpi.install");
inline const QString kY2jbAutoBlock = QStringLiteral("ps5.y2jb.autoBlock");
inline const QString kShadowMountStop = QStringLiteral("ps5.shadowmount.stop");
// {host?, apiPort?, path? (mount point; default the only ejectable drive)} → ShadowMount+ 1.7 API eject
inline const QString kShadowMountEject = QStringLiteral("ps5.shadowmount.eject");
// {host?, port?, folder?, speedTest: bool, speedTestBytes? (tests)} → details = FtpDiagnosis::toJson()
inline const QString kFtpDiagnose = QStringLiteral("ps5.ftp.diagnose");
// {host?, port?, outputDir? (default <Desktop>/PS5 Logs), minutes? (0 = until stopped), root?, settleMs? (tests)}
// → details {crashes: [{folder, localDir, files, core, zip}], baseline, outputDir} (CrashDumps.h)
inline const QString kCrashDumps = QStringLiteral("ps5.crashdumps.capture");
} // namespace jobkinds

} // namespace fw::ps5
