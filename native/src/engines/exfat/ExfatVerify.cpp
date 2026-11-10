#include "engines/exfat/ExfatVerify.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace fw::exfat {

QString VerifyReport::summary() const
{
    if (passed()) {
        if (full)
            return QStringLiteral("%1 files and %2 folders compared byte for byte, all identical (%3).")
                .arg(formatCount(filesChecked), formatCount(dirsChecked), formatBytes(bytesCompared));
        return QStringLiteral("%1 files and %2 folders checked: names, sizes and the image structure match "
                              "(contents were not compared).")
            .arg(formatCount(filesChecked), formatCount(dirsChecked));
    }
    const qint64 n = qint64(mismatches.size()) + structureProblems.size();
    QStringList first;
    for (const QString& p : structureProblems) {
        if (first.size() >= 3)
            break;
        first << p;
    }
    for (const Mismatch& m : mismatches) {
        if (first.size() >= 3)
            break;
        first << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(m.path), m.detail);
    }
    QString s = n == 1 ? QStringLiteral("1 problem: ") : QStringLiteral("%1 problems: ").arg(formatCount(n));
    s += first.join(QStringLiteral("; "));
    if (n > first.size())
        s += QStringLiteral("; and %1 more").arg(formatCount(n - first.size()));
    return s + u'.';
}

QJsonObject VerifyReport::toJson(int maxMismatches) const
{
    QJsonObject o;
    o.insert(QStringLiteral("mode"), full ? QStringLiteral("full") : QStringLiteral("structure"));
    o.insert(QStringLiteral("passed"), passed());
    o.insert(QStringLiteral("files"), double(filesChecked));
    o.insert(QStringLiteral("folders"), double(dirsChecked));
    o.insert(QStringLiteral("bytesCompared"), double(bytesCompared));
    o.insert(QStringLiteral("mismatchCount"), double(mismatches.size()));
    QJsonArray arr;
    for (size_t i = 0; i < mismatches.size() && int(i) < maxMismatches; ++i) {
        QJsonObject m;
        m.insert(QStringLiteral("path"), mismatches[i].path);
        m.insert(QStringLiteral("kind"), mismatches[i].kind);
        m.insert(QStringLiteral("detail"), mismatches[i].detail);
        arr.append(m);
    }
    o.insert(QStringLiteral("mismatches"), arr);
    o.insert(QStringLiteral("structureProblems"), QJsonArray::fromStringList(structureProblems));
    o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
    o.insert(QStringLiteral("elapsedMs"), double(elapsedMs));
    return o;
}

std::vector<ExpectedFile> expectedFromFolder(const SourceTree& tree)
{
    std::vector<ExpectedFile> out;
    out.reserve(tree.nodes.size());
    for (size_t i = 1; i < tree.nodes.size(); ++i) {
        const SourceNode& n = tree.nodes[i];
        ExpectedFile e;
        e.path = n.relativePath;
        e.isDir = n.isDir;
        e.size = n.size;
        if (!n.isDir)
            e.localPath = n.absolutePath;
        out.push_back(std::move(e));
    }
    return out;
}

namespace {

struct ContentJob {
    const ExpectedFile* expected;
    const ImageEntry* entry;
};

// Byte comparison of one file. Returns a mismatch, or nullopt when identical.
// Cancellation is reported through `cancelled`.
std::optional<Mismatch> compareContent(const ContentJob& job, ImageFileStream& image, ImageFileStream* other,
                                       std::vector<char>& a, std::vector<char>& b, const CancellationToken& token,
                                       ProgressReporter* progress, std::atomic<qint64>& bytes, bool& cancelled)
{
    const ImageEntry& e = *job.entry;
    const ExpectedFile& x = *job.expected;
    std::unique_ptr<QFile> local;
    if (!x.localPath.isEmpty()) {
        local = std::make_unique<QFile>(toLongPath(x.localPath));
        if (!local->open(QIODevice::ReadOnly))
            return Mismatch{x.path, QStringLiteral("unreadable"),
                            QStringLiteral("the source file could not be opened (%1)").arg(local->errorString())};
    } else if (!x.imageEntry || !other) {
        return Mismatch{x.path, QStringLiteral("unreadable"), QStringLiteral("no source for its contents")};
    }
    const qint64 chunk = qint64(a.size());
    for (qint64 off = 0; off < e.size;) {
        if (!token.checkpoint()) {
            cancelled = true;
            return std::nullopt;
        }
        const qint64 n = std::min(chunk, e.size - off);
        qint64 got = 0;
        if (local) {
            qint64 r = 0;
            while (got < n && (r = local->read(a.data() + got, n - got)) > 0)
                got += r;
        } else {
            auto r = other->read(*x.imageEntry, off, a.data(), n);
            got = r.ok() ? r.value() : -1;
        }
        if (got != n)
            return Mismatch{x.path, QStringLiteral("unreadable"),
                            QStringLiteral("the source could not be read at byte %1").arg(off + std::max<qint64>(0, got))};
        auto r = image.read(e, off, b.data(), n);
        if (!r.ok() || r.value() != n)
            return Mismatch{x.path, QStringLiteral("unreadable"),
                            QStringLiteral("the image could not be read at byte %1%2")
                                .arg(off)
                                .arg(r.ok() ? QString() : QStringLiteral(" (") + r.error().message + u')')};
        if (std::memcmp(a.data(), b.data(), size_t(n)) != 0) {
            qint64 i = 0;
            while (i < n && a[size_t(i)] == b[size_t(i)])
                ++i;
            return Mismatch{x.path, QStringLiteral("content"),
                            QStringLiteral("the contents differ from byte %1").arg(formatCount(off + i))};
        }
        off += n;
        bytes.fetch_add(n, std::memory_order_relaxed);
        if (progress)
            progress->advance(n);
    }
    return std::nullopt;
}

} // namespace

Result<VerifyReport> verifyImage(const QString& imagePath, const std::vector<ExpectedFile>& expected,
                                 const VerifyOptions& o, const CancellationToken& token, ProgressReporter* progress,
                                 const QString& otherImage, const Geometry* otherGeometry)
{
    QElapsedTimer timer;
    timer.start();
    VerifyReport report;
    report.full = o.full;

    auto opened = ExfatReader::open(imagePath);
    if (!opened.ok()) {
        if (opened.error().code != ErrorCode::CorruptData)
            return opened.takeError();
        report.structureProblems << opened.error().message;
        if (!opened.error().detail.isEmpty())
            report.warnings << QStringLiteral("Detail: ") + opened.error().detail;
        report.elapsedMs = timer.elapsed();
        return report;
    }
    ExfatReader& reader = *opened.value();
    auto treeR = reader.readTree(token, true);
    if (!treeR.ok())
        return treeR.takeError();
    const ImageTree& tree = treeR.value();
    report.structureProblems = tree.problems;
    if (tree.problemCount > tree.problems.size())
        report.structureProblems << QStringLiteral("and %1 more structure problems").arg(tree.problemCount - tree.problems.size());
    report.warnings = tree.warnings;
    if (o.requireBackupBoot && !reader.volume().backupBootMatches)
        report.structureProblems << QStringLiteral("the backup boot region does not match the main one (%1)")
                                        .arg(reader.volume().backupProblem);

    // Names, types and sizes.
    QHash<QString, int> folded;
    for (int i = 1; i < int(tree.entries.size()); ++i)
        folded.insert(reader.upcase().upcase(tree.entries[size_t(i)].path), i);
    std::vector<bool> matched(tree.entries.size(), false);
    std::vector<ContentJob> content;
    qint64 contentBytes = 0;
    for (const ExpectedFile& x : expected) {
        int idx = tree.find(x.path);
        if (idx <= 0) {
            const int ci = folded.value(reader.upcase().upcase(x.path), -1);
            if (ci > 0) {
                matched[size_t(ci)] = true;
                report.mismatches.push_back({x.path, QStringLiteral("name-case"),
                                             QStringLiteral("the image has it as %1").arg(tree.entries[size_t(ci)].path)});
            } else {
                report.mismatches.push_back({x.path, QStringLiteral("missing"),
                                             x.isDir ? QStringLiteral("the folder is missing from the image")
                                                     : QStringLiteral("missing from the image (%1)").arg(formatBytes(x.size))});
            }
            continue;
        }
        matched[size_t(idx)] = true;
        const ImageEntry& e = tree.entries[size_t(idx)];
        if (e.isDir != x.isDir) {
            report.mismatches.push_back({x.path, QStringLiteral("type"),
                                         x.isDir ? QStringLiteral("a folder in the source but a file in the image")
                                                 : QStringLiteral("a file in the source but a folder in the image")});
            continue;
        }
        if (x.isDir) {
            ++report.dirsChecked;
            continue;
        }
        ++report.filesChecked;
        if (e.size != x.size) {
            report.mismatches.push_back({x.path, QStringLiteral("size"),
                                         QStringLiteral("%1 in the image, %2 expected")
                                             .arg(formatCount(e.size), formatCount(x.size))});
            continue;
        }
        if (x.unchangedFirstCluster >= 0) {
            if (e.size > 0 && qint64(e.firstCluster) != x.unchangedFirstCluster)
                report.mismatches.push_back({x.path, QStringLiteral("content"),
                                             QStringLiteral("the file no longer points at its original data")});
            continue;
        }
        if (o.full && e.size > 0) {
            content.push_back({&x, &e});
            contentBytes += e.size;
        }
    }
    for (int i = 1; i < int(tree.entries.size()); ++i)
        if (!matched[size_t(i)])
            report.mismatches.push_back({tree.entries[size_t(i)].path, QStringLiteral("extra"),
                                         tree.entries[size_t(i)].isDir ? QStringLiteral("a folder that should not be in the image")
                                                                       : QStringLiteral("a file that should not be in the image")});

    // Contents, in image order, several files at once.
    if (o.onCompareStart)
        o.onCompareStart(o.full ? contentBytes : qint64(expected.size()), qint64(content.size()), o.full);
    if (!o.full && progress)
        progress->advance(qint64(expected.size()));
    if (!content.empty()) {
        std::sort(content.begin(), content.end(), [](const ContentJob& a, const ContentJob& b) {
            return a.entry->firstCluster < b.entry->firstCluster;
        });
        const int threads = std::clamp(o.threads, 1, int(std::min<size_t>(content.size(), 64)));
        std::atomic<size_t> next{0};
        std::atomic<bool> stop{false};
        std::atomic<qint64> bytes{0};
        std::mutex mutex;
        std::optional<Error> fatal;
        bool anyCancelled = false;
        const auto worker = [&] {
            ImageFileStream image(imagePath, reader.geometry());
            std::unique_ptr<ImageFileStream> other;
            if (!otherImage.isEmpty() && otherGeometry) {
                other = std::make_unique<ImageFileStream>(otherImage, *otherGeometry);
                if (const Status st = other->open(); !st.ok()) {
                    std::lock_guard lock(mutex);
                    fatal = st.error();
                    stop = true;
                    return;
                }
            }
            if (const Status st = image.open(); !st.ok()) {
                std::lock_guard lock(mutex);
                fatal = st.error();
                stop = true;
                return;
            }
            std::vector<char> a(size_t(std::max<qint64>(64 * 1024, o.bufferBytes)));
            std::vector<char> b(a.size());
            while (!stop.load()) {
                const size_t i = next.fetch_add(1);
                if (i >= content.size())
                    break;
                bool cancelled = false;
                auto m = compareContent(content[i], image, other.get(), a, b, token, progress, bytes, cancelled);
                std::lock_guard lock(mutex);
                if (cancelled) {
                    anyCancelled = true;
                    stop = true;
                    break;
                }
                if (progress)
                    progress->advanceFiles(1);
                if (m)
                    report.mismatches.push_back(std::move(*m));
            }
        };
        // An exception that leaves a thread ends the whole app. Buffers that
        // cannot be allocated (many threads, large I/O buffers) become this
        // job's error instead.
        const auto guarded = [&] {
            try {
                worker();
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                if (!fatal)
                    fatal = Error(ErrorCode::Internal,
                                  QStringLiteral("Not enough memory to compare the files. Lower the I/O buffer size or the "
                                                 "number of worker threads in Settings, then try again."),
                                  QString::fromLocal8Bit(e.what()));
                stop = true;
            }
        };
        std::vector<std::thread> pool;
        try {
            for (int t = 0; t < threads; ++t)
                pool.emplace_back(guarded);
        } catch (const std::exception& e) {
            // No more threads: the ones that started do the work.
            if (pool.empty()) {
                std::lock_guard lock(mutex);
                fatal = Error(ErrorCode::Internal, QStringLiteral("Could not start a worker thread."),
                              QString::fromLocal8Bit(e.what()));
            }
        }
        for (auto& t : pool)
            t.join();
        if (anyCancelled || token.isCancelled())
            return Error::cancelled();
        if (fatal)
            return *fatal;
        report.bytesCompared = bytes.load();
    }
    std::sort(report.mismatches.begin(), report.mismatches.end(),
              [](const Mismatch& a, const Mismatch& b) { return a.path < b.path; });
    report.elapsedMs = timer.elapsed();
    return report;
}

Result<VerifyReport> verifyImageAgainstFolder(const QString& image, const QString& folder, const ScanRules& rules,
                                              const VerifyOptions& options, const CancellationToken& token,
                                              ProgressReporter* progress)
{
    auto tree = scanSource(folder, rules, token, progress);
    if (!tree.ok())
        return tree.takeError();
    const auto expected = expectedFromFolder(tree.value());
    return verifyImage(image, expected, options, token, progress);
}

} // namespace fw::exfat
