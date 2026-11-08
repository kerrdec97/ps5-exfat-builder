#pragma once

// ShadowMount+ (/data/shadowmount/config.ini + profiles.ini) and MicroMount
// (/data/micromount/config.ini, archived upstream) configuration: the key
// schema, parse, in-place edit, render, validation, load/push over FTP and
// the ShadowMount+ "Safe to unplug" handshake.
//
// ShadowMount+ config formats (drakmor/ShadowMountPlus, research §1):
//   * 1.6 flat     — key=value lines; the six kstuff_* keys exist.
//   * 1.7 flat     — 1.7beta11: same layout, kstuff_* / legacy_mount_* are
//                    gone ("[CFG] unknown key"), many new keys, new defaults
//                    (scan_interval_seconds 15, lvd_pfs/md_ufs sector 4096).
//   * v2 sectioned — branch 1.7 after beta11: "[config] version = 2" is
//                    required, every key moves to a section ([general] debug,
//                    [scan] interval_seconds ...), per-title and per-image
//                    rules move to /data/shadowmount/profiles.ini, and
//                    [smb:NAME] connections live in config.ini.
//
// Values are a flat key -> text map whatever the format: v2 keys are mapped to
// their flat names (sm_config_keys[]), v2 booleans come back as 1/0 and
// "mode = ro|rw" as mount_read_only 1/0. Repeatable keys (scanpath and the
// rule keys fakelib_mode, fakelib_exclude, global_fakelib_exclude, image_ro,
// image_rw, image_sector) are newline-joined text. In v2 the rule keys are
// read from and written to profiles.ini. Unknown v2 keys appear as
// "section.key".
//
// Pushing never re-renders a loaded file: applyMountConfigChanges() edits only
// the keys the user changed and keeps every other line, comment, unknown key
// and [smb:*] section (IniDocument.h). A file created from nothing contains
// only the keys that differ from the upstream defaults, so later upstream
// default changes still apply.

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "core/Cancellation.h"
#include "core/Result.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace fw::ps5 {

enum class MountTool { ShadowMountPlus, MicroMount };

using MountConfigValues = QMap<QString, QString>;

QString mountToolName(MountTool tool);         // "ShadowMountPlus", "MicroMount"
QString mountRemoteDir(MountTool tool);        // /data/shadowmount, /data/micromount
QString mountConfigRemotePath(MountTool tool); // <dir>/config.ini
QString mountDebugLogRemotePath(MountTool tool); // <dir>/debug.log
QString mountPreviousDebugLogRemotePath(MountTool tool); // <dir>/debug.log.1 (the previous run)
QString smpProfilesRemotePath();               // /data/shadowmount/profiles.ini

// ---- ShadowMount+ formats and versions ---------------------------------------------------

enum class SmpConfigFormat { Flat16, Flat17, V2 };
QString smpConfigFormatName(SmpConfigFormat format); // "1.6 (flat)", "1.7 (flat)", "v2 (sections)"

// True when the text has a [config] section with version = 2.
bool isSmpV2Config(const QString& configText);
// "1.7beta11" from the last "ShadowMount+ v1.7beta11 exFAT/UFS/PFS/LVD/MD." banner; empty when none.
QString smpVersionFromLog(const QString& debugLog);
// Numeric compare of "1.6test15-fix2", "1.7beta11", "1.7" (major, minor, then
// the trailing number; a release sorts after its betas). <0, 0, >0.
int compareSmpVersions(const QString& a, const QString& b);
bool smpVersionAtLeast(const QString& version, const QString& minimum);
// Research §1.3: v2 when the file says so; else 1.7 flat when the running
// version (API or debug.log banner) is >= 1.7; else 1.6 flat.
SmpConfigFormat detectSmpConfigFormat(const QString& configText, const QString& smpVersion);

// ---- the key schema (ShadowMount+ 1.7beta11 config.ini.example + parser) -------------

enum class SmpKeyType { Bool, Int, Enum, PowerOfTwo, Path, Url, Text, Address, Temperature, PathList, TitleIdList, FakelibRules, ImageNames, ImageSectors };

struct SmpKey {
    QString key;          // flat name ("scan_interval_seconds")
    QString section;      // v2 section ("scan"); empty = profiles.ini rule or flat only
    QString v2Key;        // v2 key ("interval_seconds")
    SmpKeyType type = SmpKeyType::Text;
    QString defaultValue; // upstream default as flat text; empty = unset
    qint64 minimum = 0, maximum = 0;
    QStringList choices;  // Enum
    QStringList aliases;  // older names the parser also accepts
    QString group;        // UI group: General, Scanning, Mounting, Library, Fakelib, Emulators, Fan, API, Sectors, Rules
    QString label;        // sentence case
    QString help;         // one short sentence
    bool repeatable = false;
    bool advanced = false;
    bool v2Only = false;  // only in the v2 format
    QString since;        // "1.7" for keys a 1.6 build does not know
    int maxItems = 0;     // repeatable: upstream limit (0 = none)
};
const std::vector<SmpKey>& smpKeys();
const SmpKey* findSmpKey(const QString& keyOrAlias);  // by flat name or alias, any case
QString canonicalSmpKey(const QString& keyOrAlias);    // flat name, or the input when unknown
// kstuff_* and legacy_mount_*: removed in 1.7 (logged as unknown keys).
bool isObsoleteSmpKey(const QString& key);
// The rule keys that live in profiles.ini in v2.
bool isSmpProfileKey(const QString& key);

// The defaults the editor starts from (ShadowMount+: the 1.7 upstream defaults;
// MicroMount: its config.ini.example).
MountConfigValues mountConfigDefaults(MountTool tool);
// Every key the editor offers, sorted.
QStringList mountConfigKeys(MountTool tool);

// Parses config.ini (flat or v2, detected) and, for v2, profiles.ini.
MountConfigValues parseMountConfig(MountTool tool, const QString& configText, const QString& profilesText = {});

// A config written from nothing: a short header and only the keys whose value
// differs from the default (never the obsolete kstuff_* keys). V2 adds
// "[config] version = 2" and the sections.
QString renderMountConfig(MountTool tool, const MountConfigValues& values,
                          SmpConfigFormat format = SmpConfigFormat::Flat17);
// The profiles.ini for the rule keys of `values` (v2), from nothing.
QString renderSmpProfiles(const MountConfigValues& values);

// The keys whose value differs between the two maps; a key that disappeared
// maps to "" (= remove it from the file, back to the default).
MountConfigValues changedMountValues(const MountConfigValues& before, const MountConfigValues& after);

struct MountConfigFiles {
    QString config;
    QString profiles;
    bool configChanged = false;
    bool profilesChanged = false;
};
// Applies `changes` (key -> new value, "" = remove) to the files as they were
// loaded ("" = the file does not exist yet). Every line not touched by a
// change is kept. A changed key that is missing is appended to its section; a
// key whose new value equals the default and that is not in the file is not
// written. On 1.7 flat and v2 the obsolete kstuff_* / legacy_mount_* lines are
// removed. V2 rule keys go to profiles.ini.
MountConfigFiles applyMountConfigChanges(MountTool tool, const QString& config, const QString& profiles,
                                         const MountConfigValues& changes, SmpConfigFormat format);

// Problems that make SMP reject or misread the values; empty when they may be
// pushed. Ranges, enums, power-of-two sector sizes, title-ID shapes and the
// upstream rule limits (research §1.3).
// `flatFormat`: the console's config is the 1.6/1.7 flat file, whose reader
// ends an unquoted value at the first '#' or ';' (so such values are refused).
QStringList validateMountConfig(MountTool tool, const MountConfigValues& values, bool flatFormat = false);
// Things that are allowed but probably wrong (debug off breaks Safe to
// unplug, an exFAT sector size other than 512, ...).
QStringList mountConfigWarnings(MountTool tool, const MountConfigValues& values);

// ---- FTP ---------------------------------------------------------------------------------

// RETR of config.ini; NotFound when the tool has never written one (normal
// on first use).
Result<QString> loadMountConfig(FtpClient& client, MountTool tool, const CancellationToken& token = {});
Result<QString> fetchMountDebugLog(FtpClient& client, MountTool tool, const CancellationToken& token = {});

// Everything the ShadowMount+ editor reads in one go.
struct SmpRemoteState {
    QString config;            // "" when missing
    bool configExists = false;
    QString profiles;          // "" when missing
    bool profilesExists = false;
    QString logVersion;        // from the debug.log banner ("" when unknown)
    SmpConfigFormat format = SmpConfigFormat::Flat16;
};
// config.ini, profiles.ini and the debug.log banner (only when debug.log is
// at most `maxLogBytes`; the banner is its first line). `apiVersion` (from
// the HTTP API, may be empty) wins over the banner for the format.
Result<SmpRemoteState> loadSmpState(FtpClient& client, const QString& apiVersion = {}, const CancellationToken& token = {},
                                    qint64 maxLogBytes = 2 * 1024 * 1024);

struct MountPushResult {
    QString backupPath; // local copy of the config that was replaced (empty if none existed)
    bool swapped = false; // written via ".new" + rename (false: direct STOR fallback)
};
// Backs up the current remote file locally, then MKD <dir>, STOR
// <file>.new and swap it in (the old file is restored if the swap fails); a
// server that cannot rename gets a direct STOR. With `expectedRemote`, the
// file is downloaded first and the push is refused (AlreadyExists) when it
// changed since it was loaded ("" = it did not exist): the console's web UI
// or API may have written it meanwhile.
Result<MountPushResult> pushMountConfig(FtpClient& client, MountTool tool, const QString& text, const QString& backupDir,
                                        const CancellationToken& token = {},
                                        const std::optional<QString>& expectedRemote = std::nullopt);
Result<MountPushResult> pushSmpProfiles(FtpClient& client, const QString& text, const QString& backupDir,
                                        const CancellationToken& token = {},
                                        const std::optional<QString>& expectedRemote = std::nullopt);

// ---- ShadowMount+ "Safe to unplug" ----------------------------------------------------

// Feeds klog (or debug.log) lines after the STOP file was written.
//
// ShadowMount+ 1.7 (src/main.c @1.7beta11): the ack is "[SHUTDOWN] requested
// by STOP file ..." or "[SHUTDOWN] stop requested ..."; teardown problems are
// "[SHUTDOWN] some image mounts or devices were not fully released",
// "[SHUTDOWN] image teardown skipped while title layers remain",
// "[KSTUFF] keeping automount disabled after incomplete teardown" and the
// per-image "filesystem release pending" / "... device detach pending" /
// "... mount directory remains"; and "[SHUTDOWN] cleanup complete for ..."
// is logged last in every case. So: cleanup complete after the ack and no
// problem line = confirmed; with a problem line = incomplete (amber, with the
// lines as the reason). 1.7 mounts images on demand, so zero unmounts is
// normal.
//
// Older builds (fallback, kept): "shadowmountplus.elf calls exit" confirms at
// once; otherwise every "unmount start" after the ack must have reached
// "unmount complete" and no new "unmount start" may appear for `settleMs`.
// An ack alone is never enough. Times are milliseconds on any monotonic clock.
class StopConfirmTracker {
public:
    static constexpr int kSettleMs = 6000;

    explicit StopConfirmTracker(int settleMs = kSettleMs);
    bool feed(const QString& line, qint64 nowMs); // true once finished (confirmed or incomplete)
    bool poll(qint64 nowMs);                      // re-checks the settle time without a line
    bool feed(const QString& line);               // same, on the tracker's own clock
    bool poll();
    bool confirmed() const { return m_confirmed; }
    bool incomplete() const { return m_incomplete; }   // cleanup ended with problem lines
    bool finished() const { return m_confirmed || m_incomplete; }
    bool sawAck() const { return m_ack; }
    bool sawExit() const { return m_exit; }
    bool sawCleanup() const { return m_cleanup; }
    QStringList problems() const { return m_problems; } // problem lines after the ack
    int unmountsStarted() const { return m_started; }
    int unmountsCompleted() const { return m_completed; }
    int settleMs() const { return m_settleMs; }

private:
    int m_settleMs;
    qint64 m_clockStart;
    bool m_confirmed = false;
    bool m_incomplete = false;
    bool m_ack = false;
    bool m_exit = false;
    bool m_cleanup = false;
    QStringList m_problems;
    int m_started = 0;
    int m_completed = 0;
    qint64 m_balancedSince = -1; // when the counts last became equal (-1: not equal)
};

enum class StopOutcome {
    Confirmed,  // green: safe to unplug
    NoKlog,     // amber: STOP sent, neither klog nor debug.log could be followed
    KlogLost,   // amber: klog dropped while waiting
    Timeout,    // amber: no confirmation in time
    Incomplete, // amber: ShadowMount+ reported mounts it could not release
};
QString stopOutcomeMessage(StopOutcome outcome);

struct StopResult {
    StopOutcome outcome = StopOutcome::Timeout;
    int unmountsStarted = 0;
    int unmountsCompleted = 0;
    QStringList klogLines;
    QStringList problems;   // Incomplete: the lines ShadowMount+ logged
    QString source;         // "klog" or "debug.log": where the lines came from
};

// Connects a private klog socket first (5 s), writes the empty
// /data/shadowmount/STOP sentinel over FTP, then watches klog up to 45 s
// (see StopConfirmTracker; `settleMs` is its quiet period). When klog cannot
// be reached, the tail of /data/shadowmount/debug.log (same log_debug sink)
// is polled over FTP every `logPollMs` instead.
Result<StopResult> stopShadowMount(const ConsoleEndpoint& endpoint, const CancellationToken& token = {},
                                   const std::function<void(const QString&)>& status = {},
                                   int confirmTimeoutMs = 45000, int klogConnectTimeoutMs = 5000,
                                   int settleMs = StopConfirmTracker::kSettleMs, int logPollMs = 1500);

} // namespace fw::ps5
