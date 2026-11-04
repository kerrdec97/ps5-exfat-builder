#pragma once

// FTP connection diagnostics and the speed test (legacy _ftpdiag_run and
// _ftpspeed_run, LEGACY_UI_INVENTORY FTP-18), run by the job
// `ps5.ftp.diagnose`.
//
// Checks, in order (a check that cannot run leaves its cell "not run"):
//  1. Reachable — a plain TCP connect to the FTP port, timed (RTT);
//  2. Server banner — the 220 greeting of an FTP connection; then the login;
//  3. FEAT — the advertised extensions (FtpClient::features() rules);
//  4. Passive mode — PASV, and EPSV when PASV is refused;
//  5. Binary mode — TYPE I;
//  6. Writable — STOR of a 28-byte probe `.exfatbuilder_wtest_<epoch>.tmp`
//     in the chosen folder, then DELE; a probe that could not be removed is
//     reported (never silently left behind);
//  7. Speed test (optional) — STOR then RETR of 16 MiB of random data as
//     `.exfatbuilder_speedtest_<epoch>.bin` in the same folder, MB/s each way
//     (MiB per second, like the legacy), the read-back compared byte for byte,
//     then DELE; a leftover is reported. After a cancel or a dropped
//     connection the file is deleted over a fresh connection.
// The server is also classified (zftpd or generic) from FEAT and the banner
// with the legacy scores; the legacy ZHTTP web-UI probe is not ported.
//
// Blocking; run on a Qt-started thread (queue worker).

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace fw::ps5 {

inline constexpr qint64 kSpeedTestBytes = 16 * 1024 * 1024; // legacy _SPEEDTEST_BYTES

struct DiagnoseOptions {
    QString folder;            // remote folder for the probes; empty = the endpoint's games folder
    bool speedTest = false;
    qint64 speedTestBytes = kSpeedTestBytes;
    qint64 epochSeconds = 0;   // probe file names; 0 = now
};

struct ServerGuess {
    QString label = QStringLiteral("Unknown / generic FTP");
    QString confidence = QStringLiteral("n/a"); // "likely", "very likely"
    QStringList evidence;
};

// Legacy _ftp_detect_server without the ZHTTP probe: XCRYPT +2, server-side
// copy +2, MLSD+UTF8+REST +1, "zftpd" in the banner +3; ≥ 2 likely, ≥ 4 very likely.
ServerGuess classifyFtpServer(const FtpFeatures& features, const QString& banner);

struct SpeedTestResult {
    bool ran = false;
    qint64 bytes = 0;
    double uploadMBps = -1;    // MiB/s; < 0 = not measured
    double downloadMBps = -1;
    std::optional<bool> readBackMatches; // nullopt = not downloaded
    QString path;              // remote file used
    bool cleaned = true;       // false: `path` may remain on the console
    QString error;             // why the test did not complete
};

struct FtpDiagnosis {
    QString host;
    quint16 port = 0;
    QString folder;
    bool reachable = false;
    int rttMs = -1;
    bool greeted = false;
    QString banner;
    bool loggedIn = false;
    bool featOk = false;       // the server answered FEAT with 211
    FtpFeatures features;
    std::optional<bool> passive;   // nullopt = not checked
    QString passiveMode;           // "PASV" or "EPSV"
    std::optional<bool> binary;
    std::optional<bool> writable;
    bool probeCleaned = true;
    QString probePath;
    QString writableDetail;
    QString destination;           // classifyDestination(folder).label
    ServerGuess server;
    SpeedTestResult speed;
    QString error;                 // the problem that stopped the checks (empty when they all ran)
    QStringList notes;

    // The FTP server can be used at all: connected, greeted and logged in.
    bool usable() const { return reachable && greeted && loggedIn; }
    QJsonObject toJson() const;
    static FtpDiagnosis fromJson(const QJsonObject& o);
};

// What the diagnostics are doing, for the job's phases and current-item line
// ("Writing a probe file to /data/etaHEN/games", "Uploading 16.0 MiB", …).
enum class DiagnoseStage { Connecting, Checking, SpeedTest };
using DiagnoseStep = std::function<void(DiagnoseStage stage, const QString& text)>;

// Runs the checks against `endpoint` (host, FTP port, timeouts, passive
// setting). Problems are reported in the result, not as errors: the only
// errors are Cancelled and InvalidArgument (no host). `bytesMoved` reports
// the speed test's progress.
Result<FtpDiagnosis> diagnoseFtp(const ConsoleEndpoint& endpoint, const DiagnoseOptions& options,
                                 const CancellationToken& token = {}, const DiagnoseStep& step = {},
                                 const FtpProgress& bytesMoved = {});

} // namespace fw::ps5
