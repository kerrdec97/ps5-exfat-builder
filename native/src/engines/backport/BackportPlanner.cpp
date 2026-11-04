#include "engines/backport/BackportPlanner.h"

#include "engines/backport/CanonicalTrio.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/SelfReader.h"
#include "engines/backport/RightSprx.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/backport/ApplyJournal.h"
#include "engines/backport/BackupArchive.h"
#include "engines/backport/ProviderSet.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>

#include <algorithm>

namespace fw::backport {

QString severityName(Severity s)
{
    switch (s) {
    case Severity::Refuse: return QStringLiteral("refuse");
    case Severity::Confirm: return QStringLiteral("confirm");
    case Severity::Warn: return QStringLiteral("warn");
    case Severity::Info: return QStringLiteral("info");
    }
    return QStringLiteral("info");
}

const Issue* BackportPlan::blocking() const
{
    for (const Issue& i : issues)
        if (i.severity == Severity::Refuse)
            return &i;
    if (!options.confirmDiffers)
        for (const Issue& i : issues)
            if (i.severity == Severity::Confirm)
                return &i;
    return nullptr;
}

bool BackportPlan::canRun() const
{
    return blocking() == nullptr;
}

Error BackportPlan::blockingError() const
{
    const Issue* i = blocking();
    if (!i)
        return makeError(Code::Internal, QStringLiteral("Nothing blocks this backport."));
    QString detail;
    if (!i->files.isEmpty())
        detail = i->files.join(QStringLiteral(", "));
    return makeError(i->code, i->message, detail);
}

QList<Issue> BackportPlan::issuesOf(Severity s) const
{
    QList<Issue> out;
    for (const Issue& i : issues)
        if (i.severity == s)
            out << i;
    return out;
}

QString BackportPlan::summary() const
{
    const QString zips = options.backportDir.isEmpty() ? QStringLiteral("originals ZIP only")
                                                       : QStringLiteral("backport + originals ZIPs");
    const QString game = options.applyToGame ? QStringLiteral("game folder patched")
                                             : QStringLiteral("game folder unchanged");
    QString s = QStringLiteral("Next run: SDK %1 · %2 · %3").arg(options.targetSdk).arg(zips, game);
    if (options.proFix)
        s += QStringLiteral(" · PS5 Pro fix");
    return s;
}

namespace {

QString native(const QString& p)
{
    return QDir::toNativeSeparators(p);
}

QString listFiles(const QStringList& files, int max = 3)
{
    QStringList shown = files.mid(0, max);
    QString s = shown.join(QStringLiteral(", "));
    if (files.size() > max)
        s += QStringLiteral(" and %1 more").arg(files.size() - max);
    return s;
}

} // namespace

Result<BackportPlan> planBackport(const BackportOptions& opt, const CancellationToken& token)
{
    BackportPlan plan;
    plan.options = opt;
    auto add = [&plan](Code c, Severity s, const QString& msg, const QStringList& files = {}) {
        plan.issues << Issue{c, s, msg, files};
    };

    // S3: a supported pair, never clamped.
    plan.pair = sdkPair(opt.targetSdk);
    if (!plan.pair)
        add(Code::TargetSdkInvalid, Severity::Refuse,
            QStringLiteral("Target SDK %1 is not supported. Choose SDK %2 to %3.")
                .arg(opt.targetSdk)
                .arg(kMinTargetSdk)
                .arg(kMaxTargetSdk));

    const QFileInfo gi(opt.game);
    if (opt.game.trimmed().isEmpty() || !gi.isDir()) {
        add(Code::NotAGame, Severity::Refuse,
            QStringLiteral("The game folder %1 does not exist.").arg(native(opt.game)));
        return plan;
    }
    const QString game = QDir::cleanPath(gi.absoluteFilePath());
    plan.options.game = game;
    plan.folderName = QFileInfo(game).fileName();

    // S9: the originals ZIP folder is required.
    if (opt.originalsDir.trimmed().isEmpty())
        add(Code::ZipFolderInvalid, Severity::Refuse,
            QStringLiteral("Choose where the originals ZIP is saved (Settings > Backport). Without it the original "
                           "files would have no lasting copy. Nothing was changed."));
    if (!opt.applyToGame && opt.backportDir.trimmed().isEmpty())
        add(Code::ZipFolderInvalid, Severity::Refuse,
            QStringLiteral("Apply to game folders is off and no backport ZIP folder is set, so the run would "
                           "produce nothing. Choose a backport ZIP folder or turn Apply on."));
    // S38: nested ZIP folders are refused; equal folders are fine.
    if (!opt.originalsDir.isEmpty() && !opt.backportDir.isEmpty()) {
        const bool a = isSameOrInside(opt.originalsDir, opt.backportDir);
        const bool b = isSameOrInside(opt.backportDir, opt.originalsDir);
        if (a != b)
            add(Code::ZipFolderInvalid, Severity::Refuse,
                QStringLiteral("The backport ZIP folder and the originals ZIP folder must not be inside each other "
                               "(%1, %2). Use the same folder or two separate ones.")
                    .arg(native(opt.backportDir), native(opt.originalsDir)));
    }

    // S35: an interrupted apply on this game must be rolled back first.
    for (const PendingJournal& j : ApplyJournal::findInterrupted(opt.journalDir.isEmpty() ? defaultJournalDir() : opt.journalDir))
        if (isSameOrInside(j.game, game) && isSameOrInside(game, j.game))
            add(Code::InterruptedApply, Severity::Refuse,
                QStringLiteral("An earlier backport of this game was interrupted while it was being applied. Roll it "
                               "back first (it is offered at start-up, or restore the originals from %1).")
                    .arg(native(j.originalsZip)),
                {j.path});

    // Scan (read-only). The ZIP folders are never entered (S38).
    QStringList excludes;
    for (const QString& d : {opt.originalsDir, opt.backportDir})
        if (!d.isEmpty())
            excludes << QDir::cleanPath(QFileInfo(d).absoluteFilePath());
    FW_TRY_ASSIGN(plan.scan, scanGame(game, token, ScanMode::Full, excludes));
    const ScanResult& scan = plan.scan;

    // S5
    if (!scan.hasEboot)
        add(Code::NotAGame, Severity::Refuse,
            QStringLiteral("eboot.bin is not in %1. Pick the game's root folder.").arg(native(game)));

    // #35: a game-root sce_sys/about/right.sprx that cannot be converted takes
    // the user's standard copy (pinned SHA-256) instead of refusing the run.
    for (const ExecFile& e : scan.executables) {
        if (e.area != Area::Game || !isRightSprx(e.rel))
            continue;
        if (!e.isEncryptedOrCompressed() && e.problemCode == Code::Ok)
            break; // converts normally
        const QString reason = e.problem.isEmpty() ? fileClassName(e.cls) : e.problem;
        if (opt.standardRightSprx.trimmed().isEmpty()) {
            add(Code::Ok, Severity::Info,
                QStringLiteral("%1 cannot be converted (%2). To use the standard right.sprx in its place, set Backport "
                               "> Standard right.sprx to your own copy.")
                    .arg(e.rel, reason),
                {e.rel});
        } else if (const QString why = standardRightSprxProblem(opt.standardRightSprx, opt.standardRightSprxSha256);
                   !why.isEmpty()) {
            add(Code::RightSprxInvalid, Severity::Refuse, QStringLiteral("Standard right.sprx: %1").arg(why), {e.rel});
        } else {
            plan.rightSprxSubstitute = e.rel;
            plan.rightSprxReason = reason;
            add(Code::Ok, Severity::Info,
                QStringLiteral("%1 cannot be converted (%2); the standard right.sprx takes its place. The dump's copy is "
                               "kept in the originals ZIP.")
                    .arg(e.rel, reason),
                {e.rel});
        }
        break;
    }

    // S32: encrypted / compressed SELFs, reported before anything changes.
    QStringList enc;
    for (const ExecFile* e : scan.encryptedOrCompressed())
        if (e->rel != plan.rightSprxSubstitute)
            enc << e->rel;
    if (!enc.isEmpty()) {
        if (opt.encryptedPolicy == EncryptedPolicy::Refuse) {
            add(Code::SelfEncrypted, Severity::Refuse,
                QStringLiteral("%1 %2 still encrypted or compressed. The dump may not be fully decrypted; dump the "
                               "game again or choose \"Leave encrypted files unchanged\". Nothing was changed.")
                    .arg(listFiles(enc), enc.size() == 1 ? QStringLiteral("is") : QStringLiteral("are")),
                enc);
        } else {
            plan.leaveUnchanged = enc;
            add(Code::SelfEncrypted, Severity::Warn,
                QStringLiteral("%1 will be left unchanged because %2 encrypted or compressed. The result is marked "
                               "Partial; whether the game loads is UNVERIFIED.")
                    .arg(listFiles(enc), enc.size() == 1 ? QStringLiteral("it is") : QStringLiteral("they are")),
                enc);
        }
    }

    // Every executable the run would rewrite must be rewritable (D4 up front).
    for (const ExecFile& e : scan.executables) {
        if (e.area != Area::Game || e.isEncryptedOrCompressed() || e.problemCode == Code::Ok
            || e.rel == plan.rightSprxSubstitute)
            continue;
        QString msg;
        switch (e.problemCode) {
        case Code::ParamMissing:
        case Code::ParamSizeInvalid:
        case Code::ParamMagicInvalid:
        case Code::ParamOutOfRange:
            msg = QStringLiteral("%1: %2.").arg(e.rel, e.problem);
            break;
        case Code::SignUnsupported:
            msg = QStringLiteral("The SDK %1 build was refused: %2 is not a PS5 executable this tool can rewrite (%3).")
                      .arg(opt.targetSdk)
                      .arg(e.rel, e.problem);
            break;
        default:
            msg = QStringLiteral("%1 is not a PS5 executable this tool can rewrite (%2).").arg(e.rel, e.problem);
            break;
        }
        add(e.problemCode, Severity::Refuse, msg, {e.rel});
    }

    // D7 / S4: the game's own SDK comes from eboot.bin.
    plan.meta = readGameMetadata(game);
    plan.gameSdk = scan.gameSdk();
    if (scan.hasEboot && plan.pair) {
        const ExecFile* eb = scan.eboot();
        if (!plan.gameSdk) {
            add(Code::GameSdkUnknown, Severity::Refuse,
                QStringLiteral("The game's own SDK could not be read from eboot.bin%1, so the target SDK cannot be "
                               "checked.")
                    .arg(eb && !eb->problem.isEmpty() ? QStringLiteral(" (%1)").arg(eb->problem) : QString()));
        } else if (opt.targetSdk > *plan.gameSdk) {
            add(Code::TargetAboveGameSdk, Severity::Refuse,
                QStringLiteral("Target SDK %1 is higher than this game's own SDK %2. Choose SDK %2 or lower.")
                    .arg(opt.targetSdk)
                    .arg(*plan.gameSdk));
        }
        if (plan.meta.paramSdk && plan.gameSdk && *plan.meta.paramSdk != *plan.gameSdk)
            add(Code::Ok, Severity::Info,
                QStringLiteral("param.json says SDK %1; eboot.bin says SDK %2, which is used.")
                    .arg(*plan.meta.paramSdk)
                    .arg(*plan.gameSdk));
    }

    // #41: imported module names that match a sce_module/ library only when
    // letter case is ignored (detector only; nothing is renamed).
    plan.moduleCaseImports = moduleCaseMismatches(scan);
    for (const ModuleCaseMismatch& m : plan.moduleCaseImports)
        add(Code::ModuleNameCase, Severity::Warn,
            QStringLiteral("%1 imports the module %2, but the file is %3 (the letters differ in case). Firmware that "
                           "looks modules up case-sensitively may not load it and the game can crash at launch. "
                           "This is reported only; the file is not renamed.")
                .arg(m.importer, m.imported, m.file),
            {m.importer, m.file});

    // #41 fix (ModuleCaseFix.h): the module paths the eboot loads by name
    // ("%s/Modules/<Name>.prx"); a dump file spelled differently is staged,
    // zipped and applied under the game's spelling.
    if (const ExecFile* eb = scan.eboot(); eb && eb->isProcessable()) {
        auto bytes = readFileBytes(joinPath(game, eb->rel));
        if (bytes.ok()) {
            QByteArray image = bytes.value();
            if (!elf::hasElfMagic(image)) {
                auto u = selfio::unwrap(image);
                image = u.ok() ? u.value() : QByteArray();
            }
            plan.moduleCase = planModuleCase(scan, requestedModulePaths(image));
        }
    }
    for (const ModuleCaseRename& m : plan.moduleCase.renames)
        add(Code::ModuleNameCase, Severity::Warn,
            QStringLiteral("%1: eboot.bin loads it as %2 (the console looks it up by exact name), so the backport ZIP and "
                           "Apply write it with that spelling; the originals ZIP and Restore keep the dump's. Not yet "
                           "confirmed on a console.%3")
                .arg(m.from, m.to.section(u'/', -1),
                     m.folderCaseDiffers ? QStringLiteral(" The folder's letter case also differs and is not changed.")
                                         : QString()),
            {m.from});
    for (const QString& n : std::as_const(plan.moduleCase.notes))
        add(Code::ModuleNameCase, Severity::Info, n);

    // S8 / S6 / S14 / D3: fakelib. #34: the user's AMPR/PlayGo providers take
    // the place of the same-named libraries of the set (or are added to it).
    std::optional<FakelibSource> set;
    bool setUsable = true;
    if (!opt.fakelibDir.trimmed().isEmpty()) {
        FakelibSource src;
        const QStringList problems = fakelibProblems(opt.fakelibDir, game, &src, token);
        if (!problems.isEmpty()) {
            setUsable = false;
            add(Code::FakelibInvalid, Severity::Refuse,
                QStringLiteral("The fakelib folder %1 cannot be used: %2")
                    .arg(native(opt.fakelibDir), problems.join(QStringLiteral(" "))),
                problems);
        } else {
            set = src;
        }
    }
    if (!opt.providerDir.trimmed().isEmpty()) {
        FakelibSource prov;
        QStringList ignored;
        const QStringList problems = providerProblems(opt.providerDir, game, &prov, &ignored, token);
        if (!problems.isEmpty()) {
            setUsable = false;
            add(Code::FakelibInvalid, Severity::Refuse,
                QStringLiteral("The AMPR/PlayGo folder %1 cannot be used: %2")
                    .arg(native(opt.providerDir), problems.join(QStringLiteral(" "))),
                problems);
        } else {
            plan.providers = prov;
            plan.providersIgnored = ignored;
            const QString pinsPath = opt.providerPinsPath.isEmpty() ? defaultProviderPinsPath() : opt.providerPinsPath;
            const ProviderPinCheck pins = checkProviderPins(prov, pinsPath);
            QStringList names;
            for (const FakelibLib& l : prov.libs)
                names << QStringLiteral("%1 (%2…)").arg(l.name, l.sha256.left(8));
            if (!pins.pinned) {
                plan.pinProvidersOnRun = true;
                add(Code::Ok, Severity::Info,
                    QStringLiteral("The AMPR/PlayGo libraries in %1 are not pinned yet; this backport pins them: %2. A "
                                   "later change to them is refused until you pin them again.")
                        .arg(native(prov.dir), names.join(QStringLiteral(", "))));
            } else if (!pins.ok()) {
                setUsable = false;
                add(Code::ProviderChanged, Severity::Refuse,
                    QStringLiteral("The AMPR/PlayGo libraries in %1 differ from the ones pinned on %2 (%3). If you "
                                   "replaced them on purpose, pin them again on the Backport page; nothing was changed.")
                        .arg(native(prov.dir), pins.pinnedAt, pins.describe()));
            } else {
                add(Code::Ok, Severity::Info,
                    QStringLiteral("AMPR/PlayGo libraries from %1 match their pins: %2.")
                        .arg(native(prov.dir), names.join(QStringLiteral(", "))));
            }
            if (!ignored.isEmpty())
                add(Code::Ok, Severity::Info,
                    QStringLiteral("Only libSceAmpr and libScePlayGo are used from %1; ignored: %2.")
                        .arg(native(prov.dir), listFiles(ignored)),
                    ignored);
            set = withProviders(set, prov);
        }
    }
    if (set && setUsable) {
        {
            const FakelibSource& src = *set;
            plan.fakelib = src;
            plan.fakelibState = fakelibState(game, &src, token);
            if (plan.fakelibState == FakelibState::Same) {
                add(Code::AlreadyBackported, Severity::Refuse,
                    QStringLiteral("This game folder already contains backport files: fakelib/ matches the chosen "
                                   "set, so the originals can't be captured from it. Restore the originals first."),
                    {QStringLiteral("fakelib/")});
            } else if (plan.fakelibState == FakelibState::Other) {
                plan.fakelibExtras = fakelibExtras(game, src);
                add(Code::Ok, Severity::Warn,
                    QStringLiteral("The game already has a fakelib folder from another tool. Its files that the set "
                                   "replaces are kept in the originals ZIP.%1")
                        .arg(plan.fakelibExtras.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" These are carried over unchanged: %1.")
                                       .arg(plan.fakelibExtras.join(QStringLiteral(", ")))),
                    plan.fakelibExtras);
            }
            QStringList dirs = {QString()};
            dirs << scan.nestedEbootDirs;
            for (const QString& d : std::as_const(dirs)) {
                for (const FakelibLib& l : src.libs) {
                    const QString wanted = (d.isEmpty() ? QString() : d + u'/') + QStringLiteral("fakelib/") + l.name;
                    const QString existing = scan.resolve(wanted);
                    plan.stagedFakelibTargets << (existing.isEmpty() ? wanted : existing);
                    if (!existing.isEmpty())
                        plan.replacedFakelibFiles << existing;
                }
            }
        }
    } else if (opt.fakelibDir.trimmed().isEmpty() && opt.providerDir.trimmed().isEmpty() && !scan.fakelibFiles.isEmpty()) {
        add(Code::Ok, Severity::Info,
            QStringLiteral("The dump's own fakelib/ (%1 files) is carried over unchanged; no fakelib set was chosen.")
                .arg(scan.fakelibFiles.size()));
    }

    // The owner's canonical AGC/AGCDriver/PSML trio (CanonicalTrio.h): one of
    // those exact files in the game's fakelib/ means the folder was backported
    // before, so its files are not originals (the Backport GUI refuses too).
    {
        QStringList pinnedHere;
        for (const QString& rel : scan.fakelibFiles) {
            const QString name = rel.section(u'/', -1);
            if (rel.count(u'/') != 1 || !isCanonicalTrioName(name))
                continue; // root fakelib/ only
            const QString path = joinPath(game, rel);
            const qint64 size = QFileInfo(path).size();
            if (std::none_of(kCanonicalTrio.begin(), kCanonicalTrio.end(),
                             [size](const PinnedLibrary& p) { return p.size == size; }))
                continue; // only size matches are hashed
            auto h = sha256File(path, token);
            if (!h.ok()) {
                if (h.error().isCancelled())
                    return h.takeError();
                continue;
            }
            if (canonicalTrioMember(name, size, h.value()))
                pinnedHere << rel;
        }
        if (!pinnedHere.isEmpty())
            add(Code::AlreadyBackported, Severity::Refuse,
                QStringLiteral("This game folder already contains backport files (%1 of the canonical fakelib set). "
                               "Use an unmodified game folder, or restore its originals first.")
                    .arg(listFiles(pinnedHere)),
                pinnedHere);
    }
    // Dump note (the owner's engine, make_baseline.unsigned_fakelibs): a plain
    // decoded ELF among the dump's own fakelib/ libraries is kept as it is.
    // Several confirmed-working backports ship such providers; it is only a
    // lead when the game fails to start (PRX_PROCESS_STARTUP_FAILURE 0xa0020104).
    {
        QStringList plain;
        for (const ExecFile& e : scan.executables)
            if (e.area == Area::Fakelib && e.cls == FileClass::Elf && e.rel.count(u'/') == 1
                && e.rel.startsWith(QStringLiteral("fakelib/"), Qt::CaseInsensitive)
                && !isCanonicalTrioName(e.rel.section(u'/', -1)))
                plain << e.rel;
        if (!plain.isEmpty())
            add(Code::Ok, Severity::Info,
                QStringLiteral("Dump note: plain (not signed) ELF libraries in fakelib/ are kept as they are: %1. "
                               "Several working backports ship such libraries; if the game fails to start with "
                               "PRX_PROCESS_STARTUP_FAILURE (0xa0020104), check these first.")
                    .arg(listFiles(plain)),
                plain);
    }
    if (plan.fakelib) {
        int pinned = 0;
        for (const FakelibLib& l : plan.fakelib->libs)
            if (canonicalTrioMember(l.name, l.size, l.sha256))
                ++pinned;
        if (pinned == int(kCanonicalTrio.size()))
            add(Code::Ok, Severity::Info,
                QStringLiteral("The fakelib folder holds the canonical AGC/AGCDriver/PSML trio (all three match their "
                               "pinned SHA-256)."));
    }

    // S10 / S30 / S28: ZIP folders.
    plan.prefix = zipPrefix(plan.meta, plan.folderName);
    plan.titleId = manifestTitleId(plan.meta, game);
    if (!opt.originalsDir.trimmed().isEmpty()) {
        const QString why = zipDestinationProblem(opt.originalsDir, game, originalsZipName(plan.prefix));
        if (!why.isEmpty())
            add(Code::ZipFolderInvalid, Severity::Refuse,
                QStringLiteral("Originals ZIP folder %1: %2").arg(native(opt.originalsDir), why));
        else
            plan.originalsZipPath = uniqueZipPath(QDir(opt.originalsDir).filePath(originalsZipName(plan.prefix)));
    }
    if (!opt.backportDir.trimmed().isEmpty()) {
        const QString why = zipDestinationProblem(opt.backportDir, game, backportZipName(plan.prefix, kMaxTargetSdk, opt.proFix));
        if (!why.isEmpty())
            add(Code::ZipFolderInvalid, Severity::Refuse,
                QStringLiteral("Backport ZIP folder %1: %2").arg(native(opt.backportDir), why));
        else if (plan.pair)
            plan.backportZipPath =
                uniqueZipPath(QDir(opt.backportDir).filePath(backportZipName(plan.prefix, opt.targetSdk)));
    }

    // S7: manifest verdict. Records are kept per game folder: this folder's
    // own record decides "differs"; any record of the same title ID (another
    // copy or version, or this folder before it was moved) whose patched
    // hashes match the current files means the folder is already backported.
    if (!opt.originalsDir.trimmed().isEmpty() && QFileInfo(opt.originalsDir).isDir()) {
        const ManifestSet records = findManifests(opt.originalsDir, plan.titleId, game);
        plan.manifestPath = records.ownPath;
        plan.manifest = records.own;
        for (const QString& d : records.damaged)
            add(Code::Ok, Severity::Warn,
                records.ownPathDamaged ? QStringLiteral("%1 It is set aside and replaced by this run's record.").arg(d)
                                       : QStringLiteral("%1 It is ignored.").arg(d));
        auto [verdict, files] = manifestVerdict(scan, plan.manifest ? &*plan.manifest : nullptr, token);
        plan.verdict = verdict;
        plan.verdictFiles = files;
        if (verdict == Verdict::Patched)
            add(Code::AlreadyBackported, Severity::Refuse,
                QStringLiteral("This game folder already contains backport files: %1 match the files an earlier "
                               "backport produced. Restore the originals first.")
                    .arg(listFiles(files)),
                files);
        else if (verdict == Verdict::Differs)
            add(Code::DiffersFromOriginals, opt.confirmDiffers ? Severity::Warn : Severity::Confirm,
                QStringLiteral("%1 differ from the recorded originals (already patched, or another version of the "
                               "game). Continue only if this folder holds the unmodified game.")
                    .arg(listFiles(files)),
                files);
        for (const Manifest& other : records.others) {
            if (plan.verdict == Verdict::Patched)
                break;
            const auto otherVerdict = manifestVerdict(scan, &other, token);
            if (otherVerdict.first != Verdict::Patched)
                continue;
            plan.verdict = Verdict::Patched;
            plan.verdictFiles = otherVerdict.second;
            add(Code::AlreadyBackported, Severity::Refuse,
                QStringLiteral("This game folder already contains backport files: %1 match the files an earlier "
                               "backport of %2 produced. Restore the originals first.")
                    .arg(listFiles(otherVerdict.second),
                         native(other.doc.value(QStringLiteral("game_folder")).toString())),
                otherVerdict.second);
        }
        if (!records.own && !records.others.empty())
            add(Code::Ok, Severity::Info,
                QStringLiteral("Other folders of %1 have backport records; this folder gets its own record and "
                               "theirs are not changed.")
                    .arg(plan.titleId));
    }

    // NothingToDo: no executable to rewrite and no fakelib to add.
    const auto processable = scan.processable();
    int toRewrite = 0;
    for (const ExecFile* e : processable)
        if (!plan.leaveUnchanged.contains(e->rel))
            ++toRewrite;
    if (scan.hasEboot && toRewrite == 0 && plan.stagedFakelibTargets.isEmpty())
        add(Code::NothingToDo, Severity::Refuse,
            QStringLiteral("No backport is needed: %1 has no executable this tool can rewrite.").arg(native(game)));

    // S16 / S15: reported, kept.
    if (!scan.leftoverBaks.isEmpty())
        add(Code::Ok, Severity::Info,
            QStringLiteral("Existing .bak files are left alone: %1.").arg(listFiles(scan.leftoverBaks)),
            scan.leftoverBaks);
    if (!scan.skippedSymlinks.isEmpty())
        add(Code::Ok, Severity::Warn,
            QStringLiteral("Links are not followed and not backported: %1.").arg(listFiles(scan.skippedSymlinks)),
            scan.skippedSymlinks);

    // S11 / S28: staging paths are long-path aware (\\?\ via fw::toLongPath),
    // so a long path is a warning, not a refusal.
    const QString stagingBase = !opt.stagingHint.isEmpty()
        ? opt.stagingHint
        : QDir(Paths::tempRoot()).filePath(QStringLiteral("fw-job-00000000-00000000000000-000000"));
    int longest = 0;
    QString longestRel;
    for (const ExecFile* e : processable)
        if (e->rel.size() > longestRel.size())
            longestRel = e->rel;
    for (const QString& t : std::as_const(plan.stagedFakelibTargets))
        if (t.size() > longestRel.size())
            longestRel = t;
    if (!longestRel.isEmpty()) {
        // "<staging>/staging/unwrapped/<rel>"; 32 characters of slack for the
        // folder name a failed run's staging is moved to.
        longest = int(native(QDir(stagingBase).filePath(QStringLiteral("staging/unwrapped/") + longestRel)).size()) + 32;
        plan.longestStagingPath = longest;
        if (longest > kMaxWinPath)
            add(Code::PathTooLong, Severity::Warn,
                QStringLiteral("%1 will be staged under a path of about %2 characters; the app uses long-path "
                               "handling for it. If your system refuses long paths, choose a shorter temporary "
                               "folder in Settings > Storage.")
                    .arg(longestRel)
                    .arg(longest),
                {longestRel});
    }

    // S12: free space for staging, both ZIPs and (Apply on) the game.
    {
        const qint64 exec = scan.processableBytes();
        const qint64 fakelibStaged = plan.fakelib ? plan.fakelib->totalBytes() * (1 + scan.nestedEbootDirs.size()) : 0;
        qint64 replacedFakelib = 0;
        for (const QString& r : std::as_const(plan.replacedFakelibFiles))
            replacedFakelib += QFileInfo(joinPath(game, r)).size();
        qint64 largest = 0;
        for (const ExecFile* e : processable)
            largest = std::max(largest, e->size);
        qint64 added = 0;
        if (plan.fakelib)
            for (const QString& t : std::as_const(plan.stagedFakelibTargets))
                if (scan.resolve(t).isEmpty())
                    if (const FakelibLib* l = plan.fakelib->find(t.section(u'/', -1)))
                        added += l->size;

        struct Need {
            QString path;
            QString use;
            qint64 bytes;
        };
        std::vector<Need> needs;
        needs.push_back({stagingBase, QStringLiteral("staging"), 3 * exec + fakelibStaged + replacedFakelib});
        if (!opt.originalsDir.isEmpty())
            needs.push_back({opt.originalsDir, QStringLiteral("originals ZIP"),
                             qint64(1.1 * double(exec + scan.paramJsonBytes + scan.decryptedBytes + replacedFakelib))});
        if (!opt.backportDir.isEmpty())
            needs.push_back({opt.backportDir, QStringLiteral("backport ZIP"), qint64(1.1 * double(exec + fakelibStaged))});
        if (opt.applyToGame)
            needs.push_back({game, QStringLiteral("game folder"),
                             added + largest + 64 * 1024 * qint64(processable.size())});
        QMap<QString, SpaceNeed> byVolume;
        for (const Need& n : needs) {
            auto ds = opt.diskSpaceProbe ? opt.diskSpaceProbe(n.path) : diskSpace(n.path);
            if (!ds.ok())
                continue; // a missing folder is already refused above
            SpaceNeed& s = byVolume[ds->rootPath];
            s.volume = ds->rootPath;
            s.uses << n.use;
            s.needBytes += n.bytes;
            s.availableBytes = ds->available;
        }
        for (const SpaceNeed& s : std::as_const(byVolume)) {
            plan.space.push_back(s);
            if (s.availableBytes < s.needBytes + opt.freeSpaceMarginBytes)
                add(Code::InsufficientSpace, Severity::Refuse,
                    QStringLiteral("Not enough space on %1 for the %2: about %3 needed (with the free-space reserve), "
                                   "%4 free.")
                        .arg(native(s.volume), s.uses.join(QStringLiteral(", ")),
                             formatBytes(s.needBytes + opt.freeSpaceMarginBytes), formatBytes(s.availableBytes)));
        }
    }
    return plan;
}

} // namespace fw::backport
