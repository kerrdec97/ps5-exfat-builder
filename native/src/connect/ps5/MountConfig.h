#pragma once

// ShadowMount+ (/data/shadowmount/config.ini) and MicroMount
// (/data/micromount/config.ini) configuration, with the legacy parse and
// render rules (ui/tab_shadowmount.py _parse_ini/_render_ini,
// ui/tab_micromount.py), plus load/push over FTP and the ShadowMount+
// "Safe to unplug" handshake.
//
// Values are a flat key -> text map like the legacy form. List keys
// (ShadowMount+: kstuff_no_pause, kstuff_delay, scanpath,
// global_fakelib_exclude; MicroMount: scanpath) are newline-joined text and
// are written as one repeated key per line; ShadowMount+ image_ro / image_rw /
// image_sector lines are kept together as "per_image_rules".

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

namespace fw::ps5 {

enum class MountTool { ShadowMountPlus, MicroMount };

using MountConfigValues = QMap<QString, QString>;

QString mountToolName(MountTool tool);         // "ShadowMountPlus", "MicroMount"
QString mountRemoteDir(MountTool tool);        // /data/shadowmount, /data/micromount
QString mountConfigRemotePath(MountTool tool); // <dir>/config.ini
QString mountDebugLogRemotePath(MountTool tool); // <dir>/debug.log

// The documented defaults the legacy form started from.
MountConfigValues mountConfigDefaults(MountTool tool);
// Keys the legacy form showed (for "N of M settings present").
QStringList mountConfigKeys(MountTool tool);

MountConfigValues parseMountConfig(MountTool tool, const QString& text);
QString renderMountConfig(MountTool tool, const MountConfigValues& values);
// The legacy push checks; empty when the values may be pushed.
QStringList validateMountConfig(MountTool tool, const MountConfigValues& values);

// RETR of config.ini; NotFound when the tool has never written one (normal
// on first use).
Result<QString> loadMountConfig(FtpClient& client, MountTool tool, const CancellationToken& token = {});
Result<QString> fetchMountDebugLog(FtpClient& client, MountTool tool, const CancellationToken& token = {});

struct MountPushResult {
    QString backupPath; // local copy of the config that was replaced (empty if none existed)
    bool swapped = false; // written via ".new" + rename (false: direct STOR fallback)
};
// Backs up the current remote config locally, then MKD <dir>, STOR
// config.ini.new and swap it in (legacy ShadowMount+ pattern; the old file is
// restored if the swap fails); a server that cannot rename gets a direct STOR.
Result<MountPushResult> pushMountConfig(FtpClient& client, MountTool tool, const QString& text, const QString& backupDir,
                                        const CancellationToken& token = {});

// ---- ShadowMount+ "Safe to unplug" ----------------------------------------------------

// Feeds klog lines. Confirmed at once by "shadowmountplus.elf calls exit"
// (the process ends only after every mount is released). Otherwise, after the
// "[shutdown] stop requested" ack, every "unmount start" (counted only after
// the ack) must have reached "unmount complete" AND no new "unmount start" may
// appear for `settleMs`: ShadowMount+ may unmount images one after another, so
// matching counts alone are not enough (the legacy app confirmed on the first
// match). Times are milliseconds on any monotonic clock.
class StopConfirmTracker {
public:
    static constexpr int kSettleMs = 6000;

    explicit StopConfirmTracker(int settleMs = kSettleMs);
    bool feed(const QString& line, qint64 nowMs); // true once confirmed
    bool poll(qint64 nowMs);                      // re-checks the settle time without a line
    bool feed(const QString& line);               // same, on the tracker's own clock
    bool poll();
    bool confirmed() const { return m_confirmed; }
    bool sawAck() const { return m_ack; }
    bool sawExit() const { return m_exit; }
    int unmountsStarted() const { return m_started; }
    int unmountsCompleted() const { return m_completed; }
    int settleMs() const { return m_settleMs; }

private:
    int m_settleMs;
    qint64 m_clockStart;
    bool m_confirmed = false;
    bool m_ack = false;
    bool m_exit = false;
    int m_started = 0;
    int m_completed = 0;
    qint64 m_balancedSince = -1; // when the counts last became equal (-1: not equal)
};

enum class StopOutcome {
    Confirmed, // green: safe to unplug
    NoKlog,    // amber: STOP sent, klog unreachable
    KlogLost,  // amber: klog dropped while waiting
    Timeout,   // amber: no confirmation in time
};
QString stopOutcomeMessage(StopOutcome outcome);

struct StopResult {
    StopOutcome outcome = StopOutcome::Timeout;
    int unmountsStarted = 0;
    int unmountsCompleted = 0;
    QStringList klogLines;
};

// Connects a private klog socket first (5 s), writes the empty
// /data/shadowmount/STOP sentinel over FTP, then watches klog up to 45 s
// (see StopConfirmTracker; `settleMs` is its quiet period).
Result<StopResult> stopShadowMount(const ConsoleEndpoint& endpoint, const CancellationToken& token = {},
                                   const std::function<void(const QString&)>& status = {},
                                   int confirmTimeoutMs = 45000, int klogConnectTimeoutMs = 5000,
                                   int settleMs = StopConfirmTracker::kSettleMs);

} // namespace fw::ps5
