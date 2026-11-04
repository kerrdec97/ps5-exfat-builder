#include "engines/exfat/ExfatExtract.h"

#include "core/FileUtil.h"
#include "engines/exfat/Pipeline.h"
#include "engines/exfat/PlatformFs.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace fw::exfat {

namespace {

class FileFinisher {
public:
    FileFinisher(const ExtractOptions& o) : m_o(o) {}
    void finish(const ImageEntry& e, const QString& target)
    {
        bool ok = true;
        if (m_o.preserveTimes)
            ok = setPathTimes(target, e.created, e.modified, e.accessed) && ok;
        if (m_o.preserveAttributes)
            ok = applyAttributes(target, e.attributes, e.isDir) && ok;
        if (!ok)
            m_failed.fetch_add(1);
    }
    qint64 failed() const { return m_failed.load(); }

private:
    const ExtractOptions& m_o;
    std::atomic<qint64> m_failed{0};
};

Status writeAll(QFile& out, const char* p, qint64 n, const QString& shown)
{
    qint64 done = 0;
    while (done < n) {
        const qint64 w = out.write(p + done, n - done);
        if (w <= 0)
            return fileError(QStringLiteral("Could not write %1.").arg(shown), out);
        done += w;
    }
    return success();
}

} // namespace

Result<ExtractStats> extractTree(const ExfatReader& reader, const ImageTree& tree, const QString& destDir,
                                 const ExtractOptions& o, const CancellationToken& token, ProgressReporter* progress)
{
    QElapsedTimer timer;
    timer.start();
    ExtractStats stats;
    const Geometry& g = reader.geometry();
    const QString image = reader.imagePath();
    const auto target = [&](const ImageEntry& e) { return destDir + u'/' + e.path; };

    std::vector<int> files;
    for (int i = 1; i < int(tree.entries.size()); ++i) {
        const ImageEntry& e = tree.entries[size_t(i)];
        if (e.isDir) {
            if (!QDir().mkpath(toLongPath(target(e))))
                return Error(ErrorCode::IoError,
                             QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(target(e))));
            ++stats.dirs;
        } else {
            files.push_back(i);
        }
    }
    std::sort(files.begin(), files.end(), [&](int a, int b) {
        return tree.entries[size_t(a)].firstCluster < tree.entries[size_t(b)].firstCluster;
    });
    FileFinisher finisher(o);

    if (!o.parallel || o.threads <= 1 || files.size() < 2) {
        // One reader thread streams the image in order; this thread writes.
        ChunkPipeline pipe(o.bufferCount, o.bufferBytes);
        ScopedThread producer(std::thread([&] {
            ImageFileStream in(image, g);
            if (const Status st = in.open(); !st.ok()) {
                pipe.setProducerError(st.error());
                pipe.finishProducing();
                return;
            }
            for (int k = 0; k < int(files.size()); ++k) {
                const ImageEntry& e = tree.entries[size_t(files[size_t(k)])];
                if (e.size == 0) {
                    if (!pipe.post({nullptr, 0, k, true}))
                        break;
                    continue;
                }
                bool stop = false;
                for (qint64 off = 0; off < e.size;) {
                    if (!token.checkpoint()) {
                        pipe.setProducerError(Error::cancelled());
                        stop = true;
                        break;
                    }
                    std::vector<char>* buf = pipe.takeFree();
                    if (!buf) {
                        stop = true;
                        break;
                    }
                    auto r = in.read(e, off, buf->data(), std::min<qint64>(qint64(buf->size()), e.size - off));
                    if (!r.ok() || r.value() <= 0) {
                        pipe.giveBack(buf);
                        pipe.setProducerError(r.ok() ? corruptImage(QStringLiteral("%1 is cut short").arg(e.path)) : r.takeError());
                        stop = true;
                        break;
                    }
                    off += r.value();
                    if (!pipe.post({buf, r.value(), k, off >= e.size})) {
                        stop = true;
                        break;
                    }
                }
                if (stop)
                    break;
            }
            pipe.finishProducing();
        }));
        struct AbortOnExit {
            ChunkPipeline& p;
            ScopedThread& t;
            ~AbortOnExit()
            {
                p.abort();
                t.join();
            }
        } guard{pipe, producer};

        QFile out;
        int current = -1;
        while (auto c = pipe.next()) {
            const ImageEntry& e = tree.entries[size_t(files[size_t(c->item)])];
            const QString path = target(e);
            const QString shown = QDir::toNativeSeparators(path);
            if (c->item != current) {
                current = c->item;
                out.setFileName(toLongPath(path));
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    return fileError(QStringLiteral("Could not create %1.").arg(shown), out);
                if (progress)
                    progress->setCurrentItem(e.path);
            }
            if (c->length > 0) {
                const Status st = writeAll(out, c->buffer->data(), c->length, shown);
                pipe.giveBack(c->buffer);
                if (!st.ok())
                    return st.error();
                stats.bytes += c->length;
                if (progress)
                    progress->advance(c->length);
            }
            if (c->endOfItem) {
                out.close();
                if (out.error() != QFileDevice::NoError)
                    return fileError(QStringLiteral("Could not write %1.").arg(shown), out);
                finisher.finish(e, path);
                ++stats.files;
                if (progress)
                    progress->advanceFiles(1);
                if (!token.checkpoint())
                    return Error::cancelled();
            }
        }
        if (auto err = pipe.producerError())
            return *err;
    } else {
        // Several workers, each with its own image handle.
        std::atomic<size_t> next{0};
        std::atomic<bool> stop{false};
        std::atomic<qint64> bytes{0}, done{0};
        std::mutex mutex;
        std::optional<Error> failure;
        const auto fail = [&](Error e) {
            std::lock_guard lock(mutex);
            if (!failure)
                failure = std::move(e);
            stop = true;
        };
        const auto worker = [&] {
            ImageFileStream in(image, g);
            if (const Status st = in.open(); !st.ok())
                return fail(st.error());
            std::vector<char> buf(size_t(std::max<qint64>(64 * 1024, o.bufferBytes)));
            while (!stop.load()) {
                const size_t k = next.fetch_add(1);
                if (k >= files.size())
                    return;
                const ImageEntry& e = tree.entries[size_t(files[k])];
                const QString path = target(e);
                const QString shown = QDir::toNativeSeparators(path);
                QFile out(toLongPath(path));
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    return fail(fileError(QStringLiteral("Could not create %1.").arg(shown), out));
                for (qint64 off = 0; off < e.size;) {
                    if (!token.checkpoint())
                        return fail(Error::cancelled());
                    auto r = in.read(e, off, buf.data(), std::min<qint64>(qint64(buf.size()), e.size - off));
                    if (!r.ok())
                        return fail(r.takeError());
                    if (r.value() <= 0)
                        return fail(corruptImage(QStringLiteral("%1 is cut short").arg(e.path)));
                    if (const Status st = writeAll(out, buf.data(), r.value(), shown); !st.ok())
                        return fail(st.error());
                    off += r.value();
                    bytes.fetch_add(r.value());
                    if (progress)
                        progress->advance(r.value());
                }
                out.close();
                if (out.error() != QFileDevice::NoError)
                    return fail(fileError(QStringLiteral("Could not write %1.").arg(shown), out));
                finisher.finish(e, path);
                done.fetch_add(1);
                if (progress)
                    progress->advanceFiles(1);
            }
        };
        std::vector<std::thread> pool;
        const int n = std::clamp(o.threads, 1, int(std::min<size_t>(files.size(), 128)));
        for (int t = 0; t < n; ++t)
            pool.emplace_back(worker);
        for (auto& t : pool)
            t.join();
        if (token.isCancelled())
            return Error::cancelled();
        if (failure)
            return *failure;
        stats.bytes = bytes.load();
        stats.files = done.load();
    }

    // Folder times last (writing files changed them), deepest first.
    for (int i = int(tree.entries.size()) - 1; i >= 1; --i) {
        const ImageEntry& e = tree.entries[size_t(i)];
        if (e.isDir)
            finisher.finish(e, target(e));
    }
    if (finisher.failed())
        stats.warnings << QStringLiteral("The timestamps or attributes of %1 items could not be restored.")
                              .arg(formatCount(finisher.failed()));
    stats.elapsedMs = timer.elapsed();
    return stats;
}

} // namespace fw::exfat
