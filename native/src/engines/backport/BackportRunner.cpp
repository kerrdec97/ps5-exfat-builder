#include "engines/backport/BackportRunner.h"

#include "core/FileUtil.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackupArchive.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>

#include <memory>

namespace fw::backport {

QString runStatusName(RunStatus s)
{
    switch (s) {
    case RunStatus::Succeeded: return QStringLiteral("succeeded");
    case RunStatus::Partial: return QStringLiteral("partial");
    case RunStatus::NothingToDo: return QStringLiteral("nothingToDo");
    }
    return QStringLiteral("succeeded");
}

QStringList runPhases()
{
    return {QStringLiteral("Scan"), QStringLiteral("Patch"), QStringLiteral("Package"), QStringLiteral("Verify"),
            QStringLiteral("Apply")};
}

namespace {

QString native(const QString& p)
{
    return QDir::toNativeSeparators(p);
}

QJsonObject fileJson(const FileResult& f)
{
    QJsonObject o;
    o.insert(QStringLiteral("file"), f.rel);
    o.insert(QStringLiteral("ok"), f.ok);
    o.insert(QStringLiteral("stage"), fileStageName(f.stage));
    if (!f.ok) {
        o.insert(QStringLiteral("code"), codeName(f.code));
        o.insert(QStringLiteral("message"), f.message);
    }
    if (f.sdkBefore)
        o.insert(QStringLiteral("sdkBefore"), *f.sdkBefore);
    if (f.sdkAfter)
        o.insert(QStringLiteral("sdkAfter"), *f.sdkAfter);
    if (!f.shaBefore.isEmpty())
        o.insert(QStringLiteral("sha256Before"), f.shaBefore);
    if (!f.shaAfter.isEmpty())
        o.insert(QStringLiteral("sha256After"), f.shaAfter);
    if (f.nid.action != NidAction::None)
        o.insert(QStringLiteral("nid"), nidActionName(f.nid.action));
    return o;
}

QJsonArray issuesJson(const BackportPlan& plan)
{
    QJsonArray a;
    for (const Issue& i : plan.issues) {
        QJsonObject o;
        o.insert(QStringLiteral("severity"), severityName(i.severity));
        o.insert(QStringLiteral("code"), codeName(i.code));
        o.insert(QStringLiteral("message"), i.message);
        if (!i.files.isEmpty())
            o.insert(QStringLiteral("files"), QJsonArray::fromStringList(i.files));
        a.append(o);
    }
    return a;
}

} // namespace

Result<RunResult> runBackport(const BackportOptions& optIn, JobContext& ctx)
{
    if (ctx.tempDir.isEmpty() || !QFileInfo(ctx.tempDir).isDir())
        return makeError(Code::Internal, QStringLiteral("The job has no temporary folder."));
    BackportOptions opt = optIn;
    if (opt.stagingHint.isEmpty())
        opt.stagingHint = ctx.tempDir;
    if (opt.journalDir.isEmpty())
        opt.journalDir = defaultJournalDir();
    const QString staging = QDir(ctx.tempDir).filePath(QStringLiteral("staging"));
    if (QFileInfo::exists(staging))
        FW_TRY(removeTreeSafely(staging, ctx.tempDir)); // private to this job: start empty
    const ZipHook zipHook = [&opt](const QString& point, const QString& item) -> Status {
        return opt.testHook ? opt.testHook(point, item) : success();
    };

    // Keeps the staging folder for diagnosis (backport.keepStagingOnFailure).
    const auto keepStaging = [&]() -> QString {
        if (!opt.keepStagingOnFailure || !QFileInfo(staging).isDir())
            return {};
        const QString target = QDir(QFileInfo(ctx.tempDir).absolutePath())
                                   .filePath(QStringLiteral("fw-backport-staging-%1-%2")
                                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")))
                                                 .arg(QRandomGenerator::global()->generate() & 0xFFFFFF, 6, 16, QLatin1Char('0')));
        if (QDir().rename(staging, target)) {
            ctx.log.info(QStringLiteral("The staging folder was kept for diagnosis: %1").arg(native(target)));
            return target;
        }
        return {};
    };
    const auto cancelledUnchanged = []() {
        return makeError(Code::Cancelled, QStringLiteral("Cancelled. The game folder was not changed."));
    };

    // ---- Scan --------------------------------------------------------------------
    if (ctx.progress)
        ctx.progress->beginPhase(0, 0);
    auto planned = planBackport(opt, ctx.token);
    if (!planned.ok())
        return planned.error().isCancelled() ? cancelledUnchanged() : planned.takeError();
    BackportPlan& plan = planned.value();
    for (const Issue& i : plan.issues) {
        const QString line = QStringLiteral("[%1] %2").arg(severityName(i.severity), i.message);
        if (i.severity == Severity::Info)
            ctx.log.info(line);
        else
            ctx.log.warn(line);
    }
    if (!plan.canRun())
        return plan.blockingError();
    ctx.log.info(plan.summary());

    RunResult result;
    result.details.insert(QStringLiteral("issues"), issuesJson(plan));
    result.details.insert(QStringLiteral("game"), native(plan.options.game));
    result.details.insert(QStringLiteral("targetSdk"), opt.targetSdk);
    if (plan.gameSdk)
        result.details.insert(QStringLiteral("gameSdk"), *plan.gameSdk);
    for (const Issue& i : plan.issuesOf(Severity::Warn))
        result.warnings << i.message;

    // ---- Patch -------------------------------------------------------------------
    auto built = buildBackport(plan, staging, ctx);
    if (!built.ok()) {
        if (built.error().isCancelled())
            return cancelledUnchanged();
        const QString kept = keepStaging();
        Error e = built.takeError();
        return makeError(codeOf(e), QStringLiteral("No safe backport was produced: %1 The game folder was not changed.%2")
                                        .arg(e.message, kept.isEmpty() ? QString() : QStringLiteral(" Staging kept: %1").arg(native(kept))),
                         e.detail);
    }
    BuildResult& build = built.value();
    QJsonArray files;
    for (const FileResult& f : build.files)
        files.append(fileJson(f));
    result.details.insert(QStringLiteral("files"), files);
    result.warnings << build.warnings;
    if (build.failed()) {
        const FileResult* f = build.firstFailure();
        QStringList all;
        for (const FileResult& x : build.files)
            if (!x.ok)
                all << x.message;
        const QString kept = keepStaging();
        return makeError(f->code == Code::Ok ? Code::VerifyFailed : f->code,
                         QStringLiteral("No safe backport was produced: %1 The game folder was not changed; no backport ZIP "
                                        "was written.%2")
                             .arg(f->message, kept.isEmpty() ? QString() : QStringLiteral(" Staging kept: %1").arg(native(kept))),
                         all.join(QStringLiteral(" | ")));
    }
    if (build.nothingChanged) {
        result.status = RunStatus::NothingToDo;
        result.summary = QStringLiteral("No backport is needed: every file already matches the SDK %1 build. The game "
                                        "folder was not changed.")
                             .arg(opt.targetSdk);
        return result;
    }
    result.leftUnchanged = build.leftUnchanged;
    result.buildVerified = opt.verifyBuild;
    result.filesRewritten = build.okCount();

    // ---- Package -----------------------------------------------------------------
    std::vector<ZipMember> origMembers;
    qint64 packBytes = 0;
    for (const StagedFile& s : build.originals) {
        origMembers.push_back({s.rel, s.path, s.sha256});
        packBytes += s.size;
    }
    const QString game = plan.options.game;
    QStringList extras;
    if (!plan.scan.paramJsonRel.isEmpty())
        extras << plan.scan.paramJsonRel;
    extras << plan.scan.decryptedFiles;
    for (const QString& rel : std::as_const(extras)) {
        const QString src = joinPath(game, rel);
        auto h = sha256File(src, ctx.token);
        if (!h.ok()) {
            if (h.error().isCancelled())
                return cancelledUnchanged();
            return makeError(Code::ZipWriteFailed,
                             QStringLiteral("No safe backport was produced: %1 could not be read for the originals ZIP. The game "
                                            "folder was not changed.")
                                 .arg(rel),
                             h.error().toLogString());
        }
        origMembers.push_back({rel, src, h.value()});
        packBytes += QFileInfo(src).size();
    }
    std::vector<ZipMember> bpMembers;
    for (const StagedFile& s : build.staged) {
        bpMembers.push_back({s.rel, s.path, s.sha256});
        packBytes += s.size;
    }
    if (ctx.progress)
        ctx.progress->beginPhase(2, opt.backportDir.isEmpty() ? packBytes - qint64(0) : packBytes,
                                 qint64(origMembers.size() + bpMembers.size()));

    StagedZip origZip(uniqueZipPath(QDir(opt.originalsDir).filePath(originalsZipName(plan.prefix))));
    if (Status st = origZip.write(origMembers, ctx.token, ctx.progress, zipHook); !st.ok()) {
        if (st.error().isCancelled())
            return cancelledUnchanged();
        keepStaging();
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("No safe backport was produced: the originals ZIP could not be written (%1). The "
                                        "game folder was not changed.")
                             .arg(st.error().message),
                         st.error().detail);
    }
    std::unique_ptr<StagedZip> bpZip;
    std::optional<Error> backportError;
    if (!opt.backportDir.isEmpty()) {
        bpZip = std::make_unique<StagedZip>(
            uniqueZipPath(QDir(opt.backportDir).filePath(backportZipName(plan.prefix, opt.targetSdk))));
        if (Status st = bpZip->write(bpMembers, ctx.token, ctx.progress, zipHook); !st.ok()) {
            if (st.error().isCancelled())
                return cancelledUnchanged();
            backportError = st.takeError();
            bpZip.reset(); // removes the .part
        }
    }

    // ---- Verify ------------------------------------------------------------------
    if (ctx.progress)
        ctx.progress->beginPhase(3, packBytes, qint64(origMembers.size() + bpMembers.size()));
    {
        Status st = origZip.verify(ctx.token, ctx.progress);
        if (st.ok())
            st = origZip.commit(zipHook);
        if (!st.ok()) {
            if (st.error().isCancelled())
                return cancelledUnchanged();
            keepStaging();
            return makeError(Code::ZipVerifyFailed,
                             QStringLiteral("No safe backport was produced: the originals ZIP did not verify (%1). The game "
                                            "folder was not changed.")
                                 .arg(st.error().message),
                             st.error().detail);
        }
    }
    result.originalsZip = origZip.finalPath();
    ctx.log.info(QStringLiteral("Originals ZIP written and verified: %1").arg(native(result.originalsZip)));
    if (bpZip) {
        Status st = bpZip->verify(ctx.token, ctx.progress);
        if (st.ok())
            st = bpZip->commit(zipHook);
        if (!st.ok()) {
            if (st.error().isCancelled())
                return makeError(Code::Cancelled,
                                 QStringLiteral("Cancelled. The game folder was not changed. Originals: %1").arg(native(result.originalsZip)));
            backportError = st.takeError();
            bpZip.reset();
        } else {
            result.backportZip = bpZip->finalPath();
            ctx.log.info(QStringLiteral("Backport ZIP written and verified: %1").arg(native(result.backportZip)));
        }
    }

    // Manifest (S21), recorded once the originals ZIP is final.
    Manifest manifest = plan.manifest ? *plan.manifest
                                      : newManifest(plan.manifestPath, game, plan.titleId, plan.meta.title);
    if (!plan.manifest && QFileInfo::exists(plan.manifestPath)) {
        const QString aside = plan.manifestPath + QStringLiteral(".corrupt-")
            + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QFile::rename(plan.manifestPath, aside);
    }
    manifest.path = plan.manifestPath;
    recordOriginals(manifest, {result.originalsZip, origZip.result().zipSha256, origZip.result().sha256});
    TargetRecord target;
    target.sdk = opt.targetSdk;
    target.notes = opt.notes;
    for (const StagedFile& s : build.staged) {
        if (s.executable)
            target.postHashes.insert(s.rel, s.sha256);
        target.filesAfter.insert(s.rel, qMakePair(s.sha256, s.size));
    }
    recordTarget(manifest, target);
    if (Status st = writeManifest(manifest); !st.ok())
        result.warnings << QStringLiteral("The backport manifest could not be written: %1").arg(st.error().message);
    result.manifestPath = manifest.path;

    if (backportError)
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("Partial: the build passed and the originals were saved to %1, but the backport ZIP "
                                        "could not be written (%2). The game folder was not changed.")
                             .arg(native(result.originalsZip), backportError->message),
                         backportError->detail);

    // ---- Apply -------------------------------------------------------------------
    if (opt.applyToGame) {
        if (ctx.progress)
            ctx.progress->beginPhase(4, 0, qint64(build.staged.size()));
        ApplyJournal::prune(opt.journalDir, opt.journalRetentionDays);
        std::vector<JournalEntry> entries;
        for (const StagedFile& s : build.staged) {
            JournalEntry e;
            e.stagedPath = s.path;
            e.sha256After = s.sha256;
            e.sizeAfter = s.size;
            const QString existing = plan.scan.resolve(s.rel);
            if (existing.isEmpty()) {
                e.rel = s.rel;
                e.action = ApplyAction::Add;
            } else {
                e.rel = existing;
                e.action = ApplyAction::Replace;
                for (const StagedFile& o : build.originals)
                    if (relKey(o.rel) == relKey(existing)) {
                        e.sha256Before = o.sha256;
                        e.backupStagingPath = o.path;
                        e.backupZipMember = o.rel;
                    }
                if (e.sha256Before.isEmpty())
                    return makeError(Code::Internal,
                                     QStringLiteral("Partial: %1 has no backup in the originals ZIP, so nothing was applied. "
                                                    "Originals: %2")
                                         .arg(existing, native(result.originalsZip)));
            }
            entries.push_back(e);
        }
        const QString id = ctx.jobId.isEmpty() ? QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmsszzz"))
                                               : sanitizeFileName(ctx.jobId);
        QString journalPath = QDir(opt.journalDir).filePath(id + QStringLiteral(".json"));
        for (int n = 2; QFileInfo::exists(journalPath); ++n) // a retried job id
            journalPath = QDir(opt.journalDir).filePath(QStringLiteral("%1-%2.json").arg(id).arg(n));
        auto journal = ApplyJournal::create(journalPath, ctx.jobId, game, result.originalsZip, entries);
        if (!journal.ok())
            return makeError(Code::ApplyFailed,
                             QStringLiteral("Partial: the build passed and the originals were saved to %1, but %2")
                                 .arg(native(result.originalsZip), journal.error().message));
        result.journalPath = journalPath;
        const ApplyJournal::Hook hook = [&opt](int, const QString& rel) {
            if (!opt.testHook)
                return ApplyJournal::HookAction::Continue;
            if (!opt.testHook(QStringLiteral("apply.crash"), rel).ok())
                return ApplyJournal::HookAction::Crash;
            if (!opt.testHook(QStringLiteral("apply.file"), rel).ok())
                return ApplyJournal::HookAction::Fail;
            return ApplyJournal::HookAction::Continue;
        };
        Status st = journal->apply(ctx.token, ctx.progress, hook);
        if (!st.ok()) {
            const Code c = codeOf(st.error());
            if (c == Code::Cancelled)
                return makeError(Code::Cancelled, QStringLiteral("%1 Originals: %2").arg(st.error().message, native(result.originalsZip)));
            ctx.log.error(st.error().message);
            return makeError(c, QStringLiteral("Partial: the build passed and the ZIPs were saved (originals: %1), but %2")
                                    .arg(native(result.originalsZip), st.error().message),
                             st.error().detail);
        }
        result.applied = true;
        TargetRecord applied = target;
        applied.applied = true;
        applied.addedFiles = journal->addedFiles();
        applied.journal = QFileInfo(journalPath).completeBaseName();
        recordTarget(manifest, applied);
        if (Status ms = writeManifest(manifest); !ms.ok())
            result.warnings << QStringLiteral("The backport manifest could not be updated: %1").arg(ms.error().message);
        ctx.log.info(QStringLiteral("Applied %1 files to %2").arg(entries.size()).arg(native(game)));
    }

    result.status = result.leftUnchanged.isEmpty() ? RunStatus::Succeeded : RunStatus::Partial;
    QString s = QStringLiteral("Backported to SDK %1 (loads on FW %1.00+). Originals: %2")
                    .arg(opt.targetSdk)
                    .arg(QFileInfo(result.originalsZip).fileName());
    if (!opt.applyToGame)
        s += QStringLiteral(". The game folder was not changed");
    if (result.status == RunStatus::Partial) {
        s = QStringLiteral("Partial: ") + s
            + QStringLiteral(". Left unchanged (encrypted): %1").arg(result.leftUnchanged.join(QStringLiteral(", ")));
        result.warnings << QStringLiteral("Encrypted files were left unchanged; whether the game loads is UNVERIFIED.");
    }
    result.summary = s + u'.';
    result.details.insert(QStringLiteral("status"), runStatusName(result.status));
    result.details.insert(QStringLiteral("originalsZip"), native(result.originalsZip));
    result.details.insert(QStringLiteral("backportZip"), native(result.backportZip));
    result.details.insert(QStringLiteral("applied"), result.applied);
    result.details.insert(QStringLiteral("manifest"), native(result.manifestPath));
    result.details.insert(QStringLiteral("journal"), native(result.journalPath));
    return result;
}

JobOutcome toOutcome(const RunResult& r)
{
    JobOutcome o;
    o.summary = r.summary;
    o.warnings = r.warnings;
    o.details = r.details;
    if (!r.originalsZip.isEmpty())
        o.outputs << r.originalsZip;
    if (!r.backportZip.isEmpty())
        o.outputs << r.backportZip;
    if (r.applied)
        o.outputs << r.details.value(QStringLiteral("game")).toString();
    switch (r.status) {
    case RunStatus::NothingToDo:
        o.verification = Verification::Skipped;
        o.verificationDetail = QStringLiteral("Nothing to verify: no file changed.");
        break;
    case RunStatus::Partial:
        o.verification = Verification::Inconclusive;
        o.verificationDetail = QStringLiteral("%1 files signed and verified; %2 left unchanged.")
                                   .arg(r.filesRewritten)
                                   .arg(r.leftUnchanged.size());
        break;
    case RunStatus::Succeeded:
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("%1 files staged%2; ZIPs read back (CRC and SHA-256)%3.")
                                   .arg(r.filesRewritten)
                                   .arg(r.buildVerified ? QStringLiteral(" and verified")
                                                        : QStringLiteral(" (build checks turned off)"),
                                        r.applied ? QStringLiteral("; applied files checked") : QString());
        break;
    }
    return o;
}

} // namespace fw::backport
