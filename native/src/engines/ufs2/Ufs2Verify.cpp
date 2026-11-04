#include "engines/ufs2/Ufs2Verify.h"

#include "core/FileUtil.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <cstring>
#include <future>
#include <map>

namespace fw::ufs2 {

namespace {

constexpr qint64 kAsyncThreshold = 1024 * 1024; // overlap reads only for large chunks

QString joinPath(const QString& dir, const QString& name) { return dir.isEmpty() ? name : dir + u'/' + name; }

// Reads `n` bytes from two streams, the second one on another thread when
// the chunk is large, so both files are read at the same time.
struct PairRead {
    Result<qint64> a = qint64(0);
    Result<qint64> b = qint64(0);
};

PairRead readBoth(ContentStream& sa, char* a, ContentStream& sb, char* b, qint64 n)
{
    PairRead r;
    if (n >= kAsyncThreshold) {
        auto fut = std::async(std::launch::async, [&sb, b, n] { return sb.read(b, n); });
        r.a = sa.read(a, n);
        r.b = fut.get();
    } else {
        r.a = sa.read(a, n);
        r.b = sb.read(b, n);
    }
    return r;
}

// Compares two streams of `size` bytes; returns an empty string when equal.
Result<QString> compareStreams(ContentStream& expected, ContentStream& actual, qint64 size, QByteArray& ba, QByteArray& bb,
                               const CancellationToken& token, ProgressReporter* progress, qint64* compared)
{
    qint64 done = 0;
    while (done < size) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 n = std::min<qint64>(ba.size(), size - done);
        PairRead r = readBoth(expected, ba.data(), actual, bb.data(), n);
        if (!r.a.ok())
            return QStringLiteral("the source could not be read (%1)").arg(r.a.error().message);
        if (!r.b.ok())
            return QStringLiteral("could not be read back (%1)").arg(r.b.error().message);
        if (r.a.value() != n || r.b.value() != n)
            return QStringLiteral("is shorter than expected");
        if (std::memcmp(ba.constData(), bb.constData(), size_t(n)) != 0) {
            qint64 at = 0;
            while (at < n && ba[qsizetype(at)] == bb[qsizetype(at)])
                ++at;
            return QStringLiteral("content differs at byte %1").arg(done + at);
        }
        done += n;
        if (compared)
            *compared += n;
        if (progress)
            progress->advance(n);
    }
    return QString();
}

QString formatList(const QStringList& items, int max = 3)
{
    QStringList head = items.mid(0, max);
    QString s = head.join(QStringLiteral(", "));
    if (items.size() > max)
        s += QStringLiteral(" and %1 more").arg(items.size() - max);
    return s;
}

} // namespace

CompareDepth compareDepthFromSetting(const QString& mode)
{
    return mode == QLatin1String("structure") ? CompareDepth::Structure : CompareDepth::Full;
}

QString CompareReport::summary(CompareDepth depth) const
{
    if (ok()) {
        if (depth == CompareDepth::Full)
            return QStringLiteral("%1 files compared byte for byte, all identical (%2).")
                .arg(formatCount(filesCompared), formatBytes(bytesCompared));
        return QStringLiteral("%1 files and %2 folders match by name, type and size.")
            .arg(formatCount(filesCompared), formatCount(dirsCompared));
    }
    QStringList parts;
    if (!missing.isEmpty())
        parts << QStringLiteral("%1 missing (%2)").arg(missing.size()).arg(formatList(missing));
    if (!extra.isEmpty())
        parts << QStringLiteral("%1 unexpected (%2)").arg(extra.size()).arg(formatList(extra));
    if (!changed.isEmpty())
        parts << QStringLiteral("%1 different (%2)").arg(changed.size()).arg(formatList(changed));
    return parts.join(QStringLiteral("; ")) + u'.';
}

Result<CompareReport> compareImage(const Ufs2Reader& image, const SourceTree& expected, CompareDepth depth,
                                   const CancellationToken& token, ProgressReporter* progress, qint64 bufferBytes,
                                   const QString& fsPath)
{
    CompareReport rep;
    QStringList skipped;
    FW_TRY_ASSIGN(SourceTree img, image.loadTree(token, &skipped, fsPath));
    for (const QString& s : skipped)
        rep.extra << s + QStringLiteral(" (link or special file)");
    QByteArray ba(qsizetype(bufferBytes), '\0'), bb(qsizetype(bufferBytes), '\0');
    struct Item {
        int exp, img;
        QString rel;
    };
    std::vector<Item> stack{{0, 0, QString()}};
    qint64 n = 0;
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        std::map<QByteArray, int> byName;
        for (int c : img.liveChildren(it.img))
            byName.emplace(img.nodes[size_t(c)].rawName, c);
        for (int ec : expected.liveChildren(it.exp)) {
            if ((++n & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const TreeNode& e = expected.nodes[size_t(ec)];
            const QString rel = joinPath(it.rel, e.name);
            auto found = byName.find(e.rawName);
            if (found == byName.end()) {
                rep.missing << rel;
                continue;
            }
            const int ic = found->second;
            byName.erase(found);
            const TreeNode& i = img.nodes[size_t(ic)];
            if (e.isDir() != i.isDir()) {
                rep.changed << rel + QStringLiteral(" (file/folder mismatch)");
                continue;
            }
            if (e.isDir()) {
                ++rep.dirsCompared;
                stack.push_back({ec, ic, rel});
                continue;
            }
            ++rep.filesCompared;
            if (e.size != i.size) {
                rep.changed << QStringLiteral("%1 (%2 bytes, expected %3)").arg(rel).arg(i.size).arg(e.size);
                if (progress && depth == CompareDepth::Full)
                    progress->advance(e.size);
                continue;
            }
            if (depth == CompareDepth::Structure) {
                if (progress)
                    progress->advanceFiles(1);
                continue;
            }
            auto se = openContent(expected, ec);
            if (!se.ok()) {
                rep.changed << QStringLiteral("%1 (the source could not be read)").arg(rel);
                continue;
            }
            auto si = image.openFile(i.imageIno);
            if (!si.ok()) {
                rep.changed << QStringLiteral("%1 (could not be read back: %2)").arg(rel, si.error().message);
                continue;
            }
            FW_TRY_ASSIGN(QString diff, compareStreams(*se.value(), *si.value(), e.size, ba, bb, token, progress, &rep.bytesCompared));
            if (!diff.isEmpty())
                rep.changed << QStringLiteral("%1 (%2)").arg(rel, diff);
            if (progress)
                progress->advanceFiles(1);
        }
        for (const auto& [raw, ic] : byName)
            rep.extra << joinPath(it.rel, img.nodes[size_t(ic)].name);
    }
    return rep;
}

Result<qint64> readEveryFile(const Ufs2Reader& image, const SourceTree& tree, const CancellationToken& token,
                             ProgressReporter* progress, qint64 bufferBytes)
{
    QByteArray buf(qsizetype(bufferBytes), '\0');
    qint64 total = 0;
    for (size_t idx = 0; idx < tree.nodes.size(); ++idx) {
        const TreeNode& n = tree.nodes[idx];
        if (n.isDir() || n.removed)
            continue;
        FW_TRY_ASSIGN(auto s, image.openFile(n.imageIno));
        qint64 done = 0;
        while (done < n.size) {
            if (!token.checkpoint())
                return Error::cancelled();
            FW_TRY_ASSIGN(qint64 got, s->read(buf.data(), std::min<qint64>(buf.size(), n.size - done)));
            if (got <= 0)
                return image.corrupt(QStringLiteral("%1 is shorter than its size.").arg(tree.relativePath(int(idx))));
            done += got;
            if (progress)
                progress->advance(got);
        }
        total += done;
        if (progress)
            progress->advanceFiles(1);
    }
    return total;
}

// ---- extraction ------------------------------------------------------------------------

QString hostFileName(const QString& name, bool windowsRules)
{
    if (!windowsRules)
        return name;
    QString out = sanitizeFileName(name);
    static const QSet<QString> reserved = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                           QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
                                           QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
                                           QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
                                           QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
                                           QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
                                           QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
                                           QStringLiteral("LPT9")};
    const QString stem = out.section(u'.', 0, 0).toUpper();
    if (reserved.contains(stem))
        out.prepend(u'_');
    return out;
}

namespace {

#ifdef Q_OS_WIN
constexpr bool kWindowsNames = true;
#else
constexpr bool kWindowsNames = false;
#endif

QString nameKey(const QString& n) { return kWindowsNames ? n.toLower() : n; }

Status writeOne(const SourceTree& tree, int node, const QString& path, QByteArray& ba, QByteArray& bb,
                const CancellationToken& token, ProgressReporter* progress)
{
    const TreeNode& n = tree.nodes[size_t(node)];
    FW_TRY_ASSIGN(auto in, openContent(tree, node));
    QFile out(toLongPath(path));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(path)), out.errorString());
    qint64 done = 0;
    // Read the next chunk while the current one is written.
    QByteArray* cur = &ba;
    QByteArray* next = &bb;
    qint64 curLen = 0;
    if (n.size > 0) {
        FW_TRY_ASSIGN(curLen, in->read(cur->data(), std::min<qint64>(cur->size(), n.size)));
    }
    while (done < n.size) {
        if (!token.checkpoint())
            return Error::cancelled();
        if (curLen <= 0)
            return tree.image ? tree.image->corrupt(QStringLiteral("%1 is shorter than its size.").arg(tree.relativePath(node)))
                              : Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(tree.relativePath(node)));
        const qint64 remainingAfter = n.size - done - curLen;
        std::future<Result<qint64>> fut;
        const qint64 nextWant = std::min<qint64>(next->size(), remainingAfter);
        if (nextWant >= kAsyncThreshold)
            fut = std::async(std::launch::async, [&in, next, nextWant] { return in->read(next->data(), nextWant); });
        qint64 w = 0;
        while (w < curLen) {
            const qint64 k = out.write(cur->constData() + w, curLen - w);
            if (k <= 0) {
                if (fut.valid())
                    fut.wait();
                return Error(ErrorCode::IoError,
                             QStringLiteral("Could not write %1. The drive may be full.").arg(QDir::toNativeSeparators(path)),
                             out.errorString());
            }
            w += k;
        }
        done += curLen;
        if (progress)
            progress->advance(curLen);
        if (remainingAfter <= 0)
            break;
        Result<qint64> r = fut.valid() ? fut.get() : in->read(next->data(), nextWant);
        if (!r.ok())
            return r.takeError();
        curLen = r.value();
        std::swap(cur, next);
    }
    if (n.mtime > 0)
        out.setFileTime(QDateTime::fromSecsSinceEpoch(n.mtime), QFileDevice::FileModificationTime);
    out.close();
    if (out.error() != QFileDevice::NoError)
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path)), out.errorString());
    return success();
}

} // namespace

Result<ExtractReport> extractTree(const SourceTree& tree, const QString& dest, const CancellationToken& token,
                                  ProgressReporter* progress, qint64 bufferBytes)
{
    ExtractReport rep;
    QByteArray ba(qsizetype(bufferBytes), '\0'), bb(qsizetype(bufferBytes), '\0');
    std::vector<std::pair<int, QString>> stack{{0, dest}};
    while (!stack.empty()) {
        const auto [dirNode, dirPath] = stack.back();
        stack.pop_back();
        QSet<QString> used;
        for (int c : tree.liveChildren(dirNode)) {
            if (!token.checkpoint())
                return Error::cancelled();
            const TreeNode& n = tree.nodes[size_t(c)];
            QString local = hostFileName(n.name, kWindowsNames);
            if (used.contains(nameKey(local))) {
                const QString stem = local;
                for (int k = 2; used.contains(nameKey(local)); ++k)
                    local = QStringLiteral("%1 (%2)").arg(stem).arg(k);
            }
            used.insert(nameKey(local));
            if (local != n.name)
                rep.renamed << QStringLiteral("%1 → %2").arg(tree.relativePath(c), local);
            const QString path = dirPath + u'/' + local;
            if (n.isDir()) {
                if (!QDir().mkpath(path))
                    return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(path)));
                ++rep.dirs;
                stack.emplace_back(c, path);
            } else {
                if (progress)
                    progress->setCurrentItem(tree.relativePath(c));
                FW_TRY(writeOne(tree, c, path, ba, bb, token, progress));
                ++rep.files;
                rep.bytes += n.size;
                rep.written.emplace_back(c, path);
                if (progress)
                    progress->advanceFiles(1);
            }
        }
    }
    return rep;
}

namespace {

class FileStream final : public ContentStream {
public:
    explicit FileStream(const QString& p) : m_f(toLongPath(p)) {}
    bool open() { return m_f.open(QIODevice::ReadOnly); }
    Result<qint64> read(char* dst, qint64 max) override
    {
        qint64 total = 0;
        while (total < max) {
            const qint64 k = m_f.read(dst + total, max - total);
            if (k < 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(m_f.fileName()), m_f.errorString());
            if (k == 0)
                break;
            total += k;
        }
        return total;
    }

private:
    QFile m_f;
};

} // namespace

Result<CompareReport> verifyExtracted(const SourceTree& tree, const ExtractReport& report, CompareDepth depth,
                                      const CancellationToken& token, ProgressReporter* progress, qint64 bufferBytes)
{
    CompareReport rep;
    rep.dirsCompared = report.dirs;
    QByteArray ba(qsizetype(bufferBytes), '\0'), bb(qsizetype(bufferBytes), '\0');
    for (const auto& [node, path] : report.written) {
        const TreeNode& n = tree.nodes[size_t(node)];
        const QString rel = tree.relativePath(node);
        const QFileInfo fi(toLongPath(path));
        ++rep.filesCompared;
        if (!fi.exists()) {
            rep.missing << rel;
            continue;
        }
        if (fi.size() != n.size) {
            rep.changed << QStringLiteral("%1 (%2 bytes, expected %3)").arg(rel).arg(fi.size()).arg(n.size);
            continue;
        }
        if (depth == CompareDepth::Structure) {
            if (progress)
                progress->advanceFiles(1);
            continue;
        }
        FW_TRY_ASSIGN(auto img, openContent(tree, node));
        FileStream disk(path);
        if (!disk.open()) {
            rep.changed << QStringLiteral("%1 (could not be read back)").arg(rel);
            continue;
        }
        FW_TRY_ASSIGN(QString diff, compareStreams(*img, disk, n.size, ba, bb, token, progress, &rep.bytesCompared));
        if (!diff.isEmpty())
            rep.changed << QStringLiteral("%1 (%2)").arg(rel, diff);
        if (progress)
            progress->advanceFiles(1);
    }
    return rep;
}

} // namespace fw::ufs2
