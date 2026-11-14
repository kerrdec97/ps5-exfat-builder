#pragma once

// Cooperative cancellation and pause.
//
// A CancellationSource is owned by whoever may cancel (the job queue); engines
// receive a CancellationToken by value and call `checkpoint()` in their loops.
// `checkpoint()` blocks while the job is paused and returns false once the job
// is cancelled. Callbacks registered with `onCancel()` run exactly once, on the
// cancelling thread — used to kill child process trees and abort sockets.
//
// A cancel while finished work is verified keeps that work (GitHub #97); the
// job asks keepFinishedWork() and runs its last short checks with an
// untilShutdown() token. Closing the app (CancellationSource::shutdown())
// keeps nothing unfinished and stops those checks too.

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fw {

class CancellationToken;

namespace detail {
struct CancelState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> shuttingDown{false}; // CancellationSource::shutdown()
    std::atomic<bool> workKept{false};     // keepFinishedWork() said yes
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::uint64_t, std::function<void()>> callbacks;
    std::uint64_t nextId = 1;
    QString reason;
    // The tokens untilShutdown() made, which shutdown() cancels.
    std::vector<std::weak_ptr<CancelState>> untilShutdown;
    // Held while cancel callbacks run, so unregistering waits for a callback
    // that is executing on another thread (its captures must stay valid).
    std::mutex runMutex;
    std::atomic<std::thread::id> runningThread{};
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

    // Cancelled because the app is closing (JobQueue::shutdown, reason
    // shutdownReason()), not by a Cancel. Work still being verified is then
    // discarded, as closing says it is, instead of kept unverified as a Cancel
    // during verification keeps it (GitHub #97): the queue cannot record what
    // a job does once the app closes, and restores it as interrupted.
    bool isShutdown() const;
    static QString shutdownReason();
    // Asked by a job whose verification ended because of the cancel: is the
    // work it had finished kept, unverified, as with verification turned off
    // (GitHub #97)? False when the token is not cancelled or the app is
    // closing (CancellationSource::shutdown()): nothing unfinished is kept
    // then. A yes is remembered (finishedWorkKept()): an error the job returns
    // after it, such as finished work that could not take its name, is the
    // job's own and not the cancel's, and the queue reports it as it is.
    bool keepFinishedWork() const;
    bool finishedWorkKept() const { return m_state->workKept.load(std::memory_order_acquire); }
    // True once the source's shutdown() was called.
    bool isShuttingDown() const { return m_state->shuttingDown.load(std::memory_order_acquire); }
    // A token for the short checks a job still runs on work it keeps after a
    // cancel: the cancel this token already has does not stop them, the app
    // closing (shutdown()) does. Cancelled from the start once it has.
    CancellationToken untilShutdown() const;

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
    // The app is closing: cancels with the reason "shutdown" and, unlike a
    // second cancel(), also reaches a job that is already cancelled. Its
    // untilShutdown() tokens are cancelled, and keepFinishedWork() says no
    // from now on.
    void shutdown();
    void pause();
    void resume();
    bool isCancelled() const { return m_state->cancelled.load(); }
    bool isPaused() const { return m_state->paused.load(); }

private:
    explicit CancellationSource(std::shared_ptr<detail::CancelState> s) : m_state(std::move(s)) {}
    std::shared_ptr<detail::CancelState> m_state;
};

} // namespace fw
