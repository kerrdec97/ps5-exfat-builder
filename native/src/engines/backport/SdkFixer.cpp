#include "engines/backport/SdkFixer.h"

#include "core/FileUtil.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackportEngine.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/BackupArchive.h"
#include "engines/backport/GameMetadata.h"
#include "engines/backport/RunReport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>

#include <memory>

namespace fw::backport {

namespace {

QString native(const QString& p)
{
    return QDir::toNativeSeparators(p);
}

struct FixTrace {
    bool planned = false;
    QString title, titleId;
    QJsonObject inputs, target, verification;
    QJsonArray files, zips, skipped;
};

bool isParamFile(const QString& rel)
{
    const QString n = rel.section(u'/', -1).toLower();
    return n == u"param.json" || n == u"param.sfo";
}

qsizetype countOf(const QByteArray& data, const QByteArray& needle)
{
    qsizetype n = 0;
    for (qsizetype i = data.indexOf(needle); i >= 0; i = data.indexOf(needle, i + 1))
        ++n;
    return n;
}

// "<base>", else "<base> (N)": neither the folder nor its ".fwpart" exists.
QString freeFolder(const QString& base)
{
    const auto taken = [](const QString& p) {
        return QFileInfo::exists(p) || QFileInfo::exists(p + QStringLiteral(".fwpart"));
    };
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString c = QStringLiteral("%1 (%2)").arg(base).arg(n);
        if (!taken(c))
            return c;
    }
}

// Removes a half-written copy unless it was committed.
struct CopyGuard {
    QString work, outDir;
    bool keep = false;
    ~CopyGuard()
    {
        if (!keep && !work.isEmpty() && QFileInfo::exists(work))
            (void)removeTreeSafely(work, outDir);
    }
};

QJsonObject fileJson(const FileResult& f, int targetSdk)
{
    QJsonObject o;
    o.insert(QStringLiteral("file"), f.rel);
    QString result, why;
    if (!f.ok) {
        result = QStringLiteral("failed");
        why = f.message;
        o.insert(QStringLiteral("code"), codeName(f.code));
    } else if (f.stage == FileStage::LeftUnchanged) {
        result = QStringLiteral("left unchanged");
        why = QStringLiteral("Encrypted or compressed; carried over as it is (policy: leave unchanged).");
    } else if (f.shaBefore == f.shaAfter) {
        result = QStringLiteral("unchanged");
        why = QStringLiteral("Already the SDK %1 build; not written.").arg(targetSdk);
    } else {
        result = QStringLiteral("rewritten");
        why = f.sdkBefore ? QStringLiteral("SDK marker %1 -> %2, fake-signed again.").arg(*f.sdkBefore).arg(targetSdk)
                          : QStringLiteral("Fake-signed again (no SDK param).");
    }
    o.insert(QStringLiteral("result"), result);
    o.insert(QStringLiteral("why"), why);
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

Result<SdkFixResult> runImpl(const SdkFixOptions& optIn, JobContext& ctx, FixTrace& t)
{
    if (ctx.tempDir.isEmpty() || !QFileInfo(ctx.tempDir).isDir())
        return makeError(Code::Internal, QStringLiteral("The job has no temporary folder."));
    SdkFixOptions opt = optIn;
    if (opt.journalDir.isEmpty())
        opt.journalDir = defaultJournalDir();
    const bool inPlace = opt.mode == SdkFixMode::InPlace;
    if (ctx.progress)
        ctx.progress->beginPhase(0, 0);

    // ---- Scan ----------------------------------------------------------------------
    const SdkPair* pair = sdkPair(opt.targetSdk);
    if (!pair)
        return makeError(Code::TargetSdkInvalid, QStringLiteral("Target SDK %1 is not supported. Choose SDK %2 to %3.")
                                                     .arg(opt.targetSdk)
                                                     .arg(kMinTargetSdk)
                                                     .arg(kMaxTargetSdk));
    const QFileInfo fi(opt.folder);
    if (opt.folder.trimmed().isEmpty() || !fi.isDir())
        return makeError(Code::NotAGame, QStringLiteral("The folder %1 does not exist.").arg(native(opt.folder)));
    const QString folder = QDir::cleanPath(fi.absoluteFilePath());
    const QString name = QFileInfo(folder).fileName();
    const QString unchanged = inPlace ? QStringLiteral("The folder was not changed.")
                                      : QStringLiteral("Nothing was written.");
    const auto cancelled = [&]() { return makeError(Code::Cancelled, QStringLiteral("Cancelled. %1").arg(unchanged)); };

    if (inPlace) {
        if (opt.originalsDir.trimmed().isEmpty())
            return makeError(Code::ZipFolderInvalid,
                             QStringLiteral("Choose the originals ZIP folder in Settings > Backport first: an in-place "
                                            "SDK fix saves the files it replaces there."));
        for (const PendingJournal& j : ApplyJournal::findInterrupted(opt.journalDir))
            if (samePath(j.game, folder))
                return makeError(Code::InterruptedApply,
                                 QStringLiteral("An earlier change to this folder was interrupted while it was being "
                                                "applied. Roll it back first (it is offered at start-up)."),
                                 j.path);
    }

    QStringList excludes = opt.excludeDirs;
    if (!opt.originalsDir.isEmpty())
        excludes << opt.originalsDir;
    for (QString& e : excludes)
        e = QDir::cleanPath(QFileInfo(e).absoluteFilePath());
    auto scanned = scanGame(folder, ctx.token, ScanMode::Full, excludes);
    if (!scanned.ok())
        return scanned.error().isCancelled() ? cancelled() : scanned.takeError();
    const ScanResult& scan = scanned.value();

    QStringList enc;
    for (const ExecFile* e : scan.encryptedOrCompressed())
        enc << e->rel;
    QStringList warnings;
    if (!enc.isEmpty()) {
        if (opt.encryptedPolicy == EncryptedPolicy::Refuse)
            return makeError(Code::SelfEncrypted,
                             QStringLiteral("No SDK fix was made: %1 %2 still encrypted or compressed. %3")
                                 .arg(enc.join(QStringLiteral(", ")), enc.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                      unchanged),
                             enc.join(QStringLiteral(", ")));
        warnings << QStringLiteral("Left unchanged because encrypted or compressed: %1.").arg(enc.join(QStringLiteral(", ")));
    }
    QStringList problems;
    Code firstProblem = Code::Ok;
    for (const ExecFile& e : scan.executables) {
        if (e.area != Area::Game || e.isEncryptedOrCompressed() || e.problemCode == Code::Ok)
            continue;
        problems << QStringLiteral("%1: %2.").arg(e.rel, e.problem);
        if (firstProblem == Code::Ok)
            firstProblem = e.problemCode;
    }
    if (!problems.isEmpty())
        return makeError(firstProblem, QStringLiteral("No SDK fix was made: %1 %2").arg(problems.first(), unchanged),
                         problems.join(QStringLiteral(" | ")));
    int toRewrite = 0;
    for (const ExecFile* e : scan.processable())
        if (!enc.contains(e->rel))
            ++toRewrite;
    if (toRewrite == 0)
        return makeError(Code::NothingToDo,
                         QStringLiteral("No SDK fix is needed: %1 has no executable this tool can rewrite.").arg(native(folder)));

    const GameMetadata meta = readGameMetadata(folder);
    const QString prefix = zipPrefix(meta, name);
    const QString titleId = manifestTitleId(meta, folder);
    if (const auto sdk = scan.gameSdk(); sdk && opt.targetSdk > *sdk)
        warnings << QStringLiteral("SDK %1 is higher than the SDK %2 that eboot.bin has now; the result only loads on "
                                   "FW %1.00 and later.")
                        .arg(opt.targetSdk)
                        .arg(*sdk);
    QMap<QString, QString> paramBefore;
    for (const QString& rel : scan.fileIndex) {
        if (!isParamFile(rel))
            continue;
        auto h = sha256File(joinPath(folder, rel), ctx.token);
        if (!h.ok())
            return h.error().isCancelled() ? cancelled() : h.takeError();
        paramBefore.insert(rel, h.value());
    }

    // Destinations and free space (S10, S12).
    QString outDir, dest;
    FileTree tree;
    if (!inPlace) {
        outDir = opt.outputDir.trimmed().isEmpty() ? QFileInfo(folder).absolutePath()
                                                    : QDir::cleanPath(QFileInfo(opt.outputDir).absoluteFilePath());
        if (!QFileInfo(outDir).isDir())
            return makeError(Code::ZipFolderInvalid, QStringLiteral("The output folder %1 does not exist.").arg(native(outDir)));
        if (isSameOrInside(outDir, folder))
            return makeError(Code::ZipFolderInvalid,
                             QStringLiteral("The copy can't be made inside the folder it is copied from (%1).").arg(native(outDir)));
        dest = freeFolder(QDir(outDir).filePath(sdkFixCopyName(name, opt.targetSdk)));
        ScanOptions so;
        for (const QString& ex : excludesBelow(folder, excludes))
            so.excludes << QDir(folder).relativeFilePath(ex);
        auto tr = scanTree(folder, ctx.token, so);
        if (!tr.ok())
            return tr.error().isCancelled() ? cancelled() : tr.takeError();
        tree = tr.value();
    } else {
        const QString why = zipDestinationProblem(opt.originalsDir, folder, sdkFixBackupZipName(prefix, kMaxTargetSdk));
        if (!why.isEmpty())
            return makeError(Code::ZipFolderInvalid, QStringLiteral("Originals ZIP folder %1: %2").arg(native(opt.originalsDir), why));
    }
    {
        const qint64 exec = scan.processableBytes();
        struct Need {
            QString path, use;
            qint64 bytes;
        };
        std::vector<Need> needs{{ctx.tempDir, QStringLiteral("staging"), 3 * exec}};
        if (inPlace) {
            qint64 largest = 0;
            for (const ExecFile* e : scan.processable())
                largest = std::max(largest, e->size);
            needs.push_back({opt.originalsDir, QStringLiteral("backup ZIP"), qint64(1.1 * double(exec))});
            needs.push_back({folder, QStringLiteral("folder"), largest + 64 * 1024 * qint64(toRewrite)});
        } else {
            needs.push_back({outDir, QStringLiteral("copy"), tree.totalBytes + exec});
        }
        QMap<QString, QPair<qint64, qint64>> byVolume; // root → need, available
        QMap<QString, QStringList> uses;
        for (const Need& n : needs) {
            auto ds = opt.diskSpaceProbe ? opt.diskSpaceProbe(n.path) : diskSpace(n.path);
            if (!ds.ok())
                continue;
            auto& v = byVolume[ds->rootPath];
            v.first += n.bytes;
            v.second = ds->available;
            uses[ds->rootPath] << n.use;
        }
        for (auto it = byVolume.cbegin(); it != byVolume.cend(); ++it)
            if (it.value().second < it.value().first + opt.freeSpaceMarginBytes)
                return makeError(Code::InsufficientSpace,
                                 QStringLiteral("Not enough space on %1 for the %2: about %3 needed (with the free-space "
                                                "reserve), %4 free.")
                                     .arg(native(it.key()), uses.value(it.key()).join(QStringLiteral(", ")),
                                          formatBytes(it.value().first + opt.freeSpaceMarginBytes),
                                          formatBytes(it.value().second)));
    }

    t.planned = true;
    t.title = meta.title;
    t.titleId = titleId;
    t.inputs.insert(QStringLiteral("folder"), native(folder));
    t.inputs.insert(QStringLiteral("titleId"), meta.titleId);
    t.inputs.insert(QStringLiteral("title"), meta.title);
    t.inputs.insert(QStringLiteral("version"), meta.version);
    if (const auto sdk = scan.gameSdk())
        t.inputs.insert(QStringLiteral("ebootSdkBefore"), *sdk);
    t.inputs.insert(QStringLiteral("mode"), inPlace ? QStringLiteral("in place (backup ZIP + journal)") : QStringLiteral("copy"));
    if (!inPlace)
        t.inputs.insert(QStringLiteral("output"), native(dest));
    t.inputs.insert(QStringLiteral("originalsZipFolder"), native(opt.originalsDir));
    t.inputs.insert(QStringLiteral("fakelib"), QStringLiteral("off (never added)"));
    t.inputs.insert(QStringLiteral("nidPatchMode"), QStringLiteral("off"));
    t.inputs.insert(QStringLiteral("verifyBuild"), opt.verifyBuild);
    t.target.insert(QStringLiteral("sdk"), pair->n);
    t.target.insert(QStringLiteral("label"), firmwareLabel(pair->n));
    t.target.insert(QStringLiteral("ps5Sdk"), QStringLiteral("0x%1").arg(pair->ps5, 8, 16, QLatin1Char('0')));
    t.target.insert(QStringLiteral("ps4Sdk"), QStringLiteral("0x%1").arg(pair->ps4, 8, 16, QLatin1Char('0')));
    for (const ExecFile& e : scan.executables)
        if (e.area != Area::Game) {
            QJsonObject s;
            s.insert(QStringLiteral("file"), e.rel);
            s.insert(QStringLiteral("why"), e.area == Area::Fakelib ? QStringLiteral("In fakelib/: never processed.")
                                                                     : QStringLiteral("In decrypted/: never processed."));
            t.skipped.append(s);
        }
    ctx.log.info(QStringLiteral("SDK fix of %1 to SDK %2 (%3)").arg(native(folder)).arg(opt.targetSdk).arg(inPlace ? QStringLiteral("in place") : QStringLiteral("copy")));

    // ---- Patch -----------------------------------------------------------------------
    BackportPlan plan;
    plan.options.game = folder;
    plan.options.targetSdk = opt.targetSdk;
    plan.options.nidMode = NidMode::Off; // S24
    plan.options.verifyBuild = opt.verifyBuild;
    plan.options.encryptedPolicy = opt.encryptedPolicy;
    plan.options.testHook = opt.testHook;
    plan.scan = scan;
    plan.pair = pair;
    plan.leaveUnchanged = enc;
    const QString staging = QDir(ctx.tempDir).filePath(QStringLiteral("staging"));
    if (QFileInfo::exists(staging))
        FW_TRY(removeTreeSafely(staging, ctx.tempDir));
    auto built = buildBackport(plan, staging, ctx);
    if (!built.ok()) {
        t.verification.insert(QStringLiteral("build"), QStringLiteral("failed: %1").arg(built.error().message));
        if (built.error().isCancelled())
            return cancelled();
        Error e = built.takeError();
        return makeError(codeOf(e), QStringLiteral("No SDK fix was made: %1 %2").arg(e.message, unchanged), e.detail);
    }
    BuildResult& build = built.value();
    for (const FileResult& f : build.files)
        t.files.append(fileJson(f, opt.targetSdk));
    if (build.failed()) {
        const FileResult* f = build.firstFailure();
        t.verification.insert(QStringLiteral("build"), QStringLiteral("failed: %1").arg(f->message));
        return makeError(f->code == Code::Ok ? Code::VerifyFailed : f->code,
                         QStringLiteral("No SDK fix was made: %1 %2").arg(f->message, unchanged));
    }
    t.verification.insert(QStringLiteral("build"), opt.verifyBuild
                                                       ? QStringLiteral("passed: structure, round trip and SDK of every signed executable (V1-V3)")
                                                       : QStringLiteral("V1-V3 turned off (backport.verifyBuild)"));

    SdkFixResult result;
    result.warnings = warnings;
    result.leftUnchanged = build.leftUnchanged;
    std::vector<const StagedFile*> changed;
    QMap<QString, const StagedFile*> changedByKey;
    QMap<QString, const StagedFile*> originalByKey;
    for (const StagedFile& o : build.originals)
        originalByKey.insert(relKey(o.rel), &o);
    for (const StagedFile& s : build.staged) {
        const StagedFile* o = originalByKey.value(relKey(s.rel));
        if (o && o->sha256 == s.sha256) {
            ++result.unchanged;
            continue;
        }
        changed.push_back(&s);
        changedByKey.insert(relKey(s.rel), &s);
    }
    result.rewritten = int(changed.size());
    // S24: the libc import rename stays off; check that no pattern was introduced.
    for (const StagedFile* s : changed) {
        const StagedFile* o = originalByKey.value(relKey(s->rel));
        auto in = readFileBytes(o->path);
        auto out = readFileBytes(s->path);
        if (!in.ok() || !out.ok())
            return makeError(Code::Unreadable, QStringLiteral("No SDK fix was made: %1 could not be read back. %2").arg(s->rel, unchanged));
        for (const NidSubstitution& n : nidSubstitutions())
            for (const QByteArray& pattern : {n.from, n.to})
                if (countOf(out.value(), pattern) > countOf(in.value(), pattern))
                    return makeError(Code::VerifyFailed,
                                     QStringLiteral("No SDK fix was made: %1 would gain a libc import pattern. %2").arg(s->rel, unchanged));
    }
    t.verification.insert(QStringLiteral("libcImports"), QStringLiteral("no import pattern introduced (rename off)"));
    if (changed.empty()) {
        result.nothingToDo = true;
        result.output = folder;
        result.summary = QStringLiteral("No SDK fix is needed: every executable already is the SDK %1 build. %2")
                             .arg(opt.targetSdk)
                             .arg(unchanged);
        return result;
    }

    // ---- Write / Verify: copy ----------------------------------------------------------
    if (!inPlace) {
        CopyGuard guard{dest + QStringLiteral(".fwpart"), outDir};
        const QString work = guard.work;
        if (!QDir().mkpath(work))
            return makeError(Code::ApplyFailed, QStringLiteral("Could not create %1. %2").arg(native(work), unchanged));
        if (ctx.progress)
            ctx.progress->beginPhase(2, tree.totalBytes, tree.fileCount);
        for (const FileEntry& e : tree.entries) {
            if (!ctx.token.checkpoint())
                return makeError(Code::Cancelled, QStringLiteral("Cancelled. The half-written copy was removed; %1 was not changed.")
                                                      .arg(native(folder)));
            const QString dst = joinPath(work, e.relativePath);
            if (e.isDir) {
                if (!QDir().mkpath(dst))
                    return makeError(Code::ApplyFailed, QStringLiteral("Could not create %1. The half-written copy was removed.").arg(native(dst)));
                continue;
            }
            if (opt.testHook)
                if (Status st = opt.testHook(QStringLiteral("copy.file"), e.relativePath); !st.ok())
                    return makeError(Code::ApplyFailed,
                                     QStringLiteral("The copy failed at %1: %2 The half-written copy was removed; %3 was not changed.")
                                         .arg(e.relativePath, st.error().message, native(folder)));
            const StagedFile* s = changedByKey.value(relKey(e.relativePath));
            const QString src = s ? s->path : joinPath(folder, e.relativePath);
            QDir().mkpath(QFileInfo(dst).absolutePath());
            if (Status st = copyFileStreaming(src, dst, ctx.token, ctx.progress); !st.ok()) {
                if (st.error().isCancelled())
                    return makeError(Code::Cancelled, QStringLiteral("Cancelled. The half-written copy was removed; %1 was not changed.")
                                                          .arg(native(folder)));
                return makeError(Code::ApplyFailed,
                                 QStringLiteral("The copy failed at %1: %2 The half-written copy was removed; %3 was not changed.")
                                     .arg(e.relativePath, st.error().message, native(folder)),
                                 st.error().detail);
            }
            if (ctx.progress)
                ctx.progress->advanceFiles(1);
        }
        // Verify: executables and param files by SHA-256, everything else by size.
        if (ctx.progress)
            ctx.progress->beginPhase(3, 0, tree.fileCount);
        QMap<QString, QString> expected;
        for (const ExecFile& e : scan.executables)
            if (!e.sha256.isEmpty())
                expected.insert(relKey(e.rel), e.sha256);
        for (const StagedFile* s : changed)
            expected.insert(relKey(s->rel), s->sha256);
        for (auto it = paramBefore.cbegin(); it != paramBefore.cend(); ++it)
            expected.insert(relKey(it.key()), it.value());
        int checked = 0;
        for (const FileEntry& e : tree.entries) {
            if (e.isDir)
                continue;
            if (!ctx.token.checkpoint())
                return makeError(Code::Cancelled, QStringLiteral("Cancelled. The half-written copy was removed; %1 was not changed.")
                                                      .arg(native(folder)));
            const QString dst = joinPath(work, e.relativePath);
            const StagedFile* s = changedByKey.value(relKey(e.relativePath));
            const qint64 size = s ? s->size : e.size;
            const QFileInfo dfi(dst);
            if (!dfi.isFile() || dfi.size() != size)
                return makeError(Code::VerifyFailed,
                                 QStringLiteral("%1 in the copy does not match. The copy was removed; %2 was not changed.")
                                     .arg(e.relativePath, native(folder)));
            const QString want = expected.value(relKey(e.relativePath));
            if (!want.isEmpty()) {
                auto h = sha256File(dst, ctx.token);
                if (!h.ok() || h.value() != want)
                    return makeError(Code::VerifyFailed,
                                     QStringLiteral("%1 in the copy does not match. The copy was removed; %2 was not changed.")
                                         .arg(e.relativePath, native(folder)));
                ++checked;
            }
            if (ctx.progress)
                ctx.progress->advanceFiles(1);
        }
        if (!QFileInfo::exists(joinPath(folder, QStringLiteral("fakelib"))) && QFileInfo::exists(joinPath(work, QStringLiteral("fakelib"))))
            return makeError(Code::VerifyFailed, QStringLiteral("The copy gained a fakelib folder; it was removed."));
        if (QFileInfo::exists(dest))
            return makeError(Code::ApplyFailed, QStringLiteral("%1 appeared while the copy was made; the copy was removed.").arg(native(dest)));
        if (!QDir().rename(work, dest))
            return makeError(Code::ApplyFailed, QStringLiteral("Could not rename %1 to %2; the copy was removed.")
                                                    .arg(QFileInfo(work).fileName(), QFileInfo(dest).fileName()));
        guard.keep = true;
        result.output = dest;
        t.verification.insert(QStringLiteral("copy"),
                              QStringLiteral("%1 files copied; %2 executables and param files checked by SHA-256, the "
                                             "others by size; no fakelib added")
                                  .arg(tree.fileCount)
                                  .arg(checked));
        t.verification.insert(QStringLiteral("source"), QStringLiteral("only read"));
        result.summary = QStringLiteral("Copied %1 to %2 with the SDK marker set to SDK %3 (loads on FW %3.00+): %4 "
                                        "files rewritten. The original folder was not changed.")
                             .arg(name, QFileInfo(dest).fileName())
                             .arg(opt.targetSdk)
                             .arg(result.rewritten);
        ctx.log.info(result.summary);
        return result;
    }

    // ---- Write / Verify: in place ------------------------------------------------------
    qint64 zipBytes = 0;
    std::vector<ZipMember> members;
    for (const StagedFile* s : changed) {
        const StagedFile* o = originalByKey.value(relKey(s->rel));
        members.push_back({o->rel, o->path, o->sha256});
        zipBytes += o->size;
    }
    if (ctx.progress)
        ctx.progress->beginPhase(2, 2 * zipBytes, qint64(changed.size()));
    const ZipHook zipHook = [&opt](const QString& point, const QString& item) -> Status {
        return opt.testHook ? opt.testHook(point, item) : success();
    };
    StagedZip zip(uniqueZipPath(QDir(opt.originalsDir).filePath(sdkFixBackupZipName(prefix, opt.targetSdk))));
    {
        Status st = zip.write(members, ctx.token, ctx.progress, zipHook);
        if (st.ok())
            st = zip.verify(ctx.token, ctx.progress);
        if (st.ok())
            st = zip.commit(zipHook);
        if (!st.ok()) {
            if (st.error().isCancelled())
                return cancelled();
            return makeError(Code::ZipWriteFailed,
                             QStringLiteral("No SDK fix was made: the backup ZIP could not be written (%1). %2")
                                 .arg(st.error().message, unchanged),
                             st.error().detail);
        }
    }
    result.backupZip = zip.finalPath();
    {
        QJsonObject z;
        z.insert(QStringLiteral("role"), QStringLiteral("backup of the replaced files"));
        z.insert(QStringLiteral("result"), QStringLiteral("written and verified"));
        z.insert(QStringLiteral("path"), native(zip.finalPath()));
        z.insert(QStringLiteral("sha256"), zip.result().zipSha256);
        z.insert(QStringLiteral("members"), zip.result().count);
        t.zips.append(z);
    }
    t.verification.insert(QStringLiteral("backupZip"),
                          QStringLiteral("read back before its rename: member count, CRC and size of every member, "
                                         "SHA-256 of every member against its source"));
    ctx.log.info(QStringLiteral("Backup ZIP written and verified: %1").arg(native(result.backupZip)));

    ApplyJournal::prune(opt.journalDir, opt.journalRetentionDays);
    std::vector<JournalEntry> entries;
    QMap<QString, QPair<QString, qint64>> filesAfter;
    for (const StagedFile* s : changed) {
        const StagedFile* o = originalByKey.value(relKey(s->rel));
        JournalEntry e;
        e.rel = scan.resolve(s->rel).isEmpty() ? s->rel : scan.resolve(s->rel);
        e.action = ApplyAction::Replace;
        e.sha256Before = o->sha256;
        e.sha256After = s->sha256;
        e.sizeAfter = s->size;
        e.stagedPath = s->path;
        e.backupZipMember = o->rel;
        e.backupStagingPath = o->path;
        entries.push_back(e);
        filesAfter.insert(e.rel, qMakePair(s->sha256, s->size));
    }
    const QString id = (ctx.jobId.isEmpty() ? QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmsszzz"))
                                            : sanitizeFileName(ctx.jobId))
        + QStringLiteral("-sdkfix");
    QString journalPath = QDir(opt.journalDir).filePath(id + QStringLiteral(".json"));
    for (int n = 2; QFileInfo::exists(journalPath); ++n)
        journalPath = QDir(opt.journalDir).filePath(QStringLiteral("%1-%2.json").arg(id).arg(n));
    QDir().mkpath(opt.journalDir);
    auto journal = ApplyJournal::create(journalPath, ctx.jobId, folder, result.backupZip, entries);
    if (!journal.ok())
        return makeError(Code::ApplyFailed, QStringLiteral("The SDK fix was not applied: %1 Backup: %2")
                                                .arg(journal.error().message, native(result.backupZip)));
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
    if (Status st = journal->apply(ctx.token, ctx.progress, hook); !st.ok()) {
        t.verification.insert(QStringLiteral("apply"), QStringLiteral("failed: %1").arg(st.error().message));
        if (codeOf(st.error()) == Code::Cancelled)
            return makeError(Code::Cancelled, QStringLiteral("%1 Backup: %2").arg(st.error().message, native(result.backupZip)));
        return makeError(codeOf(st.error()), QStringLiteral("The SDK fix was not applied: %1 Backup: %2")
                                                 .arg(st.error().message, native(result.backupZip)),
                         st.error().detail);
    }
    t.verification.insert(QStringLiteral("apply"), QStringLiteral("every replaced file holds the new bytes (V7)"));

    if (ctx.progress)
        ctx.progress->beginPhase(3, 0, qint64(paramBefore.size()));
    for (auto it = paramBefore.cbegin(); it != paramBefore.cend(); ++it) {
        auto h = sha256File(joinPath(folder, it.key()), CancellationToken());
        if (!h.ok() || h.value() != it.value())
            result.warnings << QStringLiteral("%1 changed while the SDK fix ran (this tool never writes it).").arg(it.key());
    }
    t.verification.insert(QStringLiteral("paramFiles"), QStringLiteral("%1 param.json/param.sfo checked unchanged").arg(paramBefore.size()));

    // The record that undoes the fix, kept with the game's backup records.
    const QString recordDir = sdkFixRecordDir(opt.originalsDir, folder);
    if (!recordDir.isEmpty() && QDir().mkpath(recordDir)) {
        QString record = QDir(recordDir).filePath(reportBaseName(QStringLiteral("SDK %1 fix").arg(opt.targetSdk)) + QStringLiteral(".json"));
        for (int n = 2; QFileInfo::exists(record); ++n)
            record = QDir(recordDir).filePath(reportBaseName(QStringLiteral("SDK %1 fix (%2)").arg(opt.targetSdk).arg(n)) + QStringLiteral(".json"));
        if (QFile::copy(journalPath, record))
            result.recordPath = record;
    }
    if (result.recordPath.isEmpty())
        result.warnings << QStringLiteral("The undo record could not be saved with the backup records; the fix can still "
                                          "be undone from %1 (journal %2).")
                               .arg(native(result.backupZip), native(journalPath));
    const ManifestSet records = findManifests(opt.originalsDir, titleId, folder);
    if (records.own) {
        Manifest m = *records.own;
        recordSdkFix(m, {opt.targetSdk, result.backupZip, zip.result().zipSha256, result.recordPath, filesAfter});
        if (Status st = writeManifest(m); !st.ok())
            result.warnings << QStringLiteral("The backport manifest could not be updated: %1").arg(st.error().message);
    }
    result.output = folder;
    result.summary = QStringLiteral("Set the SDK marker of %1 files in %2 to SDK %3 (loads on FW %3.00+). Backup: %4. "
                                    "Undo it with Undo the last SDK fix in the game's menu on the Backport page.")
                         .arg(result.rewritten)
                         .arg(name)
                         .arg(opt.targetSdk)
                         .arg(QFileInfo(result.backupZip).fileName());
    ctx.log.info(result.summary);
    return result;
}

} // namespace

QStringList sdkFixPhases()
{
    return {QStringLiteral("Scan"), QStringLiteral("Patch"), QStringLiteral("Write"), QStringLiteral("Verify")};
}

QString sdkFixCopyName(const QString& folderName, int targetSdk)
{
    QString n = folderName;
    n.remove(QRegularExpression(QStringLiteral("\\bFW\\s*\\d+(?:\\.\\d+)?\\s*xx\\b"), QRegularExpression::CaseInsensitiveOption));
    n.remove(QRegularExpression(QStringLiteral("\\bSDK\\s*\\d+\\b"), QRegularExpression::CaseInsensitiveOption));
    static const QRegularExpression copySuffix(QStringLiteral("\\s*-\\s*Copy(?:\\s*\\(\\d+\\))?\\s*$"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression countSuffix(QStringLiteral("\\s*\\(\\d+\\)\\s*$"));
    for (int i = 0; i < 3; ++i) {
        n.remove(copySuffix);
        n.remove(countSuffix);
    }
    n = n.simplified();
    while (!n.isEmpty() && QStringLiteral(" -_.").contains(n.back()))
        n.chop(1);
    while (!n.isEmpty() && QStringLiteral(" -_.").contains(n.front()))
        n.remove(0, 1);
    if (n.isEmpty())
        n = QStringLiteral("Game");
    return QStringLiteral("%1 SDK %2").arg(n).arg(targetSdk);
}

QString sdkFixBackupZipName(const QString& prefix, int targetSdk)
{
    return QStringLiteral("%1 SDK %2 fix original files.zip").arg(prefix).arg(targetSdk);
}

QString sdkFixRecordDir(const QString& originalsDir, const QString& game)
{
    if (originalsDir.trimmed().isEmpty() || !QFileInfo(originalsDir).isDir() || game.isEmpty())
        return {};
    const QString abs = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    const ManifestSet set = findManifests(originalsDir, manifestTitleId(readGameMetadata(abs), abs), abs);
    return QDir(QFileInfo(set.ownPath).absolutePath()).filePath(QStringLiteral("sdk-fixes"));
}

QString latestSdkFixRecord(const QString& originalsDir, const QString& game)
{
    const QString dir = sdkFixRecordDir(originalsDir, game);
    if (dir.isEmpty())
        return {};
    const QString abs = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QFileInfo& fi : files) {
        auto j = ApplyJournal::load(fi.absoluteFilePath());
        if (j.ok() && j->state() == JournalState::Done && samePath(j->game(), abs))
            return fi.absoluteFilePath();
    }
    return {};
}

Result<SdkFixResult> runSdkFix(const SdkFixOptions& opt, JobContext& ctx)
{
    FixTrace t;
    Result<SdkFixResult> r = runImpl(opt, ctx, t);
    if (!t.planned)
        return r;
    QJsonObject rep = newReport(QStringLiteral("sdkFix"));
    rep.insert(QStringLiteral("jobId"), ctx.jobId);
    rep.insert(QStringLiteral("title"), t.title);
    rep.insert(QStringLiteral("titleId"), t.titleId);
    QJsonObject outcome;
    if (r.ok()) {
        outcome.insert(QStringLiteral("status"), r->nothingToDo ? QStringLiteral("nothingToDo")
                                                 : r->leftUnchanged.isEmpty() ? QStringLiteral("succeeded")
                                                                              : QStringLiteral("partial"));
        outcome.insert(QStringLiteral("summary"), r->summary);
    } else {
        const Code c = codeOf(r.error());
        outcome.insert(QStringLiteral("status"), c == Code::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed"));
        outcome.insert(QStringLiteral("summary"), r.error().message);
        QJsonObject err;
        err.insert(QStringLiteral("code"), codeName(c));
        err.insert(QStringLiteral("detail"), r.error().detail);
        outcome.insert(QStringLiteral("error"), err);
    }
    rep.insert(QStringLiteral("outcome"), outcome);
    rep.insert(QStringLiteral("inputs"), t.inputs);
    rep.insert(QStringLiteral("target"), t.target);
    rep.insert(QStringLiteral("verification"), t.verification);
    rep.insert(QStringLiteral("zips"), t.zips);
    rep.insert(QStringLiteral("files"), t.files);
    rep.insert(QStringLiteral("skipped"), t.skipped);
    if (r.ok()) {
        QJsonObject out;
        out.insert(QStringLiteral("output"), native(r->output));
        if (!r->journalPath.isEmpty())
            out.insert(QStringLiteral("journal"), native(r->journalPath));
        if (!r->recordPath.isEmpty())
            out.insert(QStringLiteral("undoRecord"), native(r->recordPath));
        rep.insert(QStringLiteral("result"), out);
        rep.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(r->warnings));
    }
    const QString dir = !opt.reportDir.isEmpty() ? opt.reportDir : reportDirForGame(opt.originalsDir, opt.folder);
    auto written = writeReport(dir, reportBaseName(QStringLiteral("SDK %1 fix").arg(opt.targetSdk)), rep);
    if (!written.ok()) {
        ctx.log.warn(QStringLiteral("The SDK fix report could not be written: %1").arg(written.error().message));
        if (r.ok())
            r->warnings << QStringLiteral("The report could not be written: %1").arg(written.error().message);
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

JobOutcome toOutcome(const SdkFixResult& r)
{
    JobOutcome o;
    o.summary = r.summary;
    o.warnings = r.warnings;
    o.details = r.details;
    o.details.insert(QStringLiteral("output"), native(r.output));
    o.details.insert(QStringLiteral("rewritten"), r.rewritten);
    o.details.insert(QStringLiteral("unchanged"), r.unchanged);
    if (!r.backupZip.isEmpty())
        o.details.insert(QStringLiteral("backupZip"), native(r.backupZip));
    if (!r.recordPath.isEmpty())
        o.details.insert(QStringLiteral("undoRecord"), native(r.recordPath));
    if (!r.nothingToDo)
        o.outputs << r.output;
    if (!r.backupZip.isEmpty())
        o.outputs << r.backupZip;
    if (r.nothingToDo) {
        o.verification = Verification::Skipped;
        o.verificationDetail = QStringLiteral("Nothing to verify: no file changed.");
    } else if (!r.leftUnchanged.isEmpty()) {
        o.verification = Verification::Inconclusive;
        o.verificationDetail = QStringLiteral("%1 files rewritten and checked; %2 left unchanged.").arg(r.rewritten).arg(r.leftUnchanged.size());
    } else {
        o.verification = Verification::Passed;
        o.verificationDetail = QStringLiteral("%1 files rewritten and checked; param files unchanged.").arg(r.rewritten);
    }
    return o;
}

} // namespace fw::backport
