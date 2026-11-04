#pragma once

#include "jobs/Module.h"

#include <QString>

namespace fw::ps5 {

// Registers the "PS5 connection" settings and the job kinds below.
void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kConsoleIp = QStringLiteral("ps5.consoleIp");               // legacy ftp_ip / ps5_ip
inline const QString kFtpPort = QStringLiteral("ps5.ftpPort");                   // legacy ftp_port (2121)
inline const QString kGamesFolder = QStringLiteral("ps5.gamesFolder");           // legacy ftp_path
inline const QString kPayloadPort = QStringLiteral("ps5.payloadPort");           // legacy pl_port (9090)
inline const QString kShadowMountPort = QStringLiteral("ps5.shadowMountPort");   // legacy smp_payload_port (9021)
inline const QString kMicroMountPort = QStringLiteral("ps5.microMountPort");     // legacy mm_payload_port (9021)
inline const QString kKlogPort = QStringLiteral("ps5.klogPort");                 // legacy klog_port (3232)
inline const QString kDpiPort = QStringLiteral("ps5.dpiPort");                   // legacy y2jb_dpi_port (12800)
inline const QString kConnectTimeout = QStringLiteral("ps5.connectTimeoutSeconds");
inline const QString kIdleTimeout = QStringLiteral("ps5.idleTimeoutSeconds");
inline const QString kParallelTransfers = QStringLiteral("ps5.parallelTransfers");
inline const QString kPassiveMode = QStringLiteral("ps5.passiveMode");
} // namespace keys

namespace jobkinds {
inline const QString kUpload = QStringLiteral("ps5.ftp.upload");
inline const QString kDownload = QStringLiteral("ps5.ftp.download");
inline const QString kPayloadSend = QStringLiteral("ps5.payload.send");
inline const QString kDpiInstall = QStringLiteral("ps5.dpi.install");
inline const QString kY2jbAutoBlock = QStringLiteral("ps5.y2jb.autoBlock");
inline const QString kShadowMountStop = QStringLiteral("ps5.shadowmount.stop");
// {host?, port?, folder?, speedTest: bool, speedTestBytes? (tests)} → details = FtpDiagnosis::toJson()
inline const QString kFtpDiagnose = QStringLiteral("ps5.ftp.diagnose");
} // namespace jobkinds

} // namespace fw::ps5
