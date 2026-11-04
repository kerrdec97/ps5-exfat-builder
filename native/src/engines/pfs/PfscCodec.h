#pragma once

// PFSC block codec: zlib-compresses 64 KiB logical blocks on a bounded pool of
// worker threads and inflates stored blocks.
//
// Every block is compressed on its own (zlib format, window 15, memLevel 8,
// default strategy, Z_FINISH in one call) exactly like Python's
// zlib.compress(block, level) in Lazy_MkPFS compression.py, so the output does
// not depend on the number of threads or on which thread handled a block.
// GPL-3.0-or-later (port of Lazy_MkPFS).

#include <QByteArray>
#include <QString>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fw::pfs {

// Blocks per pool task (1 MiB of input).
inline constexpr int kBlocksPerTask = 16;

class CompressPool {
public:
    struct Task {
        QByteArray raw;      // nblocks * 64 KiB; the tail of the last block is zero-filled
        qint64 rawBytes = 0; // real input bytes in `raw`
        int nblocks = 0;
        QByteArray out;      // compressed blocks, block i at i * bound()
        std::vector<int> outLen;
        QString error;       // non-empty if zlib failed
        bool done = false;   // guarded by the pool mutex

        const char* block(int i) const { return raw.constData() + qint64(i) * 0x10000; }
    };

    // threads <= 1 compresses inline in submit().
    CompressPool(int threads, int level);
    ~CompressPool();
    CompressPool(const CompressPool&) = delete;
    CompressPool& operator=(const CompressPool&) = delete;

    int threads() const { return m_threads; }
    int level() const { return m_level; }
    int bound() const { return m_bound; }
    const char* compressed(const Task& t, int i) const { return t.out.constData() + qint64(i) * m_bound; }

    // Allocates a task able to hold `nblocks` blocks.
    std::shared_ptr<Task> makeTask(int nblocks) const;
    void submit(const std::shared_ptr<Task>& task);
    void wait(const std::shared_ptr<Task>& task);

private:
    struct Deflater;
    void workerMain();
    void run(Task& task, Deflater& d);

    int m_threads = 1;
    int m_level = 6;
    int m_bound = 0;
    std::vector<std::thread> m_workers;
    std::unique_ptr<Deflater> m_inline;
    std::mutex m_mutex;
    std::condition_variable m_work;
    std::condition_variable m_doneCv;
    std::deque<std::shared_ptr<Task>> m_queue;
    bool m_stop = false;
};

// Reusable inflater for stored PFSC blocks.
class Inflater {
public:
    Inflater();
    ~Inflater();
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    // Decodes one stored block into exactly 64 KiB at `out`. Returns an empty
    // string on success, otherwise what is wrong.
    QString inflateBlock(const char* in, qint64 len, char* out);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Version of the zlib library the engine is linked against.
QString zlibVersionString();

} // namespace fw::pfs
