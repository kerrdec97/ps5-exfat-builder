#pragma once

// Kernel log (klogsrv, TCP 3232): a stream of text lines, nothing is sent.
//
// Legacy framing: UTF-8 (invalid bytes replaced), split on '\n', '\r'
// stripped, empty lines dropped. Legacy tags: error if the line contains any
// of error/fatal/panic/assert/crash, warning for warn/caution, debug for
// debug/trace/verbose, else info (case-insensitive substrings).
//
//  * KlogClient   — background reader with automatic reconnect; lines are
//                   delivered through a callback on the reader's own thread.
//  * KlogSession  — synchronous reader for "wait for lines matching X within
//                   T seconds" (ShadowMount+ "Safe to unplug").

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QTcpSocket;
class QThread;

namespace fw::ps5 {

enum class KlogTag { Info, Warning, Error, Debug };
KlogTag klogTag(const QString& line);
QString klogTagName(KlogTag tag); // "info", "warning", "error", "debug"

struct KlogLine {
    QDateTime received;
    QString text;
    KlogTag tag = KlogTag::Info;
};

// Splits a byte stream into klog lines (UTF-8 is decoded per complete line,
// so a character split across two reads survives). A line longer than
// kMaxLineBytes (with or without its '\n' yet) is cut there, at a character
// boundary, and ends in "…"; the rest of it up to the next '\n' is dropped.
class KlogLineSplitter {
public:
    static constexpr qsizetype kMaxLineBytes = 16 * 1024;
    QStringList feed(const QByteArray& bytes);
    QString flush(); // the unterminated rest, if any

private:
    QByteArray m_pending;
    bool m_skipping = false; // inside the dropped rest of a cut line
};

enum class KlogState { Connecting, Connected, Disconnected, Reconnecting, Stopped };
QString klogStateName(KlogState s);

class KlogClient {
public:
    struct Options {
        QString host;
        quint16 port = 3232;
        int connectTimeoutMs = 10000;
        bool reconnect = true;
        int reconnectDelayMs = 2000;
    };
    using LineCallback = std::function<void(const KlogLine&)>;
    // Lines in arrival order, handed over at most every kBatchMs (and when
    // the connection ends), so a flood of lines costs one call per interval.
    using BatchCallback = std::function<void(const std::vector<KlogLine>&)>;
    using StateCallback = std::function<void(KlogState, const QString& detail)>;
    static constexpr int kBatchMs = 100;

    // Callbacks run on the client's reader thread; marshal to the UI yourself.
    // `onLine` is called once per line of every batch.
    KlogClient(Options options, LineCallback onLine, StateCallback onState = {});
    static std::unique_ptr<KlogClient> withBatches(Options options, BatchCallback onBatch, StateCallback onState = {});
    ~KlogClient(); // stop()
    KlogClient(const KlogClient&) = delete;
    KlogClient& operator=(const KlogClient&) = delete;

    void start();
    void stop(); // returns once the reader thread has ended
    bool isRunning() const;
    qint64 linesReceived() const { return m_lines.load(); }
    int connections() const { return m_connections.load(); }

private:
    void run();

    KlogClient(Options options, BatchCallback onBatch, StateCallback onState, int);

    Options m_opt;
    BatchCallback m_onBatch;
    StateCallback m_onState;
    std::unique_ptr<CancellationSource> m_stop;
    QThread* m_thread = nullptr;
    std::atomic<qint64> m_lines{0};
    std::atomic<int> m_connections{0};
};

class KlogSession {
public:
    enum class Outcome { Matched, Timeout, ConnectionLost, Cancelled };
    struct WaitResult {
        Outcome outcome = Outcome::Timeout;
        QStringList lines;  // every line seen while waiting
        QString matchedLine;
    };

    // Connects now (so no line is missed while the caller triggers something).
    static Result<std::unique_ptr<KlogSession>> open(const QString& host, quint16 port, int connectTimeoutMs,
                                                     const CancellationToken& token = {});
    ~KlogSession();

    // Reads lines until `matcher` returns true for one, `timeoutMs` passes, the
    // stream closes or `token` is cancelled. `matcher` may keep state.
    WaitResult waitFor(const std::function<bool(const QString& line)>& matcher, int timeoutMs,
                       const CancellationToken& token = {});

    struct ReadResult {
        QStringList lines;
        bool closed = false;    // the stream ended (nothing more will come)
        bool cancelled = false;
    };
    // Waits at most `waitMs` for data and returns the complete lines received
    // (possibly none), for callers that must also act when nothing arrives.
    ReadResult read(int waitMs, const CancellationToken& token = {});

private:
    KlogSession() = default;
    std::unique_ptr<QTcpSocket> m_socket;
    KlogLineSplitter m_split;
};

// open() + waitFor() in one call.
KlogSession::WaitResult waitForKlogLine(const QString& host, quint16 port,
                                        const std::function<bool(const QString&)>& matcher, int timeoutMs,
                                        int connectTimeoutMs, const CancellationToken& token, Error* connectError = nullptr);

} // namespace fw::ps5
