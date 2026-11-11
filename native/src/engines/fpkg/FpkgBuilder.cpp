#include "engines/fpkg/FpkgBuilder.h"

#include "core/FileUtil.h"
#include "core/ProcessRunner.h"
#include "engines/fpkg/AsciiAliases.h"
#include "engines/fpkg/BackportUpdate.h"
#include "engines/fpkg/DumpRules.h"
#include "engines/fpkg/PackageHeader.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/fpkg/Toolchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

namespace fw::fpkg {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

bool isHex32(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    return re.match(s).hasMatch();
}

QString listSome(const QStringList& items, int max = 20)
{
    QStringList shown = items.mid(0, max);
    QString s = shown.join(QStringLiteral("; "));
    if (items.size() > max)
        s += QStringLiteral(" … and %1 more").arg(items.size() - max);
    return s;
}

// Runs a callable when leaving the scope unless dismissed.
class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : m_fn(std::move(fn)) {}
    ~ScopeExit()
    {
        if (m_fn)
            m_fn();
    }
    void dismiss() { m_fn = nullptr; }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> m_fn;
};

// Two builds of the same Content ID into the same folder never run at once.
class OutputLock {
public:
    OutputLock() = default;
    OutputLock(const OutputLock&) = delete;
    OutputLock& operator=(const OutputLock&) = delete;
    ~OutputLock() { release(); }

    Status acquire(const QString& key, const CancellationToken& token, const Log& log)
    {
        std::unique_lock lock(mutex());
        bool logged = false;
        while (held().count(key)) {
            if (!logged) {
                log.info(QStringLiteral("Waiting for another build of the same package into the same folder to finish."));
                logged = true;
            }
            cv().wait_for(lock, std::chrono::milliseconds(100));
            if (token.isCancelled())
                return Error::cancelled();
        }
        held().insert(key);
        m_key = key;
        return success();
    }
    void release()
    {
        if (m_key.isEmpty())
            return;
        {
            std::lock_guard lock(mutex());
            held().erase(m_key);
        }
        cv().notify_all();
        m_key.clear();
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::condition_variable& cv()
    {
        static std::condition_variable c;
        return c;
    }
    static std::set<QString>& held()
    {
        static std::set<QString> h;
        return h;
    }
    QString m_key;
};

QString lockKey(const QString& contentId, const QString& outputDir)
{
    return contentId.toLower() + u'|' + QDir::cleanPath(QFileInfo(outputDir).absoluteFilePath()).toLower();
}

Status mapDir(AsciiAliases& aliases, const QString& dir, const QString& tag)
{
    auto m = aliases.map(dir, tag);
    if (!m.ok())
        return m.takeError();
    return success();
}

} // namespace

// ---- requests ----------------------------------------------------------------------------

QJsonObject BuildRequest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("source"), source);
    if (!output.isEmpty()) o.insert(QStringLiteral("output"), output);
    if (!basePackage.isEmpty()) o.insert(QStringLiteral("basePackage"), basePackage);
    if (!baseFolder.isEmpty()) o.insert(QStringLiteral("baseFolder"), baseFolder);
    if (exactSource) o.insert(QStringLiteral("exactSource"), true);
    if (isUpdate()) o.insert(QStringLiteral("updateOutput"), updateOutputName(updateOutput));
    if (!contentId.isEmpty()) o.insert(QStringLiteral("contentId"), contentId);
    if (!contentVersion.isEmpty()) o.insert(QStringLiteral("contentVersion"), contentVersion);
    if (!title.isEmpty()) o.insert(QStringLiteral("title"), title);
    return o;
}

Result<BuildRequest> BuildRequest::fromJson(const QJsonObject& o)
{
    BuildRequest r;
    r.source = o.value(QStringLiteral("source")).toString();
    r.output = o.value(QStringLiteral("output")).toString();
    r.basePackage = o.value(QStringLiteral("basePackage")).toString();
    r.baseFolder = o.value(QStringLiteral("baseFolder")).toString();
    r.exactSource = o.value(QStringLiteral("exactSource")).toBool(false);
    const auto uo = parseUpdateOutput(o.value(QStringLiteral("updateOutput")).toString());
    if (!uo)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("updateOutput must be \"both\", \"update\" or \"full\"."));
    r.updateOutput = *uo;
    r.contentId = o.value(QStringLiteral("contentId")).toString().trimmed();
    r.contentVersion = o.value(QStringLiteral("contentVersion")).toString().trimmed();
    r.title = o.value(QStringLiteral("title")).toString();
    if (r.source.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a game folder, image or .gp5 project to package."));
    return r;
}

QJsonObject AddonRequest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("payloadFolder"), payloadFolder);
    if (!output.isEmpty()) o.insert(QStringLiteral("output"), output);
    if (!contentId.isEmpty()) o.insert(QStringLiteral("contentId"), contentId);
    o.insert(QStringLiteral("entitlementKey"), entitlementKey);
    o.insert(QStringLiteral("volumeType"), volumeType);
    return o;
}

Result<AddonRequest> AddonRequest::fromJson(const QJsonObject& o)
{
    AddonRequest r;
    r.payloadFolder = o.value(QStringLiteral("payloadFolder")).toString();
    r.output = o.value(QStringLiteral("output")).toString();
    r.contentId = o.value(QStringLiteral("contentId")).toString().trimmed();
    r.entitlementKey = o.value(QStringLiteral("entitlementKey")).toString().trimmed();
    r.volumeType = o.value(QStringLiteral("volumeType")).toString(QStringLiteral("prospero_ac"));
    if (r.payloadFolder.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose the add-on's content folder."));
    return r;
}

QJsonObject VerifyRequest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("package"), package);
    if (!passcode.isEmpty()) o.insert(QStringLiteral("passcode"), passcode);
    o.insert(QStringLiteral("integrity"), integrity);
    return o;
}

Result<VerifyRequest> VerifyRequest::fromJson(const QJsonObject& o)
{
    VerifyRequest r;
    r.package = o.value(QStringLiteral("package")).toString();
    r.passcode = o.value(QStringLiteral("passcode")).toString();
    r.integrity = o.value(QStringLiteral("integrity")).toBool(true);
    if (r.package.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a .pkg file to verify."));
    return r;
}

QJsonObject ExtractRequest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("package"), package);
    o.insert(QStringLiteral("destination"), destination);
    if (!passcode.isEmpty()) o.insert(QStringLiteral("passcode"), passcode);
    return o;
}

Result<ExtractRequest> ExtractRequest::fromJson(const QJsonObject& o)
{
    ExtractRequest r;
    r.package = o.value(QStringLiteral("package")).toString();
    r.destination = o.value(QStringLiteral("destination")).toString();
    r.passcode = o.value(QStringLiteral("passcode")).toString();
    if (r.package.isEmpty() || r.destination.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a .pkg file and an empty destination folder."));
    return r;
}

// ---- naming / conflicts ----------------------------------------------------------------

QString defaultOutputFolder(const QString& source, const QString& defaultOutputDir)
{
    if (!defaultOutputDir.isEmpty())
        return QFileInfo(defaultOutputDir).absoluteFilePath();
    const QFileInfo fi(source);
    if (fi.isDir())
        return QDir(fi.absolutePath()).filePath(fi.fileName() + QStringLiteral("-pkg"));
    if (fi.suffix().compare(QStringLiteral("gp5"), Qt::CaseInsensitive) == 0) {
        const QFileInfo dir(fi.absolutePath());
        return QDir(dir.absolutePath()).filePath(dir.fileName() + QStringLiteral("-pkg"));
    }
    return QDir(fi.absolutePath()).filePath(fi.completeBaseName() + QStringLiteral("-pkg"));
}

QString keepBothName(const QString& finalPath)
{
    const QFileInfo fi(finalPath);
    for (int n = 1; n < 10000; ++n) {
        const QString cand = fi.dir().filePath(numberedName(fi.fileName(), n));
        if (!QFileInfo::exists(cand))
            return cand;
    }
    return {};
}

namespace {

// A name is taken by anything there, a dangling link included.
bool nameTaken(const QString& path)
{
    const QFileInfo fi(path);
    return fi.exists() || fi.isSymLink();
}

QString joinNative(const QStringList& paths)
{
    QStringList shown;
    for (const QString& p : paths)
        shown << QDir::toNativeSeparators(p);
    return shown.join(QStringLiteral(", "));
}

QString numberedPath(const QString& path, int n)
{
    const QFileInfo fi(path);
    return fi.dir().filePath(numberedName(fi.fileName(), n));
}

// The first n for which "name (n)" is free for every path (0: none, each
// file then takes its own first free number). One number for all of them
// keeps an update and its .remastered.pkg a pair.
int freeNumber(const QStringList& paths)
{
    for (int n = 1; n < 10000; ++n)
        if (std::none_of(paths.begin(), paths.end(), [n](const QString& p) { return nameTaken(numberedPath(p, n)); }))
            return n;
    return 0;
}

} // namespace

Status placeOutputs(const std::vector<OutputMove>& moves, OutputConflict policy, QStringList& notes, QStringList* kept,
                    const RenameFn& renameFn, std::vector<ReplacedOutput>* replaced)
{
    const RenameFn doRename = renameFn ? renameFn : RenameFn([](const QString& from, const QString& to) {
        return QFile::rename(from, to);
    });
    const auto fileName = [](const QString& p) { return QFileInfo(p).fileName(); };
    struct Aside {
        QString finalPath;
        QString aside;
    };
    std::vector<Aside> asides;
    std::optional<Error> failure;
    QStringList finals;
    for (const OutputMove& m : moves)
        finals << m.finalPath;
    const int keepBothNumber = policy == OutputConflict::KeepBoth ? freeNumber(finals) : 0;

    // 1. Every existing output is set aside before anything new takes its name.
    for (const OutputMove& m : moves) {
        if (!nameTaken(m.finalPath))
            continue;
        if (policy == OutputConflict::Fail) {
            failure = Error(ErrorCode::AlreadyExists,
                            QStringLiteral("%1 already exists. Choose another output folder, or change \"When the "
                                           "package already exists\".").arg(fileName(m.finalPath)));
            break;
        }
        QString aside;
        if (policy == OutputConflict::KeepBoth) {
            aside = keepBothNumber > 0 ? numberedPath(m.finalPath, keepBothNumber) : keepBothName(m.finalPath);
        } else {
            aside = m.finalPath + QStringLiteral(".fw-replaced");
            for (int n = 2; nameTaken(aside); ++n)
                aside = m.finalPath + QStringLiteral(".fw-replaced-%1").arg(n);
        }
        if (aside.isEmpty() || !doRename(m.finalPath, aside)) {
            failure = policy == OutputConflict::KeepBoth
                ? Error(ErrorCode::AlreadyExists,
                        QStringLiteral("%1 already exists and could not be renamed to keep both. Close any program "
                                       "using it and try again.").arg(fileName(m.finalPath)))
                : Error(ErrorCode::PermissionDenied,
                        QStringLiteral("Could not replace %1. Close any program using it and try again.")
                            .arg(fileName(m.finalPath)));
            failure->detail = QStringLiteral("rename %1 -> %2 failed").arg(m.finalPath, aside);
            break;
        }
        asides.push_back({m.finalPath, aside});
    }

    // 2. Then every new file takes its name.
    size_t placed = 0; // moves[0, placed) are at their final names
    for (; !failure && placed < moves.size(); ++placed) {
        const OutputMove& m = moves[placed];
        if (!doRename(m.partial, m.finalPath)) {
            failure = Error(ErrorCode::IoError,
                            QStringLiteral("Could not rename the finished package to %1.").arg(fileName(m.finalPath)),
                            QStringLiteral("rename %1 -> %2 failed").arg(m.partial, m.finalPath));
            break;
        }
    }

    if (!failure) {
        std::vector<ReplacedOutput> old;
        for (const Aside& a : asides) {
            if (policy == OutputConflict::KeepBoth)
                notes << QStringLiteral("The existing %1 was renamed to %2.").arg(fileName(a.finalPath), fileName(a.aside));
            else
                old.push_back({a.finalPath, a.aside});
        }
        if (replaced)
            *replaced = std::move(old);
        else
            discardReplaced(old, notes);
        return success();
    }

    // 3. Undo everything: new files back to their partial names, then the
    // files that were set aside back to their own names.
    QStringList userNotes;
    QStringList detail{failure->detail};
    std::vector<bool> stuckAtFinal(moves.size(), false);
    for (size_t i = placed; i-- > 0;) {
        if (!doRename(moves[i].finalPath, moves[i].partial)) {
            stuckAtFinal[i] = true;
            detail << QStringLiteral("rollback %1 -> %2 failed").arg(moves[i].finalPath, moves[i].partial);
        }
    }
    for (auto it = asides.rbegin(); it != asides.rend(); ++it) {
        if (nameTaken(it->finalPath) || !doRename(it->aside, it->finalPath)) {
            userNotes << QStringLiteral("The previous %1 is now %2.")
                             .arg(fileName(it->finalPath), QDir::toNativeSeparators(it->aside));
            detail << QStringLiteral("restore %1 -> %2 failed").arg(it->aside, it->finalPath);
        }
    }

    // 4. Keep the new files (they may be hours of packing): under a free
    // numbered name, else under their partial name.
    QStringList keptHere;
    const int keptNumber = freeNumber(finals);
    for (size_t i = 0; i < moves.size(); ++i) {
        const OutputMove& m = moves[i];
        if (stuckAtFinal[i]) {
            keptHere << m.finalPath;
            continue;
        }
        if (!nameTaken(m.partial))
            continue;
        const QString numbered = keptNumber > 0 ? numberedPath(m.finalPath, keptNumber) : keepBothName(m.finalPath);
        if (!numbered.isEmpty() && doRename(m.partial, numbered))
            keptHere << numbered;
        else
            keptHere << m.partial;
    }
    if (kept)
        *kept = keptHere;

    QString message = failure->message;
    for (const QString& n : std::as_const(userNotes))
        message += u' ' + n;
    if (keptHere.size() == 1)
        message += QStringLiteral(" The new package was kept as %1.").arg(joinNative(keptHere));
    else if (!keptHere.isEmpty())
        message += QStringLiteral(" The new packages were kept as %1.").arg(joinNative(keptHere));
    return Error(failure->code, message, detail.join(QStringLiteral("; ")));
}

Status placeOutput(const QString& partial, const QString& finalPath, OutputConflict policy, QStringList& notes)
{
    return placeOutputs({OutputMove{partial, finalPath}}, policy, notes);
}

void discardReplaced(const std::vector<ReplacedOutput>& replaced, QStringList& notes)
{
    for (const ReplacedOutput& r : replaced) {
        const QString name = QFileInfo(r.finalPath).fileName();
        if (QFile::remove(r.aside))
            notes << QStringLiteral("The existing %1 was replaced.").arg(name);
        else
            notes << QStringLiteral("The existing %1 was replaced, but its old copy could not be deleted: %2")
                         .arg(name, QDir::toNativeSeparators(r.aside));
    }
}

void restoreReplaced(const std::vector<ReplacedOutput>& replaced, QStringList& placed, QStringList& notes)
{
    if (replaced.empty())
        return;
    // The new files first, all under the same number, so the old names are free again.
    const int n = freeNumber(placed);
    for (QString& p : placed) {
        const QString to = n > 0 ? numberedPath(p, n) : keepBothName(p);
        if (!to.isEmpty() && QFile::rename(p, to))
            p = to;
    }
    for (auto it = replaced.rbegin(); it != replaced.rend(); ++it) {
        const QString name = QFileInfo(it->finalPath).fileName();
        if (!nameTaken(it->finalPath) && QFile::rename(it->aside, it->finalPath))
            notes << QStringLiteral("The previous %1 was put back.").arg(name);
        else
            notes << QStringLiteral("The previous %1 could not be put back; it is now %2.")
                         .arg(name, QDir::toNativeSeparators(it->aside));
    }
    notes << (placed.size() == 1 ? QStringLiteral("The new package was kept as %1.").arg(joinNative(placed))
                                 : QStringLiteral("The new packages were kept as %1.").arg(joinNative(placed)));
}

// ---- planning ----------------------------------------------------------------------------

namespace {

std::optional<QByteArray> readSmallFile(const QStringList& roots, const QString& rel, qint64 maxBytes = 64 * 1024 * 1024)
{
    const QString p = findInRoots(roots, rel);
    if (p.isEmpty())
        return std::nullopt;
    QFile f(toLongPath(p));
    if (!f.open(QIODevice::ReadOnly) || f.size() > maxBytes)
        return std::nullopt;
    return f.readAll();
}

QString relUnder(const QString& base, const QString& rel) { return QDir(base).filePath(rel); }

bool underSceSys(const QString& rel)
{
    return rel.compare(QStringLiteral("sce_sys"), Qt::CaseInsensitive) == 0
        || rel.startsWith(QStringLiteral("sce_sys/"), Qt::CaseInsensitive);
}

QStringList planEmptyDirs(BuildPlan& p, const FpkgSettings& s)
{
    QStringList out;
    QSet<QString> seen;
    const auto add = [&](const QString& rel, const QString& why) {
        const QString key = rel.toLower();
        if (rel.isEmpty() || seen.contains(key) || underSceSys(rel))
            return;
        const QString problem = pathRuleProblem(rel);
        if (!problem.isEmpty()) {
            p.log << QStringLiteral("empty_dir not declared: %1 (%2)").arg(rel, problem);
            return;
        }
        seen.insert(key);
        out << rel;
        p.log << QStringLiteral("empty_dir: %1 (%2)").arg(rel, why);
    };
    const auto spelled = [&](const QString& rel) {
        for (const QString& d : std::as_const(p.scan.dirs))
            if (d.compare(rel, Qt::CaseInsensitive) == 0)
                return d;
        return rel;
    };
    if (s.keepDlcEmulator) {
        if (const auto ini = readSmallFile(p.roots, QStringLiteral("dlc_emu.ini"), kDlcEmuIniMaxBytes * 4)) {
            const DlcEmuIni parsed = parseDlcEmuIni(*ini);
            for (const QString& w : parsed.warnings)
                p.warnings << w;
            for (const DlcEmuEntry& e : parsed.entries) {
                const QString mf = e.mountFolder();
                if (mf.isEmpty()) {
                    if (!e.mountPoint.isEmpty())
                        p.warnings << QStringLiteral("dlc_emu.ini: the mount point %1 of %2 is not under /app0/.")
                                          .arg(e.mountPoint, e.contentId);
                    continue;
                }
                if (!p.scan.hasDir(mf)) {
                    p.warnings << QStringLiteral("dlc_emu.ini names the mount folder %1 for %2, but the dump has no such "
                                                 "folder. That DLC will not mount.").arg(mf, e.contentId);
                    continue;
                }
                if (!p.scan.hasFileUnder(mf))
                    add(spelled(mf), QStringLiteral("dlc_emu.ini mount folder"));
            }
        }
        for (const QString& d : std::as_const(p.scan.dirs))
            if (!d.contains(u'/') && isAddcontFolderName(d) && !p.scan.hasFileUnder(d))
                add(d, QStringLiteral("DLC mount folder"));
    }
    if (s.keepEmptyFolders)
        for (const QString& d : std::as_const(p.scan.emptyDirs))
            add(d, QStringLiteral("empty folder"));
    return out;
}

} // namespace

Status finalizeParam(BuildPlan& p, const PlanOptions& o, const FpkgSettings& s)
{
    p.param = p.sourceParam;
    ParamPatchOptions po;
    po.clearVersionFileUri = s.clearVersionFileUri;
    po.clearAttribute3 = s.clearAttribute3;
    po.lowerRequiredFirmware = s.lowerRequiredFirmware;
    po.contentVersion = o.contentVersion;
    po.title = o.title;
    // An update keeps the versions of its own param.json.
    const QStringList sdkNotes = o.isUpdate ? QStringList() : applySdkVersionRule(p.param, p.executableSdk);
    p.paramNotes = sdkNotes + applyParamPatch(p.param, po);

    qint64 u = 0;
    for (const PlannedEntry& e : p.entries)
        if (e.kind != EntryKind::ParamJson)
            u += e.size;
    p.uncompressedBytes = u;
    p.fileEntries = int(p.entries.size());
    p.size = SizeClassDecision{};
    if (o.isUpdate) {
        p.size.notes << QStringLiteral("size class: not changed for an update");
    } else if (!s.autoSizeClass) {
        p.size.notes << QStringLiteral("size class: not changed (\"Choose the package size class\" is off)");
    } else {
        auto d = applyPackageSizeClass(p.param, u, p.fileEntries);
        if (!d.ok())
            return d.takeError();
        p.size = std::move(d).value();
    }
    p.paramBytes = serializeParamJson(p.param);
    for (PlannedEntry& e : p.entries)
        if (e.kind == EntryKind::ParamJson)
            e.size = p.paramBytes.size();
    return success();
}

Result<BuildPlan> planBuild(const PlanOptions& o, const FpkgSettings& s, const CancellationToken& token)
{
    BuildPlan p;
    p.roots = o.roots;
    p.outputDir = QFileInfo(o.outputDir).absoluteFilePath();
    p.isUpdate = o.isUpdate;

    const QString paramPath = findInRoots(o.roots, QStringLiteral("sce_sys/param.json"));
    if (paramPath.isEmpty())
        return Error(ErrorCode::NotFound,
                     o.isUpdate ? QStringLiteral("sce_sys/param.json was found neither in the update folder nor in the base.")
                                : QStringLiteral("sce_sys/param.json was not found in the game folder. Is this a PS5 game dump?"));
    {
        auto prm = readParamJson(paramPath);
        if (!prm.ok())
            return prm.takeError();
        p.sourceParam = std::move(prm).value();
    }

    p.contentId = p.sourceParam.value(QStringLiteral("contentId")).toString().trimmed();
    if (!isValidContentId(p.contentId))
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("contentId \"%1\" in param.json is not a valid PS5 Content ID (for example "
                                    "UP0000-PPSA00000_00-0000000000000000).").arg(p.contentId));
    if (!o.contentId.isEmpty() && o.contentId != p.contentId)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The Content ID %1 does not match param.json (%2). Edit param.json or clear the field.")
                         .arg(o.contentId, p.contentId));
    if (!hasStandardContentIdSuffix(p.contentId))
        p.warnings << QStringLiteral("The Content ID %1 does not end its title part with \"_00\".").arg(p.contentId);

    const QString verText = o.contentVersion.isEmpty()
        ? p.sourceParam.value(QStringLiteral("contentVersion")).toString()
        : o.contentVersion;
    const auto ver = ContentVersion::parse(verText);
    if (!ver)
        return Error(o.contentVersion.isEmpty() ? ErrorCode::CorruptData : ErrorCode::InvalidArgument,
                     QStringLiteral("contentVersion \"%1\" is not in the form 01.000.000.").arg(verText));
    p.version = *ver;
    p.stem = packageStem(p.contentId, p.version);
    p.assetsDir = QDir(p.outputDir).filePath(QStringLiteral(".gp5-assets/") + p.stem);
    p.gp5Path = QDir(p.outputDir).filePath(p.stem + QStringLiteral(".gp5"));
    p.scenarioPath = QDir(p.outputDir).filePath(p.stem + QStringLiteral(".playgo-scenario.json"));
    p.logsDir = QDir(p.outputDir).filePath(p.stem + QStringLiteral("-build-logs"));

    ScanOptions so;
    so.leftovers.keepDlcEmulator = s.keepDlcEmulator;
    so.leftovers.removeAmprLeftovers = s.removeAmprLeftovers;
    so.leftovers.removeDumpLeftovers = s.removeDumpLeftovers;
    {
        auto scan = scanSource(o.roots, so, token);
        if (!scan.ok())
            return scan.takeError();
        p.scan = std::move(scan).value();
    }

    if (p.scan.keystoneSize < 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("sce_sys/keystone is missing. The debug Publishing Tools need "
                                                            "the dump's own 96-byte keystone."));
    if (p.scan.keystoneSize != 96)
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("sce_sys/keystone is %1 bytes; the dump's own keystone is exactly 96 bytes.")
                         .arg(p.scan.keystoneSize));
    if (!p.scan.badPaths.empty()) {
        QStringList items;
        for (const BadPath& b : p.scan.badPaths)
            items << QStringLiteral("%1 (%2)").arg(b.rel, b.reason);
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 file name(s) are not accepted by the Publishing Tools. Rename them in the dump: %2")
                         .arg(p.scan.badPaths.size())
                         .arg(listSome(items)));
    }

    for (const ExcludedEntry& e : p.scan.excluded)
        p.log << QStringLiteral("excluded: %1%2 (%3)").arg(e.rel, e.isDir ? QStringLiteral("/") : QString(), e.reason);

    if (const ScannedFile* eboot = p.scan.findFile(QStringLiteral("eboot.bin"))) {
        QFile f(toLongPath(eboot->absPath));
        if (f.open(QIODevice::ReadOnly) && bytesImportAmpr(f.read(4 * 1024 * 1024)))
            p.warnings << QStringLiteral("eboot.bin imports libSceAmpr. If the game relied on an AMPR emulator in fakelib, "
                                         "it may not start from this package.");
        p.executableSdk = executableSdkVersion(eboot->absPath);
        if (p.executableSdk)
            p.log << QStringLiteral("eboot.bin: SDK version 0x%1 in its .sceversion trailer")
                         .arg(*p.executableSdk, 16, 16, QLatin1Char('0'));
    }

    // Executables Sony's publisher would misread: the PS4 signature and a
    // .sceversion trailer that starts before the offset the header names.
    QHash<QString, SelfRepairPlan> selfRepair;
    QSet<QString> ucpRepair;
    {
        QStringList ps4Left;
        int misplaced = 0;
        for (const ScannedFile& f : p.scan.files) {
            if (f.size < 0x20 || !isExecutableName(f.rel.section(u'/', -1)))
                continue;
            if (!token.checkpoint())
                return Error::cancelled();
            const auto fix = selfRepairPlan(f.absPath);
            if (!fix)
                continue;
            if (s.repairSelf) {
                selfRepair.insert(f.rel.toLower(), *fix);
                p.log << QStringLiteral("self: %1 (%2, in a copy)").arg(f.rel, fix->describe());
            } else {
                if (fix->rewriteMagic)
                    ps4Left << f.rel;
                if (fix->padding > 0)
                    ++misplaced;
            }
        }
        if (!ps4Left.isEmpty())
            p.warnings << QStringLiteral("%1 executable(s) carry the PS4 SELF signature (for example %2); the package may "
                                         "not run. Turn on \"Fix PS4-format executables\".")
                              .arg(ps4Left.size())
                              .arg(ps4Left.first());
        if (misplaced > 0)
            p.warnings << QStringLiteral("%1 executable(s) have a misplaced version trailer that Sony's publisher cannot "
                                         "read. Turn on \"Fix PS4-format executables\".").arg(misplaced);
    }

    // sce_sys/uds archives named by npbind.dat: a stale digest is corrected,
    // a missing or unusable archive is replaced by an empty one.
    QHash<QString, QByteArray> udsReplace;               // lower rel -> archive
    std::vector<std::pair<QString, QByteArray>> udsAdd;  // archives the dump lacks
    QSet<QString> udsChecked;
    if (s.repairUcp) {
        if (const ScannedFile* npbind = p.scan.findFile(QStringLiteral("sce_sys/uds/npbind.dat"))) {
            const Result<QStringList> ids = [&]() -> Result<QStringList> {
                QFile nf(toLongPath(npbind->absPath));
                if (!nf.open(QIODevice::ReadOnly) || nf.size() > 64 * 1024)
                    return Error(ErrorCode::IoError, QStringLiteral("it could not be read"));
                return readNpbindIds(nf.readAll());
            }();
            if (!ids.ok()) {
                p.log << QStringLiteral("ucp: sce_sys/uds/npbind.dat was not used (%1); the UDS archives are packed as they are")
                             .arg(ids.error().message);
            } else {
                for (int i = 0; i < ids->size(); ++i) {
                    const QString id = ids->at(i);
                    const QString rel = QStringLiteral("sce_sys/uds/uds%1.ucp").arg(i, 2, 10, QLatin1Char('0'));
                    const ScannedFile* src = p.scan.findFile(rel);
                    QString reason = QStringLiteral("missing");
                    if (src) {
                        if (src->size > kUcpMaxBytes) {
                            p.log << QStringLiteral("ucp: %1 not checked (larger than 128 MiB)").arg(src->rel);
                            udsChecked.insert(src->rel.toLower());
                            continue;
                        }
                        QFile in(toLongPath(src->absPath));
                        if (!in.open(QIODevice::ReadOnly))
                            return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(src->rel), in.errorString());
                        const UdsUcpVerdict v = checkUdsUcp(in.readAll(), id);
                        udsChecked.insert(src->rel.toLower());
                        if (v.problems.isEmpty())
                            continue;
                        if (v.digestOnly) {
                            ucpRepair.insert(src->rel.toLower());
                            p.log << QStringLiteral("ucp: %1 (stale SHA-1 digest corrected in a copy)").arg(src->rel);
                            continue;
                        }
                        reason = QStringLiteral("not usable: ") + v.problems.join(QStringLiteral("; "));
                    }
                    const QByteArray stub = emptyUdsUcp(id);
                    p.log << QStringLiteral("ucp: %1 is %2; the package gets an empty archive for %3 (%4 bytes)")
                                 .arg(src ? src->rel : rel, reason, id)
                                 .arg(stub.size());
                    p.warnings << QStringLiteral("%1 is %2. The package gets an empty activity archive for %3 instead, so "
                                                 "it builds and the game finds the file; activity data of the original "
                                                 "is not in the package. The dump is not changed.")
                                      .arg(src ? src->rel : rel, src ? QStringLiteral("not usable") : QStringLiteral("missing"), id);
                    if (src)
                        udsReplace.insert(src->rel.toLower(), stub);
                    else
                        udsAdd.emplace_back(rel, stub);
                }
            }
        }
        for (const ScannedFile& f : p.scan.files) {
            if (!isUcpPath(f.rel) || udsChecked.contains(f.rel.toLower()))
                continue;
            if (f.size > kUcpMaxBytes) {
                p.log << QStringLiteral("ucp: %1 not checked (larger than 128 MiB)").arg(f.rel);
                continue;
            }
            QFile in(toLongPath(f.absPath));
            if (!in.open(QIODevice::ReadOnly))
                return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(f.rel), in.errorString());
            const UcpCheck c = checkUcp(in.readAll());
            if (!c.structureOk) {
                // Kept as it is (a sealed copy from the original package, a cut-short file): the tool decides.
                p.log << QStringLiteral("ucp: %1 cannot be read (%2); packed as it is").arg(f.rel, c.problem);
                p.warnings << QStringLiteral("%1 cannot be read (%2) and is packed as it is. Sony's publisher may refuse "
                                             "it; replace it from the original dump if the build stops there.")
                                  .arg(f.rel, c.problem);
                continue;
            }
            if (!c.digestOk) {
                ucpRepair.insert(f.rel.toLower());
                p.log << QStringLiteral("ucp: %1 (stale SHA-1 digest corrected in a copy)").arg(f.rel);
            }
        }
    }

    {
        auto images = planImageRecovery(p.scan, s.recoverPngFromDds, o.haveDds2png);
        if (!images.ok())
            return images.takeError();
        p.images = std::move(images).value();
    }
    QSet<QString> replaced;
    for (const ImageConversion& c : p.images.conversions) {
        if (!c.replacedPngRel.isEmpty())
            replaced.insert(c.replacedPngRel.toLower());
        p.log << QStringLiteral("image: %1 from %2 (%3)").arg(c.dst, c.ddsRel, c.reason);
    }
    for (const QString& n : std::as_const(p.images.notes))
        p.log << QStringLiteral("image: %1").arg(n);

    // PlayGo
    PlayGoInputs in;
    in.sourceScenario = readSmallFile(o.roots, QStringLiteral("sce_sys/playgo-scenario.json"), 4 * 1024 * 1024);
    if (s.useSourcePlayGoTable) {
        in.chunkDat = readSmallFile(o.roots, QStringLiteral("sce_sys/playgo-chunk.dat"));
        in.ficm = readSmallFile(o.roots, QStringLiteral("sce_sys/playgo-ficm.dat"));
        in.hashTable = readSmallFile(o.roots, QStringLiteral("sce_sys/playgo-hash-table.dat"));
    }
    in.paramDefaultLanguage = paramDefaultLanguage(p.sourceParam);
    in.useSourceTable = s.useSourcePlayGoTable;
    in.fallback = s.playGoFallback;
    in.fillEmptyChunks = s.fillEmptyChunks;
    in.chunkCount = s.playGoChunkCount;
    in.allLanguages = s.playGoAllLanguages;
    in.forcedLanguages = s.playGoForcedLanguages;
    in.files.emplace_back(QStringLiteral("sce_sys/playgo-scenario.json"), 1);
    QHash<QString, qint64> ddsSize;
    for (const ImageConversion& c : p.images.conversions) {
        const qint64 sz = QFileInfo(c.ddsAbs).size();
        ddsSize.insert(c.dst.toLower(), sz);
        in.files.emplace_back(c.dst, sz);
    }
    for (const auto& u : udsAdd)
        in.files.emplace_back(u.first, u.second.size());
    for (const ScannedFile& f : p.scan.files)
        if (!replaced.contains(f.rel.toLower()))
            in.files.emplace_back(f.rel, f.size);
    {
        auto pg = planPlayGo(in);
        if (!pg.ok())
            return pg.takeError();
        p.playgo = std::move(pg).value();
    }
    for (const QString& n : std::as_const(p.playgo.notes))
        p.log << QStringLiteral("playgo: %1").arg(n);
    p.warnings << p.playgo.warnings;

    // Project entries, in the order the image is laid out.
    {
        PlannedEntry e;
        e.dst = QStringLiteral("sce_sys/playgo-scenario.json");
        e.src = p.scenarioPath;
        e.size = p.playgo.scenarioBytes.size();
        e.chunk = p.playgo.fileChunk(e.dst);
        e.kind = EntryKind::Sidecar;
        p.entries.push_back(e);
    }
    for (const ImageConversion& c : p.images.conversions) {
        PlannedEntry e;
        e.dst = c.dst;
        e.src = relUnder(p.assetsDir, QStringLiteral("sce_sys/") + c.dst.section(u'/', -1));
        e.origin = c.ddsAbs;
        e.size = ddsSize.value(c.dst.toLower());
        e.chunk = p.playgo.fileChunk(e.dst);
        e.kind = EntryKind::RecoveredImage;
        p.entries.push_back(e);
    }
    for (const auto& u : udsAdd) {
        PlannedEntry e;
        e.dst = u.first;
        e.src = relUnder(p.assetsDir, u.first);
        e.size = u.second.size();
        e.chunk = p.playgo.fileChunk(e.dst);
        e.kind = EntryKind::GeneratedFile;
        e.data = u.second;
        p.entries.push_back(e);
    }
    for (const PlannedPayload& pl : p.playgo.payloads) {
        PlannedEntry e;
        e.dst = pl.dst;
        e.src = relUnder(p.assetsDir, pl.dst);
        e.size = kPayloadBytes;
        e.chunk = pl.chunk;
        e.noCompression = true;
        e.kind = pl.filler ? EntryKind::Filler : EntryKind::Payload;
        p.entries.push_back(e);
    }
    for (const ScannedFile& f : p.scan.files) {
        const QString low = f.rel.toLower();
        if (replaced.contains(low))
            continue;
        PlannedEntry e;
        e.dst = f.rel;
        e.size = f.size;
        e.chunk = p.playgo.fileChunk(f.rel);
        if (low == u"sce_sys/param.json") {
            e.kind = EntryKind::ParamJson;
            e.src = relUnder(p.assetsDir, QStringLiteral("sce_sys/param.json"));
            e.origin = f.absPath;
        } else if (const auto fix = selfRepair.constFind(low); fix != selfRepair.cend()) {
            e.kind = EntryKind::RepairedSelf;
            e.src = relUnder(p.assetsDir, QStringLiteral("normalized-self/") + f.rel);
            e.origin = f.absPath;
            e.selfPlan = *fix;
            e.size = f.size + fix->padding;
        } else if (const auto stub = udsReplace.constFind(low); stub != udsReplace.cend()) {
            e.kind = EntryKind::GeneratedFile;
            e.src = relUnder(p.assetsDir, f.rel);
            e.data = *stub;
            e.size = stub->size();
        } else if (ucpRepair.contains(low)) {
            e.kind = EntryKind::RepairedUcp;
            e.src = relUnder(p.assetsDir, QStringLiteral("repaired-ucp/") + f.rel);
            e.origin = f.absPath;
        } else {
            e.src = f.absPath;
        }
        p.entries.push_back(e);
    }

    p.emptyDirs = planEmptyDirs(p, s);
    FW_TRY(finalizeParam(p, o, s));
    return p;
}

Gp5Project projectFromPlan(const BuildPlan& plan, const FpkgSettings& s, const std::function<QString(const QString&)>& toTool)
{
    Gp5Project g;
    g.volumeType = QStringLiteral("prospero_app");
    g.passcode = s.passcode;
    g.contentId = plan.contentId;
    g.chunkInfo = plan.playgo.chunkInfo;
    g.files.reserve(plan.entries.size());
    for (const PlannedEntry& e : plan.entries)
        g.files.push_back({e.dst, toTool ? toTool(e.src) : e.src, e.chunk, e.noCompression});
    g.emptyDirs = plan.emptyDirs;
    return g;
}

// ---- validation ----------------------------------------------------------------------------

namespace {

Status checkOutputFolder(const QString& output, const QString& sourceDir)
{
    if (!sourceDir.isEmpty() && isSameOrInside(output, sourceDir))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The output folder is inside the game folder. Choose a folder outside it."));
    const QFileInfo fi(output);
    if (fi.exists() && !fi.isDir())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output path %1 is a file, not a folder.").arg(native(output)));
    return success();
}

Status checkBasePackagePath(const QString& base, const QStringList& finals)
{
    const QFileInfo fi(base);
    if (!fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("The base package %1 was not found.").arg(native(base)));
    if (fi.suffix().compare(QStringLiteral("pkg"), Qt::CaseInsensitive) != 0)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The base package must be a .pkg file."));
    if (fi.fileName().endsWith(QStringLiteral(".remastered.pkg"), Qt::CaseInsensitive))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Choose the base package installed on the console, not a .remastered.pkg."));
    for (const QString& f : finals)
        if (!f.isEmpty() && QFileInfo(f).absoluteFilePath().compare(fi.absoluteFilePath(), Qt::CaseInsensitive) == 0)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The base package is the file this build would write. Choose another output folder."));
    return success();
}

QString sourceDirOf(const QString& source)
{
    const QFileInfo fi(source);
    if (fi.isDir())
        return fi.absoluteFilePath();
    if (fi.suffix().compare(QStringLiteral("gp5"), Qt::CaseInsensitive) == 0)
        return fi.absolutePath();
    return {};
}

} // namespace

Status validateBuildRequest(const BuildRequest& r, const FpkgSettings& s)
{
    const QFileInfo src(r.source);
    if (!src.exists())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(native(r.source)));
    if (src.isDir()) {
        if (!r.isUpdate() && findPathCaseInsensitive(src.absoluteFilePath(), QStringLiteral("sce_sys"), true).isEmpty())
            return Error(ErrorCode::NotFound,
                         QStringLiteral("%1 has no sce_sys folder. Choose the game's top folder.").arg(native(r.source)));
    } else {
        const QString ext = src.suffix().toLower();
        if (ext == u"gp5") {
            if (!r.contentVersion.isEmpty() || !r.title.isEmpty())
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("The version and title of a .gp5 project come from its param.json; they "
                                            "cannot be changed here."));
        } else if (imageSourceExtensions().contains(ext)) {
            auto stager = sourceStagerFor(r.source);
            if (!stager)
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("Building straight from .%1 images is not available in this build. Extract "
                                            "the image to a folder first.").arg(ext));
            FW_TRY(stager->probe(r.source));
        } else {
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("Choose a game folder, a .gp5 project or an .exfat, .ffpkg, .ffpfs or .ffpfsc image."));
        }
    }
    const QString output = r.output.isEmpty() ? defaultOutputFolder(r.source, s.defaultOutputDir) : r.output;
    FW_TRY(checkOutputFolder(output, sourceDirOf(r.source)));
    if (!r.contentId.isEmpty() && !isValidContentId(r.contentId))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" is not a valid PS5 Content ID.").arg(r.contentId));
    if (!r.contentVersion.isEmpty() && !ContentVersion::parse(r.contentVersion))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The version \"%1\" is not in the form 01.000.000.").arg(r.contentVersion));
    const QString pc = passcodeProblem(s.passcode);
    if (!pc.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(pc));
    if (r.isUpdate()) {
        FW_TRY(checkBasePackagePath(r.basePackage, {}));
        if (!r.baseFolder.isEmpty()) {
            const QFileInfo bf(r.baseFolder);
            if (!bf.isDir() || findPathCaseInsensitive(bf.absoluteFilePath(), QStringLiteral("sce_sys/param.json"), false).isEmpty())
                return Error(ErrorCode::NotFound,
                             QStringLiteral("The original game folder %1 has no sce_sys/param.json.").arg(native(r.baseFolder)));
            const QString sd = sourceDirOf(r.source);
            if (!sd.isEmpty() && (isSameOrInside(bf.absoluteFilePath(), sd) || isSameOrInside(sd, bf.absoluteFilePath())))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("The original game folder and the update folder must not contain each other."));
        }
    } else if (!r.baseFolder.isEmpty() || r.exactSource) {
        return Error(ErrorCode::InvalidArgument, QStringLiteral("An original game folder only applies to update builds."));
    }
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    // A name conflict that would stop the job is reported before it is queued.
    if (s.outputConflict == OutputConflict::Fail && src.isDir()) {
        const QString pj = findPathCaseInsensitive(src.absoluteFilePath(), QStringLiteral("sce_sys/param.json"), false);
        if (!pj.isEmpty()) {
            auto prm = readParamJson(pj);
            if (prm.ok()) {
                const QString cid = prm->value(QStringLiteral("contentId")).toString();
                const auto v = ContentVersion::parse(r.contentVersion.isEmpty() ? prm->value(QStringLiteral("contentVersion")).toString()
                                                                                : r.contentVersion);
                if (isValidContentId(cid) && v) {
                    const QString stem = packageStem(cid, *v);
                    QStringList names;
                    if (!r.isUpdate())
                        names << fullPackageName(stem);
                    else {
                        if (r.updateOutput != UpdateOutput::FullOnly) names << updatePackageName(stem);
                        if (r.updateOutput != UpdateOutput::UpdateOnly) names << companionName(stem);
                    }
                    for (const QString& n : names)
                        if (QFileInfo::exists(QDir(output).filePath(n)))
                            return Error(ErrorCode::AlreadyExists,
                                         QStringLiteral("%1 already exists in the output folder.").arg(n));
                }
            }
        }
    }
    return success();
}

Status validateAddonRequest(const AddonRequest& r, const FpkgSettings& s)
{
    if (r.volumeType == u"prospero_al")
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("Entitlement-only add-ons (prospero_al) are not built: they have no file system and "
                                    "the PS5 debug installer refuses them. Build an add-on with a content folder instead."));
    if (r.volumeType != u"prospero_ac")
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown add-on volume type \"%1\".").arg(r.volumeType));
    if (!isHex32(r.entitlementKey))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The entitlement key must be 32 hexadecimal characters (it has %1).").arg(r.entitlementKey.size()));
    const QFileInfo src(r.payloadFolder);
    if (!src.isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The add-on folder %1 does not exist.").arg(native(r.payloadFolder)));
    if (findPathCaseInsensitive(src.absoluteFilePath(), QStringLiteral("sce_sys/param.json"), false).isEmpty())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("The add-on folder has no sce_sys/param.json. An add-on package needs the add-on's own "
                                    "param.json (as in a dumped add-on); the app does not make one up."));
    if (!r.contentId.isEmpty() && !isValidContentId(r.contentId))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" is not a valid PS5 Content ID.").arg(r.contentId));
    const QString output = r.output.isEmpty() ? defaultOutputFolder(r.payloadFolder, s.defaultOutputDir) : r.output;
    FW_TRY(checkOutputFolder(output, src.absoluteFilePath()));
    const QString pc = passcodeProblem(s.passcode);
    if (!pc.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(pc));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    return success();
}

// ---- running ---------------------------------------------------------------------------------

QStringList buildPhases()
{
    return {QStringLiteral("Preparing the source"), QStringLiteral("Planning"), QStringLiteral("Writing project files"),
            QStringLiteral("Pre-reading files"), QStringLiteral("Packing"), QStringLiteral("Finishing"),
            QStringLiteral("Checking the package"), QStringLiteral("Verifying with img_verify"),
            QStringLiteral("Computing SHA-256")};
}

QStringList addonPhases() { return buildPhases(); }

namespace {

const QList<double> kBuildWeights = {8, 0.5, 0.5, 3, 90, 0.5, 1, 8, 6};
// An extracted package tree against the size of the (Kraken-compressed) .pkg:
// the free-space checks before img_extract ask for this much.
constexpr qint64 kExtractedTreeFactor = 2;
enum Phase { PhPrepare = 0, PhPlan, PhGenerate, PhPrescan, PhPack, PhFinish, PhCheck, PhVerify, PhSha };

QStringList aliasBases(const QString& jobTemp, const QString& output)
{
    QStringList b{QDir::tempPath(), jobTemp};
#ifdef Q_OS_WIN
    const QString pd = qEnvironmentVariable("ProgramData");
    if (!pd.isEmpty()) {
        const QString dir = QDir(pd).filePath(QStringLiteral("fw-fpkg"));
        QDir().mkpath(dir);
        b << dir;
    }
#endif
    const QStorageInfo si(QFileInfo(output).absolutePath());
    if (si.isValid())
        b << si.rootPath();
    return b;
}

QString chooseWorkingDir(const PubToolchain& tc, const QString& gp5Tool)
{
    if (isAsciiPath(tc.binDir) && QFileInfo(tc.binDir).isDir())
        return tc.binDir;
    if (isAsciiPath(QDir::tempPath()))
        return QDir::tempPath();
    return QFileInfo(gp5Tool).absolutePath();
}

// A warning only: the size of the compressed package is not known before packing.
QString fatWarning(const QString& output)
{
    const QString fs = QString::fromLatin1(QStorageInfo(output).fileSystemType()).toUpper();
    if (fs.startsWith(QStringLiteral("FAT")) || fs == u"VFAT")
        return QStringLiteral("The output drive is FAT32, which cannot hold files of 4 GB or more.");
    return {};
}

QByteArray logBytes(const QStringList& lines)
{
    QByteArray out;
    for (const QString& l : lines) {
        out += l.toUtf8();
        out += '\n';
    }
    return out;
}

Status writeZeroFile(const QString& path, qint64 size, const CancellationToken& token)
{
    const QFileInfo fi(path);
    static const QByteArray zeros(256 * 1024, '\0');
    // A file kept from an earlier build is reused only when it really is all zeros.
    if (fi.isFile() && fi.size() == size) {
        QFile old(toLongPath(path));
        bool same = old.open(QIODevice::ReadOnly);
        for (qint64 left = size; same && left > 0;) {
            const QByteArray part = old.read(std::min<qint64>(left, zeros.size()));
            same = !part.isEmpty() && std::all_of(part.begin(), part.end(), [](char c) { return c == '\0'; });
            left -= part.size();
        }
        if (same)
            return success();
    }
    QDir().mkpath(fi.absolutePath());
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(fi.fileName()), f.errorString());
    qint64 left = size;
    while (left > 0) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 n = std::min<qint64>(left, zeros.size());
        if (f.write(zeros.constData(), n) != n)
            return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(fi.fileName()), f.errorString());
        left -= n;
    }
    // QFile buffers: a full disk may only show at the flush.
    if (!f.flush() || f.error() != QFileDevice::NoError)
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(fi.fileName()), f.errorString());
    return success();
}

// Opens every file once (read 1 byte), in parallel.
Status prescanFiles(const QStringList& files, int threads, const CancellationToken& token, ProgressReporter* pr)
{
    std::atomic<qsizetype> next{0};
    std::mutex m;
    QStringList failed;
    const int n = std::max(1, std::min<int>(threads, int(files.size())));
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) {
        pool.emplace_back([&] {
            for (qsizetype i = next++; i < files.size(); i = next++) {
                if (!token.checkpoint())
                    return;
                QFile f(toLongPath(files[i]));
                char b = 0;
                if (!f.open(QIODevice::ReadOnly) || (f.size() > 0 && f.read(&b, 1) != 1)) {
                    std::lock_guard lock(m);
                    failed << files[i];
                }
                pr->advanceFiles(1);
            }
        });
    }
    for (auto& th : pool)
        th.join();
    if (token.isCancelled())
        return Error::cancelled();
    if (!failed.isEmpty())
        return Error(ErrorCode::IoError,
                     QStringLiteral("%1 file(s) of the game could not be read: %2").arg(failed.size()).arg(listSome(failed, 5)));
    return success();
}

void logToolLine(const Log& log, const ToolLine& l)
{
    switch (l.kind) {
    case LineKind::Error: log.error(QStringLiteral("SDK: %1").arg(l.text)); break;
    case LineKind::Warning: log.warn(QStringLiteral("SDK: %1").arg(l.text)); break;
    case LineKind::Info: log.info(QStringLiteral("SDK: %1").arg(l.text.isEmpty() ? l.raw.trimmed() : l.text)); break;
    case LineKind::Dropped: break;
    }
}

// What every publisher run of a job shares: the performance mode's priority
// and the CPU-core limit, which the user can change while the job runs.
void applyJobLimits(ToolRunOptions& o, const FpkgSettings& s, const JobContext& ctx)
{
    o.cpuCoreLimit = s.cpuCoreLimit;
    o.priorityMode = s.priorityMode;
    o.liveLimitKey = ctx.jobId;
    o.log = ctx.log;
}

// TEMP/TMP for a publisher run: a private folder under the job's temporary folder.
QProcessEnvironment publisherEnv(const QString& tempDir, AsciiAliases& aliases, const Log& log)
{
    QDir().mkpath(tempDir);
    // An ASCII path for the SDK (the user folder may not be), then its 8.3 alias when shorter.
    auto mapped = aliases.map(tempDir, QStringLiteral("tmp"));
    bool aliased = false;
    const QString t = publisherTempPath(mapped.ok() ? mapped.value() : tempDir, &aliased);
    if (aliased)
        log.debug(QStringLiteral("Using a short path alias for the same private publisher temporary folder (SDK "
                                 "path-length limit)."));
    if (t.size() > kPublisherTempMaxChars)
        log.warn(QStringLiteral("The publisher's temporary path is %1 characters long (the SDK may fail above %2). Choose a "
                                "shorter temporary folder in Settings > Storage if this step fails.")
                     .arg(t.size()).arg(kPublisherTempMaxChars));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TEMP"), t);
    env.insert(QStringLiteral("TMP"), t);
    return env;
}

Status writeImgCreateLog(const QString& logsDir, const ToolRunResult& r)
{
    QStringList lines;
    lines << QStringLiteral("command=%1").arg(r.commandLine)
          << QStringLiteral("started_utc=%1").arg(r.startedUtc.toString(Qt::ISODateWithMs))
          << QStringLiteral("finished_utc=%1").arg(r.finishedUtc.toString(Qt::ISODateWithMs))
          << QStringLiteral("elapsed=%1").arg(formatElapsed(r.elapsedMs))
          << QStringLiteral("exit_code=%1").arg(r.cancelled ? QStringLiteral("cancelled") : QString::number(r.exitCode));
    for (const QString& e : r.errors)
        lines << QStringLiteral("[Error] %1").arg(e);
    for (const QString& w : r.warnings)
        lines << QStringLiteral("[Warn] %1").arg(w);
    return writeFileAtomic(QDir(logsDir).filePath(QStringLiteral("02-img-create.log")), logBytes(lines));
}

Error packFailure(const ToolRunResult& r, const QString& what)
{
    const ErrorCode code = exitCodeMeansMissingRuntime(r.exitCode) && r.errors.isEmpty() ? ErrorCode::DependencyMissing
                                                                                          : ErrorCode::ExternalToolFailed;
    return Error(code, QStringLiteral("%1 failed: %2").arg(what, r.failureText()),
                 QStringLiteral("exit code %1, %2 error line(s)").arg(r.exitCode).arg(r.errors.size()));
}

// Everything from img_create to the finished, checked, verified package.
struct PackJob {
    const PubToolchain* tc = nullptr;
    const FpkgSettings* s = nullptr;
    JobContext* ctx = nullptr;
    AsciiAliases* aliases = nullptr;

    QString gp5;                 // real path of the project
    QString outputDir;
    QString stem;
    QString logsDir;
    QString contentId;
    QString passcode;            // for img_verify; "" = the settings' (a .gp5 project has its own)
    bool update = false;
    QString basePackage;         // real path
    UpdateOutput updateOutput = UpdateOutput::Both;
    bool keepNapsMetric = false;
    QString workingDir;          // "" = choose
    QString what = QStringLiteral("Packing");
    bool keepProjectOnSuccess = true;
    QStringList projectFiles;    // removed after success unless kept
    QStringList projectDirs;
    qint64 estimatedBytes = 0;   // for progress text only

    JobOutcome outcome;
};

Result<JobOutcome> packAndFinish(PackJob& j)
{
    const FpkgSettings& s = *j.s;
    JobContext& ctx = *j.ctx;
    ProgressReporter* pr = ctx.progress;
    const Log& log = ctx.log;

    const QString partial = QDir(j.outputDir).filePath(j.stem + QStringLiteral(".partial.pkg"));
    const QString partialCompanion = partial + QStringLiteral(".remastered.pkg");
    const QString metric = partial + QStringLiteral(".naps_metric.json");
    const QStringList partials{partial, partialCompanion, metric};
    for (const QString& p : partials)
        QFile::remove(p);
    QStringList keptPartials; // finished packages that could not be put in place: never deleted
    ScopeExit dropPartials([&] {
        for (const QString& p : partials)
            if (!keptPartials.contains(p) && QFileInfo::exists(p) && !QFile::remove(p))
                log.warn(QStringLiteral("Could not remove %1.").arg(native(p)));
    });

    // The SDK's own temporary folder: TEMP/TMP of img_create.
    const QString sdkTmp = QDir(ctx.tempDir).filePath(
        QStringLiteral("sdk-tmp-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
    if (!QDir().mkpath(sdkTmp))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create a temporary folder for the Publishing Tools."));
    ScopeExit dropTmp([&] { removeTreeNoFollow(sdkTmp); });
    QString sdkTmpTool;
    {
        auto m = j.aliases->map(sdkTmp, QStringLiteral("tmp"));
        if (!m.ok())
            return m.takeError();
        sdkTmpTool = native(m.value());
    }
    if (sdkTmpTool.size() > kPublisherTempMaxChars) {
        bool aliased = false;
        sdkTmpTool = publisherTempPath(sdkTmpTool, &aliased);
        if (aliased)
            log.info(QStringLiteral("Using a short path alias for the same private publisher temporary folder (SDK "
                                    "path-length limit)."));
        if (sdkTmpTool.size() > kPublisherTempMaxChars)
            log.warn(QStringLiteral("The publisher's temporary path is %1 characters long (the SDK may fail above %2). "
                                    "Choose a shorter temporary folder in Settings > Storage if the build fails.")
                         .arg(sdkTmpTool.size()).arg(kPublisherTempMaxChars));
    }
    const QString gp5Tool = j.aliases->toToolPath(j.gp5);
    const QString partialTool = j.aliases->toToolPath(partial);
    const QString baseTool = j.update ? j.aliases->toToolPath(j.basePackage) : QString();

    ToolRunOptions ro;
    ro.program = native(j.tc->exe);
    ro.arguments = imgCreateArgs(gp5Tool, partialTool, s.compressionLevel, baseTool);
    ro.workingDirectory = j.workingDir.isEmpty() ? chooseWorkingDir(*j.tc, gp5Tool) : j.workingDir;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TEMP"), sdkTmpTool);
    env.insert(QStringLiteral("TMP"), sdkTmpTool);
    ro.environment = env;
    ro.watchFile = partial;
    applyJobLimits(ro, s, ctx);
    // Monotonic: a second ruler line from the tool must not send the bar backwards.
    ro.onBar = [pr, best = std::make_shared<int>(0)](int steps) {
        *best = std::max(*best, steps);
        pr->setDone(qint64(*best) * 1000 / kBarSteps);
    };
    ro.onBytesWritten = [pr](qint64 b) { pr->setCurrentItem(QStringLiteral("%1 written").arg(formatBytes(b))); };
    ro.onLine = [&log](const ToolLine& l) { logToolLine(log, l); };

    pr->beginPhase(PhPack, 1000, 0, false);
    log.info(QStringLiteral("%1 with %2").arg(j.what, native(j.tc->exe)));
    {
        const int cpus = std::max(1, QThread::idealThreadCount());
        log.info(QStringLiteral("Sony publisher: priority %1, %2.")
                     .arg(processPriorityName(s.priorityMode),
                          s.cpuCoreLimit <= 0 || s.cpuCoreLimit >= cpus ? QStringLiteral("all %1 CPUs").arg(cpus)
                                                                         : QStringLiteral("%1 of %2 CPUs").arg(s.cpuCoreLimit).arg(cpus)));
    }
    auto run = runPubTool(ro, ctx.token);
    if (!run.ok()) {
        Error e = run.takeError();
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("The Publishing Tools could not be started: %1").arg(e.message), e.detail);
    }
    const ToolRunResult r = std::move(run).value();
    log.debug(QStringLiteral("img_create: %1").arg(r.commandLine));
    if (const Status st = writeImgCreateLog(j.logsDir, r); !st.ok())
        log.warn(st.error().message);
    for (const QString& w : r.warnings)
        j.outcome.warnings << QStringLiteral("Publishing Tools: %1").arg(w);
    if (r.cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    if (!r.ok())
        return packFailure(r, j.what);
    if (!QFileInfo(partial).isFile())
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("%1 failed: the Publishing Tools reported success but wrote no package.").arg(j.what));
    if (j.update && !QFileInfo(partialCompanion).isFile())
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("The update was built, but its full-package companion (.remastered.pkg) is missing."));

    // Structure check: the full image (the update delta has no FIH header).
    pr->beginPhase(PhFinish, 0);
    pr->beginPhase(PhCheck, 0);
    const QString checkTarget = j.update ? partialCompanion : partial;
    const HeaderCheck hc = checkPackageHeader(checkTarget);
    if (!hc.ok)
        return Error(ErrorCode::VerificationFailed,
                     QStringLiteral("The package failed the header check: %1.").arg(hc.summary()));
    log.info(QStringLiteral("Header check passed: %1").arg(hc.summary()));

    // Place the outputs, all of them in one step (an update and its
    // .remastered.pkg are never left half replaced).
    QStringList notes;
    QStringList placed;
    QString fullPackage; // the file img_verify and SHA-256 look at
    std::vector<OutputMove> moves;
    if (!j.update) {
        moves.push_back({partial, QDir(j.outputDir).filePath(fullPackageName(j.stem))});
        fullPackage = moves.back().finalPath;
    } else {
        if (j.updateOutput != UpdateOutput::FullOnly)
            moves.push_back({partial, QDir(j.outputDir).filePath(updatePackageName(j.stem))});
        if (j.updateOutput != UpdateOutput::UpdateOnly) {
            moves.push_back({partialCompanion, QDir(j.outputDir).filePath(companionName(j.stem))});
            fullPackage = moves.back().finalPath;
        } else {
            fullPackage = partialCompanion; // checked below, then removed
        }
    }
    // "Overwrite": the old files stay set aside until the checks below are
    // over, and come back when the new package does not pass them.
    std::vector<ReplacedOutput> replaced;
    {
        QStringList kept;
        if (Status st = placeOutputs(moves, s.outputConflict, notes, &kept, {}, &replaced); !st.ok()) {
            keptPartials = kept;
            for (const QString& k : std::as_const(kept))
                log.warn(QStringLiteral("The finished package was kept as %1.").arg(native(k)));
            return st.takeError();
        }
    }
    for (const OutputMove& m : moves)
        placed << m.finalPath;
    if (j.keepNapsMetric && j.update && j.updateOutput != UpdateOutput::FullOnly && QFileInfo::exists(metric)) {
        const QString keptMetric = placed.value(0) + QStringLiteral(".naps_metric.json");
        QFile::remove(keptMetric);
        if (!QFile::rename(metric, keptMetric))
            log.warn(QStringLiteral("Could not keep %1.").arg(native(keptMetric)));
    }
    QFile::remove(metric);
    for (const QString& n : notes)
        log.info(n);
    j.outcome.warnings << notes;
    j.outcome.outputs = placed;
    for (const QString& p : placed)
        log.info(QStringLiteral("Package written: %1 (%2)").arg(native(p), formatBytes(QFileInfo(p).size())));
    if (j.update)
        j.outcome.warnings << QStringLiteral("Install the base package on the console first, then %1.")
                                  .arg(updatePackageName(j.stem));

    const auto settleReplaced = [&](bool passed) {
        QStringList more;
        if (passed)
            discardReplaced(replaced, more);
        else
            restoreReplaced(replaced, placed, more);
        replaced.clear();
        for (const QString& n : std::as_const(more))
            log.info(n);
        j.outcome.warnings << more;
        j.outcome.outputs = placed;
    };

    // Optional deep check with the user's tool.
    j.outcome.verification = Verification::Passed;
    j.outcome.verificationDetail = QStringLiteral("Header and plaintext debug marker checked only; the package content "
                                                  "was not read (img_verify is off).");
    if (s.postVerify != PostVerify::HeaderOnly) {
        pr->beginPhase(PhVerify, 0);
        ToolRunOptions vo;
        vo.program = native(j.tc->exe);
        vo.arguments = imgVerifyArgs(j.passcode.isEmpty() ? s.passcode : j.passcode, s.postVerify == PostVerify::ToolFull,
                                     j.aliases->toToolPath(fullPackage));
        vo.workingDirectory = ro.workingDirectory;
        vo.environment = ro.environment;
        vo.withBar = false;
        // "param.json: SDK Version = 6.00.00.00-00.00.00.0.0." as the package carries it.
        QString packagedSdk;
        vo.onLine = [&](const ToolLine& l) {
            static const QLatin1String marker("param.json: SDK Version = ");
            const qsizetype at = l.text.indexOf(marker);
            if (at >= 0)
                packagedSdk = l.text.mid(at + marker.size()).trimmed();
            if (ro.onLine)
                ro.onLine(l);
        };
        applyJobLimits(vo, s, ctx);
        (void)ctx.token.checkpoint(); // a paused job waits here; a cancelled one is reported by the run below
        auto vr = runPubTool(vo, ctx.token);
        if (vr.ok() && (vr->cancelled || ctx.token.isCancelled())) {
            settleReplaced(false);
            log.warn(QStringLiteral("img_verify was cancelled; the package was kept: %1").arg(native(placed.value(0))));
            return Error::cancelled();
        }
        if (packagedSdk.startsWith(QLatin1String("0.00."))) {
            // Toolkits before sdk-fpkg279-fix15 write 0 whatever param.json says.
            const QString w = QStringLiteral("The package carries SDK version 0 in its param.json. Publishing Tools toolkits "
                                             "older than sdk-fpkg279-fix15 write 0 whatever the game needs, and a game can "
                                             "crash or misbehave when the console starts it that way. Build with the fix15 "
                                             "toolkit (Settings > Tools).");
            j.outcome.warnings << w;
            log.warn(w);
        }
        if (vr.ok() && vr->ok()) {
            j.outcome.verificationDetail = s.postVerify == PostVerify::ToolFull
                ? QStringLiteral("Header checked; img_verify passed (format and integrity).")
                : QStringLiteral("Header checked; img_verify passed (format).");
            log.info(QStringLiteral("img_verify passed."));
        } else {
            // A package built from a dump never passes img_verify cleanly: its
            // fake-signed executables and zero SDK version fields are always
            // reported. Only what is left after those is worth a warning.
            const ImgVerifyFindings found = vr.ok() && !vr->crashed && !vr->errors.isEmpty() ? sortImgVerifyErrors(vr->errors)
                                                                                         : ImgVerifyFindings{};
            const QString usual = found.expected == 1
                ? QStringLiteral("1 message every package built from a dump gets (fake-signed executables, SDK version fields)")
                : QStringLiteral("%1 messages every package built from a dump gets (fake-signed executables, SDK version fields)")
                      .arg(found.expected);
            if (vr.ok() && !vr->crashed && !vr->errors.isEmpty() && found.problems.isEmpty()) {
                j.outcome.verificationDetail = s.postVerify == PostVerify::ToolFull
                    ? QStringLiteral("Header checked; img_verify (format and integrity) found nothing beyond the %1.").arg(usual)
                    : QStringLiteral("Header checked; img_verify (format) found nothing beyond the %1.").arg(usual);
                log.info(QStringLiteral("img_verify: only the %1.").arg(usual));
            } else {
                QString why;
                if (!found.problems.isEmpty()) {
                    why = found.problems.mid(0, 3).join(QStringLiteral(" "));
                    if (found.problems.size() > 3)
                        why += QStringLiteral(" (and %1 more in the log)").arg(found.problems.size() - 3);
                    for (const QString& problem : found.problems)
                        log.warn(QStringLiteral("img_verify: %1").arg(problem));
                } else {
                    why = vr.ok() ? vr->failureText() : vr.error().message;
                }
                j.outcome.verification = Verification::Inconclusive;
                j.outcome.verificationDetail = QStringLiteral("The header check passed, but img_verify reported a problem: %1").arg(why);
                if (found.expected > 0)
                    j.outcome.verificationDetail += QStringLiteral(" It also reported the %1.").arg(usual);
                j.outcome.warnings << QStringLiteral("img_verify reported a problem (%1). The package was kept; check it before "
                                                     "use.").arg(why);
                log.warn(j.outcome.verificationDetail);
            }
        }
    }
    settleReplaced(j.outcome.verification == Verification::Passed);
    if (j.update && j.updateOutput == UpdateOutput::UpdateOnly)
        QFile::remove(partialCompanion);
    dropPartials.dismiss();
    for (const QString& p : partials)
        QFile::remove(p);

    if (s.computeSha256) {
        QJsonObject hashes;
        for (const QString& p : placed) {
            pr->beginPhase(PhSha, QFileInfo(p).size());
            auto h = sha256File(p, ctx.token, pr);
            if (!h.ok())
                return h.takeError();
            log.info(QStringLiteral("SHA-256 of %1: %2").arg(QFileInfo(p).fileName(), h.value()));
            hashes.insert(QFileInfo(p).fileName(), h.value());
        }
        j.outcome.details.insert(QStringLiteral("sha256"), hashes);
    }

    if (!j.keepProjectOnSuccess) {
        for (const QString& f : std::as_const(j.projectFiles))
            QFile::remove(f);
        for (const QString& d : std::as_const(j.projectDirs)) {
            (void)removeTreeSafely(d, j.outputDir);
            QDir().rmdir(QFileInfo(d).absolutePath()); // .gp5-assets when empty
        }
    }
    const QString main = placed.value(0);
    j.outcome.summary = QStringLiteral("%1 · %2").arg(QFileInfo(main).fileName(), formatBytes(QFileInfo(main).size()));
    j.outcome.details.insert(QStringLiteral("contentId"), j.contentId);
    j.outcome.details.insert(QStringLiteral("logs"), j.logsDir);
    j.outcome.details.insert(QStringLiteral("exitCode"), r.exitCode);
    pr->flush();
    return j.outcome;
}

// Same Content ID and a higher version than the base (needs the base's param.json).
Status checkUpdateAgainstBase(const BuildPlan& plan, const QStringList& roots, QStringList& warnings)
{
    if (roots.size() < 2) {
        warnings << QStringLiteral("The base package's Content ID and version were not checked (exact source without the "
                                   "original game folder). It must have the same Content ID and a lower contentVersion.");
        return success();
    }
    const QString bp = findPathCaseInsensitive(roots[1], QStringLiteral("sce_sys/param.json"), false);
    if (bp.isEmpty()) {
        warnings << QStringLiteral("The base has no sce_sys/param.json; its Content ID and version were not checked.");
        return success();
    }
    auto prm = readParamJson(bp);
    if (!prm.ok()) {
        warnings << QStringLiteral("The base's param.json could not be read; its Content ID and version were not checked.");
        return success();
    }
    const QString baseId = prm->value(QStringLiteral("contentId")).toString().trimmed();
    if (!baseId.isEmpty() && baseId != plan.contentId)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The update's Content ID %1 differs from the base's %2.").arg(plan.contentId, baseId));
    const auto baseVer = ContentVersion::parse(prm->value(QStringLiteral("contentVersion")).toString());
    if (baseVer && plan.version.compare(*baseVer) <= 0)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The update's version %1 must be higher than the base's %2. Suggested: %3.")
                         .arg(plan.version.text(), baseVer->text(), baseVer->next().text()));
    return success();
}

// The user's temporary folder, when one is set and still exists (the queue
// uses the default one otherwise).
bool hasUserTempFolder(const FpkgSettings& s)
{
    return !s.userTempFolder.isEmpty() && QFileInfo(s.userTempFolder).isDir();
}

// A folder for a large temporary tree (a staged image, an extracted base
// package): under the job's temporary folder when the user chose one, else in
// a tracked work folder next to the output, because the app data drive is
// often the small one. `guard` removes that work folder when the build ends.
Result<QString> largeWorkFolder(const QString& name, const QString& output, const FpkgSettings& s, const JobContext& ctx,
                                TrackedWorkFolder& guard)
{
    QString parent = ctx.tempDir;
    if (!hasUserTempFolder(s)) {
        (void)sweepStaleBackportWork();
        parent = QDir(output).filePath(
            QStringLiteral(".fw-stage-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
        FW_TRY(createTrackedWorkFolder(parent));
        guard.dir = parent;
    }
    const QString dir = QDir(parent).filePath(name);
    removeTreeNoFollow(dir);
    if (!QDir().mkpath(dir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create a temporary folder in %1.").arg(native(parent)));
    return dir;
}

bool onSameVolume(const QString& a, const QString& b)
{
    const QStorageInfo sa(a), sb(b);
    return sa.isValid() && sb.isValid() && sa.rootPath().compare(sb.rootPath(), Qt::CaseInsensitive) == 0;
}

Result<QString> extractBaseForMerge(const QString& basePackage, const QString& output, const PubToolchain& tc,
                                    const FpkgSettings& s, JobContext& ctx, AsciiAliases& aliases, TrackedWorkFolder& guard)
{
    FW_TRY_ASSIGN(const QString dest, largeWorkFolder(QStringLiteral("patch-base"), output, s, ctx, guard));
    // The base's files are needed only where the update folder lacks them.
    // img_extract cannot pick files, so the whole base is extracted; packages
    // are Kraken-compressed, so the tree is usually larger than the file.
    if (Status st = ensureFreeSpace(dest, kExtractedTreeFactor * QFileInfo(basePackage).size(), s.freeSpaceMarginBytes);
        !st.ok()) {
        Error e = st.takeError();
        e.message += hasUserTempFolder(s)
            ? QStringLiteral(" The base package is extracted to the temporary folder first: give the original game "
                             "folder to skip this, or choose a temporary folder with more room in Settings > Storage.")
            : QStringLiteral(" The base package is extracted next to the output first: give the original game folder "
                             "to skip this, or set a temporary folder on another drive in Settings > Storage.");
        return e;
    }
    FW_TRY(mapDir(aliases, QFileInfo(basePackage).absolutePath(), QStringLiteral("ref")));
    auto destTool = aliases.map(dest, QStringLiteral("base"));
    if (!destTool.ok())
        return destTool.takeError();
    ctx.log.info(QStringLiteral("Extracting the base package to find the files the update folder does not have "
                                "(give the original game folder to skip this)."));
    ToolRunOptions o;
    o.program = native(tc.exe);
    o.arguments = imgExtractArgs(s.passcode, aliases.toToolPath(basePackage), native(destTool.value()));
    o.workingDirectory = chooseWorkingDir(tc, dest);
    o.environment = publisherEnv(QDir(ctx.tempDir).filePath(QStringLiteral("extract-tmp")), aliases, ctx.log);
    o.withBar = false;
    applyJobLimits(o, s, ctx);
    const qint64 free0 = diskSpace(dest).ok() ? diskSpace(dest)->available : 0;
    ProgressReporter* pr = ctx.progress;
    o.pollBytes = [dest, free0] {
        auto d = diskSpace(dest);
        return d.ok() ? std::max<qint64>(0, free0 - d->available) : 0;
    };
    o.onBytesWritten = [pr](qint64 b) { pr->setCurrentItem(QStringLiteral("%1 extracted").arg(formatBytes(b))); };
    o.onLine = [&ctx](const ToolLine& l) { logToolLine(ctx.log, l); };
    auto r = runPubTool(o, ctx.token);
    if (!r.ok())
        return r.takeError();
    if (r->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    if (!r->ok())
        return packFailure(r.value(), QStringLiteral("Extracting the base package"));
    const QString root = findAppRoot(dest, 2);
    if (root.isEmpty()) {
        ctx.log.warn(QStringLiteral("The extracted base has no sce_sys folder; its files are used as extracted."));
        return dest;
    }
    return root;
}

} // namespace

Result<JobOutcome> runBuild(const BuildRequest& req, const FpkgSettings& s, JobContext& ctx)
{
    ProgressReporter* pr = ctx.progress;
    const Log& log = ctx.log;
    pr->setPhases(buildPhases(), kBuildWeights);
    FW_TRY(validateBuildRequest(req, s));

    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    JobOutcome outcome;
    for (const QString& w : tc.warnings) {
        log.warn(w);
        outcome.warnings << w;
    }
    log.info(QStringLiteral("Publishing Tools: %1").arg(tc.summary()));
    const QString dds2png = locateDds2png(tc, s.dds2pngPath);

    const QFileInfo srcInfo(req.source);
    const QString output = QFileInfo(req.output.isEmpty() ? defaultOutputFolder(req.source, s.defaultOutputDir) : req.output)
                               .absoluteFilePath();
    const bool outputIsNew = !QFileInfo::exists(output);
    if (!QDir().mkpath(output))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the output folder %1.").arg(native(output)));
    // A build that fails before it wrote anything leaves no empty folder
    // behind (rmdir only removes an empty one).
    ScopeExit dropEmptyOutput([&] {
        if (outputIsNew)
            QDir().rmdir(output);
    });

    // ---- source -------------------------------------------------------------------
    pr->beginPhase(PhPrepare, 0);
    QString sourceRoot;
    const bool gp5Source = srcInfo.isFile() && srcInfo.suffix().compare(QStringLiteral("gp5"), Qt::CaseInsensitive) == 0;
    QStringList forbidden;
    std::shared_ptr<SourceStager> folderStager = srcInfo.isDir() ? folderStagerFor(req.source) : nullptr;
    // Work folders on the output drive (a staged image, an extracted base),
    // removed when the build ends, after the ASCII links into them.
    TrackedWorkFolder stageGuard, baseGuard;
    if (srcInfo.isDir() && !folderStager) {
        sourceRoot = srcInfo.absoluteFilePath();
    } else if (!gp5Source) {
        auto stager = folderStager ? folderStager : sourceStagerFor(req.source);
        if (!stager)
            return Error(ErrorCode::Unsupported, QStringLiteral("No reader for %1 images is available.").arg(srcInfo.suffix()));
        // Staged next to the output (the drive the package goes to) unless the
        // user chose a temporary folder: the app data drive is often the small one.
        FW_TRY_ASSIGN(const QString stage, largeWorkFolder(QStringLiteral("stage"), output, s, ctx, stageGuard));
        const qint64 estimated = stager->estimateStagedBytes(req.source); // opens the image once
        const qint64 est = estimated > 0 ? estimated : srcInfo.size();
        // When the staged tree shares the drive with the package, both must
        // fit: asking only for the tree would copy it and then stop.
        qint64 stageNeed = est + est / 50;
        if (onSameVolume(stage, output))
            stageNeed += qint64(double(est) * 1.1 * (req.isUpdate() ? 2.0 : 1.0));
        FW_TRY(ensureFreeSpace(stage, stageNeed, s.freeSpaceMarginBytes));
        if (!stageGuard.dir.isEmpty()) {
            // Staged onto the output drive: a FAT32 drive cannot hold the game's
            // files above 4 GiB (nor the package), so stop before copying anything.
            FW_TRY_ASSIGN(const QStringList volumeNotes, checkOutputVolume(output, est));
            outcome.warnings << volumeNotes;
        }
        const QString note = stager->stagingNote(req.source);
        log.info(note.isEmpty() ? QStringLiteral("Copying the files of %1 (%2) to a temporary folder.").arg(srcInfo.fileName(), stager->displayName())
                                : note);
        pr->beginPhase(PhPrepare, est);
        FW_TRY(stager->stage(req.source, stage, ctx.token, pr));
        if (!ctx.token.checkpoint())
            return Error::cancelled();
        sourceRoot = findAppRoot(stage, 3);
        if (sourceRoot.isEmpty())
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 holds no PS5 app (no folder with sce_sys was found).").arg(srcInfo.fileName()));
    }
    if (!sourceRoot.isEmpty())
        forbidden << sourceRoot;
    if (gp5Source)
        forbidden << srcInfo.absolutePath();
    AsciiAliases aliases(aliasBases(ctx.tempDir, output), forbidden, log);

    // ---- a user's .gp5 project, passed to img_create as it is ------------------------------
    if (gp5Source) {
        pr->beginPhase(PhPlan, 0);
        auto gp = readGp5(srcInfo.absoluteFilePath());
        if (!gp.ok())
            return gp.takeError();
        const Gp5Source& g = gp.value();
        if (g.hasRootDir || g.files.empty())
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 lists no files (folder-walk projects are not supported). Use a project with "
                                        "explicit <file> entries or build from the game folder.").arg(srcInfo.fileName()));
        if (g.volumeType != u"prospero_app")
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 is a %2 project; only prospero_app projects are built from .gp5 files (use "
                                        "\"Build an add-on\" for add-ons).").arg(srcInfo.fileName(), g.volumeType));
        if (const QString pc = passcodeProblem(g.passcode); !pc.isEmpty())
            return Error(ErrorCode::CorruptData, QStringLiteral("The project's passcode: %1").arg(pc));
        QStringList missing, nonAscii, badDst;
        bool relative = false;
        QString paramSrc, keystoneSrc;
        qint64 sourceBytes = 0;
        for (const Gp5FileEntry& f : g.files) {
            const QString real = resolveGp5SourcePath(g, f.src);
            if (!QFileInfo(f.src).isAbsolute())
                relative = true;
            if (!QFileInfo(real).isFile())
                missing << f.src;
            else
                sourceBytes += QFileInfo(real).size();
            if (!isAsciiPath(f.src))
                nonAscii << f.src;
            const QString pr2 = pathRuleProblem(f.dst);
            if (!pr2.isEmpty())
                badDst << QStringLiteral("%1 (%2)").arg(f.dst, pr2);
            if (f.dst.compare(QStringLiteral("sce_sys/param.json"), Qt::CaseInsensitive) == 0)
                paramSrc = real;
            if (f.dst.compare(QStringLiteral("sce_sys/keystone"), Qt::CaseInsensitive) == 0)
                keystoneSrc = real;
        }
        if (!missing.isEmpty())
            return Error(ErrorCode::NotFound, QStringLiteral("%1 file(s) named in the project do not exist: %2")
                                                  .arg(missing.size()).arg(listSome(missing, 5)));
        if (!nonAscii.isEmpty())
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("The project names source paths with non-ASCII characters, which the Publishing "
                                        "Tools reject: %1").arg(listSome(nonAscii, 5)));
        if (!badDst.isEmpty())
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 package path(s) in the project are not accepted by the Publishing Tools: %2")
                             .arg(badDst.size()).arg(listSome(badDst)));
        if (paramSrc.isEmpty())
            return Error(ErrorCode::CorruptData, QStringLiteral("The project has no sce_sys/param.json entry."));
        auto prm = readParamJson(paramSrc);
        if (!prm.ok())
            return prm.takeError();
        const QString cid = prm->value(QStringLiteral("contentId")).toString().trimmed();
        const auto ver = ContentVersion::parse(prm->value(QStringLiteral("contentVersion")).toString());
        if (!isValidContentId(cid) || !ver)
            return Error(ErrorCode::CorruptData, QStringLiteral("The project's param.json has no valid contentId and contentVersion."));
        if (!g.contentId.isEmpty() && g.contentId != cid)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The project's content_id %1 differs from its param.json (%2).").arg(g.contentId, cid));
        if (!req.contentId.isEmpty() && req.contentId != cid)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The Content ID %1 does not match the project (%2).").arg(req.contentId, cid));
        if (keystoneSrc.isEmpty() || QFileInfo(keystoneSrc).size() != 96)
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("The project needs the dump's own 96-byte sce_sys/keystone."));
        const QString stem = packageStem(cid, *ver);
        // The checks a folder build makes before packing: the name conflict
        // that would stop the job, and room for the package.
        QStringList finals;
        if (!req.isUpdate()) {
            finals << QDir(output).filePath(fullPackageName(stem));
        } else {
            if (req.updateOutput != UpdateOutput::FullOnly) finals << QDir(output).filePath(updatePackageName(stem));
            if (req.updateOutput != UpdateOutput::UpdateOnly) finals << QDir(output).filePath(companionName(stem));
        }
        if (s.outputConflict == OutputConflict::Fail)
            for (const QString& f : std::as_const(finals))
                if (QFileInfo::exists(f))
                    return Error(ErrorCode::AlreadyExists,
                                 QStringLiteral("%1 already exists in the output folder.").arg(QFileInfo(f).fileName()));
        const qint64 needOut = qint64(double(sourceBytes) * 1.1 * (req.isUpdate() ? 2.0 : 1.0));
        FW_TRY(ensureFreeSpace(output, needOut, s.freeSpaceMarginBytes));
        if (const QString w = fatWarning(output); !w.isEmpty())
            outcome.warnings << w;
        const QString logsDir = QDir(output).filePath(stem + QStringLiteral("-build-logs"));
        QDir().mkpath(logsDir);
        (void)writeFileAtomic(QDir(logsDir).filePath(QStringLiteral("01-create-gp5.log")),
                              logBytes({QStringLiteral("project=%1 (used as it is)").arg(native(g.path)),
                                        QStringLiteral("files=%1").arg(g.files.size()),
                                        QStringLiteral("empty_dirs=%1").arg(g.emptyDirs.size())}));
        if (req.isUpdate())
            FW_TRY(checkBasePackagePath(req.basePackage, {QDir(output).filePath(updatePackageName(stem)),
                                                          QDir(output).filePath(companionName(stem))}));
        FW_TRY(mapDir(aliases, srcInfo.absolutePath(), QStringLiteral("prj")));
        FW_TRY(mapDir(aliases, output, QStringLiteral("out")));
        if (req.isUpdate())
            FW_TRY(mapDir(aliases, QFileInfo(req.basePackage).absolutePath(), QStringLiteral("ref")));
        OutputLock lock;
        FW_TRY(lock.acquire(lockKey(cid, output), ctx.token, log));
        PackJob j;
        j.tc = &tc;
        j.s = &s;
        j.ctx = &ctx;
        j.aliases = &aliases;
        j.gp5 = srcInfo.absoluteFilePath();
        j.outputDir = output;
        j.stem = stem;
        j.logsDir = logsDir;
        j.contentId = cid;
        j.passcode = g.passcode;
        j.update = req.isUpdate();
        j.basePackage = QFileInfo(req.basePackage).absoluteFilePath();
        j.updateOutput = req.updateOutput;
        if (relative) {
            if (!isAsciiPath(srcInfo.absolutePath()))
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("The project uses relative paths and its folder path is not ASCII. Move it to "
                                            "an ASCII path."));
            j.workingDir = srcInfo.absolutePath();
        }
        j.outcome = outcome;
        auto res = packAndFinish(j);
        if (res.ok())
            res->details.insert(QStringLiteral("source"), QStringLiteral("gp5"));
        return res;
    }

    // ---- update: the base ------------------------------------------------------------
    QStringList roots{sourceRoot};
    if (req.isUpdate()) {
        const HeaderCheck bh = checkPackageHeader(req.basePackage);
        if (!bh.hasFih)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 is not a full PS5 package (%2). Choose the base package installed on the console.")
                             .arg(QFileInfo(req.basePackage).fileName(), bh.summary()));
        if (!bh.ok)
            outcome.warnings << QStringLiteral("The base package's header: %1.").arg(bh.summary());
        log.info(QStringLiteral("Update against %1. The base must have been built by the Publishing Tools, with the same "
                                "Content ID and a lower contentVersion.").arg(native(req.basePackage)));
        if (!req.exactSource) {
            if (!req.baseFolder.isEmpty()) {
                roots << QFileInfo(req.baseFolder).absoluteFilePath();
            } else {
                auto extracted = extractBaseForMerge(QFileInfo(req.basePackage).absoluteFilePath(), output, tc, s, ctx,
                                                     aliases, baseGuard);
                if (!extracted.ok())
                    return extracted.takeError();
                roots << extracted.value();
            }
        }
    }
    if (!ctx.token.checkpoint())
        return Error::cancelled();

    // ---- plan ---------------------------------------------------------------------------
    pr->beginPhase(PhPlan, 0);
    PlanOptions po;
    po.roots = roots;
    po.outputDir = output;
    po.isUpdate = req.isUpdate();
    po.contentId = req.contentId;
    po.contentVersion = req.contentVersion;
    po.title = req.title;
    po.haveDds2png = !dds2png.isEmpty();
    auto planned = planBuild(po, s, ctx.token);
    if (!planned.ok())
        return planned.takeError();
    BuildPlan plan = std::move(planned).value();
    outcome.warnings << plan.warnings;
    log.info(QStringLiteral("%1 version %2: %3 files, %4; PlayGo %5; %6 left out; %7 empty folder(s)")
                 .arg(plan.contentId, plan.version.text())
                 .arg(plan.scan.files.size())
                 .arg(formatBytes(plan.scan.totalBytes), playGoLayoutName(plan.playgo.layout))
                 .arg(plan.scan.excluded.size())
                 .arg(plan.emptyDirs.size()));
    for (const QString& w : std::as_const(plan.warnings))
        log.warn(w);

    QStringList finals;
    if (!plan.isUpdate) {
        finals << QDir(output).filePath(fullPackageName(plan.stem));
    } else {
        if (req.updateOutput != UpdateOutput::FullOnly) finals << QDir(output).filePath(updatePackageName(plan.stem));
        if (req.updateOutput != UpdateOutput::UpdateOnly) finals << QDir(output).filePath(companionName(plan.stem));
        FW_TRY(checkBasePackagePath(req.basePackage, finals));
        if (plan.scan.filesFromRoot[0] == 0)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The update folder adds or replaces no file of the base. There is nothing to put "
                                        "in an update."));
        FW_TRY(checkUpdateAgainstBase(plan, roots, outcome.warnings));
        log.info(QStringLiteral("Update: %1 file(s) from the update folder, %2 from the base.")
                     .arg(plan.scan.filesFromRoot[0]).arg(plan.scan.filesFromRoot[1]));
    }
    if (s.outputConflict == OutputConflict::Fail)
        for (const QString& f : finals)
            if (QFileInfo::exists(f))
                return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists in the output folder.").arg(QFileInfo(f).fileName()));

    OutputLock lock;
    FW_TRY(lock.acquire(lockKey(plan.contentId, output), ctx.token, log));

    qint64 generated = 0;
    for (const PlannedEntry& e : plan.entries)
        if (e.kind != EntryKind::GameFile)
            generated += e.size;
    const qint64 needOut = qint64(double(plan.uncompressedBytes) * 1.1 * (plan.isUpdate ? 2.0 : 1.0)) + generated;
    FW_TRY(ensureFreeSpace(output, needOut, s.freeSpaceMarginBytes));
    if (const QString w = fatWarning(output); !w.isEmpty())
        outcome.warnings << w;

    // ---- generated inputs -------------------------------------------------------------------
    pr->beginPhase(PhGenerate, generated);
    for (const QString& r : roots)
        FW_TRY(mapDir(aliases, r, plan.isUpdate && r == roots.first() ? QStringLiteral("upd") : QStringLiteral("src")));
    FW_TRY(mapDir(aliases, output, QStringLiteral("out")));
    if (plan.isUpdate)
        FW_TRY(mapDir(aliases, QFileInfo(req.basePackage).absolutePath(), QStringLiteral("ref")));
    if (!QDir().mkpath(plan.assetsDir) || !QDir().mkpath(plan.logsDir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the project folders in %1.").arg(native(output)));

    // Image conversions (parallel), then the copies and payloads.
    if (!plan.images.conversions.empty()) {
        const auto& conv = plan.images.conversions;
        std::atomic<size_t> next{0};
        std::vector<std::optional<Error>> errors(conv.size());
        const int threads = std::clamp(s.workerThreads / 2, 2, 8);
        std::vector<std::thread> pool;
        for (int t = 0; t < std::min<int>(threads, int(conv.size())); ++t) {
            pool.emplace_back([&] {
                for (size_t i = next++; i < conv.size(); i = next++) {
                    if (!ctx.token.checkpoint())
                        return;
                    const ImageConversion& c = conv[i];
                    const QString out = QDir(plan.assetsDir).filePath(QStringLiteral("sce_sys/") + c.dst.section(u'/', -1));
                    QDir().mkpath(QFileInfo(out).absolutePath());
                    QFile::remove(out);
                    ProcessSpec spec;
                    spec.program = native(dds2png);
                    spec.arguments = dds2pngArgs(aliases.toToolPath(c.ddsAbs), aliases.toToolPath(out),
                                                 c.requirement != PngRequirement::Rgb);
                    spec.workingDirectory = isAsciiPath(QFileInfo(dds2png).absolutePath()) ? QFileInfo(dds2png).absolutePath()
                                                                                         : QDir::tempPath();
                    spec.keepOutput = true;
                    auto r = runProcess(spec, ctx.token);
                    if (!r.ok()) {
                        errors[i] = r.takeError();
                        continue;
                    }
                    if (r->cancelled)
                        return;
                    const PngInfo info = readPngInfo(out);
                    if (!r->succeeded() || !pngMeets(info, c.requirement))
                        errors[i] = Error(ErrorCode::ExternalToolFailed,
                                          QStringLiteral("prospero-dds2png could not rebuild %1 from %2 (%3).")
                                              .arg(c.dst, c.ddsRel,
                                                   r->succeeded() ? QStringLiteral("the result is not %1").arg(requirementText(c.requirement))
                                                                  : QStringLiteral("exit code %1").arg(r->exitCode)),
                                          r->output.left(2000));
                    else
                        log.info(QStringLiteral("Rebuilt %1 from %2.").arg(c.dst, c.ddsRel));
                }
            });
        }
        for (auto& th : pool)
            th.join();
        if (ctx.token.isCancelled())
            return Error::cancelled();
        for (auto& e : errors)
            if (e)
                return *e;
        for (PlannedEntry& e : plan.entries)
            if (e.kind == EntryKind::RecoveredImage)
                e.size = QFileInfo(e.src).size();
        FW_TRY(finalizeParam(plan, po, s));
    }
    for (const PlannedEntry& e : plan.entries) {
        if (!ctx.token.checkpoint())
            return Error::cancelled();
        switch (e.kind) {
        case EntryKind::Payload:
        case EntryKind::Filler:
            FW_TRY(writeZeroFile(e.src, kPayloadBytes, ctx.token));
            pr->advance(e.size);
            break;
        case EntryKind::RepairedSelf:
            FW_TRY(writeSelfRepairCopy(e.origin, e.src, e.selfPlan.value_or(SelfRepairPlan{}), ctx.token));
            pr->advance(e.size);
            break;
        case EntryKind::GeneratedFile:
            FW_TRY(writeFileAtomic(e.src, e.data));
            pr->advance(e.size);
            break;
        case EntryKind::RepairedUcp: {
            QFile in(toLongPath(e.origin));
            if (!in.open(QIODevice::ReadOnly))
                return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(e.dst), in.errorString());
            // Read again since planning: only an archive that still passes the check is corrected.
            const QByteArray archive = in.readAll();
            if (!checkUcp(archive).structureOk)
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 changed while it was being packed.").arg(e.dst));
            FW_TRY(writeFileAtomic(e.src, withRepairedUcpDigest(archive)));
            pr->advance(e.size);
            break;
        }
        default: break;
        }
    }
    FW_TRY(writeFileAtomic(QDir(plan.assetsDir).filePath(QStringLiteral("sce_sys/param.json")), plan.paramBytes));
    FW_TRY(writeFileAtomic(plan.scenarioPath, plan.playgo.scenarioBytes));
    const Gp5Project project = projectFromPlan(plan, s, [&](const QString& real) { return aliases.toToolPath(real); });
    FW_TRY(writeFileAtomic(plan.gp5Path, writeGp5(project)));

    QStringList createLog;
    createLog << QStringLiteral("project=%1").arg(native(plan.gp5Path))
              << QStringLiteral("source=%1").arg(native(roots.first()));
    if (roots.size() > 1)
        createLog << QStringLiteral("base=%1").arg(native(roots[1]));
    createLog << QStringLiteral("content_id=%1").arg(plan.contentId)
              << QStringLiteral("content_version=%1").arg(plan.version.text())
              << QStringLiteral("files=%1").arg(plan.fileEntries)
              << QStringLiteral("bytes=%1").arg(plan.uncompressedBytes)
              << QStringLiteral("empty_dirs=%1").arg(plan.emptyDirs.size());
    // Staged images, an extracted base and ASCII links are gone when the build ends.
    if (!aliases.links().isEmpty() || folderStager || !srcInfo.isDir()
        || (req.isUpdate() && !req.exactSource && req.baseFolder.isEmpty()))
        createLog << QStringLiteral("note: the project names temporary paths (a staged image, an extracted base package "
                                    "or ASCII folder links) that are removed when the build ends; a kept project cannot "
                                    "be built again as it is");
    createLog << plan.log;
    for (const QString& n : std::as_const(plan.paramNotes))
        createLog << QStringLiteral("param.json: %1").arg(n);
    createLog << plan.size.notes;
    for (const QString& w : std::as_const(plan.size.warnings)) {
        log.warn(w);
        outcome.warnings << w;
    }
    for (const QString& w : std::as_const(outcome.warnings))
        createLog << QStringLiteral("warning: %1").arg(w);
    FW_TRY(writeFileAtomic(QDir(plan.logsDir).filePath(QStringLiteral("01-create-gp5.log")), logBytes(createLog)));
    log.info(QStringLiteral("Project written: %1").arg(native(plan.gp5Path)));
    if (!ctx.token.checkpoint())
        return Error::cancelled();

    // ---- pre-scan ------------------------------------------------------------------------------
    if (s.prescan) {
        QStringList files;
        for (const PlannedEntry& e : plan.entries)
            if (e.kind == EntryKind::GameFile)
                files << e.src;
        pr->beginPhase(PhPrescan, 0, files.size(), false);
        FW_TRY(prescanFiles(files, std::clamp(2 * s.workerThreads, 8, 32), ctx.token, pr));
    }

    // ---- pack and finish -------------------------------------------------------------------------
    PackJob j;
    j.tc = &tc;
    j.s = &s;
    j.ctx = &ctx;
    j.aliases = &aliases;
    j.gp5 = plan.gp5Path;
    j.outputDir = output;
    j.stem = plan.stem;
    j.logsDir = plan.logsDir;
    j.contentId = plan.contentId;
    j.update = plan.isUpdate;
    j.basePackage = QFileInfo(req.basePackage).absoluteFilePath();
    j.updateOutput = req.updateOutput;
    j.keepNapsMetric = req.keepNapsMetric;
    j.keepProjectOnSuccess = s.keepIntermediate;
    j.projectFiles = {plan.gp5Path, plan.scenarioPath};
    j.projectDirs = {plan.assetsDir};
    j.outcome = outcome;
    auto res = packAndFinish(j);
    if (!res.ok())
        return res;
    JobOutcome o = std::move(res).value();
    o.details.insert(QStringLiteral("version"), plan.version.text());
    o.details.insert(QStringLiteral("files"), plan.fileEntries);
    o.details.insert(QStringLiteral("bytes"), double(plan.uncompressedBytes));
    o.details.insert(QStringLiteral("playgo"), playGoLayoutName(plan.playgo.layout));
    o.details.insert(QStringLiteral("excluded"), int(plan.scan.excluded.size()));
    o.details.insert(QStringLiteral("emptyDirs"), int(plan.emptyDirs.size()));
    if (plan.size.applied)
        o.details.insert(QStringLiteral("attributePub"), plan.size.attributePub);
    if (plan.isUpdate)
        o.details.insert(QStringLiteral("update"), true);
    return o;
}

// ---- add-ons ------------------------------------------------------------------------------------

Result<JobOutcome> runAddon(const AddonRequest& req, const FpkgSettings& s, JobContext& ctx)
{
    ProgressReporter* pr = ctx.progress;
    const Log& log = ctx.log;
    pr->setPhases(addonPhases(), kBuildWeights);
    FW_TRY(validateAddonRequest(req, s));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    JobOutcome outcome;
    outcome.warnings << tc.warnings;
    outcome.warnings << QStringLiteral("Add-on packages built with the Publishing Tools are experimental: they have not "
                                       "been tested on a console.");

    const QString src = QFileInfo(req.payloadFolder).absoluteFilePath();
    const QString output = QFileInfo(req.output.isEmpty() ? defaultOutputFolder(src, s.defaultOutputDir) : req.output).absoluteFilePath();
    const bool outputIsNew = !QFileInfo::exists(output);
    if (!QDir().mkpath(output))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the output folder %1.").arg(native(output)));
    ScopeExit dropEmptyOutput([&] {
        if (outputIsNew)
            QDir().rmdir(output);
    });
    pr->beginPhase(PhPlan, 0);
    auto prm = readParamJson(findPathCaseInsensitive(src, QStringLiteral("sce_sys/param.json"), false));
    if (!prm.ok())
        return prm.takeError();
    const QString cid = prm->value(QStringLiteral("contentId")).toString().trimmed();
    if (!isValidContentId(cid))
        return Error(ErrorCode::CorruptData, QStringLiteral("The add-on's param.json has no valid contentId (\"%1\").").arg(cid));
    if (!req.contentId.isEmpty() && req.contentId != cid)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The Content ID %1 does not match the add-on's param.json (%2).").arg(req.contentId, cid));
    const auto ver = ContentVersion::parse(prm->value(QStringLiteral("contentVersion")).toString()).value_or(ContentVersion{});
    const QString stem = packageStem(cid, ver);

    ScanOptions so;
    so.leftovers.keepDlcEmulator = true;
    so.leftovers.removeAmprLeftovers = s.removeAmprLeftovers;
    so.leftovers.removeDumpLeftovers = s.removeDumpLeftovers;
    auto scanned = scanSource({src}, so, ctx.token);
    if (!scanned.ok())
        return scanned.takeError();
    const ScanResult scan = std::move(scanned).value();
    if (!scan.badPaths.empty()) {
        QStringList items;
        for (const BadPath& b : scan.badPaths)
            items << QStringLiteral("%1 (%2)").arg(b.rel, b.reason);
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 file name(s) are not accepted by the Publishing Tools: %2").arg(items.size()).arg(listSome(items)));
    }
    FW_TRY(ensureFreeSpace(output, qint64(double(scan.totalBytes) * 1.1), s.freeSpaceMarginBytes));
    const QStringList finals{QDir(output).filePath(fullPackageName(stem))};
    if (s.outputConflict == OutputConflict::Fail && QFileInfo::exists(finals.first()))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists in the output folder.").arg(fullPackageName(stem)));

    AsciiAliases aliases(aliasBases(ctx.tempDir, output), {src}, log);
    FW_TRY(mapDir(aliases, src, QStringLiteral("src")));
    FW_TRY(mapDir(aliases, output, QStringLiteral("out")));
    OutputLock lock;
    FW_TRY(lock.acquire(lockKey(cid, output), ctx.token, log));

    pr->beginPhase(PhGenerate, 0);
    const QString logsDir = QDir(output).filePath(stem + QStringLiteral("-build-logs"));
    const QString gp5Path = QDir(output).filePath(stem + QStringLiteral(".gp5"));
    QDir().mkpath(logsDir);
    Gp5Project g;
    g.volumeType = QStringLiteral("prospero_ac");
    g.passcode = s.passcode;
    g.contentId = cid;
    g.entitlementKey = req.entitlementKey.toLower();
    for (const ScannedFile& f : scan.files)
        g.files.push_back({f.rel, aliases.toToolPath(f.absPath), std::nullopt, false});
    if (s.keepEmptyFolders)
        for (const QString& d : scan.emptyDirs)
            if (pathRuleProblem(d).isEmpty())
                g.emptyDirs << d;
    FW_TRY(writeFileAtomic(gp5Path, writeGp5(g)));
    QStringList createLog{QStringLiteral("project=%1").arg(native(gp5Path)), QStringLiteral("volume_type=prospero_ac"),
                          QStringLiteral("content_id=%1").arg(cid), QStringLiteral("files=%1").arg(g.files.size())};
    for (const ExcludedEntry& e : scan.excluded)
        createLog << QStringLiteral("excluded: %1%2 (%3)").arg(e.rel, e.isDir ? QStringLiteral("/") : QString(), e.reason);
    FW_TRY(writeFileAtomic(QDir(logsDir).filePath(QStringLiteral("01-create-gp5.log")), logBytes(createLog)));

    PackJob j;
    j.tc = &tc;
    j.s = &s;
    j.ctx = &ctx;
    j.aliases = &aliases;
    j.gp5 = gp5Path;
    j.outputDir = output;
    j.stem = stem;
    j.logsDir = logsDir;
    j.contentId = cid;
    j.what = QStringLiteral("Packing the add-on");
    j.keepProjectOnSuccess = s.keepIntermediate;
    j.projectFiles = {gp5Path};
    j.outcome = outcome;
    auto res = packAndFinish(j);
    if (res.ok())
        res->details.insert(QStringLiteral("volumeType"), QStringLiteral("prospero_ac"));
    return res;
}

// ---- verify / extract existing packages ----------------------------------------------------------

namespace {
// A compact update cannot be parsed by img_verify/img_extract: use its .remastered.pkg.
QString toolTargetFor(const QString& package, const Log& log)
{
    const QFileInfo fi(package);
    const HeaderCheck hc = checkPackageHeader(package);
    if (hc.hasFih)
        return fi.absoluteFilePath();
    QString name = fi.fileName();
    if (name.startsWith(QStringLiteral("UPDATE_"), Qt::CaseInsensitive))
        name = name.mid(7);
    const QString stem = name.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive) ? name.chopped(4) : name;
    const QString companion = fi.dir().filePath(companionName(stem));
    if (QFileInfo(companion).isFile()) {
        log.info(QStringLiteral("%1 is an update delta; using %2 instead.").arg(fi.fileName(), QFileInfo(companion).fileName()));
        return companion;
    }
    return fi.absoluteFilePath();
}
} // namespace

Result<JobOutcome> runVerify(const VerifyRequest& req, const FpkgSettings& s, JobContext& ctx)
{
    ProgressReporter* pr = ctx.progress;
    pr->setPhases({QStringLiteral("Checking the header"), QStringLiteral("Verifying with img_verify")}, {1, 99});
    if (!QFileInfo(req.package).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 was not found.").arg(native(req.package)));
    const QString passcode = req.passcode.isEmpty() ? s.passcode : req.passcode;
    if (!passcodeProblem(passcode).isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(passcodeProblem(passcode)));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    JobOutcome o;
    o.warnings << tc.warnings;
    pr->beginPhase(0, 0);
    const QString target = toolTargetFor(req.package, ctx.log);
    const HeaderCheck hc = checkPackageHeader(target);
    if (hc.ok)
        ctx.log.info(QStringLiteral("Header: %1").arg(hc.summary()));
    else
        ctx.log.warn(QStringLiteral("Header: %1").arg(hc.summary()));

    pr->beginPhase(1, 0);
    AsciiAliases aliases(aliasBases(ctx.tempDir, target), {}, ctx.log);
    auto dir = aliases.map(QFileInfo(target).absolutePath(), QStringLiteral("pkg"));
    if (!dir.ok())
        return dir.takeError();
    ToolRunOptions ro;
    ro.program = native(tc.exe);
    ro.arguments = imgVerifyArgs(passcode, req.integrity, aliases.toToolPath(target));
    ro.workingDirectory = chooseWorkingDir(tc, target);
    ro.environment = publisherEnv(QDir(ctx.tempDir).filePath(QStringLiteral("verify-tmp")), aliases, ctx.log);
    ro.withBar = false;
    ro.onLine = [&ctx](const ToolLine& l) { logToolLine(ctx.log, l); };
    applyJobLimits(ro, s, ctx);
    auto r = runPubTool(ro, ctx.token);
    if (!r.ok())
        return r.takeError();
    if (r->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    o.outputs << QFileInfo(target).absoluteFilePath();
    if (r->ok()) {
        o.verification = hc.ok ? Verification::Passed : Verification::Inconclusive;
        o.verificationDetail = hc.ok ? QStringLiteral("Header checked; img_verify passed (%1).").arg(req.integrity ? QStringLiteral("format and integrity") : QStringLiteral("format"))
                                     : QStringLiteral("img_verify passed, but the header check found: %1").arg(hc.summary());
        o.summary = QStringLiteral("%1 verified").arg(QFileInfo(target).fileName());
    } else {
        o.verification = Verification::Failed;
        o.verificationDetail = QStringLiteral("img_verify: %1%2").arg(r->failureText(), hc.ok ? QString() : QStringLiteral(" Header: %1.").arg(hc.summary()));
        o.summary = QStringLiteral("%1 failed verification").arg(QFileInfo(target).fileName());
    }
    o.details.insert(QStringLiteral("plaintext"), hc.plaintext);
    o.details.insert(QStringLiteral("exitCode"), r->exitCode);
    return o;
}

Result<JobOutcome> runExtract(const ExtractRequest& req, const FpkgSettings& s, JobContext& ctx)
{
    ProgressReporter* pr = ctx.progress;
    pr->setPhases({QStringLiteral("Extracting")}, {1});
    if (!QFileInfo(req.package).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 was not found.").arg(native(req.package)));
    const QString dest = QFileInfo(req.destination).absoluteFilePath();
    const QFileInfo di(dest);
    if (di.exists() && (!di.isDir() || !QDir(dest).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("The destination folder is not empty: %1").arg(native(dest)));
    const QString passcode = req.passcode.isEmpty() ? s.passcode : req.passcode;
    if (!passcodeProblem(passcode).isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode: %1").arg(passcodeProblem(passcode)));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    const bool created = !di.exists();
    if (!QDir().mkpath(dest))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(dest)));
    // Everything in the destination is ours (it was empty): remove it if we fail.
    ScopeExit cleanup([&] {
        QDir d(dest);
        for (const QFileInfo& fi : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (fi.isDir() && !fi.isSymbolicLink())
                removeTreeNoFollow(fi.absoluteFilePath());
            else
                QFile::remove(fi.absoluteFilePath());
        }
        if (created)
            QDir().rmdir(dest);
    });
    const QString target = toolTargetFor(req.package, ctx.log);
    FW_TRY(ensureFreeSpace(dest, kExtractedTreeFactor * QFileInfo(target).size(), s.freeSpaceMarginBytes));
    AsciiAliases aliases(aliasBases(ctx.tempDir, dest), {}, ctx.log);
    auto pkgDir = aliases.map(QFileInfo(target).absolutePath(), QStringLiteral("pkg"));
    if (!pkgDir.ok())
        return pkgDir.takeError();
    auto destTool = aliases.map(dest, QStringLiteral("dst"));
    if (!destTool.ok())
        return destTool.takeError();

    pr->beginPhase(0, 0);
    ToolRunOptions ro;
    ro.program = native(tc.exe);
    ro.arguments = imgExtractArgs(passcode, aliases.toToolPath(target), native(destTool.value()));
    ro.workingDirectory = chooseWorkingDir(tc, target);
    ro.environment = publisherEnv(QDir(ctx.tempDir).filePath(QStringLiteral("extract-tmp")), aliases, ctx.log);
    ro.withBar = false;
    applyJobLimits(ro, s, ctx);
    const qint64 free0 = diskSpace(dest).ok() ? diskSpace(dest)->available : 0;
    ro.pollBytes = [dest, free0] {
        auto d = diskSpace(dest);
        return d.ok() ? std::max<qint64>(0, free0 - d->available) : 0;
    };
    ro.onBytesWritten = [pr](qint64 b) { pr->setCurrentItem(QStringLiteral("%1 written").arg(formatBytes(b))); };
    ro.onLine = [&ctx](const ToolLine& l) { logToolLine(ctx.log, l); };
    auto r = runPubTool(ro, ctx.token);
    if (!r.ok())
        return r.takeError();
    if (r->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    if (!r->ok())
        return packFailure(r.value(), QStringLiteral("img_extract"));
    qint64 files = 0, bytes = 0;
    QDirIterator it(dest, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++files;
        bytes += it.fileInfo().size();
    }
    if (files == 0)
        return Error(ErrorCode::ExternalToolFailed, QStringLiteral("img_extract reported success but wrote no files."));
    cleanup.dismiss();
    JobOutcome o;
    o.outputs << dest;
    o.verification = Verification::NotRun;
    o.summary = QStringLiteral("%1 files, %2 extracted").arg(formatCount(files), formatBytes(bytes));
    o.details.insert(QStringLiteral("files"), double(files));
    o.details.insert(QStringLiteral("bytes"), double(bytes));
    ctx.log.info(QStringLiteral("Extracted %1 file(s) to %2").arg(files).arg(native(dest)));
    return o;
}

// ---- dlc_emu.ini helper ----------------------------------------------------------------------------

std::vector<AddonCandidate> addonsFromDlcEmu(const QString& gameFolder)
{
    std::vector<AddonCandidate> out;
    const QString iniPath = findPathCaseInsensitive(gameFolder, QStringLiteral("dlc_emu.ini"), false);
    if (iniPath.isEmpty())
        return out;
    QFile f(iniPath);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const DlcEmuIni ini = parseDlcEmuIni(f.read(kDlcEmuIniMaxBytes + 1));
    for (const DlcEmuEntry& e : ini.allEntries) {
        AddonCandidate c;
        c.contentId = e.contentId;
        c.section = e.section;
        c.entitlementKey = e.entitlementKey;
        const QString mf = e.mountFolder();
        if (!mf.isEmpty())
            c.payloadFolder = findPathCaseInsensitive(gameFolder, mf, true);
        if (e.section == u"PSAL") {
            c.reason = QStringLiteral("entitlement-only add-ons are refused by the PS5 debug installer");
        } else if (e.section != u"PSAC") {
            c.reason = QStringLiteral("not an add-on entry");
        } else if (c.payloadFolder.isEmpty()) {
            c.reason = QStringLiteral("no local content folder for its mount point");
        } else if (findPathCaseInsensitive(c.payloadFolder, QStringLiteral("sce_sys/param.json"), false).isEmpty()) {
            c.reason = QStringLiteral("its folder has no sce_sys/param.json");
        } else {
            c.supported = true;
        }
        out.push_back(c);
    }
    return out;
}

// ---- helpers for composite jobs -------------------------------------------------------------------

namespace {

QString redactedCommand(const QStringList& args)
{
    QStringList shown;
    bool nextIsPasscode = false;
    for (const QString& a : args) {
        QString v = nextIsPasscode ? QStringLiteral("<redacted>") : a;
        nextIsPasscode = (a == u"--passcode");
        if (v.contains(u' '))
            v = QStringLiteral("\"%1\"").arg(v);
        shown << v;
    }
    return QStringLiteral("prospero-pub-cmd ") + shown.join(u' ');
}

} // namespace

Result<PublisherVerifyReport> verifyPublisherPackage(const QString& package, const PubToolchain& tc, const FpkgSettings& s,
                                                     JobContext& ctx)
{
    const Log& log = ctx.log;
    PublisherVerifyReport rep;
    const QFileInfo fi(package);
    rep.package = fi.absoluteFilePath();
    AsciiAliases aliases(aliasBases(ctx.tempDir, fi.absolutePath()), {}, log);
    FW_TRY(mapDir(aliases, fi.absolutePath(), QStringLiteral("vfy")));
    const QString tool = aliases.toToolPath(fi.absoluteFilePath());
    const QProcessEnvironment env = publisherEnv(QDir(ctx.tempDir).filePath(QStringLiteral("verify-tmp")), aliases, log);

    log.info(QStringLiteral("==> Checking publisher format and full integrity: %1").arg(fi.fileName()));
    log.info(QStringLiteral("    Full package integrity is required; application/SELF format diagnostics are reported "
                            "separately for debug/no-auth builds."));
    const auto pass = [&](bool format, bool integrity) -> Result<ToolRunResult> {
        ToolRunOptions o;
        o.program = native(tc.exe);
        o.arguments = imgVerifyArgs(s.passcode, format, integrity, tool);
        o.workingDirectory = chooseWorkingDir(tc, tool);
        o.environment = env;
        o.withBar = false;
        applyJobLimits(o, s, ctx);
        o.onLine = [&log](const ToolLine& l) { logToolLine(log, l); };
        log.info(redactedCommand(o.arguments));
        return runPubTool(o, ctx.token);
    };
    auto r1 = pass(false, true);
    if (!r1.ok())
        return r1.takeError();
    if (r1->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    // An [Error] line fails the required pass even with exit code 0, as for
    // every other run of the tool.
    if (!r1->ok())
        return Error(ErrorCode::VerificationFailed,
                     QStringLiteral("Required publisher full integrity verification failed (exit %1). No final output "
                                    "was published.").arg(r1->exitCode),
                     r1->failureText());
    rep.integrityPassed = true;
    log.info(QStringLiteral("    Required full integrity verification passed."));
    auto r2 = pass(true, false);
    if (!r2.ok())
        return r2.takeError();
    if (r2->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    rep.formatExitCode = r2->exitCode;
    rep.formatPassed = r2->exitCode == 0 && !r2->crashed;
    if (rep.formatPassed)
        log.info(QStringLiteral("    SDK application/SELF format diagnostics passed; console compatibility remains untested."));
    else
        log.warn(QStringLiteral("    SDK application/SELF format diagnostics reported errors (exit %1). Package "
                                "integrity passed; format acceptance and console compatibility remain unverified. "
                                "Review the diagnostic errors above.").arg(r2->exitCode));
    return rep;
}

Result<QString> extractPackage(const QString& package, const QString& destination, const PubToolchain& tc,
                               const FpkgSettings& s, JobContext& ctx)
{
    const QFileInfo fi(package);
    if (QFileInfo::exists(destination)
        && !QDir(destination).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 is not empty.").arg(native(destination)));
    if (!QDir().mkpath(destination))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(destination)));
    FW_TRY(ensureFreeSpace(destination, kExtractedTreeFactor * fi.size(), s.freeSpaceMarginBytes));
    AsciiAliases aliases(aliasBases(ctx.tempDir, destination), {}, ctx.log);
    FW_TRY(mapDir(aliases, fi.absolutePath(), QStringLiteral("ref")));
    auto destTool = aliases.map(destination, QStringLiteral("base"));
    if (!destTool.ok())
        return destTool.takeError();
    ToolRunOptions o;
    o.program = native(tc.exe);
    o.arguments = imgExtractArgs(s.passcode, aliases.toToolPath(fi.absoluteFilePath()), native(destTool.value()));
    o.workingDirectory = chooseWorkingDir(tc, destination);
    o.environment = publisherEnv(QDir(ctx.tempDir).filePath(QStringLiteral("extract-tmp")), aliases, ctx.log);
    o.withBar = false;
    applyJobLimits(o, s, ctx);
    const qint64 free0 = diskSpace(destination).ok() ? diskSpace(destination)->available : 0;
    ProgressReporter* pr = ctx.progress;
    o.pollBytes = [destination, free0] {
        auto d = diskSpace(destination);
        return d.ok() ? std::max<qint64>(0, free0 - d->available) : 0;
    };
    o.onBytesWritten = [pr](qint64 b) { pr->setCurrentItem(QStringLiteral("%1 extracted").arg(formatBytes(b))); };
    o.onLine = [&ctx](const ToolLine& l) { logToolLine(ctx.log, l); };
    ctx.log.info(redactedCommand(o.arguments));
    auto r = runPubTool(o, ctx.token);
    if (!r.ok())
        return r.takeError();
    if (r->cancelled || !ctx.token.checkpoint())
        return Error::cancelled();
    // Exit code 0 with an [Error] line is a failure too (§4.4): a partly
    // extracted base must never become the tree an update delta is planned on.
    if (!r->ok())
        return packFailure(r.value(), QStringLiteral("img_extract"));
    const QString root = findAppRoot(destination, 2);
    return root.isEmpty() ? destination : root;
}

} // namespace fw::fpkg
