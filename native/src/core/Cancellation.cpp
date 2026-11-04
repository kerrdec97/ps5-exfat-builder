#include "core/Cancellation.h"

#include <chrono>
#include <vector>

namespace fw {

CancelRegistration& CancelRegistration::operator=(CancelRegistration&& o) noexcept
{
    if (this != &o) {
        reset();
        m_state = std::move(o.m_state);
        m_id = o.m_id;
        o.m_id = 0;
    }
    return *this;
}

void CancelRegistration::reset()
{
    if (m_id == 0)
        return;
    if (auto s = m_state.lock()) {
        {
            std::lock_guard lock(s->mutex);
            s->callbacks.erase(m_id);
        }
        // If cancel() is running callbacks on another thread, wait for it so
        // the callback cannot outlive the objects it captured.
        if (s->runningThread.load() != std::this_thread::get_id()) {
            std::lock_guard runLock(s->runMutex);
        }
    }
    m_id = 0;
    m_state.reset();
}

bool CancellationToken::checkpoint() const
{
    if (isCancelled())
        return false;
    if (!isPaused())
        return true;
    std::unique_lock lock(m_state->mutex);
    m_state->cv.wait(lock, [this] { return m_state->cancelled.load() || !m_state->paused.load(); });
    return !m_state->cancelled.load();
}

bool CancellationToken::sleepFor(int ms) const
{
    std::unique_lock lock(m_state->mutex);
    m_state->cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_state->cancelled.load(); });
    return !m_state->cancelled.load();
}

CancelRegistration CancellationToken::onCancel(std::function<void()> fn) const
{
    {
        std::lock_guard lock(m_state->mutex);
        if (!m_state->cancelled.load()) {
            const auto id = m_state->nextId++;
            m_state->callbacks.emplace(id, std::move(fn));
            return CancelRegistration(m_state, id);
        }
    }
    fn();
    return {};
}

QString CancellationToken::reason() const
{
    std::lock_guard lock(m_state->mutex);
    return m_state->reason;
}

void CancellationSource::cancel(const QString& reason)
{
    std::vector<std::function<void()>> toRun;
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->cancelled.exchange(true))
            return;
        m_state->reason = reason;
        for (auto& [id, fn] : m_state->callbacks)
            toRun.push_back(std::move(fn));
        m_state->callbacks.clear();
    }
    m_state->cv.notify_all();
    std::lock_guard runLock(m_state->runMutex);
    m_state->runningThread.store(std::this_thread::get_id());
    for (auto& fn : toRun)
        if (fn)
            fn();
    m_state->runningThread.store(std::thread::id{});
}

void CancellationSource::pause()
{
    m_state->paused.store(true);
}

void CancellationSource::resume()
{
    {
        std::lock_guard lock(m_state->mutex);
        m_state->paused.store(false);
    }
    m_state->cv.notify_all();
}

} // namespace fw
