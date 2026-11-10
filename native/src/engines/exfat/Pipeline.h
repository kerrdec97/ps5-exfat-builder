#pragma once

// Overlapped streaming between one producer thread and one consumer: a fixed
// pool of large buffers circulates between a "free" queue and a "filled"
// queue, so memory stays at bufferCount x bufferBytes however big the data is.

#include "core/Result.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace fw::exfat {

template <typename T>
class BoundedQueue {
public:
    // Blocks while full. Returns false once the queue is closed.
    bool push(T v)
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
        if (m_closed)
            return false;
        m_items.push_back(std::move(v));
        m_cv.notify_all();
        return true;
    }
    // Blocks while empty. Returns nullopt once closed and drained.
    std::optional<T> pop()
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_closed || !m_items.empty(); });
        if (m_items.empty())
            return std::nullopt;
        T v = std::move(m_items.front());
        m_items.pop_front();
        m_cv.notify_all();
        return v;
    }
    void close()
    {
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }
    void setCapacity(size_t c) { m_capacity = c; }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_items;
    size_t m_capacity = 8;
    bool m_closed = false;
};

struct Chunk {
    std::vector<char>* buffer = nullptr; // null for an empty item
    qint64 length = 0;
    int item = 0;           // index into the caller's work list
    bool endOfItem = false; // last chunk of this item
};

// Owns the buffers and the two queues. The producer takes a free buffer,
// fills it and posts a Chunk; the consumer returns the buffer when done.
class ChunkPipeline {
public:
    ChunkPipeline(int bufferCount, qint64 bufferBytes)
    {
        m_buffers.resize(size_t(std::max(2, bufferCount)));
        // Before the first push: a queue that is full blocks its producer.
        m_free.setCapacity(m_buffers.size());
        m_filled.setCapacity(m_buffers.size());
        for (auto& b : m_buffers) {
            b.resize(size_t(bufferBytes));
            m_free.push(&b);
        }
    }
    ~ChunkPipeline() { abort(); }

    std::vector<char>* takeFree() { auto b = m_free.pop(); return b ? *b : nullptr; }
    void giveBack(std::vector<char>* b) { if (b) m_free.push(b); }
    bool post(const Chunk& c) { return m_filled.push(c); }
    std::optional<Chunk> next() { return m_filled.pop(); }

    // Producer finished (normally or with an error): the consumer drains
    // what is queued and then sees nullopt.
    void finishProducing() { m_filled.close(); }
    // Stop both sides now.
    void abort()
    {
        m_filled.close();
        m_free.close();
    }

    void setProducerError(Error e)
    {
        std::lock_guard lock(m_errMutex);
        if (!m_error)
            m_error = std::move(e);
    }
    std::optional<Error> producerError() const
    {
        std::lock_guard lock(m_errMutex);
        return m_error;
    }

private:
    std::vector<std::vector<char>> m_buffers;
    BoundedQueue<std::vector<char>*> m_free;
    BoundedQueue<Chunk> m_filled;
    mutable std::mutex m_errMutex;
    std::optional<Error> m_error;
};

// Joins a thread on scope exit (after the pipeline was aborted or drained).
class ScopedThread {
public:
    explicit ScopedThread(std::thread t) : m_t(std::move(t)) {}
    ~ScopedThread() { join(); }
    void join() { if (m_t.joinable()) m_t.join(); }
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;

private:
    std::thread m_t;
};

// True when every byte is zero (used to leave holes instead of writing zeros).
inline bool isAllZero(const char* p, qint64 n)
{
    qint64 i = 0;
    for (; i + 8 <= n; i += 8) {
        quint64 v;
        std::memcpy(&v, p + i, 8);
        if (v)
            return false;
    }
    for (; i < n; ++i)
        if (p[i])
            return false;
    return true;
}

} // namespace fw::exfat
