#include "engines/fpkg/BackportUpdate.h"

#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "core/Paths.h"
#include "core/Zip.h"
#include "engines/fpkg/AsciiAliases.h"
#include "engines/fpkg/ContentNaming.h"
#include "engines/fpkg/FpkgBuilder.h"
#include "engines/fpkg/PackageHeader.h"
#include "engines/fpkg/PackageInspect.h"
#include "engines/fpkg/PlayGo.h"
#include "engines/fpkg/ParamJson.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/fpkg/SourceScanner.h"
#include "engines/fpkg/Toolchain.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QRegularExpression>

#include <functional>
#include <optional>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::fpkg {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

QString full(const QString& p)
{
    QString f = QDir::cleanPath(QFileInfo(p.trimmed()).absoluteFilePath());
    while (f.size() > 3 && f.endsWith(u'/'))
        f.chop(1);
    return f;
}

// a is b or inside b (case-insensitive).
bool within(const QString& a, const QString& b)
{
    const QString fa = full(a), fb = full(b);
    if (fa.compare(fb, Qt::CaseInsensitive) == 0)
        return true;
    const QString prefix = fb.endsWith(u'/') ? fb : fb + u'/';
    return fa.startsWith(prefix, Qt::CaseInsensitive);
}

bool overlaps(const QString& a, const QString& b) { return within(a, b) || within(b, a); }

// "A junction or symbolic link is unsupported here: <element>" for the path or any parent.
Status ensureNoLinks(const QString& path)
{
    QString p = full(path);
    while (!p.isEmpty()) {
        if (QFileInfo::exists(p) && isLinkOrReparsePoint(p))
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("A junction or symbolic link is unsupported here: %1").arg(native(p)));
        const QString parent = QFileInfo(p).path();
        if (parent == p || parent.isEmpty())
            break;
        p = parent;
    }
    return success();
}

// Every file under root (full paths); a link anywhere is refused.
Result<QStringList> treeFiles(const QString& root, const CancellationToken& token)
{
    FW_TRY(ensureNoLinks(root));
    QStringList files;
    QStringList stack{full(root)};
    while (!stack.isEmpty()) {
        if (!token.checkpoint())
            return Error::cancelled();
        const QString dir = stack.takeLast();
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& e : entries) {
            if (isLinkOrReparsePoint(e.absoluteFilePath()))
                return Error(ErrorCode::Unsupported, QStringLiteral("Linked files/folders are unsupported in a backport "
                                                                    "build: %1").arg(native(e.absoluteFilePath())));
            if (e.isDir())
                stack << e.absoluteFilePath();
            else
                files << e.absoluteFilePath();
        }
    }
    return files;
}

bool isPayload(const QString& rel)
{
    if (rel.endsWith(QStringLiteral(".esbak"), Qt::CaseInsensitive))
        return false;
    const QString r = QString(rel).replace(u'\\', u'/');
    return !r.startsWith(QStringLiteral("sce_sys/about/"), Qt::CaseInsensitive)
        && !r.contains(QStringLiteral("/sce_sys/about/"), Qt::CaseInsensitive);
}

bool isVersionText(const QString& v)
{
    static const QRegularExpression re(QStringLiteral("^[0-9]{2}\\.[0-9]{3}\\.[0-9]{3}$"));
    return re.match(v).hasMatch();
}

struct Identity {
    QString titleId;
    QString contentId;
    QString version;
    QString title;
};

Status validateIdentity(const Identity& id, const QString& where)
{
    if (!id.titleId.startsWith(QStringLiteral("PPSA")))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Missing or unsupported PS5 game identifier in %1").arg(where));
    static const QRegularExpression tid(QStringLiteral("^(?:PPSA|CUSA)[0-9]{5}$"));
    if (!tid.match(id.titleId).hasMatch())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Title ID must be PPSA or CUSA followed by exactly five digits."));
    if (!isStrictContentId(id.contentId))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Content ID must use XX0000-PPSA00000_00-XXXXXXXXXXXXXXXX (or CUSA), with a "
                                    "16-character uppercase alphanumeric/underscore suffix."));
    if (id.titleId != id.contentId.mid(7, 9))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Title ID must match the game identifier embedded in the Content ID."));
    if (!isVersionText(id.version))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("contentVersion must use NN.NNN.NNN in %1").arg(where));
    return success();
}

Status requireSameIdentity(const Identity& a, const Identity& b, const QString& label)
{
    FW_TRY(validateIdentity(a, label));
    if (a.titleId != b.titleId || a.contentId != b.contentId)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 belongs to a different game/region. It must match %2 / %3")
                                                     .arg(label, b.titleId, b.contentId));
    return success();
}

Identity identityFromParam(const json::Value& p)
{
    Identity id;
    const auto str = [&](const QString& k) {
        const json::Value& v = p.value(k);
        return v.isString() ? v.toString() : QString();
    };
    id.contentId = str(QStringLiteral("contentId"));
    const json::Value& t = p.value(QStringLiteral("titleId"));
    id.titleId = t.isString() ? t.toString() : titleIdFromContentId(id.contentId);
    id.version = str(QStringLiteral("contentVersion"));
    id.title = paramTitle(p);
    return id;
}

Result<Identity> readIdentity(const QString& paramPath)
{
    FW_TRY(ensureNoLinks(paramPath));
    const QFileInfo fi(paramPath);
    if (!fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("Missing game metadata (sce_sys\\param.json)."));
    if (fi.size() > 4 * 1024 * 1024)
        return Error(ErrorCode::CorruptData, QStringLiteral("Game metadata exceeds the 4 MiB limit: %1").arg(native(paramPath)));
    auto prm = readParamJson(paramPath);
    if (!prm.ok())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("Game metadata must be a JSON object: %1").arg(native(paramPath)), prm.error().message);
    const json::Value& t = prm->value(QStringLiteral("titleId"));
    if (!t.isNull() && !t.isString())
        return Error(ErrorCode::CorruptData, QStringLiteral("titleId must be a string when supplied: %1").arg(native(paramPath)));
    Identity id = identityFromParam(prm.value());
    FW_TRY(validateIdentity(id, native(paramPath)));
    return id;
}

QString gameParam(const QString& folder)
{
    const QString p = findPathCaseInsensitive(folder, QStringLiteral("sce_sys/param.json"), false);
    return p.isEmpty() ? QDir(folder).filePath(QStringLiteral("sce_sys/param.json")) : p;
}

struct Selected {
    QString label;   // "4.xx" / "5.xx+"
    QString tag;     // "4xx" / "5xxplus"
    QString raw;     // as given (folder or .zip)
    QString folder;  // resolved backport root
};

std::vector<Selected> selections(const BackportUpdateRequest& r)
{
    std::vector<Selected> s;
    if (!r.backport4xx.trimmed().isEmpty())
        s.push_back({QStringLiteral("4.xx"), QStringLiteral("4xx"), r.backport4xx.trimmed(), {}});
    if (!r.backport5xx.trimmed().isEmpty())
        s.push_back({QStringLiteral("5.xx+"), QStringLiteral("5xxplus"), r.backport5xx.trimmed(), {}});
    return s;
}

bool isZip(const QString& p) { return p.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) && QFileInfo(p).isFile(); }

// Maps an inner build's progress onto one phase of the outer job.
class StageProgress {
public:
    explicit StageProgress(ProgressReporter* outer)
        : m_inner([outer](const ProgressSnapshot& s) {
              outer->setDone(qint64(std::clamp(s.overallFraction, 0.0, 1.0) * 1000));
              QString item = s.phase;
              if (!s.currentItem.isEmpty())
                  item += QStringLiteral(" · ") + s.currentItem;
              outer->setCurrentItem(item);
          },
                  150)
    {
    }
    ProgressReporter* reporter() { return &m_inner; }

private:
    ProgressReporter m_inner;
};

JobContext subContext(JobContext& ctx, ProgressReporter* progress, const QString& name)
{
    JobContext sub;
    sub.jobId = ctx.jobId;
    sub.token = ctx.token;
    sub.progress = progress;
    sub.log = ctx.log;
    sub.settings = ctx.settings;
    sub.tempDir = QDir(ctx.tempDir).filePath(name);
    QDir().mkpath(sub.tempDir);
    return sub;
}

// Keeps the base package from being changed, replaced or deleted while the
// updates that reference it are built (FILE_SHARE_READ only).
class BaseLock {
public:
    explicit BaseLock(const QString& path)
    {
#ifdef Q_OS_WIN
        m_h = CreateFileW(reinterpret_cast<LPCWSTR>(toLongPath(path).utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        Q_UNUSED(path);
#endif
    }
    ~BaseLock()
    {
#ifdef Q_OS_WIN
        if (m_h != INVALID_HANDLE_VALUE)
            CloseHandle(m_h);
#endif
    }
    BaseLock(const BaseLock&) = delete;
    BaseLock& operator=(const BaseLock&) = delete;

private:
#ifdef Q_OS_WIN
    HANDLE m_h = INVALID_HANDLE_VALUE;
#endif
};

QString verificationSummary(const std::vector<PublisherVerifyReport>& reports, int updates)
{
    if (reports.empty())
        return QStringLiteral("Required package verification has not completed. Console compatibility has not been tested.");
    for (const auto& r : reports)
        if (!r.integrityPassed)
            return QStringLiteral("Required package verification has not completed. Console compatibility has not been tested.");
    int formatFailed = 0;
    for (const auto& r : reports)
        if (!r.formatPassed)
            ++formatFailed;
    QString s = QStringLiteral("Required publisher full integrity checks passed. ");
    s += formatFailed > 0 ? QStringLiteral("SDK application/SELF format diagnostics reported errors in %1 checked image(s); "
                                           "see the log. Format acceptance remains unverified. ").arg(formatFailed)
                          : QStringLiteral("SDK application/SELF format diagnostics passed. ");
    if (updates > 0)
        s += QStringLiteral("Compact updates passed container/base-digest and target-identifier/version checks; standalone "
                            "full LIH verification is unsupported by this SDK. ");
    s += QStringLiteral("Console compatibility has not been tested.");
    return s;
}

// ---- work folders (crash-safe) ----------------------------------------------------------------
QMutex& workMutex()
{
    static QMutex m;
    return m;
}

QString workIndexPath() { return QDir(Paths::localDir()).filePath(QStringLiteral("state/backport-work.txt")); }

bool isWorkFolderName(const QString& name)
{
    return name.startsWith(QStringLiteral(".ppr-backport-")) || name.startsWith(QStringLiteral(".ppr-sdkbase-"))
           || name.startsWith(QStringLiteral(".fw-zstd-")) || name.startsWith(QStringLiteral(".fw-stage-"));
}

QStringList readWorkIndex()
{
    QFile f(workIndexPath());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
        return {};
    QStringList out;
    for (const QByteArray& l : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(l).trimmed();
        if (!line.isEmpty() && !out.contains(line, Qt::CaseInsensitive))
            out << line;
    }
    return out;
}

void writeWorkIndex(const QStringList& lines)
{
    QDir().mkpath(QFileInfo(workIndexPath()).absolutePath());
    if (lines.isEmpty()) {
        QFile::remove(workIndexPath());
        return;
    }
    QSaveFile f(workIndexPath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write((lines.join(u'\n') + u'\n').toUtf8());
        f.commit();
    }
}

// Creates `dir` (a fresh folder), records it and writes the owner marker.
Status createWorkFolder(const QString& dir)
{
    if (QFileInfo::exists(dir))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("The work folder already contains files. Choose a fresh work folder."));
    if (!QDir().mkpath(dir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(native(dir)));
    {
        QMutexLocker lock(&workMutex());
        QStringList lines = readWorkIndex();
        lines << QDir::toNativeSeparators(QFileInfo(dir).absoluteFilePath());
        writeWorkIndex(lines);
    }
    QFile marker(QDir(dir).filePath(QStringLiteral(".fw-owned")));
    if (marker.open(QIODevice::WriteOnly))
        marker.write(QByteArray::number(QCoreApplication::applicationPid()));
    return success();
}

void removeWorkFolder(const QString& dir)
{
    if (qEnvironmentVariableIsSet("FW_BACKPORT_KEEP_WORK")) // diagnostics: keep the work folder
        return;
    removeTreeNoFollow(dir);
    if (QFileInfo::exists(dir))
        return; // kept in the index: the next sweep tries again
    QMutexLocker lock(&workMutex());
    QStringList lines = readWorkIndex();
    lines.removeIf([&](const QString& l) { return l.compare(QDir::toNativeSeparators(QFileInfo(dir).absoluteFilePath()), Qt::CaseInsensitive) == 0; });
    writeWorkIndex(lines);
}

struct WorkFolder {
    QString dir;
    ~WorkFolder()
    {
        if (!dir.isEmpty())
            removeWorkFolder(dir);
    }
};

QString freshName(const QString& path)
{
    QString cand = path;
    for (int n = 2; QFileInfo::exists(cand); ++n)
        cand = QStringLiteral("%1-%2").arg(path).arg(n);
    return cand;
}

QString randomTag(int hexChars)
{
    QString t;
    while (t.size() < hexChars)
        t += QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, u'0');
    return t.left(hexChars);
}

FpkgSettings innerSettings(const FpkgSettings& s)
{
    FpkgSettings in = s;
    in.postVerify = PostVerify::HeaderOnly; // the publisher checks run here, in the Companion's order
    in.computeSha256 = false;
    in.keepIntermediate = false;
    in.outputConflict = OutputConflict::Fail; // private, empty work folder
    return in;
}

} // namespace

// ---- request ------------------------------------------------------------------------------------

QJsonObject BackportUpdateRequest::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("package"), basePackage);
    o.insert(QStringLiteral("game"), gameFolder);
    o.insert(QStringLiteral("backport4xx"), backport4xx);
    o.insert(QStringLiteral("backport5xx"), backport5xx);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("output"), output);
    o.insert(QStringLiteral("buildBase"), buildBase);
    return o;
}

Result<BackportUpdateRequest> BackportUpdateRequest::fromJson(const QJsonObject& o)
{
    BackportUpdateRequest r;
    r.basePackage = o.value(QStringLiteral("package")).toString().trimmed();
    r.gameFolder = o.value(QStringLiteral("game")).toString().trimmed();
    r.backport4xx = o.value(QStringLiteral("backport4xx")).toString().trimmed();
    r.backport5xx = o.value(QStringLiteral("backport5xx")).toString().trimmed();
    r.version = o.value(QStringLiteral("version")).toString().trimmed();
    r.output = o.value(QStringLiteral("output")).toString().trimmed();
    r.buildBase = o.value(QStringLiteral("buildBase")).toBool(false);
    if (!r.hasAny())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose at least one backport folder: 4.xx or 5.xx+."));
    return r;
}

QString nextBackportVersion(const QString& baseVersion)
{
    if (!isVersionText(baseVersion))
        return {};
    const auto v = ContentVersion::parse(baseVersion);
    if (!v)
        return {};
    const auto n = v->nextPatch();
    return n ? n->text() : QString();
}

Result<QString> backportUpdateVersion(const QString& baseVersion, const QString& requested)
{
    if (!isVersionText(baseVersion))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The base contentVersion must use NN.NNN.NNN."));
    const QString v = requested.trimmed().isEmpty() ? nextBackportVersion(baseVersion) : requested.trimmed();
    if (!isVersionText(v))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The update contentVersion must use NN.NNN.NNN; the base may "
                                                                "already be at the maximum version."));
    if (QString::compare(v, baseVersion, Qt::CaseSensitive) <= 0)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The update version '%1' must be greater than the base '%2'.").arg(v, baseVersion));
    return v;
}

QString backportPackageName(const QString& titleId, const QString& tag)
{
    return tag.isEmpty() ? QStringLiteral("%1-backport.pkg").arg(titleId)
                         : QStringLiteral("%1-backport-%2.pkg").arg(titleId, tag);
}

QString availableOutputPath(const QString& wanted)
{
    const QFileInfo fi(wanted);
    const QString dir = fi.absolutePath();
    const QString stem = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    QString cand = fi.absoluteFilePath();
    for (int n = 2; QFileInfo::exists(cand) || QFileInfo::exists(cand + QStringLiteral(".naps_metric.json")); ++n)
        cand = QDir(dir).filePath(QStringLiteral("%1-%2%3").arg(stem).arg(n).arg(ext));
    return cand;
}

QString resolveBackportFolder(const QString& folder)
{
    static const QStringList markers = {QStringLiteral("sce_sys"), QStringLiteral("sce_module"), QStringLiteral("fakelib"),
                                        QStringLiteral("sce_suppl"), QStringLiteral("sce_sc"),
                                        QStringLiteral("playgo-languages")};
    QString p = full(folder);
    for (int guard = 0; guard < 32; ++guard) {
        const QFileInfoList e = QDir(p).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        if (e.size() != 1 || !e.first().isDir() || isLinkOrReparsePoint(e.first().absoluteFilePath()))
            break;
        if (markers.contains(e.first().fileName(), Qt::CaseInsensitive))
            break;
        p = e.first().absoluteFilePath();
    }
    return p;
}

QString firmwareGroupFromName(const QString& fileName)
{
    const QString n = fileName.toLower();
    static const QRegularExpression four(QStringLiteral("(fw|sdk)[ _-]*4(\\.?xx|\\b)|\\b4\\.?xx\\b"));
    static const QRegularExpression fivePlus(QStringLiteral("(fw|sdk)[ _-]*([5-9]|1[0-3])(\\.?xx|\\+|\\b)|\\b([5-9]|1[0-3])\\.?xx"));
    if (four.match(n).hasMatch())
        return QStringLiteral("4xx");
    if (fivePlus.match(n).hasMatch())
        return QStringLiteral("5xxplus");
    return {};
}

Status validateBackportRequest(const BackportUpdateRequest& r, const FpkgSettings& s)
{
    if (!r.hasAny())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose at least one backport folder: 4.xx or 5.xx+."));
    if (r.buildBase) {
        if (r.gameFolder.isEmpty() || !QFileInfo(r.gameFolder).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose the source game folder."));
    } else {
        if (r.basePackage.isEmpty() || !QFileInfo(r.basePackage).isFile())
            return Error(ErrorCode::NotFound, QStringLiteral("Choose the base package (.pkg)."));
        if (!r.gameFolder.isEmpty() && !QFileInfo(r.gameFolder).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("The game folder does not exist."));
    }
    for (const Selected& sel : selections(r)) {
        const QFileInfo fi(sel.raw);
        if (!fi.isDir() && !isZip(sel.raw))
            return Error(ErrorCode::NotFound, QStringLiteral("The %1 backport folder does not exist.").arg(sel.label));
    }
    if (!r.backport4xx.isEmpty() && !r.backport5xx.isEmpty() && overlaps(r.backport4xx, r.backport5xx))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The 4.xx and 5.xx+ backport folders must be separate trees. Select only one if the "
                                    "same files serve both."));
    const QString output = r.output.isEmpty()
        ? (r.buildBase ? defaultOutputFolder(r.gameFolder, s.defaultOutputDir) : QFileInfo(r.basePackage).absolutePath())
        : r.output;
    if (QFileInfo(output).isFile())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output folder is a file: %1").arg(native(output)));
    if (!r.gameFolder.isEmpty() && within(output, r.gameFolder))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output folder must be outside the source game folder."));
    for (const Selected& sel : selections(r)) {
        if (QFileInfo(sel.raw).isDir() && within(output, sel.raw))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The output folder must be outside the %1 backport files, including any wrapper "
                                        "directory.").arg(sel.label));
        if (!r.gameFolder.isEmpty() && QFileInfo(sel.raw).isDir() && overlaps(sel.raw, r.gameFolder))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 backport files must be separate from the source game tree.").arg(sel.label));
    }
    if (!r.version.isEmpty() && !isVersionText(r.version))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The new version must use NN.NNN.NNN (e.g. 01.000.001)."));
    const QString pc = passcodeProblem(s.passcode);
    if (!pc.isEmpty() || s.passcode.size() != 32)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Package passcode must contain exactly 32 characters."));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    if (!tc.ok())
        return Error(ErrorCode::DependencyMissing, tc.problems.join(u' '));
    return success();
}

QStringList backportPhases(const BackportUpdateRequest& r)
{
    QStringList p{QStringLiteral("Checking the inputs")};
    if (r.buildBase)
        p << QStringLiteral("Building the base package");
    p << QStringLiteral("Checking the base package");
    if (!r.backport4xx.trimmed().isEmpty())
        p << QStringLiteral("Building the 4.xx update");
    if (!r.backport5xx.trimmed().isEmpty())
        p << QStringLiteral("Building the 5.xx+ update");
    return p;
}

int sweepStaleBackportWork()
{
    QMutexLocker lock(&workMutex());
    const QStringList lines = readWorkIndex();
    QStringList keep;
    int removed = 0;
    for (const QString& line : lines) {
        const QFileInfo fi(line);
        if (!fi.isDir())
            continue; // gone: drop the line
        if (!isWorkFolderName(fi.fileName()) || isLinkOrReparsePoint(fi.absoluteFilePath())) {
            keep << line; // never delete anything this app did not name
            continue;
        }
        qint64 pid = 0;
        {
            QFile marker(QDir(fi.absoluteFilePath()).filePath(QStringLiteral(".fw-owned")));
            if (marker.open(QIODevice::ReadOnly) && marker.size() < 64)
                pid = marker.readAll().trimmed().toLongLong();
        } // closed before the folder is removed (Windows will not delete an open file)
        if (pid <= 0) {
            keep << line; // no marker: leave it alone
            continue;
        }
        if (pid == QCoreApplication::applicationPid() || processIsRunning(pid)) {
            keep << line;
            continue;
        }
        removeTreeNoFollow(fi.absoluteFilePath());
        if (QFileInfo::exists(fi.absoluteFilePath())) // not the cached QFileInfo
            keep << line;
        else
            ++removed;
    }
    if (keep != lines)
        writeWorkIndex(keep);
    return removed;
}

Status createTrackedWorkFolder(const QString& dir)
{
    if (!isWorkFolderName(QFileInfo(dir).fileName()))
        return Error(ErrorCode::Internal, QStringLiteral("Not a work folder name: %1").arg(dir));
    return createWorkFolder(dir);
}

void removeTrackedWorkFolder(const QString& dir) { removeWorkFolder(dir); }

// ---- running --------------------------------------------------------------------------------------

Result<JobOutcome> runBackportUpdates(const BackportUpdateRequest& req, const FpkgSettings& s, JobContext& ctx)
{
    const Log& log = ctx.log;
    ProgressReporter* pr = ctx.progress;
    const QStringList phases = backportPhases(req);
    QList<double> weights;
    for (const QString& ph : phases)
        weights << (ph.startsWith(QStringLiteral("Building")) ? 40.0 : ph.startsWith(QStringLiteral("Checking the base")) ? 10.0 : 2.0);
    pr->setPhases(phases, weights);
    int phase = 0;
    pr->beginPhase(phase, 0);

    FW_TRY(validateBackportRequest(req, s));
    if (const int swept = sweepStaleBackportWork(); swept > 0)
        log.info(QStringLiteral("Removed %1 work folder(s) left by an interrupted backport build.").arg(swept));
    const PubToolchain tc = locatePubTools(s.pubToolsPath);
    const SdkFixStatus sdk = checkSdkFixToolchain(tc);
    log.info(sdk.text);
    if (!sdk.ready())
        return Error(ErrorCode::DependencyMissing, QStringLiteral("A verified SDK Fix toolchain is required. %1").arg(sdk.text));

    JobOutcome outcome;
    bool baseVerified = false;
    if (!sdk.recommended)
        outcome.warnings << sdk.text;
    std::vector<PublisherVerifyReport> reports;
    QStringList updates;
    QString basePath = req.basePackage.isEmpty() ? QString() : full(req.basePackage);
    const QString gameFolder = req.gameFolder.isEmpty() ? QString() : full(req.gameFolder);
    QString output = req.output.isEmpty()
        ? (req.buildBase ? defaultOutputFolder(req.gameFolder, s.defaultOutputDir) : QFileInfo(basePath).absolutePath())
        : req.output;
    output = full(output);
    FW_TRY(ensureNoLinks(output));

    // ---- backport inputs: unpack ZIPs, unwrap one wrapper folder, check them -------------------
    std::vector<Selected> sel = selections(req);
    for (Selected& b : sel) {
        QString folder = b.raw;
        if (isZip(b.raw)) {
            const QString dest = QDir(ctx.tempDir).filePath(QStringLiteral("backport-%1").arg(b.tag));
            removeTreeNoFollow(dest);
            log.info(QStringLiteral("Unpacking the %1 backport ZIP %2").arg(b.label, native(b.raw)));
            zip::Reader zr;
            FW_TRY(zr.open(b.raw));
            qint64 bytes = 0;
            for (const zip::Entry& e : zr.entries())
                bytes += qint64(e.uncompressedSize);
            FW_TRY(ensureFreeSpace(ctx.tempDir, bytes, s.freeSpaceMarginBytes));
            FW_TRY(zr.extractAll(dest, ctx.token));
            folder = dest;
        } else {
            FW_TRY(ensureNoLinks(folder));
        }
        b.folder = resolveBackportFolder(folder);
        if (!QFileInfo(b.folder).isDir())
            return Error(ErrorCode::NotFound, QStringLiteral("Backport folder not found: %1").arg(native(b.folder)));
        if (b.folder != full(folder))
            log.info(QStringLiteral("%1 backport files: %2 (wrapper folder skipped)").arg(b.label, native(b.folder)));
    }
    for (size_t i = 0; i < sel.size(); ++i) {
        const Selected& b = sel[i];
        if (!gameFolder.isEmpty() && overlaps(b.folder, gameFolder))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 backport files must be separate from the source game tree.").arg(b.label));
        if (within(output, b.folder))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The output folder must be outside the %1 backport files, including any wrapper "
                                        "directory.").arg(b.label));
        for (size_t j = 0; j < i; ++j)
            if (overlaps(b.folder, sel[j].folder))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("The 4.xx and 5.xx+ backport folders must be separate trees. Select only one "
                                            "if the same files serve both."));
        auto files = treeFiles(b.folder, ctx.token);
        if (!files.ok())
            return files.takeError();
        bool any = false;
        for (const QString& f : files.value())
            if (isPayload(QDir(b.folder).relativeFilePath(f))) {
                any = true;
                break;
            }
        if (!any)
            return Error(ErrorCode::NotFound,
                         QStringLiteral("No usable backport files were found in the %1 folder: %2").arg(b.label, native(b.folder)));
    }

    // ---- the base: build it (dual backports) or inspect the given one --------------------------
    Identity base;
    if (req.buildBase) {
        auto gid = readIdentity(gameParam(gameFolder));
        if (!gid.ok())
            return gid.takeError();
        if (within(output, gameFolder))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The output folder must be outside the source game folder."));
        auto pre = backportUpdateVersion(gid->version, req.version);
        if (!pre.ok())
            return pre.takeError();
        for (const Selected& b : sel) {
            const QString bp = findPathCaseInsensitive(b.folder, QStringLiteral("sce_sys/param.json"), false);
            if (!bp.isEmpty()) {
                auto bid = readIdentity(bp);
                if (!bid.ok())
                    return bid.takeError();
                FW_TRY(requireSameIdentity(bid.value(), gid.value(), QStringLiteral("%1 backport metadata").arg(b.label)));
            }
        }
        log.info(sel.empty() ? QStringLiteral("Building one base package.")
                             : QStringLiteral("Building one base package and separate optional backport updates."));
        pr->beginPhase(++phase, 1000, 0, false);
        // Built in a private folder next to the outputs, checked, verified, and
        // only then given a name nothing else uses (an existing package is
        // never renamed or replaced).
        if (!QDir().mkpath(output))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create the output folder %1.").arg(native(output)));
        const QString baseWork = QDir(output).filePath(QStringLiteral(".ppr-sdkbase-%1").arg(randomTag(32)));
        FW_TRY(createWorkFolder(baseWork));
        WorkFolder baseWorkGuard{baseWork};
        StageProgress sp(pr);
        JobContext sub = subContext(ctx, sp.reporter(), QStringLiteral("base"));
        BuildRequest br;
        br.source = gameFolder;
        br.output = baseWork;
        auto built = runBuild(br, innerSettings(s), sub);
        if (!built.ok())
            return built.takeError();
        const QString partial = built->outputs.value(0);
        for (const QString& w : built->warnings)
            if (!w.startsWith(QStringLiteral("Install the base")))
                outcome.warnings << w;
        // The Companion's checks of a freshly built base.
        auto bi = inspectPackage(partial, ctx.token);
        if (!bi.ok())
            return bi.takeError();
        if (bi->kind != PackageKind::Ps5)
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The publisher did not produce the expected PS5 FIH "
                                                                       "container. No final output was published."));
        Identity bid{bi->titleId, bi->contentId, bi->version, bi->title};
        FW_TRY(requireSameIdentity(bid, gid.value(), QStringLiteral("Built base metadata")));
        if (bid.version != gid->version)
            return Error(ErrorCode::VerificationFailed,
                         QStringLiteral("The built base version differs from the source. No final output was published."));
        auto rep = verifyPublisherPackage(partial, tc, s, sub);
        if (!rep.ok())
            return rep.takeError();
        reports.push_back(rep.value());
        baseVerified = true;
        const QString finalBase = availableOutputPath(QDir(output).filePath(QFileInfo(partial).fileName()));
        if (finalBase != QDir(output).filePath(QFileInfo(partial).fileName()))
            log.info(QStringLiteral("Existing output kept; writing the new build to %1").arg(native(finalBase)));
        if (!QFile::rename(partial, finalBase))
            return Error(ErrorCode::IoError, QStringLiteral("Could not move the finished base package to %1.").arg(native(finalBase)));
        for (const QFileInfo& d : QDir(baseWork).entryInfoList({QStringLiteral("*-build-logs")}, QDir::Dirs | QDir::NoDotAndDotDot))
            QDir().rename(d.absoluteFilePath(), freshName(QFileInfo(finalBase).absoluteDir().filePath(
                                                    QFileInfo(finalBase).completeBaseName() + QStringLiteral("-build-logs"))));
        basePath = finalBase;
        outcome.outputs << basePath;
        log.info(QStringLiteral("Verified base retained: %1").arg(native(basePath)));
    }

    pr->beginPhase(++phase, 0);
    if (!ctx.token.checkpoint())
        return Error::cancelled();
    log.info(QStringLiteral("==> Inspecting the base package"));
    auto inspected = inspectPackage(basePath, ctx.token);
    if (!inspected.ok())
        return inspected.takeError();
    const PackageIdentity& bi = inspected.value();
    if (bi.kind != PackageKind::Ps5)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The base must be a full PS5 FIH package, not a game update, PS4 package or other file."));
    if (!bi.hasParam || bi.version.isEmpty())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("The base package has no readable param.json; it may be a retail (encrypted) package."));
    base = Identity{bi.titleId, bi.contentId, bi.version, bi.title};
    FW_TRY(validateIdentity(base, native(basePath)));
    log.info(QStringLiteral("    title      : %1 [%2]").arg(base.title, base.titleId));
    log.info(QStringLiteral("    contentId  : %1").arg(base.contentId));
    log.info(QStringLiteral("    version    : %1").arg(base.version));
    log.info(QStringLiteral("    digest     : %1").arg(bi.digestHex));
    log.info(QStringLiteral("    The console must have installed THIS package. A delta against any other build of the same "
                            "game fails with CE-107891-6."));
    const auto ver = backportUpdateVersion(base.version, req.version);
    if (!ver.ok())
        return ver.error();
    const QString newVersion = ver.value();
    log.info(QStringLiteral("    new version: %1").arg(newVersion));
    for (const Selected& b : sel) {
        const QString bp = findPathCaseInsensitive(b.folder, QStringLiteral("sce_sys/param.json"), false);
        if (!bp.isEmpty()) {
            auto bid = readIdentity(bp);
            if (!bid.ok())
                return bid.takeError();
            FW_TRY(requireSameIdentity(bid.value(), base, QStringLiteral("%1 backport metadata").arg(b.label)));
        }
    }
    // The game folder must be the exact source of the base.
    if (!gameFolder.isEmpty()) {
        FW_TRY(ensureNoLinks(gameFolder));
        auto gid = readIdentity(gameParam(gameFolder));
        if (!gid.ok())
            return gid.takeError();
        FW_TRY(requireSameIdentity(gid.value(), base, QStringLiteral("Game folder metadata")));
        if (gid->version != base.version)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("The game folder contentVersion must match the exact base package version; choose "
                                        "the corresponding extracted source."));
        if (within(output, gameFolder))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("The output folder must be outside the source game folder."));
    }
    if (!QDir().mkpath(output))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the output folder %1.").arg(native(output)));
    BaseLock lock(basePath);

    // The base must pass the publisher's integrity check before anything is built against it.
    if (!baseVerified) {
        auto rep = verifyPublisherPackage(basePath, tc, s, ctx);
        if (!rep.ok())
            return rep.takeError();
        reports.push_back(rep.value());
    }

    // The update keeps the base's PlayGo layout (chunk count and languages of
    // its own playgo-chunk.dat, as the Companion did).
    QStringList baseLanguages;
    int baseChunks = 0;
    {
        auto pg = findPlayGoChunkDatInPackage(basePath, ctx.token);
        if (pg.ok() && pg->has_value()) {
            auto table = parsePlayGoChunkDat(**pg);
            if (table.ok()) {
                baseChunks = table->chunkCount;
                baseLanguages = languagesInMask(table->languageMask);
            }
        }
        if (baseChunks > 0 && !baseLanguages.isEmpty())
            log.info(QStringLiteral("    PlayGo     : %1 language(s) in the base - the update keeps the same set (%2 chunks)")
                         .arg(baseLanguages.size()).arg(baseChunks));
        else
            log.info(QStringLiteral("    PlayGo     : the base's language list could not be read; the update uses the game's own"));
    }

    // The base tree: the game folder, or the base package extracted once for every update.
    QString baseTree = gameFolder;
    if (baseTree.isEmpty()) {
        log.info(QStringLiteral("==> Extracting the base package to the work tree (no game folder given)"));
        auto ex = extractPackage(basePath, QDir(ctx.tempDir).filePath(QStringLiteral("base-tree")), tc, s, ctx);
        if (!ex.ok())
            return ex.takeError();
        baseTree = ex.value();
    }
    {
        const QString ks = findPathCaseInsensitive(baseTree, QStringLiteral("sce_sys/keystone"), false);
        if (ks.isEmpty() || QFileInfo(ks).size() != 96)
            return Error(ErrorCode::CorruptData, QStringLiteral("This toolchain requires a 96-byte sce_sys\\keystone in the build "
                                                                "tree."));
    }

    // ---- one independent update per backport, all against the same base -----------------------
    const auto logRetained = [&] {
        if (!basePath.isEmpty() && QFileInfo(basePath).isFile() && req.buildBase)
            log.warn(QStringLiteral("Build set incomplete; the base package remains safe at %1").arg(native(basePath)));
        for (const QString& u : std::as_const(updates))
            log.warn(QStringLiteral("Completed update remains safe at %1").arg(native(u)));
        log.warn(QStringLiteral("Source folders were retained. Repair the failed input and retry; existing outputs will "
                                "receive new names."));
    };
    const QString titleId = base.titleId.isEmpty() ? QFileInfo(basePath).completeBaseName() : base.titleId;
    for (const Selected& b : sel) {
        pr->beginPhase(++phase, 1000, 0, false);
        if (!ctx.token.checkpoint()) {
            logRetained();
            return Error::cancelled();
        }
        log.info(QStringLiteral("==> Building independent %1 backport against %2").arg(b.label, native(basePath)));
        // Overlay statistics (the Companion's log lines).
        auto files = treeFiles(b.folder, ctx.token);
        if (!files.ok()) {
            logRetained();
            return files.takeError();
        }
        int total = 0, added = 0;
        for (const QString& f : files.value()) {
            const QString rel = QDir(b.folder).relativeFilePath(f);
            if (rel.endsWith(QStringLiteral(".esbak"), Qt::CaseInsensitive))
                continue;
            if (!isPayload(rel)) {
                log.warn(QStringLiteral("    [Warn] Not shipped (sce_sys/about is a reserved node the SDK regenerates): %1").arg(rel));
                continue;
            }
            ++total;
            if (findPathCaseInsensitive(baseTree, rel, false).isEmpty())
                ++added;
        }
        log.info(QStringLiteral("==> Overlaying backport files"));
        log.info(QStringLiteral("    %1 file(s): %2 replaced, %3 added").arg(total).arg(total - added).arg(added));
        if (added == total) {
            const QString w = QStringLiteral("None of the %1 backport files replace anything in the game. The backport "
                                             "folder is probably rooted at the wrong level - its contents must sit at the "
                                             "game root (eboot.bin, sce_module/, fakelib/ ...).").arg(b.label);
            log.warn(QStringLiteral("    [Warn] ") + w);
            outcome.warnings << w;
        }
        log.info(QStringLiteral("==> Setting contentVersion to %1").arg(newVersion));

        // A private work folder in the output folder: the finished delta is renamed, never copied.
        const QString work = QDir(output).filePath(QStringLiteral(".ppr-backport-%1-%2").arg(titleId.toLower(), randomTag(8)));
        if (Status st = createWorkFolder(work); !st.ok()) {
            logRetained();
            return st.takeError();
        }
        WorkFolder drop{work};

        StageProgress sp(pr);
        JobContext sub = subContext(ctx, sp.reporter(), QStringLiteral("update-%1").arg(b.tag));
        BuildRequest br;
        br.source = b.folder;
        br.basePackage = basePath;
        br.baseFolder = baseTree;
        br.exactSource = false;
        br.updateOutput = UpdateOutput::Both;
        br.contentVersion = newVersion;
        br.output = work;
        br.keepNapsMetric = true;
        FpkgSettings in = innerSettings(s);
        // Only contentVersion changes in the update's param.json (the backport's own param.json is kept as shipped).
        in.clearVersionFileUri = false;
        in.lowerRequiredFirmware = false;
        in.clearAttribute3 = false;
        if (baseChunks > 0 && !baseLanguages.isEmpty()) {
            in.playGoChunkCount = baseChunks;
            in.playGoForcedLanguages = baseLanguages;
            in.playGoAllLanguages = false;
        }
        log.info(QStringLiteral("==> Building the delta against the base (compression %1)")
                     .arg(s.compressionLevel ? QString::number(*s.compressionLevel) : QStringLiteral("7, tool default")));
        auto built = runBuild(br, in, sub);
        if (!built.ok()) {
            logRetained();
            return built.takeError();
        }
        QString delta, remastered;
        for (const QString& o : built->outputs) {
            if (o.endsWith(QStringLiteral(".remastered.pkg"), Qt::CaseInsensitive))
                remastered = o;
            else
                delta = o;
        }
        for (const QString& w : built->warnings)
            if (!w.startsWith(QStringLiteral("Install the base")))
                outcome.warnings << w;

        // Delta checks: a LIH container that references the base's digest.
        log.info(QStringLiteral("==> Verifying the delta references the base"));
        auto di = inspectPackage(delta, ctx.token);
        if (!di.ok()) {
            logRetained();
            return di.takeError();
        }
        auto n = countOccurrences(delta, bi.digest, ctx.token);
        if (!n.ok()) {
            logRetained();
            return n.takeError();
        }
        log.info(QStringLiteral("    references base digest: %1 occurrence(s); container: %2").arg(n.value()).arg(packageKindName(di->kind)));
        if (di->kind != PackageKind::Ps5Delta) {
            logRetained();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("Expected a delta container; got '%1'. No final output "
                                                                       "was published.").arg(packageKindName(di->kind)));
        }
        if (n.value() < 1) {
            logRetained();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The built package does not reference the base digest. "
                                                                       "It would fail on console with CE-107891-6."));
        }
        // The remastered image: base identity, the requested version, full integrity.
        if (remastered.isEmpty() || !QFileInfo(remastered).isFile()) {
            logRetained();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The publisher did not produce the remastered image needed "
                                                                       "to verify the update's metadata."));
        }
        auto ri = inspectPackage(remastered, ctx.token);
        if (!ri.ok()) {
            logRetained();
            return ri.takeError();
        }
        if (ri->kind != PackageKind::Ps5) {
            logRetained();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The publisher did not produce the expected PS5 FIH "
                                                                       "container. No final output was published."));
        }
        const Identity rid{ri->titleId, ri->contentId, ri->version, ri->title};
        if (Status st = requireSameIdentity(rid, base, QStringLiteral("Remastered update metadata")); !st.ok()) {
            logRetained();
            return st.takeError();
        }
        log.info(QStringLiteral("    target contentVersion: %1; requested: %2").arg(rid.version, newVersion));
        if (rid.version != newVersion) {
            logRetained();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The publisher's update version differs from the requested "
                                                                       "version. No final output was published."));
        }
        log.info(QStringLiteral("    Compact LIH integrity verification is unsupported by the SDK; verifying its generated "
                                "full remastered image before removing that temporary image."));
        auto rep = verifyPublisherPackage(remastered, tc, s, sub);
        if (!rep.ok()) {
            logRetained();
            return rep.takeError();
        }
        reports.push_back(rep.value());

        // Publish: the delta and its NAPS metric under a name nothing else uses.
        const QString wanted = QDir(output).filePath(backportPackageName(titleId, b.tag));
        const QString finalPath = availableOutputPath(wanted);
        if (finalPath != QFileInfo(wanted).absoluteFilePath())
            log.info(QStringLiteral("Existing output kept; writing the new backport to %1").arg(native(finalPath)));
        if (!QFile::rename(delta, finalPath)) {
            logRetained();
            return Error(ErrorCode::IoError, QStringLiteral("Could not move the finished update to %1.").arg(native(finalPath)));
        }
        const QString metric = delta + QStringLiteral(".naps_metric.json");
        if (QFileInfo::exists(metric))
            QFile::rename(metric, finalPath + QStringLiteral(".naps_metric.json"));
        // Build logs next to the update.
        const QDir workDir(work);
        for (const QFileInfo& d : workDir.entryInfoList({QStringLiteral("*-build-logs")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            // An older logs folder of the same name is kept: the new one gets -2, -3 ...
            const QString logsTo = freshName(QFileInfo(finalPath).absoluteDir().filePath(QFileInfo(finalPath).completeBaseName()
                                                                                          + QStringLiteral("-build-logs")));
            QDir().rename(d.absoluteFilePath(), logsTo);
        }
        updates << finalPath;
        log.info(QStringLiteral("Verified %1 update retained: %2").arg(b.label, native(finalPath)));
    }

    // ---- summary --------------------------------------------------------------------------------------
    outcome.outputs << updates;
    const QString summary = verificationSummary(reports, int(updates.size()));
    log.info(summary);
    outcome.verification = Verification::Passed;
    outcome.verificationDetail = summary;
    QStringList names;
    for (const QString& u : std::as_const(updates))
        names << QFileInfo(u).fileName();
    outcome.summary = QStringLiteral("%1 · version %2").arg(names.join(QStringLiteral(", ")), newVersion);
    outcome.warnings << (updates.size() > 1
                             ? QStringLiteral("Install the base package first, then the update for your firmware. The two "
                                              "outputs are alternative updates: install only one.")
                             : QStringLiteral("Install the base package first, then this update."));
    outcome.details.insert(QStringLiteral("basePackage"), basePath);
    outcome.details.insert(QStringLiteral("version"), newVersion);
    outcome.details.insert(QStringLiteral("baseDigest"), bi.digestHex);
    outcome.details.insert(QStringLiteral("sdkFix"), sdk.release);
    pr->flush();
    return outcome;
}

// ---- job ----------------------------------------------------------------------------------------------

FpkgBackportJob::FpkgBackportJob(BackportUpdateRequest request) : m_request(std::move(request))
{
    QString name;
    if (!m_request.gameFolder.isEmpty()) {
        const QString pj = findPathCaseInsensitive(m_request.gameFolder, QStringLiteral("sce_sys/param.json"), false);
        if (!pj.isEmpty())
            if (auto prm = readParamJson(pj); prm.ok())
                name = paramTitle(prm.value());
        if (name.isEmpty())
            name = QFileInfo(m_request.gameFolder).fileName();
    }
    if (name.isEmpty())
        name = QFileInfo(m_request.basePackage).completeBaseName();
    m_title = QStringLiteral("%1 · backport update").arg(name.isEmpty() ? QStringLiteral("Game") : name);
}

QString FpkgBackportJob::subtitle() const
{
    QStringList parts;
    if (!m_request.backport4xx.isEmpty())
        parts << QStringLiteral("4.xx");
    if (!m_request.backport5xx.isEmpty())
        parts << QStringLiteral("5.xx+");
    return QStringLiteral("FPKG %1update%2 (%3)")
        .arg(m_request.buildBase ? QStringLiteral("base + ") : QString(),
             parts.size() > 1 ? QStringLiteral("s") : QString(), parts.join(QStringLiteral(" and ")));
}

QStringList FpkgBackportJob::busyPaths() const
{
    QStringList p;
    for (const QString& s : {m_request.basePackage, m_request.gameFolder, m_request.backport4xx, m_request.backport5xx,
                             m_request.output})
        if (!s.isEmpty())
            p << QFileInfo(s).absoluteFilePath();
    return p;
}

Status FpkgBackportJob::validate(const SettingsValues& settings) const
{
    return validateBackportRequest(m_request, FpkgSettings::from(settings));
}

Result<JobOutcome> FpkgBackportJob::run(JobContext& ctx)
{
    const LiveCpuLimitScope live(ctx.jobId);
    return runBackportUpdates(m_request, FpkgSettings::from(ctx.settings), ctx);
}

} // namespace fw::fpkg
