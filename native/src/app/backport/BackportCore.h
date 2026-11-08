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

// "N.NN" with 1 <= N <= 30.
bool plausibleVersion(const QString& v);
// Title/version recovered from a folder name ("PPSA01234 Game Name 01.000.000").
std::pair<QString, QString> nameFromFolder(const QString& name);

// Size of what the engine reads: ELF/SELF executables + param.json.
qint64 executableBytes(const QString& folder, const std::atomic_bool* cancel = nullptr);

// ---- ledger (applied_backports.json, written by the worker) -----------------------------

QJsonObject activeLedgerRecord(const QString& gameDir); // {} when none
std::vector<QJsonObject> interruptedApplies();

// ---- settings.json (shared with the standalone program) ---------------------------------

struct Settings {
    QStringList scanRoots;
    QString outputDir;
    QString backupDir;
    int scanDepth = 6;
};
Settings loadSettings();
bool saveSettings(const Settings& s); // keeps the other keys (klog_*, geometry)

// ---- preflight (gui.App.preflight) ------------------------------------------------------

struct Preflight {
    QStringList errors;
    QStringList warnings;
};
Preflight preflight(const QString& outputDir, const QString& backupDir, const QStringList& gameDirs,
                    const std::atomic_bool* cancel = nullptr);

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
