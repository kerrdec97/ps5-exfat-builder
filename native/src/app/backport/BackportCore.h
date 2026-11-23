#pragma once

// The parts of the PS5 Backport GUI 0.3.4 that ran in its window process and
// therefore run natively here (the engine work runs in the bundled worker,
// see BackportHost.h): game discovery and metadata, the already-backported
// check, the applied-backports ledger reads, the per-user settings file, the
// pre-run destination / free-space checks, the AMPR / PlayGo variant list
// and user-library checks, and the support-logs ZIP. Rules and texts follow
// the original exactly (scanner.py, gui.py, ledger.py, settings.py,
// fakelib_variants.py, support.py).

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace fw::bp {

// ---- games ----------------------------------------------------------------------------

struct Game {
    QString path;
    QString key;          // normalised path (case-folded, resolved)
    QString titleId, title, version, sdk, fw;
    QString icon;         // sce_sys/icon0.png when present
    bool alreadyBackported = false;
    QJsonObject appliedRecord; // ledger row when this program applied a backport here
    QStringList notes;
    QString error;
};

QString normKey(const QString& path);

// Folders with a FILE eboot.bin directly inside, up to `depth` levels below
// each root (root = 0); skips system folders, `$…`, and `excludes` subtrees.
// Sorted by lower-case path, duplicates (by normKey) removed.
QStringList discoverGames(const QStringList& roots, int depth, const QStringList& excludes,
                          const std::atomic_bool* cancel = nullptr);

Game readGame(const QString& folder);
// The same with the ledger already read (readLedger()): a scan reads it once,
// not once per game.
Game readGame(const QString& folder, const QJsonArray& ledger);

// "N.NN" with 1 <= N <= 30.
bool plausibleVersion(const QString& v);
// Title/version recovered from a folder name ("PPSA01234 Game Name 01.000.000").
std::pair<QString, QString> nameFromFolder(const QString& name);

// Size of what the engine reads: ELF/SELF executables + param.json.
qint64 executableBytes(const QString& folder, const std::atomic_bool* cancel = nullptr);

// ---- ledger (applied_backports.json, written by the worker) -----------------------------

// The states of a record or journal. PS5 Backport GUI 0.3.5 adds
// ROLLBACK_INCOMPLETE (an undo that could not finish) and RESTORING (a restore
// cut off half way); a folder in either still holds backport files.
bool isActiveLedgerState(const QString& state);     // APPLIED or unfinished
bool isUnfinishedLedgerState(const QString& state); // IN_PROGRESS, ROLLBACK_INCOMPLETE, RESTORING

QJsonArray readLedger(); // the "applied" rows that are objects
// The newest active record for this folder, {} when none. Its "state" is the
// readable journal's (the journal has the last word); a journal that cannot
// be read keeps the folder flagged by the record; an undo that left none of
// the backport's files (0.3.5: backport_files_left false) does not count.
QJsonObject activeLedgerRecord(const QString& gameDir);
QJsonObject activeLedgerRecord(const QString& gameDir, const QJsonArray& ledger);

// Ledger rows whose apply or restore never finished (an active record and a
// readable journal in an unfinished state, for a game folder that still
// exists) and whose journal has not been written for `quietSeconds`: a
// journal that is still being written belongs to an apply that is running,
// not interrupted. Each row's "state" is the journal's, and "written_by_035"
// says the journal carries keys only PS5 Backport GUI 0.3.5 writes.
inline constexpr int kApplyQuietSeconds = 5 * 60;
std::vector<QJsonObject> unfinishedApplies(int quietSeconds);
// The applies to offer a restore for: unfinishedApplies(kApplyQuietSeconds),
// and nothing at all while a PS5 Backport GUI that this app did not start is
// running (it shares the ledger and may be the one applying).
std::vector<QJsonObject> interruptedApplies();

// ---- settings.json (shared with the standalone program) ---------------------------------

struct Settings {
    QStringList scanRoots;
    QString outputDir;
    QString backupDir;
    int scanDepth = 6;
};
// What loadSettings() found: no file (defaults), a readable file, or a file
// that exists but cannot be opened or parsed (in use, cut short, not JSON).
enum class SettingsFile { Missing, Ok, Unreadable };
Settings loadSettings(SettingsFile* state = nullptr, QString* error = nullptr);

enum SettingsKey { ScanRootsKey = 1, OutputDirKey = 2, BackupDirKey = 4, ScanDepthKey = 8, AllSettingsKeys = 15 };
// Re-reads settings.json and changes only `keys` (SettingsKey flags) in it;
// every other key, known or not, stays as the file has it. Folders are written
// with native separators, as the standalone program writes them. An Unreadable
// file is never replaced: false, with the reason in `error`.
bool saveSettings(const Settings& s, int keys, QString* error = nullptr);
bool saveSettings(const Settings& s); // all four keys
// Cleaned, with native separators (empty stays empty).
QString nativeFolder(const QString& path);

// ---- preflight (gui.App.preflight) ------------------------------------------------------

struct Preflight {
    QStringList errors;
    QStringList warnings;
};
Preflight preflight(const QString& outputDir, const QString& backupDir, const QStringList& gameDirs,
                    const std::atomic_bool* cancel = nullptr);
// Tests only: the free bytes preflight() sees for a folder (an empty function restores the real probe).
void setFreeSpaceProbeForTests(std::function<qint64(const QString&)> probe);

// What preflight() asks of the output and the backup folder for games whose
// executables hold `executableBytes` (0.3.5 rules: output 2.2x + 50 MB,
// backup 1.6x + 20 MB). On one drive the two are checked together as well.
struct SpaceNeed {
    qint64 output = 0, backup = 0;
};
SpaceNeed spaceNeed(qint64 executableBytes);

// The highest Target SDK a game with its own SDK `gameSdk` ("7.00") can be
// built for, when `target` is above it; nullopt when the build is allowed or
// the game's SDK is not known (the worker's own refusal rule, said before the
// run). Target SDK 4 is always allowed.
std::optional<int> sdkCeiling(const QString& gameSdk, int target);

// ---- AMPR / PlayGo ----------------------------------------------------------------------

struct Variant {
    QString id, kind, label, provenance, source, licence;
    qint64 size = 0;
};
const std::vector<Variant>& variants(); // manifest order
QString shortLabel(const QString& choiceLabel, bool userFile, const QString& sha8);

struct UserLibrary {
    bool ok = false;
    QString error;   // user-facing refusal
    QString label;   // "your file x.sprx (1234abcd)" or the bundled label
    QString sha8;
};
// kind: "ampr" | "playgo"
UserLibrary checkUserLibrary(const QString& path, const QString& kind);

// ---- support logs ZIP (support.make_zip) ------------------------------------------------

// Writes <Desktop>\PS5BackportGUI_logs_YYYYmmdd_HHMMSS.zip. Returns the path
// or an error message (prefixed "!").
QString makeSupportZip(const QString& note, const QString& engineRevision, const QStringList& hostLogFiles);

// ---- activity log file -------------------------------------------------------------------

void appendAppLog(const QString& level, const QString& message);

} // namespace fw::bp
