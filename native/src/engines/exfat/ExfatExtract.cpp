#include "engines/exfat/ExfatExtract.h"

#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "engines/exfat/Pipeline.h"
#include "engines/exfat/PlatformFs.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <exception>
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
    // The same for a file still open for writing, through its handle: one
    // call instead of opening the file again for its times and once more for
    // its attributes. False when that could not be done; the caller then
    // closes the file and calls finish().
    bool finishOpen(const ImageEntry& e, QFile& file)
    {
        return finishOpenFile(file, m_o.preserveTimes, e.created, e.modified, e.accessed, m_o.preserveAttributes,
                              e.attributes);
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

    // Where each entry goes. Parents come before their children (the tree is
    // read breadth first); names are mapped to what this drive can store.
    const size_t count = tree.entries.size();
    std::vector<QString> hostRel(count), targets(count);
    std::vector<QSet<QString>> taken(count);
    for (size_t i = 1; i < count; ++i) {
        const ImageEntry& e = tree.entries[i];
        if (e.parent < 0 || size_t(e.parent) >= i || !tree.entries[size_t(e.parent)].isDir)
            return corruptImage(QStringLiteral("the folder of %1 is not in the tree").arg(e.path));
        QString local = hostEntryName(e.name);
        QSet<QString>& used = taken[size_t(e.parent)];
        if (used.contains(hostNameKey(local))) {
            const QString stem = local;
            for (int k = 2; used.contains(hostNameKey(local)); ++k)
                local = QStringLiteral("%1 (%2)").arg(stem).arg(k);
        }
        used.insert(hostNameKey(local));
        const QString& parentRel = hostRel[size_t(e.parent)];
        hostRel[i] = parentRel.isEmpty() ? local : parentRel + u'/' + local;
        if (local != e.name)
            stats.renamed << QStringLiteral("%1 → %2").arg(e.path, local);
        if (hostRel[i] != e.path)
            stats.imagePath.insert(hostRel[i], e.path);
        FW_TRY_ASSIGN(targets[i], childPathInside(destDir, hostRel[i]));
    }
    taken.clear();
    const auto target = [&](int i) -> const QString& { return targets[size_t(i)]; };

    std::vector<int> files;
    for (int i = 1; i < int(tree.entries.size()); ++i) {
        const ImageEntry& e = tree.entries[size_t(i)];
        if (e.isDir) {
            if (!QDir().mkpath(toLongPath(target(i))))
                return Error(ErrorCode::IoError,
                             QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(target(i))));
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
            auto stream = reader.openStream();
            ImageFileStream& in = *stream;
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
            const QString path = target(files[size_t(c->item)]);
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
                const bool finished = finisher.finishOpen(e, out);
                out.close();
                if (out.error() != QFileDevice::NoError)
                    return fileError(QStringLiteral("Could not write %1.").arg(shown), out);
                if (!finished)
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
            auto stream = reader.openStream();
            ImageFileStream& in = *stream;
            if (const Status st = in.open(); !st.ok())
                return fail(st.error());
            std::vector<char> buf(size_t(std::max<qint64>(64 * 1024, o.bufferBytes)));
            while (!stop.load()) {
                const size_t k = next.fetch_add(1);
                if (k >= files.size())
                    return;
                const ImageEntry& e = tree.entries[size_t(files[k])];
                const QString path = target(files[k]);
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
                const bool finished = finisher.finishOpen(e, out);
                out.close();
                if (out.error() != QFileDevice::NoError)
                    return fail(fileError(QStringLiteral("Could not write %1.").arg(shown), out));
                if (!finished)
                    finisher.finish(e, path);
                done.fetch_add(1);
                if (progress)
                    progress->advanceFiles(1);
            }
        };
        // An exception that leaves a thread ends the whole app. A buffer that
        // cannot be allocated (many threads, large I/O buffers) becomes this
        // job's error instead.
        const auto guarded = [&] {
            try {
                worker();
            } catch (const std::exception& e) {
                fail(Error(ErrorCode::Internal,
                           QStringLiteral("Not enough memory to extract the files. Lower the I/O buffer size or the number "
                                          "of files extracted at once in Settings, then try again."),
                           QString::fromLocal8Bit(e.what())));
            }
        };
        std::vector<std::thread> pool;
        const int n = std::clamp(o.threads, 1, int(std::min<size_t>(files.size(), 128)));
        try {
            for (int t = 0; t < n; ++t)
                pool.emplace_back(guarded);
        } catch (const std::exception& e) {
            // No more threads: the ones that started do the work.
            if (pool.empty())
                fail(Error(ErrorCode::Internal, QStringLiteral("Could not start a worker thread."),
                           QString::fromLocal8Bit(e.what())));
        }
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
            finisher.finish(e, target(i));
    }
    if (finisher.failed())
        stats.warnings << QStringLiteral("The timestamps or attributes of %1 could not be restored.")
                              .arg(counted(finisher.failed(), "item", "items"));
    stats.elapsedMs = timer.elapsed();
    return stats;
}

} // namespace fw::exfat
