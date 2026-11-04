#include "engines/backport/ApplyJournal.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Zip.h"
#include "engines/backport/Common.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fw::backport {

QString journalStateName(JournalState s)
{
    switch (s) {
    case JournalState::Pending: return QStringLiteral("pending");
    case JournalState::Done: return QStringLiteral("done");
    case JournalState::RolledBack: return QStringLiteral("rolledBack");
    case JournalState::RollbackFailed: return QStringLiteral("rollbackFailed");
    }
    return QStringLiteral("pending");
}

static JournalState stateFromName(const QString& n)
{
    if (n == u"done")
        return JournalState::Done;
    if (n == u"rolledBack")
        return JournalState::RolledBack;
    if (n == u"rollbackFailed")
        return JournalState::RollbackFailed;
    return JournalState::Pending;
}

QString defaultJournalDir()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("backport/journals"));
}

QStringList ApplyJournal::addedFiles() const
{
    QStringList out;
    for (const JournalEntry& e : m_entries)
        if (e.action == ApplyAction::Add)
            out << e.rel;
    return out;
}

Status ApplyJournal::save() const
{
    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("job_id"), m_jobId);
    o.insert(QStringLiteral("game"), QDir::toNativeSeparators(m_game));
    o.insert(QStringLiteral("originals_zip"), QDir::toNativeSeparators(m_zip));
    o.insert(QStringLiteral("created"), m_created);
    o.insert(QStringLiteral("updated"), QDateTime::currentDateTime().toString(Qt::ISODate));
    o.insert(QStringLiteral("state"), journalStateName(m_state));
    o.insert(QStringLiteral("error"), m_error);
    o.insert(QStringLiteral("created_dirs"), QJsonArray::fromStringList(m_createdDirs));
    QJsonArray entries;
    for (const JournalEntry& e : m_entries) {
        QJsonObject j;
        j.insert(QStringLiteral("rel"), e.rel);
        j.insert(QStringLiteral("action"), e.action == ApplyAction::Add ? QStringLiteral("add") : QStringLiteral("replace"));
        j.insert(QStringLiteral("sha256_before"), e.sha256Before);
        j.insert(QStringLiteral("sha256_after"), e.sha256After);
        j.insert(QStringLiteral("size_after"), double(e.sizeAfter));
        j.insert(QStringLiteral("staged"), QDir::toNativeSeparators(e.stagedPath));
        j.insert(QStringLiteral("backup_zip_member"), e.backupZipMember);
        j.insert(QStringLiteral("backup_staging"), QDir::toNativeSeparators(e.backupStagingPath));
        if (!e.renameTo.isEmpty()) {
            j.insert(QStringLiteral("rename_to"), e.renameTo);
            j.insert(QStringLiteral("renamed"), e.renamed);
        }
        j.insert(QStringLiteral("done"), e.done);
        entries.append(j);
    }
    o.insert(QStringLiteral("entries"), entries);
    return writeFileAtomic(m_path, QJsonDocument(o).toJson(QJsonDocument::Indented));
}

Result<ApplyJournal> ApplyJournal::create(const QString& path, const QString& jobId, const QString& game,
                                          const QString& originalsZip, std::vector<JournalEntry> entries)
{
    ApplyJournal j;
    j.m_path = path;
    j.m_jobId = jobId;
    j.m_game = QDir::cleanPath(game);
    j.m_zip = originalsZip;
    j.m_created = QDateTime::currentDateTime().toString(Qt::ISODate);
    j.m_entries = std::move(entries);
    if (QFileInfo::exists(path))
        return makeError(Code::ApplyFailed, QStringLiteral("An apply journal %1 already exists.").arg(QDir::toNativeSeparators(path)));
    if (Status st = j.save(); !st.ok())
        return makeError(Code::ApplyFailed,
                         QStringLiteral("Could not write the apply journal, so nothing was applied: %1").arg(st.error().message),
                         st.error().detail);
    return j;
}

Result<ApplyJournal> ApplyJournal::load(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return makeError(Code::Unreadable, QStringLiteral("Could not read the apply journal %1.").arg(QDir::toNativeSeparators(path)),
                         f.errorString());
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return makeError(Code::Internal, QStringLiteral("The apply journal %1 is damaged.").arg(QDir::toNativeSeparators(path)));
    const QJsonObject o = doc.object();
    ApplyJournal j;
    j.m_path = path;
    j.m_jobId = o.value(QStringLiteral("job_id")).toString();
    j.m_game = QDir::fromNativeSeparators(o.value(QStringLiteral("game")).toString());
    j.m_zip = QDir::fromNativeSeparators(o.value(QStringLiteral("originals_zip")).toString());
    j.m_created = o.value(QStringLiteral("created")).toString();
    j.m_state = stateFromName(o.value(QStringLiteral("state")).toString());
    j.m_error = o.value(QStringLiteral("error")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("created_dirs")).toArray())
        j.m_createdDirs << v.toString();
    for (const QJsonValue& v : o.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject e = v.toObject();
        JournalEntry je;
        je.rel = e.value(QStringLiteral("rel")).toString();
        je.action = e.value(QStringLiteral("action")).toString() == u"add" ? ApplyAction::Add : ApplyAction::Replace;
        je.sha256Before = e.value(QStringLiteral("sha256_before")).toString();
        je.sha256After = e.value(QStringLiteral("sha256_after")).toString();
        je.sizeAfter = qint64(e.value(QStringLiteral("size_after")).toDouble());
        je.stagedPath = QDir::fromNativeSeparators(e.value(QStringLiteral("staged")).toString());
        je.backupZipMember = e.value(QStringLiteral("backup_zip_member")).toString();
        je.backupStagingPath = QDir::fromNativeSeparators(e.value(QStringLiteral("backup_staging")).toString());
        je.renameTo = e.value(QStringLiteral("rename_to")).toString();
        je.renamed = e.value(QStringLiteral("renamed")).toBool();
        je.done = e.value(QStringLiteral("done")).toBool();
        if (je.rel.isEmpty())
            return makeError(Code::Internal, QStringLiteral("The apply journal %1 is damaged.").arg(QDir::toNativeSeparators(path)));
        j.m_entries.push_back(je);
    }
    return j;
}

namespace {

QString hashOf(const QString& path)
{
    if (!QFileInfo(path).isFile())
        return {};
    auto h = sha256File(path, CancellationToken());
    return h.ok() ? h.value() : QString();
}

void removeTemp(const QString& tmp)
{
    QFile::remove(tmp);
    QFile::remove(tmp + QStringLiteral(".part"));
}

} // namespace

Status ApplyJournal::restoreOriginal(const JournalEntry& e, const QString& target, const CancellationToken& token) const
{
    const QString tmp = target + QStringLiteral(".fwpart");
    removeTemp(tmp);
    // 1. The staging snapshot, when it is still there and intact.
    if (!e.backupStagingPath.isEmpty() && hashOf(e.backupStagingPath) == e.sha256Before) {
        if (Status st = copyFileStreaming(e.backupStagingPath, tmp, token, nullptr, true, true); st.ok()) {
            if (hashOf(tmp) == e.sha256Before)
                return replaceFileAtomic(tmp, target);
        }
        removeTemp(tmp);
    }
    // 2. The verified originals ZIP.
    zip::Reader r;
    if (Status st = r.open(m_zip); !st.ok())
        return makeError(Code::RollbackFailed,
                         QStringLiteral("%1: the originals ZIP %2 cannot be read.").arg(e.rel, QDir::toNativeSeparators(m_zip)),
                         st.error().detail);
    const zip::Entry* ze = r.find(e.backupZipMember.isEmpty() ? e.rel : e.backupZipMember);
    if (!ze)
        return makeError(Code::RollbackFailed, QStringLiteral("%1 is not in the originals ZIP.").arg(e.rel));
    {
        QFile out(toLongPath(tmp));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return makeError(Code::RollbackFailed, QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(tmp)),
                             out.errorString());
        QCryptographicHash h(QCryptographicHash::Sha256);
        Status st = r.read(
            *ze,
            [&](const QByteArray& chunk) -> Status {
                h.addData(chunk);
                if (out.write(chunk) != chunk.size())
                    return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(tmp)));
                return success();
            },
            token);
        if (st.ok())
            st = flushToDisk(out, QFileInfo(target).fileName()); // the original survives a power cut
        out.close();
        if (!st.ok() || QString::fromLatin1(h.result().toHex()) != e.sha256Before) {
            removeTemp(tmp);
            return makeError(Code::RollbackFailed,
                             QStringLiteral("%1 could not be read back from the originals ZIP.").arg(e.rel),
                             st.ok() ? QStringLiteral("hash mismatch") : st.error().toLogString());
        }
    }
    return replaceFileAtomic(tmp, target);
}

Status ApplyJournal::rollback(const CancellationToken& token, QStringList* problemsOut)
{
    QStringList problems;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        JournalEntry& e = *it;
        const QString target = joinPath(m_game, e.rel);
        removeTemp(target + QStringLiteral(".fwpart"));
        // #41 fix: a module renamed to the game's spelling gets the dump's
        // spelling back before its bytes are judged (decided by the folder
        // listing, so a crash before or after the rename is handled alike).
        if (!e.renameTo.isEmpty()) {
            const QString dir = QFileInfo(target).absolutePath();
            const QString was = e.rel.section(u'/', -1), now = e.renameTo.section(u'/', -1);
            // Only a file this journal renamed: the dump's name is free and
            // the game's spelling is listed exactly.
            if (now != was && onDiskName(dir, now) == now && onDiskName(dir, was) != was) {
                if (Status st = renameCaseOnly(dir, now, was); !st.ok())
                    problems << QStringLiteral("%1: %2").arg(e.rel, st.error().message);
            }
            // A crash inside the fallback rename leaves "<name>.fwcase" with
            // the backported bytes; the bytes are restored below either way.
            const QString stray = target + QStringLiteral(".fwcase");
            if (QFileInfo(stray).isFile() && hashOf(stray) == e.sha256After) {
                if (!QFileInfo(target).exists())
                    (void)replaceFileAtomic(stray, target); // judged and restored like any applied file
                else
                    removeFileForce(stray);
            }
            e.renamed = false;
        }
        const bool exists = QFileInfo(target).isFile();
        const QString cur = exists ? hashOf(target) : QString();
        if (e.action == ApplyAction::Replace) {
            if (exists && cur == e.sha256Before) {
                e.done = false;
                continue;
            }
            if (!exists || cur == e.sha256After) {
                if (Status st = restoreOriginal(e, target, token); !st.ok())
                    problems << QStringLiteral("%1: %2").arg(e.rel, st.error().message);
                else
                    e.done = false;
            } else {
                problems << QStringLiteral("%1 has content this backport did not write; it was left as it is.").arg(e.rel);
            }
        } else {
            if (!exists) {
                e.done = false;
                continue;
            }
            if (cur == e.sha256After) {
                if (!removeFileForce(target))
                    problems << QStringLiteral("%1 could not be removed.").arg(e.rel);
                else
                    e.done = false;
            } else {
                problems << QStringLiteral("%1 has content this backport did not write; it was left as it is.").arg(e.rel);
            }
        }
    }
    for (auto it = m_createdDirs.crbegin(); it != m_createdDirs.crend(); ++it)
        QDir().rmdir(joinPath(m_game, *it)); // only when empty
    m_state = problems.isEmpty() ? JournalState::RolledBack : JournalState::RollbackFailed;
    if (!problems.isEmpty())
        m_error = problems.join(QStringLiteral(" "));
    (void)save();
    if (problemsOut)
        *problemsOut = problems;
    if (!problems.isEmpty())
        return makeError(Code::RollbackFailed,
                         QStringLiteral("The game folder could not be fully rolled back: %1 Restore it from %2.")
                             .arg(problems.join(QStringLiteral(" ")), QDir::toNativeSeparators(m_zip)));
    return success();
}

Status ApplyJournal::apply(const CancellationToken& token, ProgressReporter* progress, const Hook& hook)
{
    if (m_state != JournalState::Pending)
        return makeError(Code::ApplyFailed, QStringLiteral("This apply journal was already used."));

    // Every check before the first write (as the Backport GUI): each file to
    // replace still has the content that was backed up, no file to add exists
    // yet, and every staged copy still has the staged hash. A mismatch means
    // nothing is written, so there is nothing to roll back. The same checks run
    // again per file below, for changes made while Apply runs.
    for (const JournalEntry& e : m_entries) {
        if (!token.checkpoint()) {
            m_state = JournalState::RolledBack;
            (void)save();
            return makeError(Code::Cancelled, QStringLiteral("Cancelled. The game folder was not changed."));
        }
        const QString target = joinPath(m_game, e.rel);
        QString why;
        Code code = Code::GameChanged;
        if (e.action == ApplyAction::Replace && hashOf(target) != e.sha256Before) {
            why = QStringLiteral("%1 changed after the backup was made").arg(e.rel);
        } else if (e.action == ApplyAction::Add && QFileInfo::exists(target)) {
            why = QStringLiteral("%1 appeared after the scan").arg(e.rel);
        } else if (hashOf(e.stagedPath) != e.sha256After) {
            why = QStringLiteral("the staged copy of %1 changed").arg(e.rel);
            code = Code::VerifyFailed;
        } else if (!e.renameTo.isEmpty()) {
            const QString now = e.renameTo.section(u'/', -1);
            if (now != e.rel.section(u'/', -1) && onDiskName(QFileInfo(target).absolutePath(), now) == now)
                why = QStringLiteral("%1 already exists next to %2").arg(e.renameTo, e.rel);
        }
        if (!why.isEmpty()) {
            m_state = JournalState::RolledBack;
            m_error = why;
            (void)save();
            return makeError(code, QStringLiteral("%1, so nothing was applied. The game folder was not changed.").arg(why));
        }
    }

    Error failure;
    bool failed = false;
    bool cancelled = false;
    for (size_t i = 0; i < m_entries.size() && !failed; ++i) {
        JournalEntry& e = m_entries[i];
        if (!token.checkpoint()) {
            cancelled = true;
            break;
        }
        if (hook) {
            const HookAction a = hook(int(i), e.rel);
            if (a == HookAction::Crash) {
                (void)save();
                return makeError(Code::ApplyFailed, QStringLiteral("Apply was interrupted (simulated crash)."));
            }
            if (a == HookAction::Fail) {
                failure = makeError(Code::ApplyFailed, QStringLiteral("Applying failed at %1.").arg(e.rel), QStringLiteral("injected"));
                failed = true;
                break;
            }
        }
        if (progress)
            progress->setCurrentItem(e.rel);
        const QString target = joinPath(m_game, e.rel);
        if (e.action == ApplyAction::Replace) {
            if (hashOf(target) != e.sha256Before) {
                failure = makeError(Code::GameChanged,
                                    QStringLiteral("%1 changed after the backup was made, so nothing more was applied.").arg(e.rel));
                failed = true;
                break;
            }
        } else {
            if (QFileInfo::exists(target)) {
                failure = makeError(Code::GameChanged, QStringLiteral("%1 appeared after the scan, so nothing more was applied.").arg(e.rel));
                failed = true;
                break;
            }
            // Record the folders this entry creates before creating them.
            QStringList chain;
            QString dir = QFileInfo(e.rel).path();
            while (!dir.isEmpty() && dir != u'.' && !QFileInfo(joinPath(m_game, dir)).isDir()) {
                chain.prepend(dir);
                dir = QFileInfo(dir).path();
            }
            if (!chain.isEmpty()) {
                m_createdDirs << chain;
                (void)save();
                if (!QDir().mkpath(joinPath(m_game, chain.back()))) {
                    failure = makeError(Code::ApplyFailed, QStringLiteral("Could not create the folder for %1.").arg(e.rel));
                    failed = true;
                    break;
                }
            }
        }
        const QString tmp = target + QStringLiteral(".fwpart");
        if (Status st = copyFileStreaming(e.stagedPath, tmp, token, progress, true, true); !st.ok()) {
            removeTemp(tmp);
            if (st.error().isCancelled()) {
                cancelled = true;
                break;
            }
            failure = makeError(Code::ApplyFailed, QStringLiteral("Applying failed at %1: %2").arg(e.rel, st.error().message),
                                st.error().detail);
            failed = true;
            break;
        }
        if (hashOf(tmp) != e.sha256After) {
            removeTemp(tmp);
            failure = makeError(Code::VerifyFailed, QStringLiteral("Applying failed at %1: the copy does not match.").arg(e.rel));
            failed = true;
            break;
        }
        if (Status st = replaceFileAtomic(tmp, target); !st.ok()) {
            removeTemp(tmp);
            failure = makeError(Code::ApplyFailed, QStringLiteral("Applying failed at %1: %2").arg(e.rel, st.error().message),
                                st.error().detail);
            failed = true;
            break;
        }
        if (!e.renameTo.isEmpty()) {
            // #41 fix: the bytes are in place under the dump's name; now the
            // name takes the game's letter case.
            if (Status st = renameCaseOnly(QFileInfo(target).absolutePath(), e.rel.section(u'/', -1),
                                           e.renameTo.section(u'/', -1));
                !st.ok()) {
                failure = makeError(Code::ApplyFailed, QStringLiteral("Applying failed at %1: %2").arg(e.rel, st.error().message),
                                    st.error().detail);
                failed = true;
                break;
            }
            e.renamed = true;
            (void)save();
        }
        e.done = true;
        if (progress)
            progress->advanceFiles(1);
        if ((i % 8) == 7)
            (void)save();
    }

    if (!failed && !cancelled) {
        (void)save();
        // V7: every applied file holds exactly the staged bytes.
        for (const JournalEntry& e : m_entries) {
            if (hashOf(joinPath(m_game, e.renamed ? e.renameTo : e.rel)) != e.sha256After) {
                failure = makeError(Code::VerifyFailed,
                                    QStringLiteral("Applying failed at %1: the file in the game does not match the backport.").arg(e.rel));
                failed = true;
                break;
            }
        }
    }
    if (!failed && !cancelled) {
        m_state = JournalState::Done;
        (void)save();
        return success();
    }

    // Roll back with a token that cannot be cancelled: the game must not be
    // left half-patched because the user pressed Cancel.
    QStringList problems;
    Status rb = rollback(CancellationToken(), &problems);
    if (cancelled) {
        if (rb.ok())
            return makeError(Code::Cancelled, QStringLiteral("Cancelled. The game folder was rolled back."));
        return rb;
    }
    if (rb.ok())
        return makeError(codeOf(failure), failure.message + QStringLiteral(" The game folder was rolled back."), failure.detail);
    return makeError(Code::RollbackFailed, failure.message + u' ' + rb.error().message, failure.detail);
}

std::vector<PendingJournal> ApplyJournal::findInterrupted(const QString& dir)
{
    std::vector<PendingJournal> out;
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        auto j = load(fi.absoluteFilePath());
        if (!j.ok())
            continue;
        if (j->state() != JournalState::Pending && j->state() != JournalState::RollbackFailed)
            continue;
        PendingJournal p;
        p.path = fi.absoluteFilePath();
        p.jobId = j->jobId();
        p.game = j->game();
        p.originalsZip = j->originalsZip();
        p.created = j->m_created;
        p.state = j->state();
        out.push_back(p);
    }
    return out;
}

int ApplyJournal::prune(const QString& dir, int days)
{
    int removed = 0;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
    for (const QFileInfo& fi : files) {
        if (fi.lastModified() > cutoff)
            continue;
        auto j = load(fi.absoluteFilePath());
        if (!j.ok())
            continue;
        if (j->state() != JournalState::Done && j->state() != JournalState::RolledBack)
            continue; // never remove a journal that may still be needed
        if (QFile::remove(fi.absoluteFilePath()))
            ++removed;
    }
    return removed;
}

} // namespace fw::backport
