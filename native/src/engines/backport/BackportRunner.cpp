#include "engines/backport/BackportRunner.h"

#include "core/FileUtil.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackupArchive.h"
#include "engines/backport/ProFix.h"
#include "engines/backport/ProviderSet.h"
#include "engines/backport/RunReport.h"

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
    if (f.substitutedStandard)
        o.insert(QStringLiteral("substitutedStandard"), true);
    if (f.criRackPatched)
        o.insert(QStringLiteral("criAudioFix"), true);
    if (!f.renamedTo.isEmpty())
        o.insert(QStringLiteral("writtenAs"), f.renamedTo);
    if (f.shaderTrapNops > 0)
        o.insert(QStringLiteral("shaderTrapNops"), f.shaderTrapNops);
    return o;
}

// What a run did, collected for its report (#24).
struct RunTrace {
    bool planned = false; // the preflight passed: a report is written
    QString title, titleId, manifestPath;
    QJsonObject inputs, target, verification, apply;
    QJsonArray issues, files, skipped, zips;
};

QJsonArray issuesJson(const BackportPlan& plan);

QJsonObject zipJson(const QString& role, const StagedZip& z)
{
    QJsonObject o;
    o.insert(QStringLiteral("role"), role);
    o.insert(QStringLiteral("result"), QStringLiteral("written and verified"));
    o.insert(QStringLiteral("path"), native(z.finalPath()));
    o.insert(QStringLiteral("sha256"), z.result().zipSha256);
    o.insert(QStringLiteral("members"), z.result().count);
    o.insert(QStringLiteral("bytes"), double(z.result().bytes));
    return o;
}

QJsonObject fileReportJson(const FileResult& f, const BackportPlan& plan, int targetSdk)
{
    QJsonObject o;
    o.insert(QStringLiteral("file"), f.rel);
    const bool fakelibTarget = plan.stagedFakelibTargets.contains(f.rel);
    QString result, why;
    if (!f.ok) {
        result = QStringLiteral("failed");
        why = f.message;
        o.insert(QStringLiteral("code"), codeName(f.code));
    } else if (f.stage == FileStage::LeftUnchanged) {
        result = QStringLiteral("left unchanged");
        why = QStringLiteral("Encrypted or compressed; carried over as it is (policy: leave unchanged).");
    } else if (f.substitutedStandard) {
        result = QStringLiteral("replaced");
        why = QStringLiteral("The dump's copy cannot be converted (%1); the standard right.sprx was converted to SDK %2 "
                             "and takes its place. The dump's copy is in the originals ZIP.")
                  .arg(plan.rightSprxReason)
                  .arg(targetSdk);
    } else if (fakelibTarget) {
        const QString name = f.rel.section(u'/', -1);
        const bool provider = plan.providers && plan.providers->find(name);
        const QString from = provider ? QStringLiteral("the AMPR/PlayGo folder") : QStringLiteral("the fakelib folder");
        if (f.shaBefore.isEmpty())
            result = QStringLiteral("added");
        else if (f.shaBefore == f.shaAfter)
            result = QStringLiteral("unchanged");
        else
            result = QStringLiteral("replaced");
        why = result == u"replaced"
            ? QStringLiteral("Library from %1; the earlier file is kept in the originals ZIP.").arg(from)
            : QStringLiteral("Library from %1, copied unchanged (never re-signed).").arg(from);
    } else if (f.shaBefore == f.shaAfter) {
        result = QStringLiteral("unchanged");
        why = QStringLiteral("Already the SDK %1 build.").arg(targetSdk);
    } else {
        result = QStringLiteral("rewritten");
        why = f.sdkBefore ? QStringLiteral("SDK %1 -> %2, fake-signed").arg(*f.sdkBefore).arg(f.sdkAfter.value_or(targetSdk))
                          : QStringLiteral("Fake-signed (no SDK param to change)");
        if (f.nid.action == NidAction::Applied || f.nid.action == NidAction::Reverted)
            why += QStringLiteral("; libc import %1 (%2)").arg(nidActionName(f.nid.action)).arg(f.nid.count);
        if (f.shaderTrapNops > 0)
            why += QStringLiteral("; PS5 Pro fix: %1 shader-assert trap(s) -> s_nop 0").arg(f.shaderTrapNops);
        if (f.criRackPatched)
            why += QStringLiteral("; CRI audio fix: 12-channel speaker rack -> 8 channels");
        if (!f.renamedTo.isEmpty())
            why += QStringLiteral("; written as %1, the spelling the game loads").arg(f.renamedTo);
        why += u'.';
    }
    o.insert(QStringLiteral("result"), result);
    o.insert(QStringLiteral("why"), why);
    o.insert(QStringLiteral("stage"), fileStageName(f.stage));
    if (f.sdkBefore)
        o.insert(QStringLiteral("sdkBefore"), *f.sdkBefore);
    if (f.sdkAfter)
        o.insert(QStringLiteral("sdkAfter"), *f.sdkAfter);
    if (!f.shaBefore.isEmpty())
        o.insert(QStringLiteral("sha256Before"), f.shaBefore);
    if (!f.shaAfter.isEmpty())
        o.insert(QStringLiteral("sha256After"), f.shaAfter);
    return o;
}

void tracePlan(RunTrace& t, const BackportPlan& plan, const BackportOptions& opt)
{
    t.planned = true;
    t.title = plan.meta.title;
    t.titleId = plan.titleId;
    t.manifestPath = plan.manifestPath;
    QJsonObject in;
    in.insert(QStringLiteral("game"), native(plan.options.game));
    in.insert(QStringLiteral("titleId"), plan.meta.titleId);
    in.insert(QStringLiteral("title"), plan.meta.title);
    in.insert(QStringLiteral("version"), plan.meta.version);
    if (plan.gameSdk)
        in.insert(QStringLiteral("gameSdk"), *plan.gameSdk);
    if (plan.meta.paramSdk)
        in.insert(QStringLiteral("paramJsonSdk"), *plan.meta.paramSdk);
    if (!plan.meta.requiredFwText().isEmpty())
        in.insert(QStringLiteral("requiredFw"), plan.meta.requiredFwText());
    in.insert(QStringLiteral("originalsZipFolder"), native(opt.originalsDir));
    in.insert(QStringLiteral("backportZipFolder"), native(opt.backportDir));
    in.insert(QStringLiteral("fakelibFolder"), native(opt.fakelibDir));
    in.insert(QStringLiteral("amprPlaygoFolder"), native(opt.providerDir));
    if (!plan.rightSprxSubstitute.isEmpty())
        in.insert(QStringLiteral("standardRightSprx"), native(opt.standardRightSprx));
    if (plan.providers) {
        QJsonObject prov;
        for (const FakelibLib& l : plan.providers->libs)
            prov.insert(l.name, l.sha256);
        in.insert(QStringLiteral("amprPlaygoSha256"), prov);
        in.insert(QStringLiteral("amprPlaygoPins"), plan.pinProvidersOnRun ? QStringLiteral("pinned by this run")
                                                                           : QStringLiteral("matched the pins"));
    }
    in.insert(QStringLiteral("applyToGameFolder"), opt.applyToGame);
    in.insert(QStringLiteral("nidPatchMode"), opt.nidMode == NidMode::Off ? QStringLiteral("off") : QStringLiteral("auto"));
    in.insert(QStringLiteral("encryptedSelfPolicy"), opt.encryptedPolicy == EncryptedPolicy::Refuse
                                                         ? QStringLiteral("refuse")
                                                         : QStringLiteral("leaveUnchanged"));
    in.insert(QStringLiteral("verifyBuild"), opt.verifyBuild);
    in.insert(QStringLiteral("proFix"), opt.proFix);
    in.insert(QStringLiteral("criAudioFix"), opt.criAudioFix);
    in.insert(QStringLiteral("notes"), opt.notes);
    in.insert(QStringLiteral("fakelibState"), fakelibStateName(plan.fakelibState));
    in.insert(QStringLiteral("manifestVerdict"), verdictName(plan.verdict));
    t.inputs = in;
    if (plan.pair) {
        QJsonObject tg;
        tg.insert(QStringLiteral("sdk"), plan.pair->n);
        tg.insert(QStringLiteral("label"), firmwareLabel(plan.pair->n));
        tg.insert(QStringLiteral("ps5Sdk"), QStringLiteral("0x%1").arg(plan.pair->ps5, 8, 16, QLatin1Char('0')));
        tg.insert(QStringLiteral("ps4Sdk"), QStringLiteral("0x%1").arg(plan.pair->ps4, 8, 16, QLatin1Char('0')));
        t.target = tg;
    }
    t.issues = issuesJson(plan);
    const auto skip = [&t](const QString& file, const QString& why) {
        QJsonObject o;
        o.insert(QStringLiteral("file"), file);
        o.insert(QStringLiteral("why"), why);
        t.skipped.append(o);
    };
    for (const ExecFile& e : plan.scan.executables) {
        if (e.area == Area::Fakelib)
            skip(e.rel, QStringLiteral("In the dump's fakelib/: never re-signed; carried over unchanged unless the set "
                                       "replaces it (then kept in the originals ZIP)."));
        else if (e.area == Area::Decrypted)
            skip(e.rel, QStringLiteral("In decrypted/: never processed; kept in the originals ZIP."));
    }
    for (const QString& b : plan.scan.leftoverBaks)
        skip(b, QStringLiteral("A .bak file: never an executable; left alone."));
    for (const QString& l : plan.scan.skippedSymlinks)
        skip(l, QStringLiteral("A link: not followed and not backported."));
    const QStringList meta = plan.scan.metadataSkipped;
    for (int i = 0; i < meta.size() && i < 50; ++i)
        skip(meta[i], QStringLiteral("OS metadata: never an executable."));
    if (meta.size() > 50)
        skip(QStringLiteral("%1 more").arg(meta.size() - 50), QStringLiteral("OS metadata: never an executable."));
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

namespace {

Result<RunResult> runBackportImpl(const BackportOptions& optIn, JobContext& ctx, RunTrace& t)
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
    tracePlan(t, plan, opt);
    t.verification.insert(QStringLiteral("preflight"), QStringLiteral("passed (%1 findings reported)").arg(plan.issues.size()));

    // #34: the provider folder's first use pins its libraries (trust on first use).
    if (plan.pinProvidersOnRun && plan.providers) {
        const QString pinsPath = opt.providerPinsPath.isEmpty() ? defaultProviderPinsPath() : opt.providerPinsPath;
        if (Status st = pinProviders(*plan.providers, pinsPath); st.ok())
            ctx.log.info(QStringLiteral("Pinned the AMPR/PlayGo libraries in %1.").arg(native(plan.providers->dir)));
        else
            ctx.log.warn(st.error().message);
    }

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
        t.verification.insert(QStringLiteral("build"), QStringLiteral("failed: %1").arg(built.error().message));
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
    for (const FileResult& f : build.files)
        t.files.append(fileReportJson(f, plan, opt.targetSdk));
    t.verification.insert(QStringLiteral("build"),
                          build.failed() ? QStringLiteral("failed: %1").arg(build.firstFailure()->message)
                          : opt.verifyBuild
                              ? QStringLiteral("passed: structure, round trip and SDK of every signed executable (V1-V3), "
                                               "fakelib hashes (V4), coverage (V5)")
                              : QStringLiteral("V1-V3 turned off (backport.verifyBuild); fakelib hashes (V4) and coverage "
                                               "(V5) passed"));
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
    if (!build.criRackPatched.isEmpty()) {
        result.details.insert(QStringLiteral("criAudioFix"), QJsonArray::fromStringList(build.criRackPatched));
        t.target.insert(QStringLiteral("criAudioFix"), QJsonArray::fromStringList(build.criRackPatched));
        ctx.log.info(QStringLiteral("CRI audio fix: the 12-channel speaker rack became 8 channels in %1.")
                         .arg(build.criRackPatched.join(QStringLiteral(", "))));
    }
    if (opt.proFix) {
        // #32: what the Pro fix did, for the job details, the log and the report.
        QJsonObject pro;
        pro.insert(QStringLiteral("family"), QLatin1String(kShaderTrapFamily));
        pro.insert(QStringLiteral("sites"), build.shaderTrapNops);
        pro.insert(QStringLiteral("applied"), build.shaderTrapNops > 0);
        result.details.insert(QStringLiteral("proFix"), pro);
        t.target.insert(QStringLiteral("proFix"), pro);
        if (build.shaderTrapNops > 0) {
            ctx.log.info(QStringLiteral("PS5 Pro fix: %1 shader-assert trap(s) in eboot.bin turned into no-ops.")
                             .arg(build.shaderTrapNops));
            result.warnings << QStringLiteral("PS5 Pro fix applied (%1 shader-assert traps): confirmed on one title only; "
                                              "install it for a PS5 Pro black screen or GPU hang.")
                                   .arg(build.shaderTrapNops);
        } else {
            result.warnings << QStringLiteral("PS5 Pro fix: eboot.bin has no guarded shader-assert traps, so the build "
                                              "is the normal one.");
        }
    }

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
        bpZip = std::make_unique<StagedZip>(uniqueZipPath(
            QDir(opt.backportDir).filePath(backportZipName(plan.prefix, opt.targetSdk, build.shaderTrapNops > 0))));
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
    t.zips.append(zipJson(QStringLiteral("originals"), origZip));
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
            t.zips.append(zipJson(QStringLiteral("backport"), *bpZip));
        }
    }
    if (backportError) {
        QJsonObject z;
        z.insert(QStringLiteral("role"), QStringLiteral("backport"));
        z.insert(QStringLiteral("result"), QStringLiteral("failed"));
        z.insert(QStringLiteral("why"), backportError->message);
        t.zips.append(z);
    }
    t.verification.insert(QStringLiteral("zips"),
                          QStringLiteral("each ZIP read back before its rename: member count, CRC and size of every "
                                         "member, SHA-256 of every member against its source"));

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
        // #41 fix: written under the game's spelling; Restore renames it back.
        if (const QString existing = plan.scan.resolve(s.rel); !existing.isEmpty() && existing != s.rel)
            target.renamedFiles.insert(s.rel, existing);
    }
    recordTarget(manifest, target);
    if (Status st = writeManifest(manifest); !st.ok())
        result.warnings << QStringLiteral("The backport manifest could not be written: %1").arg(st.error().message);
    result.manifestPath = manifest.path;
    t.manifestPath = manifest.path;

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
                if (existing != s.rel)
                    e.renameTo = s.rel; // #41 fix: the game's spelling of this module
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
        t.apply.insert(QStringLiteral("journal"), native(journalPath));
        if (!st.ok()) {
            t.apply.insert(QStringLiteral("result"), st.error().message);
            t.verification.insert(QStringLiteral("apply"), QStringLiteral("failed: %1").arg(st.error().message));
            const Code c = codeOf(st.error());
            if (c == Code::Cancelled)
                return makeError(Code::Cancelled, QStringLiteral("%1 Originals: %2").arg(st.error().message, native(result.originalsZip)));
            ctx.log.error(st.error().message);
            return makeError(c, QStringLiteral("Partial: the build passed and the ZIPs were saved (originals: %1), but %2")
                                    .arg(native(result.originalsZip), st.error().message),
                             st.error().detail);
        }
        result.applied = true;
        t.apply.insert(QStringLiteral("result"), QStringLiteral("applied %1 files").arg(entries.size()));
        t.verification.insert(QStringLiteral("apply"), QStringLiteral("every applied file holds the staged bytes (V7)"));
        TargetRecord applied = target;
        applied.applied = true;
        applied.addedFiles = journal->addedFiles();
        applied.journal = QFileInfo(journalPath).completeBaseName();
        recordTarget(manifest, applied);
        if (Status ms = writeManifest(manifest); !ms.ok())
            result.warnings << QStringLiteral("The backport manifest could not be updated: %1").arg(ms.error().message);
        ctx.log.info(QStringLiteral("Applied %1 files to %2").arg(entries.size()).arg(native(game)));
    }

    if (!opt.applyToGame) {
        t.apply.insert(QStringLiteral("result"), QStringLiteral("not applied: Apply to game folders is off"));
        t.verification.insert(QStringLiteral("apply"), QStringLiteral("not applied; the game folder was not changed"));
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

} // namespace

Result<RunResult> runBackport(const BackportOptions& opt, JobContext& ctx)
{
    RunTrace t;
    Result<RunResult> r = runBackportImpl(opt, ctx, t);
    if (!t.planned)
        return r; // refused by the preflight: the error says why, no report

    QJsonObject rep = newReport(QStringLiteral("backport"));
    rep.insert(QStringLiteral("jobId"), ctx.jobId);
    rep.insert(QStringLiteral("title"), t.title);
    rep.insert(QStringLiteral("titleId"), t.titleId);
    QJsonObject outcome;
    QStringList warnings;
    if (r.ok()) {
        outcome.insert(QStringLiteral("status"), runStatusName(r->status));
        outcome.insert(QStringLiteral("summary"), r->summary);
        warnings = r->warnings;
    } else {
        const Error& e = r.error();
        const Code c = codeOf(e);
        outcome.insert(QStringLiteral("status"), c == Code::Cancelled ? QStringLiteral("cancelled")
                                                 : e.message.startsWith(QStringLiteral("Partial:")) ? QStringLiteral("partial")
                                                                                                   : QStringLiteral("failed"));
        outcome.insert(QStringLiteral("summary"), e.message);
        QJsonObject err;
        err.insert(QStringLiteral("code"), codeName(c));
        err.insert(QStringLiteral("detail"), e.detail);
        outcome.insert(QStringLiteral("error"), err);
        for (const QJsonValue& i : std::as_const(t.issues))
            if (i.toObject().value(QStringLiteral("severity")).toString() == u"warn")
                warnings << i.toObject().value(QStringLiteral("message")).toString();
    }
    rep.insert(QStringLiteral("outcome"), outcome);
    rep.insert(QStringLiteral("inputs"), t.inputs);
    if (!t.target.isEmpty())
        rep.insert(QStringLiteral("target"), t.target);
    rep.insert(QStringLiteral("verification"), t.verification);
    rep.insert(QStringLiteral("zips"), t.zips);
    rep.insert(QStringLiteral("files"), t.files);
    rep.insert(QStringLiteral("skipped"), t.skipped);
    if (!t.apply.isEmpty())
        rep.insert(QStringLiteral("apply"), t.apply);
    if (!t.manifestPath.isEmpty())
        rep.insert(QStringLiteral("manifest"), native(t.manifestPath));
    rep.insert(QStringLiteral("issues"), t.issues);
    rep.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));

    const QString dir = !opt.reportDir.isEmpty() ? opt.reportDir
        : !t.manifestPath.isEmpty()              ? reportDirForManifest(t.manifestPath)
                                                 : defaultReportDir();
    auto written = writeReport(dir, reportBaseName(QStringLiteral("backport SDK %1").arg(opt.targetSdk)), rep);
    if (!written.ok()) {
        ctx.log.warn(QStringLiteral("The run report could not be written: %1").arg(written.error().message));
        if (r.ok())
            r->warnings << QStringLiteral("The run report could not be written: %1").arg(written.error().message);
        return r;
    }
    ctx.log.info(QStringLiteral("Run report: %1").arg(native(written->html)));
    if (r.ok()) {
        r->reportJson = written->json;
        r->reportHtml = written->html;
        r->details.insert(QStringLiteral("report"), reportFilesJson(written.value()));
        return r;
    }
    Error e = r.takeError();
    return Error(e.code, e.message, appendReportToDetail(e.detail, written->html));
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
