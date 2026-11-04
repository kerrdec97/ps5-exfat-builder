#include "engines/backport/BackportEngine.h"

#include "core/FileUtil.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/ProFix.h"
#include "engines/backport/RightSprx.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfReader.h"
#include "engines/backport/SelfWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <cstring>

namespace fw::backport {

QString fileStageName(FileStage s)
{
    switch (s) {
    case FileStage::Snapshot: return QStringLiteral("snapshot");
    case FileStage::Unwrap: return QStringLiteral("unwrap");
    case FileStage::Patch: return QStringLiteral("SDK patch");
    case FileStage::Sign: return QStringLiteral("signing");
    case FileStage::Nid: return QStringLiteral("NID patch");
    case FileStage::Verify: return QStringLiteral("verification");
    case FileStage::Stage: return QStringLiteral("staging");
    case FileStage::Done: return QStringLiteral("done");
    case FileStage::LeftUnchanged: return QStringLiteral("left unchanged");
    }
    return QStringLiteral("unknown");
}

bool BuildResult::failed() const
{
    return firstFailure() != nullptr;
}

const FileResult* BuildResult::firstFailure() const
{
    for (const FileResult& f : files)
        if (!f.ok)
            return &f;
    return nullptr;
}

int BuildResult::okCount() const
{
    int n = 0;
    for (const FileResult& f : files)
        if (f.ok && f.stage == FileStage::Done)
            ++n;
    return n;
}

QString stagingOriginalsDir(const QString& s) { return QDir(s).filePath(QStringLiteral("originals")); }
QString stagingUnwrappedDir(const QString& s) { return QDir(s).filePath(QStringLiteral("unwrapped")); }
QString stagingStageDir(const QString& s) { return QDir(s).filePath(QStringLiteral("stage")); }

namespace {

Error verifyError(const QString& why)
{
    return makeError(Code::VerifyFailed, QStringLiteral("Build verification failed: %1.").arg(why));
}

bool rangesEqual(const QByteArray& a, const QByteArray& b, quint64 off, quint64 len)
{
    if (off > quint64(a.size()) || len > quint64(a.size()) - off)
        return false;
    if (off > quint64(b.size()) || len > quint64(b.size()) - off)
        return false;
    return std::memcmp(a.constData() + off, b.constData() + off, size_t(len)) == 0;
}

FileStage stageFor(Code c)
{
    switch (c) {
    case Code::SelfEncrypted:
    case Code::SelfCompressed:
    case Code::SelfMalformed: return FileStage::Unwrap;
    case Code::ParamMissing:
    case Code::ParamSizeInvalid:
    case Code::ParamMagicInvalid:
    case Code::ParamOutOfRange: return FileStage::Patch;
    case Code::ElfInvalid:
    case Code::SignUnsupported: return FileStage::Sign;
    case Code::VerifyFailed: return FileStage::Verify;
    default: return FileStage::Stage;
    }
}

} // namespace

Status verifySignedExecutable(const QByteArray& output, const QByteArray& patchedElf, const NidResult& nid,
                              const SdkPair& pair)
{
    // V1: structure.
    auto info = selfio::inspect(output);
    if (!info.ok())
        return verifyError(info.error().message);
    if (info->kind != SelfKind::FakeSigned)
        return verifyError(QStringLiteral("the signed file reads back as %1 (%2)").arg(selfKindName(info->kind), info->problem));
    auto elf = ElfImage::parse(patchedElf, Strictness::Signable);
    if (!elf.ok())
        return verifyError(elf.error().message);
    std::vector<const ProgramHeader*> q;
    for (const ProgramHeader& ph : elf->programHeaders())
        if (elf::isSelfSegmentType(ph.type))
            q.push_back(&ph);
    if (info->entries.size() != q.size() * 2)
        return verifyError(QStringLiteral("%1 SELF entries for %2 segments").arg(info->entries.size()).arg(q.size()));
    quint64 last = quint64(info->headerSize) + info->metaSize;
    for (const SelfEntry& e : info->entries) {
        if (e.offset % 16 != 0 || e.offset < last)
            return verifyError(QStringLiteral("SELF entry %1 is out of order or unaligned").arg(e.index));
        last = self::alignUp(e.offset + e.filesz, 16);
    }
    if (info->fileSize != last)
        return verifyError(QStringLiteral("the SELF file size field is 0x%1, expected 0x%2")
                               .arg(info->fileSize, 0, 16)
                               .arg(last, 0, 16));

    // V2: round trip (NID substitution undone; it was exact by construction).
    QByteArray copy = output;
    reverseNid(copy, nid);
    auto unwrapped = selfio::unwrap(copy);
    if (!unwrapped.ok())
        return verifyError(unwrapped.error().message);
    const QByteArray& u = unwrapped.value();
    if (u.size() < elf::kEhdrSize)
        return verifyError(QStringLiteral("the unwrapped header is truncated"));
    QByteArray ha = ElfImage::serializeHeader(ElfImage::readHeader(u));
    ElfHeader hp = ElfImage::readHeader(patchedElf);
    hp.shnum = 0;
    if (ha != ElfImage::serializeHeader(hp))
        return verifyError(QStringLiteral("the ELF header does not survive signing"));
    const quint64 table = quint64(hp.phnum) * elf::kPhdrSize;
    if (!rangesEqual(u, patchedElf, hp.phoff, table))
        return verifyError(QStringLiteral("the program headers do not survive signing"));
    for (const ProgramHeader& ph : elf->programHeaders()) {
        if (ph.filesz == 0 || (!elf::isSelfSegmentType(ph.type) && ph.type != elf::PT_SCE_VERSION))
            continue;
        if (!rangesEqual(u, patchedElf, ph.offset, ph.filesz))
            return verifyError(QStringLiteral("segment %1 differs after signing").arg(ph.index));
    }

    // V3: the target SDK is in every param segment of the signed file.
    auto expected = findParams(patchedElf);
    if (!expected.ok())
        return verifyError(expected.error().message);
    if (!expected->params.empty()) {
        auto got = findParams(u);
        if (!got.ok())
            return verifyError(QStringLiteral("the SDK param is not inside a signed segment (%1)").arg(got.error().message));
        for (const ParamInfo& p : expected->params) {
            bool found = false;
            for (const ParamInfo& g : got->params)
                if (g.offset == p.offset && g.ps5 == pair.ps5 && g.ps4 == pair.ps4)
                    found = true;
            if (!found)
                return verifyError(QStringLiteral("the param at 0x%1 does not hold SDK %2 after signing")
                                       .arg(p.offset, 0, 16)
                                       .arg(pair.n));
        }
    }
    return success();
}

Result<ProcessedExecutable> processExecutable(const QByteArray& input, const SdkPair& pair, NidMode nidMode,
                                              bool verify, QStringList* warnings, const ExecutableFixes& fixes)
{
    ProcessedExecutable p;
    if (elf::hasElfMagic(input)) {
        p.elf = input;
    } else if (selfio::hasSelfMagic(input)) {
        FW_TRY_ASSIGN(p.elf, selfio::unwrap(input, warnings));
    } else {
        return makeError(Code::ElfInvalid, QStringLiteral("it is not an ELF or SELF file"));
    }
    FW_TRY_ASSIGN(p.patch, patchSdk(p.elf, pair));
    if (fixes.criAudioRack) {
        FW_TRY_ASSIGN(p.criRack, applyCriRackFix(p.elf));
        if (p.criRack.state == CriRackState::Patch)
            p.patch.messages << QStringLiteral("%1 at 0x%2: 12-channel speaker rack -> 8 channels")
                                    .arg(QLatin1String(kCriRackFamily))
                                    .arg(p.criRack.va, 0, 16);
        else if (p.criRack.state == CriRackState::Unknown && warnings)
            *warnings << QStringLiteral("CRI audio fix not applied: %1 (the bytes do not match the confirmed "
                                        "layout, so nothing was changed)")
                             .arg(p.criRack.why);
    }
    if (fixes.shaderTrapNops) {
        FW_TRY_ASSIGN(p.shaderTrapNops, applyShaderTrapNops(p.elf));
        if (p.shaderTrapNops > 0)
            p.patch.messages << QStringLiteral("%1: %2 shader-assert trap(s) turned into s_nop 0 (PS5 Pro fix)")
                                    .arg(QLatin1String(kShaderTrapFamily))
                                    .arg(p.shaderTrapNops);
        else
            p.patch.messages << QStringLiteral("PS5 Pro fix: no guarded shader-assert trap found (nothing to change)");
    }
    FW_TRY_ASSIGN(p.output, makeFakeSelf(p.elf));
    p.nid = applyNid(p.output, pair.n, nidMode);
    if (verify)
        FW_TRY(verifySignedExecutable(p.output, p.elf, p.nid, pair));
    return p;
}

Result<BuildResult> buildBackport(const BackportPlan& plan, const QString& stagingDir, JobContext& ctx)
{
    const BackportOptions& opt = plan.options;
    const QString game = opt.game;
    BuildResult r;
    r.stagingDir = stagingDir;
    const QString origDir = stagingOriginalsDir(stagingDir);
    const QString unwrapDir = stagingUnwrappedDir(stagingDir);
    const QString stageDir = stagingStageDir(stagingDir);
    for (const QString& d : {origDir, unwrapDir, stageDir})
        if (!QDir().mkpath(d))
            return makeError(Code::Unreadable, QStringLiteral("Could not create the staging folder %1.").arg(QDir::toNativeSeparators(d)));
    if (!plan.pair)
        return makeError(Code::TargetSdkInvalid, QStringLiteral("No SDK pair was chosen."));
    const SdkPair& pair = *plan.pair;

    const auto processable = plan.scan.processable();
    qint64 total = 0;
    for (const ExecFile* e : processable)
        total += e->size;
    if (plan.fakelib)
        for (const QString& t : plan.stagedFakelibTargets)
            if (const FakelibLib* l = plan.fakelib->find(t.section(u'/', -1)))
                total += l->size;
    if (ctx.progress)
        ctx.progress->beginPhase(1, total, qint64(processable.size() + size_t(plan.stagedFakelibTargets.size())));

    // Encrypted files carried over by policy (S32 leaveUnchanged): recorded,
    // never read into the build.
    for (const QString& rel : plan.leaveUnchanged) {
        FileResult fr;
        fr.rel = rel;
        fr.ok = true;
        fr.stage = FileStage::LeftUnchanged;
        r.leftUnchanged << rel;
        r.files.push_back(fr);
    }

    for (const ExecFile* e : processable) {
        if (!ctx.token.checkpoint())
            return Error::cancelled();
        FileResult fr;
        fr.rel = e->rel;
        fr.sizeBefore = e->size;
        fr.sdkBefore = e->sdk;
        if (plan.leaveUnchanged.contains(e->rel)) {
            fr.ok = true;
            fr.stage = FileStage::LeftUnchanged;
            r.leftUnchanged << e->rel;
            r.files.push_back(fr);
            continue;
        }
        if (e->rel == plan.rightSprxSubstitute)
            continue; // #35: staged from the standard copy below
        if (ctx.progress)
            ctx.progress->setCurrentItem(e->rel);
        if (opt.testHook) {
            if (Status st = opt.testHook(QStringLiteral("build.file"), e->rel); !st.ok()) {
                if (st.error().isCancelled())
                    return st.takeError();
                fr.code = codeOf(st.error());
                fr.message = st.error().message;
                r.files.push_back(fr);
                continue;
            }
        }

        // Snapshot (S13): a copy error stops the run.
        auto data = readFileBytes(joinPath(game, e->rel));
        if (!data.ok())
            return makeError(codeOf(data.error()),
                             QStringLiteral("Could not copy %1 before patching: %2").arg(e->rel, data.error().message),
                             data.error().detail);
        fr.shaBefore = sha256Hex(data.value());
        if (!e->sha256.isEmpty() && fr.shaBefore != e->sha256) {
            fr.code = Code::GameChanged;
            fr.message = QStringLiteral("%1 changed while the backport was being prepared.").arg(e->rel);
            r.files.push_back(fr);
            continue;
        }
        const QString snap = joinPath(origDir, e->rel);
        if (Status st = writeFileBytes(snap, data.value()); !st.ok())
            return makeError(Code::Unreadable,
                             QStringLiteral("Could not copy %1 to the staging folder: %2").arg(e->rel, st.error().message),
                             st.error().detail);
        r.originals.push_back({e->rel, snap, fr.shaBefore, e->size, true});

        // Unwrap → SDK patch → sign → NID → verify, all in memory.
        QStringList warnings;
        fr.stage = FileStage::Unwrap;
        ExecutableFixes fixes;
        // #32: the Pro fix touches only the root eboot.bin (as the Backport GUI).
        fixes.shaderTrapNops = opt.proFix && relKey(e->rel) == QLatin1String("eboot.bin");
        fixes.criAudioRack = opt.criAudioFix; // only a CRI Atom module with the exact layout changes
        auto proc = processExecutable(data.value(), pair, opt.nidMode, opt.verifyBuild, &warnings, fixes);
        for (const QString& w : std::as_const(warnings))
            r.warnings << QStringLiteral("%1: %2").arg(e->rel, w);
        if (!proc.ok()) {
            fr.code = codeOf(proc.error());
            fr.stage = stageFor(fr.code);
            fr.message = QStringLiteral("%1: %2").arg(e->rel, proc.error().message);
            if (fr.code == Code::ParamMagicInvalid)
                fr.message += QStringLiteral(" (the file may still be encrypted)");
            ctx.log.warn(fr.message);
            r.files.push_back(fr);
            continue;
        }
        if (e->cls == FileClass::Self) {
            // The unwrapped ELF (before the SDK patch), for diagnosis only.
            auto un = selfio::unwrap(data.value());
            if (un.ok())
                (void)writeFileBytes(joinPath(unwrapDir, e->rel), un.value());
        }
        fr.patchMessages = proc->patch.messages;
        fr.nid = proc->nid;
        fr.shaderTrapNops = proc->shaderTrapNops;
        r.shaderTrapNops += proc->shaderTrapNops;
        fr.criRackPatched = proc->criRack.state == CriRackState::Patch;
        if (fr.criRackPatched)
            r.criRackPatched << e->rel;
        if (!proc->patch.patched.empty())
            fr.sdkAfter = pair.n;
        fr.stage = FileStage::Stage;
        // #41 fix: a module the game loads under another letter case is staged
        // (and so zipped and applied) under the game's spelling.
        QString outRel = e->rel;
        for (const ModuleCaseRename& m : plan.moduleCase.renames)
            if (m.from == e->rel)
                outRel = m.to;
        if (outRel != e->rel) {
            fr.renamedTo = outRel;
            fr.patchMessages << QStringLiteral("Module name case: written as %1, the spelling the game loads").arg(outRel);
        }
        const QString out = joinPath(stageDir, outRel);
        if (Status st = writeFileBytes(out, proc->output); !st.ok())
            return makeError(Code::Unreadable,
                             QStringLiteral("Could not write %1 to the staging folder: %2").arg(e->rel, st.error().message),
                             st.error().detail);
        fr.shaAfter = sha256Hex(proc->output);
        fr.sizeAfter = proc->output.size();
        fr.ok = true;
        fr.stage = FileStage::Done;
        for (const QString& m : std::as_const(fr.patchMessages))
            ctx.log.debug(QStringLiteral("%1: %2").arg(e->rel, m));
        if (fr.nid.action == NidAction::Applied || fr.nid.action == NidAction::Reverted)
            ctx.log.debug(QStringLiteral("%1: libc NID %2 (%3)").arg(e->rel, nidActionName(fr.nid.action)).arg(fr.nid.count));
        r.staged.push_back({outRel, out, fr.shaAfter, fr.sizeAfter, true});
        r.files.push_back(fr);
        if (ctx.progress) {
            ctx.progress->advance(e->size);
            ctx.progress->advanceFiles(1);
        }
    }

    // #35: the user's standard right.sprx in place of the dump's copy that
    // cannot be converted. The dump's copy is snapshotted like every
    // executable (originals ZIP, rollback, Restore); the standard copy goes
    // through the normal pipeline (unwrap, SDK patch, sign, V1-V3).
    if (!plan.rightSprxSubstitute.isEmpty()) {
        if (!ctx.token.checkpoint())
            return Error::cancelled();
        const QString rel = plan.rightSprxSubstitute;
        FileResult fr;
        fr.rel = rel;
        fr.substitutedStandard = true;
        auto own = readFileBytes(joinPath(game, rel));
        if (!own.ok())
            return makeError(codeOf(own.error()),
                             QStringLiteral("Could not copy %1 before replacing it: %2").arg(rel, own.error().message),
                             own.error().detail);
        fr.shaBefore = sha256Hex(own.value());
        fr.sizeBefore = own->size();
        const ExecFile* scanned = plan.scan.find(rel);
        auto standard = readFileBytes(opt.standardRightSprx, 1024 * 1024);
        if (scanned && !scanned->sha256.isEmpty() && fr.shaBefore != scanned->sha256) {
            fr.code = Code::GameChanged;
            fr.message = QStringLiteral("%1 changed while the backport was being prepared.").arg(rel);
        } else if (!standard.ok() || sha256Hex(standard.value()) != opt.standardRightSprxSha256.toLower()) {
            fr.code = Code::RightSprxInvalid;
            fr.message = QStringLiteral("The standard right.sprx changed or cannot be read: %1")
                             .arg(standardRightSprxProblem(opt.standardRightSprx, opt.standardRightSprxSha256));
        } else {
            const QString snap = joinPath(origDir, rel);
            if (Status st = writeFileBytes(snap, own.value()); !st.ok())
                return makeError(Code::Unreadable,
                                 QStringLiteral("Could not copy %1 to the staging folder: %2").arg(rel, st.error().message),
                                 st.error().detail);
            r.originals.push_back({rel, snap, fr.shaBefore, fr.sizeBefore, true});
            QStringList warnings;
            fr.stage = FileStage::Unwrap;
            auto proc = processExecutable(standard.value(), pair, opt.nidMode, opt.verifyBuild, &warnings);
            for (const QString& w : std::as_const(warnings))
                r.warnings << QStringLiteral("%1 (standard copy): %2").arg(rel, w);
            if (!proc.ok()) {
                fr.code = codeOf(proc.error());
                fr.stage = stageFor(fr.code);
                fr.message = QStringLiteral("%1 (standard copy): %2").arg(rel, proc.error().message);
            } else {
                const QString out = joinPath(stageDir, rel);
                if (Status st = writeFileBytes(out, proc->output); !st.ok())
                    return makeError(Code::Unreadable,
                                     QStringLiteral("Could not write %1 to the staging folder: %2").arg(rel, st.error().message),
                                     st.error().detail);
                fr.patchMessages = proc->patch.messages;
                fr.patchMessages << QStringLiteral("%1: the dump's copy cannot be converted (%2); the standard right.sprx "
                                                   "(SHA-256 %3) was converted in its place")
                                        .arg(QLatin1String(kRightSprxFamily), plan.rightSprxReason,
                                             opt.standardRightSprxSha256.left(8));
                fr.nid = proc->nid;
                fr.sdkBefore = proc->patch.sdkBefore();
                if (!proc->patch.patched.empty())
                    fr.sdkAfter = pair.n;
                fr.shaAfter = sha256Hex(proc->output);
                fr.sizeAfter = proc->output.size();
                fr.ok = true;
                fr.stage = FileStage::Done;
                r.staged.push_back({rel, out, fr.shaAfter, fr.sizeAfter, true});
                ctx.log.info(fr.patchMessages.back());
            }
        }
        if (!fr.ok)
            ctx.log.warn(fr.message);
        r.files.push_back(fr);
    }

    // Fakelib set: staged unchanged (never re-signed, D3), checked (V4).
    QSet<QString> sameAsGame;
    if (plan.fakelib) {
        for (const QString& target : plan.stagedFakelibTargets) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            const FakelibLib* lib = plan.fakelib->find(target.section(u'/', -1));
            if (!lib)
                continue;
            FileResult fr;
            fr.rel = target;
            fr.sizeBefore = lib->size;
            const QString existing = plan.scan.resolve(target);
            if (!existing.isEmpty()) {
                // Another tool's file that the set replaces: kept (S14).
                const QString snap = joinPath(origDir, existing);
                if (Status st = copyFileStreaming(joinPath(game, existing), snap, ctx.token); !st.ok()) {
                    if (st.error().isCancelled())
                        return st.takeError();
                    return makeError(Code::Unreadable,
                                     QStringLiteral("Could not copy %1 before replacing it: %2").arg(existing, st.error().message),
                                     st.error().detail);
                }
                auto h = sha256File(snap, ctx.token);
                if (!h.ok())
                    return h.takeError();
                fr.shaBefore = h.value();
                r.originals.push_back({existing, snap, h.value(), QFileInfo(snap).size(), false});
                if (h.value() == lib->sha256)
                    sameAsGame.insert(target);
            }
            const QString out = joinPath(stageDir, target);
            if (Status st = copyFileStreaming(lib->path, out, ctx.token); !st.ok()) {
                if (st.error().isCancelled())
                    return st.takeError();
                return makeError(Code::Unreadable,
                                 QStringLiteral("Could not stage %1: %2").arg(lib->name, st.error().message), st.error().detail);
            }
            auto h = sha256File(out, ctx.token);
            if (!h.ok())
                return h.takeError();
            if (h.value() != lib->sha256) {
                fr.code = Code::VerifyFailed;
                fr.stage = FileStage::Verify;
                fr.message = QStringLiteral("%1 changed while it was being staged (fakelib check failed).").arg(lib->name);
                r.files.push_back(fr);
                continue;
            }
            fr.ok = true;
            fr.stage = FileStage::Done;
            fr.shaAfter = h.value();
            fr.sizeAfter = lib->size;
            r.staged.push_back({target, out, h.value(), lib->size, false});
            r.files.push_back(fr);
            if (ctx.progress) {
                ctx.progress->advance(lib->size);
                ctx.progress->advanceFiles(1);
            }
        }
    }

    // V5: coverage.
    QSet<QString> stagedKeys;
    for (const StagedFile& s : r.staged)
        stagedKeys.insert(relKey(s.rel));
    for (const ExecFile* e : processable) {
        if (plan.leaveUnchanged.contains(e->rel) || stagedKeys.contains(relKey(e->rel)))
            continue;
        bool reported = false;
        for (const FileResult& f : r.files)
            if (f.rel == e->rel && !f.ok)
                reported = true;
        if (!reported) {
            FileResult fr;
            fr.rel = e->rel;
            fr.code = Code::VerifyFailed;
            fr.stage = FileStage::Verify;
            fr.message = QStringLiteral("%1 was not staged.").arg(e->rel);
            r.files.push_back(fr);
        }
    }

    // No backport needed when every output already equals the game's bytes.
    bool unchanged = !r.staged.empty();
    for (const StagedFile& s : r.staged) {
        if (s.executable) {
            for (const FileResult& f : r.files)
                if (relKey(f.rel) == relKey(s.rel) && (f.shaBefore != s.sha256 || !f.renamedTo.isEmpty()))
                    unchanged = false;
        } else if (!sameAsGame.contains(s.rel)) {
            unchanged = false;
        }
    }
    r.nothingChanged = unchanged && !r.failed();
    return r;
}

} // namespace fw::backport
