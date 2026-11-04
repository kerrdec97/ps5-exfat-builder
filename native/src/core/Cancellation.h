#pragma once

// Cooperative cancellation and pause.
//
// A CancellationSource is owned by whoever may cancel (the job queue); engines
// receive a CancellationToken by value and call `checkpoint()` in their loops.
// `checkpoint()` blocks while the job is paused and returns false once the job
// is cancelled. Callbacks registered with `onCancel()` run exactly once, on the
// cancelling thread — used to kill child process trees and abort sockets.

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace fw {

class CancellationToken;

namespace detail {
struct CancelState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> paused{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::uint64_t, std::function<void()>> callbacks;
    std::uint64_t nextId = 1;
    QString reason;
};
} // namespace detail

// RAII handle for a registered cancel callback; unregisters on destruction.
class CancelRegistration {
public:
    CancelRegistration() = default;
    CancelRegistration(std::weak_ptr<detail::CancelState> s, std::uint64_t id) : m_state(std::move(s)), m_id(id) {}
    CancelRegistration(CancelRegistration&& o) noexcept { *this = std::move(o); }
    CancelRegistration& operator=(CancelRegistration&& o) noexcept;
    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;
    ~CancelRegistration() { reset(); }
    void reset();

private:
    std::weak_ptr<detail::CancelState> m_state;
    std::uint64_t m_id = 0;
};

class CancellationToken {
public:
    // A token that can never be cancelled (tests, one-off calls).
    CancellationToken() : m_state(std::make_shared<detail::CancelState>()) {}

    bool isCancelled() const { return m_state->cancelled.load(std::memory_order_acquire); }
    bool isPaused() const { return m_state->paused.load(std::memory_order_acquire); }

    // Blocks while paused. Returns false if cancelled (before or during the wait).
    bool checkpoint() const;

    // Sleeps up to `ms`, waking early on cancel. Returns false if cancelled.
    bool sleepFor(int ms) const;

    // Registers `fn` to run once when cancellation is requested. If the token is
    // already cancelled, `fn` runs immediately on the calling thread.
    [[nodiscard]] CancelRegistration onCancel(std::function<void()> fn) const;

    QString reason() const;

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<detail::CancelState> s) : m_state(std::move(s)) {}
    std::shared_ptr<detail::CancelState> m_state;
};

class CancellationSource {
public:
    CancellationSource() : m_state(std::make_shared<detail::CancelState>()) {}

    CancellationToken token() const { return CancellationToken(m_state); }
    void cancel(const QString& reason = {});
    void pause();
    void resume();
    bool isCancelled() const { return m_state->cancelled.load(); }
    bool isPaused() const { return m_state->paused.load(); }

private:
    std::shared_ptr<detail::CancelState> m_state;
};

} // namespace fw
