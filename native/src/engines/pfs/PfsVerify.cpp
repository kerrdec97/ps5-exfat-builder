// Verification and extraction of PFS / FFPFSC images. Extraction follows
// Lazy_MkPFS lazy_mkpfs/inspect.py extract_pfs_image; verification follows
// validate_source_match but is strict about sizes. GPL-3.0-or-later.

#include "engines/pfs/PfsVerify.h"

#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "engines/pfs/AmprIndex.h"
#include "engines/pfs/PfsIo.h"
#include "engines/pfs/PfsReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <map>
#include <set>

namespace fw::pfs {

namespace {

constexpr int kMaxListed = 50;
const char* const kExtractMarker = ".fw-pfs-extract";

struct Opened {
    std::shared_ptr<FileByteSource> file;
    std::unique_ptr<PfsImage> outer;
    std::unique_ptr<PfsImage> inner; // nested image, refers to `outer`
    const PfsEntry* nestedEntry = nullptr;

    const PfsImage& target() const { return inner ? *inner : *outer; }
};

Result<std::unique_ptr<PfsImage>> openPath(const QString& path, std::shared_ptr<FileByteSource>& keep)
{
    keep = std::make_shared<FileByteSource>(path);
    FW_TRY(keep->open());
    return PfsImage::open(keep);
}

// A container holding one nested PFS image.
bool looksNested(const PfsImage& outer, bool probeContent)
{
    if (outer.files().size() != 1 || !outer.dirs().empty())
        return false;
    const PfsEntry& e = outer.files().front();
    const QByteArray low = asciiLower(e.path);
    if (low == kNestedImageName || low.endsWith(".ffpfs"))
        return true;
    if (!probeContent)
        return false;
    auto src = outer.openFile(outer.inodes()[e.inode]);
    if (!src.ok() || (*src)->size() < 0x400)
        return false;
    char head[16];
    if (!(*src)->readAt(0, head, 16).ok())
        return false;
    return getI64(head + 8) == kPfsMagic;
}

Status openNested(Opened& o)
{
    o.nestedEntry = &o.outer->files().front();
    FW_TRY_ASSIGN(auto src, o.outer->openFile(o.outer->inodes()[o.nestedEntry->inode]));
    FW_TRY_ASSIGN(o.inner, PfsImage::open(src));
    return success();
}

QString rel(const QByteArray& p) { return QString::fromLatin1(p); }

// Compares one image file with its source. `content` false: size only.
Status compareFile(const PfsImage& img, const PfsEntry& e, const SourceFile& src, bool content, VerifyReport& rep,
                   const CancellationToken& token, const ProgressHooks& hooks)
{
    const Inode& ino = img.inodes()[e.inode];
    const qint64 logical = ino.logicalSize();
    const qint64 bs = img.header().blockSize;
    bool padded = false;
    if (logical != src.size) {
        if (ino.isCompressed() && logical == ceilDiv(src.size, bs) * bs) {
            padded = true;
        } else {
            rep.fail(QStringLiteral("Different size: %1 (image %2 bytes, source %3 bytes)")
                         .arg(rel(e.path))
                         .arg(logical)
                         .arg(src.size));
            return success();
        }
    }
    ++rep.filesChecked;
    if (padded)
        ++rep.paddedFiles;
    if (!content)
        return success();

    std::unique_ptr<SeqSource> in;
    if (src.generated) {
        in = std::make_unique<MemorySeqSource>(src.content);
    } else {
        auto f = std::make_unique<FileSeqSource>(src.absPath);
        const Status st = f->open();
        if (!st.ok()) {
            rep.fail(QStringLiteral("Could not read the source of %1: %2").arg(rel(e.path), st.error().message));
            return success();
        }
        if (f->size() != src.size) {
            rep.fail(QStringLiteral("Source changed since the build: %1").arg(rel(e.path)));
            return success();
        }
        in = std::move(f);
    }
    if (hooks.item)
        hooks.item(rel(e.path));
    qint64 pos = 0;
    qint64 diffAt = -1;
    bool dirtyPadding = false;
    QByteArray buf;
    const Status st = img.readFile(ino, [&](const char* d, qint64 len) -> Status {
        const qint64 inSrc = std::clamp<qint64>(src.size - pos, 0, len);
        if (inSrc > 0) {
            buf.resize(qsizetype(inSrc));
            FW_TRY_ASSIGN(const qint64 got, in->read(buf.data(), inSrc));
            if (got != inSrc || std::memcmp(buf.constData(), d, size_t(inSrc)) != 0) {
                qint64 i = 0;
                while (i < got && buf[qsizetype(i)] == d[i])
                    ++i;
                diffAt = pos + i;
                return Error(ErrorCode::VerificationFailed, QStringLiteral("difference"));
            }
        }
        for (qint64 i = inSrc; i < len; ++i)
            if (d[i] != 0) {
                dirtyPadding = true;
                return Error(ErrorCode::VerificationFailed, QStringLiteral("padding"));
            }
        pos += len;
        if (hooks.advance && inSrc > 0)
            hooks.advance(inSrc);
        return success();
    }, token);
    if (!st.ok()) {
        if (st.error().code != ErrorCode::VerificationFailed)
            return st;
        if (diffAt >= 0 && src.generated && asciiLower(e.path) == "ampr_emu.index") {
            // The generated index stores every file's modification time. A
            // source that was copied or touched since the build gives other
            // times; the index is still right when nothing else differs.
            QByteArray stored;
            FW_TRY(img.readFile(ino, [&](const char* d, qint64 len) -> Status {
                stored.append(d, qsizetype(len));
                return success();
            }, token));
            stored.truncate(qsizetype(src.size));
            if (amprIndexesDifferOnlyInTimes(stored, src.content)) {
                rep.warnings << QStringLiteral("ampr_emu.index in the image holds other file times than the source has now "
                                               "(its paths and sizes match).");
                rep.bytesChecked += src.size;
                return success();
            }
        }
        if (diffAt >= 0)
            rep.fail(QStringLiteral("Different content: %1 (first difference at byte %2)").arg(rel(e.path)).arg(diffAt));
        else if (dirtyPadding)
            rep.fail(QStringLiteral("Different content: %1 (bytes after the end of the file are not zero)")
                         .arg(rel(e.path)));
        return success();
    }
    rep.bytesChecked += std::min(pos, src.size);
    return success();
}

Status compareTree(const PfsImage& img, const SourcePlan& plan, bool content, VerifyReport& rep,
                   const CancellationToken& token, const ProgressHooks& hooks)
{
    std::set<QByteArray> planned;
    std::set<QByteArray> plannedDirs;
    for (const SourceFile& f : plan.files) {
        planned.insert(f.relPath);
        QByteArray p = f.relPath;
        for (qsizetype slash = p.lastIndexOf('/'); slash > 0; slash = p.lastIndexOf('/')) {
            p.truncate(slash);
            plannedDirs.insert(p);
        }
    }
    for (const SourceFile& f : plan.files) {
        if (!token.checkpoint())
            return Error::cancelled();
        const PfsEntry* e = img.findFile(f.relPath);
        if (!e) {
            rep.fail(QStringLiteral("Missing from the image: %1").arg(rel(f.relPath)));
            continue;
        }
        FW_TRY(compareFile(img, *e, f, content, rep, token, hooks));
    }
    for (const PfsEntry& e : img.files())
        if (!planned.count(e.path))
            rep.fail(QStringLiteral("Not in the source: %1").arg(rel(e.path)));
    for (const PfsEntry& d : img.dirs())
        if (!plannedDirs.count(d.path))
            rep.fail(QStringLiteral("Folder not in the source: %1").arg(rel(d.path)));
    return success();
}

Status decodeAll(const PfsImage& img, VerifyReport& rep, const CancellationToken& token, const ProgressHooks& hooks)
{
    for (const PfsEntry& e : img.files()) {
        if (hooks.item)
            hooks.item(rel(e.path));
        qint64 n = 0;
        FW_TRY(img.readFile(img.inodes()[e.inode], [&](const char*, qint64 len) {
            n += len;
            if (hooks.advance)
                hooks.advance(len);
            return success();
        }, token));
        rep.bytesChecked += n;
    }
    return success();
}

Result<Opened> openForVerify(const QString& image, const VerifyExpectation& expect, VerifyReport& rep)
{
    Opened o;
    FW_TRY_ASSIGN(o.outer, openPath(image, o.file));
    rep.warnings << o.outer->warnings();
    const bool wantNested = expect.kind == VerifyExpectation::Kind::Folder && expect.format == OutputFormat::Container;
    if (wantNested) {
        if (o.outer->files().size() != 1 || !o.outer->dirs().empty()
            || o.outer->files().front().path != kNestedImageName) {
            rep.fail(QStringLiteral("The container does not hold exactly one %1.").arg(rel(kNestedImageName)));
            return o;
        }
        FW_TRY(openNested(o));
    } else if (expect.kind == VerifyExpectation::Kind::None && looksNested(*o.outer, true)) {
        FW_TRY(openNested(o));
    }
    if (o.inner) {
        rep.nested = true;
        rep.warnings << o.inner->warnings();
    }
    return o;
}

QString summaryFor(const VerifyReport& rep, VerifyDepth depth)
{
    if (!rep.passed)
        return QStringLiteral("%1 problem(s) found; first: %2").arg(rep.problemCount).arg(rep.problems.value(0));
    QString s = depth == VerifyDepth::Full && rep.contentCompared
        ? QStringLiteral("%1 file(s) compared byte for byte, all identical").arg(formatCount(rep.filesChecked))
        : QStringLiteral("%1 file(s) checked, structure and compressed blocks intact").arg(formatCount(rep.filesChecked));
    if (rep.paddedFiles > 0)
        s += QStringLiteral(" (%1 compressed file(s) zero-padded to the block size)").arg(rep.paddedFiles);
    return s;
}

} // namespace

void VerifyReport::fail(const QString& what)
{
    passed = false;
    ++problemCount;
    if (problems.size() < kMaxListed)
        problems << what;
}

Result<ImageSummary> inspectImage(const QString& image)
{
    ImageSummary sum;
    Opened o;
    FW_TRY_ASSIGN(o.outer, openPath(image, o.file));
    sum.imageBytes = o.file->size();
    sum.warnings << o.outer->warnings();
    if (o.outer->files().size() == 1 && o.outer->dirs().empty())
        sum.singleFile = o.outer->files().front().path;
    sum.outerFiles = qint64(o.outer->files().size());
    for (const PfsEntry& e : o.outer->files())
        sum.outerBytes += o.outer->inodes()[e.inode].logicalSize();
    if (looksNested(*o.outer, true)) {
        FW_TRY(openNested(o));
        sum.nested = true;
        sum.warnings << o.inner->warnings();
    }
    const PfsImage& t = o.target();
    sum.timestamp = t.inodes().empty() ? 0 : t.inodes().front().time;
    sum.files = qint64(t.files().size());
    sum.dirs = qint64(t.dirs().size());
    for (const PfsEntry& e : t.files())
        sum.logicalBytes += t.inodes()[e.inode].logicalSize();
    sum.blockSize = t.header().blockSize;
    sum.caseInsensitive = t.header().caseInsensitive();
    return sum;
}

Result<qint64> verifyWorkBytes(const QString& image, const VerifyExpectation& expect, VerifyDepth depth)
{
    if (depth == VerifyDepth::Full) {
        if (expect.kind == VerifyExpectation::Kind::Folder && expect.plan)
            return expect.plan->totalBytes;
        if (expect.kind == VerifyExpectation::Kind::File)
            return QFileInfo(expect.file).size();
    }
    // Structure: the logical size of the outer image's files.
    std::shared_ptr<FileByteSource> keep;
    FW_TRY_ASSIGN(auto img, openPath(image, keep));
    qint64 total = 0;
    for (const PfsEntry& e : img->files())
        total += img->inodes()[e.inode].logicalSize();
    return total;
}

Result<VerifyReport> verifyImage(const QString& image, const VerifyExpectation& expect, VerifyDepth depth,
                                 const CancellationToken& token, const ProgressHooks& hooks)
{
    VerifyReport rep;
    FW_TRY_ASSIGN(Opened o, openForVerify(image, expect, rep));
    if (!rep.passed) {
        rep.summary = summaryFor(rep, depth);
        return rep;
    }
    const bool full = depth == VerifyDepth::Full;
    switch (expect.kind) {
    case VerifyExpectation::Kind::Folder:
        if (!expect.plan)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("No source to compare with."));
        if (!full)
            FW_TRY(decodeAll(*o.outer, rep, token, hooks));
        {
            VerifyReport tree;
            FW_TRY(compareTree(o.target(), *expect.plan, full, tree, token, full ? hooks : ProgressHooks{}));
            rep.passed = tree.passed;
            rep.problems = tree.problems;
            rep.problemCount = tree.problemCount;
            rep.filesChecked = tree.filesChecked;
            rep.paddedFiles = tree.paddedFiles;
            rep.warnings << tree.warnings;
            if (full)
                rep.bytesChecked = tree.bytesChecked;
        }
        rep.contentCompared = full;
        break;
    case VerifyExpectation::Kind::File: {
        if (o.outer->files().size() != 1 || !o.outer->dirs().empty()
            || o.outer->files().front().path != expect.fileName) {
            rep.fail(QStringLiteral("The image does not hold exactly one file named %1.").arg(rel(expect.fileName)));
            break;
        }
        SourceFile src;
        src.relPath = expect.fileName;
        src.absPath = expect.file;
        src.size = QFileInfo(expect.file).size();
        if (!full)
            FW_TRY(decodeAll(*o.outer, rep, token, hooks));
        const qint64 bytesBefore = rep.bytesChecked;
        FW_TRY(compareFile(*o.outer, o.outer->files().front(), src, full, rep, token, full ? hooks : ProgressHooks{}));
        if (!full)
            rep.bytesChecked = bytesBefore;
        rep.contentCompared = full;
        break;
    }
    case VerifyExpectation::Kind::None:
        FW_TRY(decodeAll(*o.outer, rep, token, hooks));
        rep.filesChecked = qint64(o.target().files().size());
        break;
    }
    rep.summary = summaryFor(rep, depth);
    return rep;
}

// ---- extraction ----------------------------------------------------------------------------

Result<ExtractReport> extractImage(const QString& image, const QString& outputDir, const ExtractOptions& options,
                                   const CancellationToken& token, const ProgressHooks& hooks)
{
    ExtractReport rep;
    Opened o;
    FW_TRY_ASSIGN(o.outer, openPath(image, o.file));
    rep.warnings << o.outer->warnings();
    if (options.deep && looksNested(*o.outer, true)) {
        FW_TRY(openNested(o));
        rep.nested = true;
        rep.warnings << o.inner->warnings();
    } else if (options.deep && o.outer->files().size() == 1 && o.outer->dirs().empty()) {
        rep.warnings << QStringLiteral("The container holds %1, which is not a PFS image; it was extracted as a file.")
                            .arg(rel(o.outer->files().front().path));
    }
    const PfsImage& img = o.target();

    const QString out = QDir::cleanPath(QFileInfo(outputDir).absoluteFilePath());
    rep.output = out;
    const QFileInfo outInfo(out);
    if (outInfo.exists() && !outInfo.isDir())
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 exists and is not a folder.").arg(QDir::toNativeSeparators(out)));
    const bool outEmpty =
        !outInfo.exists() || QDir(out).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty();
    if (!outEmpty && !options.overwrite)
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 is not empty. Choose an empty folder or allow replacing its files.")
                         .arg(QDir::toNativeSeparators(out)));

    qint64 total = 0;
    for (const PfsEntry& e : img.files())
        total += img.inodes()[e.inode].logicalSize();
    QDir().mkpath(QFileInfo(out).absolutePath());
    FW_TRY(ensureFreeSpace(QFileInfo(out).absolutePath(), total, options.freeSpaceMargin));

    // A new or empty folder is filled through "<out>.part" and renamed at the end.
    const bool staged = outEmpty;
    const QString root = staged ? out + QStringLiteral(".part") : out;
    // The marker that says a leftover "<out>.part" is ours lives next to the
    // staged folder, not in it: an image may hold a file of any name.
    const QString markerPath = root + QStringLiteral(".extracting");
    if (staged) {
        if (QFileInfo::exists(root)) {
            // (Older versions kept the marker inside the staged folder.)
            if (!QFileInfo::exists(markerPath) && !QFileInfo::exists(root + u'/' + QLatin1String(kExtractMarker)))
                return Error(ErrorCode::AlreadyExists,
                             QStringLiteral("%1 already exists. Move or delete it and try again.")
                                 .arg(QDir::toNativeSeparators(root)));
            FW_TRY(removeTreeSafely(root));
        }
        QFile marker(markerPath);
        if (!marker.open(QIODevice::WriteOnly))
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not write to %1.").arg(QDir::toNativeSeparators(QFileInfo(out).absolutePath())));
        marker.close();
        if (!QDir().mkpath(root)) {
            QFile::remove(markerPath);
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(root)));
        }
    }
    struct Cleanup {
        QString path, marker;
        bool staged;
        bool active;
        ~Cleanup()
        {
            if (active)
                (void)removeTreeSafely(path);
            if (staged)
                QFile::remove(marker);
        }
    } cleanup{root, markerPath, staged, staged};

    // Image names are untrusted. Each one becomes a single safe component
    // (renamed where this drive cannot store it), and every path is checked to
    // stay inside `root`. Folders come before their contents (sorted paths).
    std::map<QByteArray, QString> hostDirs{{QByteArray(), QString()}};
    std::map<QByteArray, QSet<QString>> taken;
    const auto hostPath = [&](const QByteArray& path) -> Result<QString> {
        const qsizetype slash = path.lastIndexOf('/');
        const QByteArray parent = slash < 0 ? QByteArray() : path.left(slash);
        const auto dir = hostDirs.find(parent);
        if (dir == hostDirs.end())
            return Error(ErrorCode::Internal, QStringLiteral("The folder of %1 was not extracted.").arg(rel(path)));
        const QString name = rel(slash < 0 ? path : path.mid(slash + 1));
        QString local = hostEntryName(name);
        QSet<QString>& used = taken[parent];
        if (used.contains(hostNameKey(local))) {
            const QString stem = local;
            for (int k = 2; used.contains(hostNameKey(local)); ++k)
                local = QStringLiteral("%1 (%2)").arg(stem).arg(k);
        }
        used.insert(hostNameKey(local));
        if (local != name)
            rep.renamed << QStringLiteral("%1 → %2").arg(rel(path), local);
        return dir->second.isEmpty() ? local : dir->second + u'/' + local;
    };

    for (const PfsEntry& d : img.dirs()) {
        FW_TRY_ASSIGN(const QString dirRel, hostPath(d.path));
        hostDirs.emplace(d.path, dirRel);
        FW_TRY_ASSIGN(const QString p, childPathInside(root, dirRel));
        const QFileInfo fi(p);
        if (fi.exists() && !fi.isDir())
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 exists and is not a folder.").arg(QDir::toNativeSeparators(p)));
        if (!fi.exists()) {
            if (!QDir().mkpath(p))
                return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(p)));
            ++rep.dirs;
        }
    }
    for (const PfsEntry& e : img.files()) {
        if (!token.checkpoint())
            return Error::cancelled();
        FW_TRY_ASSIGN(const QString fileRel, hostPath(e.path));
        FW_TRY_ASSIGN(const QString target, childPathInside(root, fileRel));
        if (QFileInfo(target).isDir())
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 is a folder; the image has a file there.").arg(QDir::toNativeSeparators(target)));
        if (hooks.item)
            hooks.item(rel(e.path));
        PartFile part(target);
        QFile f(toLongPath(part.partPath()));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(target)),
                         f.errorString());
        FW_TRY(img.readFile(img.inodes()[e.inode], [&](const char* d, qint64 len) -> Status {
            if (f.write(d, len) != len)
                return Error(ErrorCode::IoError,
                             QStringLiteral("Could not write %1. The drive may be full.").arg(QDir::toNativeSeparators(target)),
                             f.errorString());
            rep.bytes += len;
            if (hooks.advance)
                hooks.advance(len);
            return success();
        }, token));
        // The last piece may still sit in Qt's buffer: a full drive shows here.
        if (!f.flush())
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not write %1. The drive may be full.").arg(QDir::toNativeSeparators(target)),
                         f.errorString());
        f.close();
        FW_TRY(part.commit(options.overwrite));
        ++rep.files;
    }
    if (staged) {
        if (QFileInfo::exists(out) && !QDir().rmdir(out))
            return Error(ErrorCode::IoError, QStringLiteral("Could not replace the empty folder %1.").arg(QDir::toNativeSeparators(out)));
        if (!QDir().rename(root, out))
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not rename %1 to %2.").arg(QDir::toNativeSeparators(root), QDir::toNativeSeparators(out)));
        cleanup.active = false;
    }
    return rep;
}

} // namespace fw::pfs
