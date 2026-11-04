#pragma once

// Journaled Apply (docs/BACKPORT_PARITY.md §2.12, S35). The journal is
// written before the first game file changes:
// <dataDir>/backport/journals/<jobId>.json with every file to replace or add,
// its SHA-256 before and after, and where its original can be read back
// (the verified originals ZIP; the staging snapshot while it exists).
//
// Each file is copied to "<target>.fwpart", checked, then renamed over the
// target (MoveFileEx(REPLACE_EXISTING) on Windows). Rollback is decided by
// content, not by the journal's progress marks, so it is also correct after
// a crash between a rename and the next journal save: a file whose hash is
// the "after" hash is restored (replace) or deleted (add); a file that still
// has its "before" hash is left alone; any other content is reported and
// left as it is. An interrupted journal (state pending) is found at start-up
// by findInterrupted() so the user can be offered the rollback.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fw::backport {

enum class JournalState { Pending, Done, RolledBack, RollbackFailed };
QString journalStateName(JournalState s);

enum class ApplyAction { Replace, Add };

struct JournalEntry {
    QString rel;
    ApplyAction action = ApplyAction::Replace;
    QString sha256Before; // Replace only
    QString sha256After;
    qint64 sizeAfter = 0;
    QString stagedPath;   // the new content
    QString backupZipMember;   // arcname in the originals ZIP (Replace)
    QString backupStagingPath; // staging snapshot (may be gone after a crash)
    // #41 fix: after the bytes are in place the file is renamed to this rel
    // (same folder, the game's letter case); rollback renames it back first.
    QString renameTo;
    bool renamed = false;
    bool done = false;
};

struct PendingJournal {
    QString path;
    QString jobId;
    QString game;
    QString originalsZip;
    QString created;
    JournalState state = JournalState::Pending;
};

QString defaultJournalDir();

class ApplyJournal {
public:
    // Points where the caller can inject faults (tests): Continue, Fail
    // (rolled back like any error) or Crash (stops at once, journal pending).
    enum class HookAction { Continue, Fail, Crash };
    using Hook = std::function<HookAction(int index, const QString& rel)>;

    // Writes the journal (state pending) before anything changes.
    static Result<ApplyJournal> create(const QString& path, const QString& jobId, const QString& game,
                                       const QString& originalsZip, std::vector<JournalEntry> entries);
    static Result<ApplyJournal> load(const QString& path);

    // Applies every entry, then checks every applied file (V7). On error or
    // cancellation the game is rolled back and the error says so.
    Status apply(const CancellationToken& token, ProgressReporter* progress = nullptr, const Hook& hook = {});
    // Rolls back by content (see above). `problems` receives what could not
    // be put back.
    Status rollback(const CancellationToken& token, QStringList* problems = nullptr);

    JournalState state() const { return m_state; }
    QString path() const { return m_path; }
    QString jobId() const { return m_jobId; }
    QString game() const { return m_game; }
    QString originalsZip() const { return m_zip; }
    const std::vector<JournalEntry>& entries() const { return m_entries; }
    QStringList addedFiles() const;

    // Journals still pending (interrupted Apply) or whose rollback failed.
    static std::vector<PendingJournal> findInterrupted(const QString& dir = defaultJournalDir());
    // Removes finished journals (done / rolled back) older than `days`.
    static int prune(const QString& dir, int days);

private:
    Status save() const;
    Status restoreOriginal(const JournalEntry& e, const QString& target, const CancellationToken& token) const;

    QString m_path;
    QString m_jobId;
    QString m_game;
    QString m_zip;
    QString m_created;
    QString m_error;
    JournalState m_state = JournalState::Pending;
    std::vector<JournalEntry> m_entries;
    QStringList m_createdDirs;
};

} // namespace fw::backport
