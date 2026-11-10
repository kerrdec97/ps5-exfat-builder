// PFSC block codec. Compression parameters match Lazy_MkPFS
// lazy_mkpfs/compression.py (zlib.compress per 64 KiB block). GPL-3.0-or-later.

#include "engines/pfs/PfscCodec.h"

#include "engines/pfs/PfsFormat.h"

#include <zlib.h>

#include <cstring>
#include <system_error>

namespace fw::pfs {

struct CompressPool::Deflater {
    z_stream zs{};
    bool ok = false;
    explicit Deflater(int level)
    {
        std::memset(&zs, 0, sizeof(zs));
        // Same defaults as Python's zlib.compress: wbits 15, memLevel 8, default strategy.
        ok = deflateInit2(&zs, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    }
    ~Deflater()
    {
        if (ok)
            deflateEnd(&zs);
    }
};

CompressPool::CompressPool(int threads, int level) : m_threads(std::max(1, threads)), m_level(level)
{
    {
        Deflater probe(level);
        m_bound = probe.ok ? int(deflateBound(&probe.zs, uLong(kPfscBlock))) : int(compressBound(uLong(kPfscBlock)));
    }
    if (m_threads <= 1) {
        m_inline = std::make_unique<Deflater>(level);
        return;
    }
    m_workers.reserve(size_t(m_threads));
    try {
        for (int i = 0; i < m_threads; ++i)
            m_workers.emplace_back([this] { workerMain(); });
    } catch (const std::system_error&) {
        // The system would not start another thread: go on with the ones
        // that run (or compress on the calling thread when none does).
    }
    m_threads = int(m_workers.size());
    if (m_workers.empty()) {
        m_threads = 1;
        m_inline = std::make_unique<Deflater>(level);
    }
}

CompressPool::~CompressPool()
{
    {
        // Queued work is dropped (cancel / error): workers only finish the task in hand.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_work.notify_all();
    for (auto& t : m_workers)
        t.join();
}

std::shared_ptr<CompressPool::Task> CompressPool::makeTask(int nblocks) const
{
    auto t = std::make_shared<Task>();
    t->nblocks = nblocks;
    t->raw = QByteArray(qsizetype(nblocks) * kPfscBlock, Qt::Uninitialized); // the caller fills it and zero-pads the tail
    t->out.resize(qsizetype(nblocks) * m_bound);
    t->outLen.assign(size_t(nblocks), 0);
    return t;
}

void CompressPool::run(Task& task, Deflater& d)
{
    if (!d.ok) {
        task.error = QStringLiteral("zlib could not be initialised");
        return;
    }
    for (int i = 0; i < task.nblocks; ++i) {
        deflateReset(&d.zs);
        d.zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(task.block(i)));
        d.zs.avail_in = uInt(kPfscBlock);
        d.zs.next_out = reinterpret_cast<Bytef*>(task.out.data() + qint64(i) * m_bound);
        d.zs.avail_out = uInt(m_bound);
        const int rc = deflate(&d.zs, Z_FINISH);
        if (rc != Z_STREAM_END) {
            task.error = QStringLiteral("zlib deflate failed (%1)").arg(rc);
            return;
        }
        task.outLen[size_t(i)] = m_bound - int(d.zs.avail_out);
    }
}

void CompressPool::workerMain()
{
    Deflater d(m_level);
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_work.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty())
                return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        run(*task, d);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            task->done = true;
        }
        m_doneCv.notify_all();
    }
}

void CompressPool::submit(const std::shared_ptr<Task>& task)
{
    if (m_inline) {
        run(*task, *m_inline);
        task->done = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(task);
    }
    m_work.notify_one();
}

void CompressPool::wait(const std::shared_ptr<Task>& task)
{
    if (m_inline)
        return;
    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait(lock, [&] { return task->done; });
}

// ---- inflater -------------------------------------------------------------------------

struct Inflater::Impl {
    z_stream zs{};
    bool ok = false;
};

Inflater::Inflater() : d(std::make_unique<Impl>())
{
    std::memset(&d->zs, 0, sizeof(d->zs));
    d->ok = inflateInit(&d->zs) == Z_OK;
}

Inflater::~Inflater()
{
    if (d->ok)
        inflateEnd(&d->zs);
}

QString Inflater::inflateBlock(const char* in, qint64 len, char* out)
{
    if (!d->ok)
        return QStringLiteral("zlib could not be initialised");
    inflateReset(&d->zs);
    d->zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in));
    d->zs.avail_in = uInt(len);
    d->zs.next_out = reinterpret_cast<Bytef*>(out);
    d->zs.avail_out = uInt(kPfscBlock);
    const int rc = inflate(&d->zs, Z_FINISH);
    if (rc == Z_STREAM_END) {
        if (d->zs.total_out != uLong(kPfscBlock))
            return QStringLiteral("block decompressed to %1 bytes instead of 65536").arg(d->zs.total_out);
        return {};
    }
    if (rc == Z_DATA_ERROR)
        return QStringLiteral("compressed data is damaged (%1)")
            .arg(QString::fromLatin1(d->zs.msg ? d->zs.msg : "invalid data"));
    if (d->zs.avail_out == 0)
        return QStringLiteral("block decompresses to more than 65536 bytes");
    return QStringLiteral("compressed block is truncated");
}

QString zlibVersionString()
{
    return QString::fromLatin1(zlibVersion());
}

} // namespace fw::pfs
