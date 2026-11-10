#pragma once

// In-process stand-ins for the console's network services, for the ps5 tests:
//
//  * FakeFtpServer  — the FTP subset the client uses, serving a local folder;
//                     LIST in three styles, MLSD, PASV/EPSV/PORT/EPRT, REST,
//                     throttling, a one-time drop and a stall for fault tests.
//  * FakeTcpSink    — accepts connections and records every byte (payloads).
//  * FakeLineServer — sends scripted text per connection (klogsrv).
//  * FakeHttpUpload — records one HTTP request and answers it (DPI).
//  * FakeHttpApi    — a JSON-over-POST API with a handler (ShadowMount+ 1.7).
//
// FakeFtpServer::Options::ftpsrv behaves like ps5-payload-dev ftpsrv v0.21:
// the three-line greeting, USER answered 230 at once, FEAT listing only MLSD
// KILL MTRW SELF, 502 for PASS, NLST, EPSV, MDTM and SITE AVAIL, and the
// per-connection SELF decryption mode (on at login): SIZE and RETR of a file
// that starts with SELF magic give a "decrypted" copy (ELF magic, 16 bytes
// shorter).
//
// The FTP server serves the folder Options::root and nothing else: a path
// that would leave it is answered 550 (toLocalPath).
//
// Every server runs on its own thread with an event loop, so the code under
// test can block the test thread the way a queue worker would. Header-only
// and without Q_OBJECT (no moc needed): signals are connected to lambdas.

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace fake {

// A thread with an event loop and a context object living in it.
class ServerThread {
public:
    ServerThread()
    {
        m_thread.start();
        m_ctx = new QObject;
        m_ctx->moveToThread(&m_thread);
        // Deleted by its own thread, when that thread ends.
        QObject::connect(&m_thread, &QThread::finished, m_ctx, &QObject::deleteLater);
    }
    ~ServerThread()
    {
        m_thread.quit();
        m_thread.wait();
    }
    template <typename F>
    void run(F f)
    {
        QMetaObject::invokeMethod(m_ctx, std::move(f), Qt::BlockingQueuedConnection);
    }
    QObject* ctx() const { return m_ctx; }

    // Deletes every object created under ctx() (on the server thread), after
    // cutting all their signal connections so no callback runs half-way
    // through the teardown.
    void destroyChildren()
    {
        run([this] {
            for (QObject* o : m_ctx->findChildren<QObject*>())
                o->disconnect();
            const QObjectList kids = m_ctx->children();
            qDeleteAll(kids);
        });
    }

private:
    QThread m_thread;
    QObject* m_ctx = nullptr;
};

// The local file a console path stands for. Empty when there is no root or
// the path would leave it (".." above the top, or a backslash where the host
// takes it for a separator): the server answers 550 for such a path and never
// looks at, writes or deletes anything outside its folder.
inline QString toLocalPath(const QString& root, const QString& cwd, const QString& arg)
{
    if (root.isEmpty())
        return {};
    QString p = arg.trimmed();
    if (!p.startsWith(u'/'))
        p = cwd + u'/' + p;
    p = QDir::cleanPath(p);
    if (!p.startsWith(u'/'))
        p.prepend(u'/');
    const QString base = QDir::cleanPath(root);
    const QString local = QDir::cleanPath(base + p);
    if (local != base && !local.startsWith(base + u'/'))
        return {};
    return local;
}

// ---------------------------------------------------------------------------------------
class FakeFtpServer {
public:
    enum class ListStyle { Unix, UnixNoGroup, Dos };
    struct Options {
        QString root;
        ListStyle listStyle = ListStyle::Unix;
        bool mlsd = false;
        bool requireLogin = false;
        QString user = QStringLiteral("ps5");
        QString pass = QStringLiteral("secret");
        bool refusePasv = false;
        bool supportRest = true;
        bool silentGreeting = false;     // accept but never greet (reply timeout tests)
        qint64 throttleBytesPerSec = 0;  // 0 = unlimited
        qint64 dropAfterBytes = -1;      // abort the first data transfer after N bytes
        qint64 stallAfterBytes = -1;     // stop moving data after N bytes (no close)
        QStringList readOnlyPaths;       // STOR/APPE/DELE refused (550) for these virtual paths
        bool renameRefused = false;      // RNFR answers 502
        QString dropOnlyPath;            // dropAfterBytes applies to this virtual path only
        int goneAfterDrop = 0;           // with the drop the control connection closes too and the next N
                                         // connections are closed ungreeted (-1: all), like a console in rest mode
        int dropControlOnPasv = 0;       // close the control connection on the first N PASV commands
        QByteArray failFirstTransferReply; // answer the first RETR/STOR/APPE with this (e.g. "425 ...")
        QString shortRetrPath;           // RETR of this path sends half the file, then 226
        QString corruptStorPath;         // after a STOR to this path completes, keep only half (once)
        QStringList extraListNames;      // fake file entries added to every LIST/MLSD (unsafe-name tests)
        bool refuseDeleteWhileOpen = false; // DELE of a file still being written answers 550 (like Windows)
        QByteArray greeting = "220 FakePS5 FTP ready"; // first line sent to every connection
        QByteArray extraFeatures;        // added to the FEAT reply, e.g. " XCRYPT\r\n CPFR\r\n"
        bool refuseEpsv = false;         // EPSV answers 502 (with refusePasv: no passive mode at all)
        QStringList refuseDeletePaths;   // DELE refused (550) for these virtual paths; STOR still allowed
        QByteArray failFirstPasvReply;   // answer the first PASV with this (e.g. "421 Too many connections")
        int lateListReplyMs = 0;         // the first listing's closing 226 comes this late (stale-reply tests)
        int dropControlOnRnfr = 0;       // close the control connection on the Nth RNFR (1-based; 0: never)
        bool ftpsrv = false;             // behave like ftpsrv v0.21 (see the header)
        bool selfIgnored = false;        // ftpsrv: SELF answers "enabled" every time (cannot be turned off)
        QStringList unreadablePaths;     // RETR refused (550) for these virtual paths; SIZE and listings still show them
        QStringList refuseRenameFromPaths; // RNFR refused (550) for these virtual paths; other renames work
    };

    explicit FakeFtpServer(Options o) : m_opt(std::move(o))
    {
        m_t.run([this] {
            m_server = new QTcpServer(m_t.ctx());
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                while (QTcpSocket* s = m_server->nextPendingConnection())
                    startSession(s);
            });
            m_port = m_server->serverPort();
        });
    }
    ~FakeFtpServer()
    {
        // Everything with signals or timers lives under the context object.
        m_t.destroyChildren();
        m_server = nullptr;
    }

    quint16 port() const { return m_port; }
    QStringList commands() const
    {
        std::lock_guard l(m_mutex);
        return m_commands;
    }
    bool sawCommand(const QString& prefix) const
    {
        for (const QString& c : commands())
            if (c.startsWith(prefix, Qt::CaseInsensitive))
                return true;
        return false;
    }
    int maxConcurrentTransfers() const { return m_maxActive.load(); }
    qint64 bytesReceived() const { return m_received.load(); }
    void setThrottle(qint64 bps)
    {
        m_t.run([this, bps] { m_opt.throttleBytesPerSec = bps; });
    }
    void setDropAfter(qint64 n)
    {
        m_t.run([this, n] {
            m_opt.dropAfterBytes = n;
            m_dropped = false;
        });
    }
    QString path(const QString& virt) const { return toLocalPath(m_opt.root, QStringLiteral("/"), virt); }

private:
    struct Session {
        QTcpSocket* ctrl = nullptr;
        QByteArray inbuf;
        QString cwd = QStringLiteral("/");
        QString user;
        bool loggedIn = false;
        qint64 rest = 0;
        QString renameFrom;
        QTcpServer* pasv = nullptr;
        QTcpSocket* data = nullptr;
        QHostAddress activeHost;
        quint16 activePort = 0;
        std::function<void()> pending; // transfer waiting for its data connection
        bool selfDecrypt = true;       // ftpsrv: SELF->ELF decryption mode of this connection
    };

    void log(const QString& c)
    {
        std::lock_guard l(m_mutex);
        m_commands << c;
    }
    void reply(Session* s, const QByteArray& line) { s->ctrl->write(line + "\r\n"); }
    // After a dropped transfer: the 426, or (goneAfterDrop) the console vanishes.
    void dropReply(Session* s)
    {
        if (m_opt.goneAfterDrop == 0) {
            reply(s, "426 Connection closed; transfer aborted.");
            return;
        }
        m_goneLeft = m_opt.goneAfterDrop;
        s->inbuf.clear();
        s->ctrl->abort();
    }

    void startSession(QTcpSocket* sock)
    {
        if (m_goneLeft != 0) {
            if (m_goneLeft > 0)
                --m_goneLeft;
            sock->abort();
            sock->deleteLater();
            return;
        }
        auto s = std::make_shared<Session>();
        s->ctrl = sock;
        sock->setParent(m_t.ctx());
        m_sessions.push_back(s);
        if (m_opt.ftpsrv && !m_opt.silentGreeting)
            reply(s.get(), "220-Welcome to ftpsrv.elf running on pid 77\r\n220-Version: v0.21.1 (built Aug 20 2026)\r\n220 Service is ready");
        else if (!m_opt.silentGreeting)
            reply(s.get(), m_opt.greeting);
        Session* raw = s.get();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, raw] {
            raw->inbuf += raw->ctrl->readAll();
            while (true) {
                const qsizetype nl = raw->inbuf.indexOf("\r\n");
                if (nl < 0)
                    break;
                const QString line = QString::fromUtf8(raw->inbuf.left(nl));
                raw->inbuf.remove(0, nl + 2);
                handle(raw, line);
            }
        });
    }

    QString virtualPath(Session* s, const QString& arg) const
    {
        QString p = arg.trimmed();
        if (p.isEmpty())
            return s->cwd;
        if (!p.startsWith(u'/'))
            p = s->cwd + u'/' + p;
        p = QDir::cleanPath(p);
        return p.startsWith(u'/') ? p : u'/' + p;
    }
    QString local(Session* s, const QString& arg) const { return toLocalPath(m_opt.root, s->cwd, arg); }
    bool readOnly(Session* s, const QString& arg) const { return m_opt.readOnlyPaths.contains(virtualPath(s, arg)); }

    QString listLine(const QFileInfo& fi) const
    {
        const QDateTime t = fi.lastModified().toUTC();
        static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const QString m = QString::fromLatin1(mon[t.date().month() - 1]);
        const qint64 size = fi.isDir() ? 4096 : fi.size();
        switch (m_opt.listStyle) {
        case ListStyle::Unix:
            return QStringLiteral("%1rw-r--r-- 1 root wheel %2 %3 %4 %5 %6")
                .arg(QChar(fi.isDir() ? u'd' : u'-'))
                .arg(size)
                .arg(m)
                .arg(t.date().day(), 2, 10, QLatin1Char('0'))
                .arg(t.toString(QStringLiteral("HH:mm")), fi.fileName());
        case ListStyle::UnixNoGroup:
            return QStringLiteral("%1rwxrwxrwx 1 ps5 %2 %3 %4  %5 %6")
                .arg(QChar(fi.isDir() ? u'd' : u'-'))
                .arg(size)
                .arg(m)
                .arg(t.date().day())
                .arg(t.date().year())
                .arg(fi.fileName());
        case ListStyle::Dos:
            return QStringLiteral("%1  %2  %3 %4")
                .arg(t.toString(QStringLiteral("MM-dd-yy")), t.toString(QStringLiteral("hh:mmAP")))
                .arg(fi.isDir() ? QStringLiteral("<DIR>") : QString::number(fi.size()), fi.fileName());
        }
        return {};
    }

    void openPassive(Session* s, bool extended)
    {
        // A data connection the client opened for a refused command is stale.
        if (s->data) {
            s->data->disconnect();
            s->data->abort();
            if (s->data->parent() != s->pasv)
                s->data->deleteLater();
            s->data = nullptr;
        }
        s->pending = nullptr;
        delete s->pasv;
        s->pasv = new QTcpServer(m_t.ctx());
        s->pasv->listen(QHostAddress::LocalHost, 0);
        QObject::connect(s->pasv, &QTcpServer::newConnection, s->pasv, [s] {
            if (QTcpSocket* d = s->pasv->nextPendingConnection()) {
                s->data = d;
                if (s->pending) {
                    auto p = std::move(s->pending);
                    s->pending = nullptr;
                    p();
                }
            }
        });
        const quint16 p = s->pasv->serverPort();
        if (extended)
            reply(s, QStringLiteral("229 Entering Extended Passive Mode (|||%1|)").arg(p).toUtf8());
        else
            reply(s, QStringLiteral("227 Entering Passive Mode (127,0,0,1,%1,%2)").arg(p >> 8).arg(p & 0xFF).toUtf8());
    }

    // Runs `transfer` once a data connection exists (passive: accepted; active: connected).
    void withData(Session* s, std::function<void()> transfer)
    {
        if (s->activePort) {
            auto* d = new QTcpSocket(m_t.ctx());
            s->data = d;
            const QHostAddress h = s->activeHost;
            const quint16 p = s->activePort;
            s->activePort = 0;
            QObject::connect(d, &QTcpSocket::connected, d, [transfer] { transfer(); });
            d->connectToHost(h, p);
            return;
        }
        if (s->data)
            transfer();
        else
            s->pending = std::move(transfer);
    }

    void finishData(Session* s, const QByteArray& final)
    {
        QTcpSocket* d = s->data;
        s->data = nullptr;
        m_active.fetch_sub(1);
        if (!d) {
            reply(s, final);
            return;
        }
        if (d->state() == QAbstractSocket::UnconnectedState) {
            d->deleteLater();
            reply(s, final);
            return;
        }
        QObject::connect(d, &QTcpSocket::disconnected, d, [this, s, d, final] {
            d->deleteLater();
            reply(s, final);
        });
        d->disconnectFromHost();
    }

    void beginActive()
    {
        const int now = m_active.fetch_add(1) + 1;
        int prev = m_maxActive.load();
        while (now > prev && !m_maxActive.compare_exchange_weak(prev, now)) {
        }
    }

    // Server -> client.
    bool dropsHere(const QString& vpath) const
    {
        return m_opt.dropAfterBytes >= 0 && !m_dropped && (m_opt.dropOnlyPath.isEmpty() || m_opt.dropOnlyPath == vpath);
    }

    void sendBytes(Session* s, const QByteArray& bytes, const QString& vpath)
    {
        const bool drop = dropsHere(vpath);
        reply(s, "150 Opening BINARY mode data connection");
        beginActive();
        QTcpSocket* d = s->data;
        auto pos = std::make_shared<qint64>(0);
        auto done = std::make_shared<bool>(false);
        // A throttled transfer is paced by a timer. Any other is driven by the
        // socket itself (the next chunk when the last one has left), so a
        // waiting or stalled transfer costs no processor time.
        QTimer* pace = m_opt.throttleBytesPerSec > 0 ? new QTimer(d) : nullptr;
        auto pump = [this, s, d, bytes, pos, done, pace, drop] {
            if (*done)
                return;
            if (s->data != d) { // this data connection was given up for a new one
                *done = true;
                if (pace)
                    pace->stop();
                return;
            }
            if (d->state() != QAbstractSocket::ConnectedState) { // the client closed it: nothing more to send
                *done = true;
                if (pace)
                    pace->stop();
                finishData(s, "226 Transfer complete");
                return;
            }
            if (d->bytesToWrite() > 256 * 1024)
                return;
            if (m_opt.stallAfterBytes >= 0 && *pos >= m_opt.stallAfterBytes) {
                if (pace)
                    pace->stop();
                return; // stall forever
            }
            qint64 n = m_opt.throttleBytesPerSec > 0 ? std::max<qint64>(1, m_opt.throttleBytesPerSec / 50) : 256 * 1024;
            if (drop && !m_dropped)
                n = std::min(n, std::max<qint64>(0, m_opt.dropAfterBytes - *pos));
            if (m_opt.stallAfterBytes >= 0)
                n = std::min(n, m_opt.stallAfterBytes - *pos);
            n = std::min(n, bytes.size() - *pos);
            if (n > 0) {
                d->write(bytes.constData() + *pos, n);
                *pos += n;
            }
            if (drop && !m_dropped && *pos >= m_opt.dropAfterBytes && *pos < bytes.size()) {
                m_dropped = true;
                *done = true;
                if (pace)
                    pace->stop();
                // Close after the bytes already written (FIN, not RST): on
                // Windows an RST discards data the client has not read yet.
                // With nothing left to write the close is immediate, so the
                // reply is tied to the signal alone (never sent twice).
                s->data = nullptr;
                m_active.fetch_sub(1);
                QObject::connect(d, &QTcpSocket::disconnected, d, [this, s, d] {
                    d->deleteLater();
                    dropReply(s);
                });
                d->disconnectFromHost();
                return;
            }
            if (*pos >= bytes.size()) {
                *done = true;
                if (pace)
                    pace->stop();
                finishData(s, "226 Transfer complete");
            }
        };
        QObject::connect(d, &QTcpSocket::disconnected, d, pump);
        if (pace) {
            pace->setInterval(20);
            QObject::connect(pace, &QTimer::timeout, d, pump);
            pace->start();
        } else {
            QObject::connect(d, &QTcpSocket::bytesWritten, d, pump);
        }
        pump();
    }

    // Client -> server, appended to `file` (opened by the caller).
    void receiveInto(Session* s, std::shared_ptr<QFile> file, const QString& vpath)
    {
        const bool drop = dropsHere(vpath);
        reply(s, "150 Ok to send data");
        beginActive();
        QTcpSocket* d = s->data;
        auto got = std::make_shared<qint64>(0);
        auto done = std::make_shared<bool>(false);
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        if (m_opt.throttleBytesPerSec > 0 || m_opt.stallAfterBytes >= 0)
            d->setReadBufferSize(64 * 1024);
        m_openForWrite.insert(file->fileName());
        auto pump = [this, s, d, file, got, done, drop, vpath, clock] {
            if (*done)
                return;
            // By the clock, not per call: the pump also runs on every readyRead,
            // and a budget per call would let a fast client finish a "slow"
            // upload long before its time.
            qint64 budget = m_opt.throttleBytesPerSec > 0
                ? std::max<qint64>(0, (clock->elapsed() + 20) * m_opt.throttleBytesPerSec / 1000 - *got)
                : (1LL << 40);
            if (m_opt.stallAfterBytes >= 0)
                budget = std::min(budget, m_opt.stallAfterBytes - *got);
            if (drop && !m_dropped)
                budget = std::min(budget, m_opt.dropAfterBytes - *got);
            if (budget > 0 && d->bytesAvailable() > 0) {
                const QByteArray b = d->read(std::min(budget, d->bytesAvailable()));
                file->write(b);
                file->flush();
                *got += b.size();
                m_received.fetch_add(b.size());
            }
            if (drop && !m_dropped && *got >= m_opt.dropAfterBytes) {
                m_dropped = true;
                *done = true;
                m_openForWrite.remove(file->fileName());
                file->close();
                d->abort();
                s->data = nullptr;
                m_active.fetch_sub(1);
                d->deleteLater();
                dropReply(s);
                return;
            }
            if (d->state() != QAbstractSocket::ConnectedState && d->bytesAvailable() == 0) {
                *done = true;
                const QString local = file->fileName();
                m_openForWrite.remove(local);
                file->close();
                if (!m_opt.corruptStorPath.isEmpty() && vpath == m_opt.corruptStorPath && m_corruptLeft > 0) {
                    --m_corruptLeft; // silent corruption: the reply still says 226
                    QFile::resize(local, QFileInfo(local).size() / 2);
                }
                finishData(s, "226 Transfer complete");
            }
        };
        auto timer = new QTimer(d);
        timer->setInterval(m_opt.throttleBytesPerSec > 0 ? 20 : 5);
        QObject::connect(timer, &QTimer::timeout, d, pump);
        QObject::connect(d, &QTcpSocket::readyRead, d, pump);
        QObject::connect(d, &QTcpSocket::disconnected, d, pump);
        timer->start();
        pump();
    }

    static bool isSelfBytes(const QByteArray& b)
    {
        return b.startsWith(QByteArray("\x4f\x15\x3d\x1d", 4)) || b.startsWith(QByteArray("\x54\x14\xf5\xee", 4));
    }
    static bool isSelfFile(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) && isSelfBytes(f.read(4));
    }

    void handle(Session* s, const QString& line)
    {
        const qsizetype sp = line.indexOf(u' ');
        const QString cmd = (sp < 0 ? line : line.left(sp)).toUpper();
        const QString arg = sp < 0 ? QString() : line.mid(sp + 1);
        log(cmd == u"PASS" ? QStringLiteral("PASS ****") : line);

        if (cmd == u"USER" && m_opt.ftpsrv) {
            s->user = arg; // ftpsrv has no PASS: 230 at once
            s->loggedIn = true;
            reply(s, "230 User logged in");
            return;
        }
        if (m_opt.ftpsrv && (cmd == u"PASS" || cmd == u"NLST" || cmd == u"EPSV" || cmd == u"MDTM"
                             || (cmd == u"SITE" && arg.compare(QLatin1String("AVAIL"), Qt::CaseInsensitive) == 0))) {
            reply(s, "502 Command not implemented");
            return;
        }
        if (m_opt.ftpsrv && (cmd == u"SELF" || (cmd == u"SITE" && arg.compare(QLatin1String("SELF"), Qt::CaseInsensitive) == 0))) {
            if (!m_opt.selfIgnored)
                s->selfDecrypt = !s->selfDecrypt;
            reply(s, s->selfDecrypt ? "226 SELF transfer mode enabled" : "226 SELF transfer mode disabled");
            return;
        }
        if (cmd == u"USER") {
            s->user = arg;
            reply(s, "331 Password required");
            return;
        }
        if (cmd == u"PASS") {
            if (m_opt.requireLogin && (s->user != m_opt.user || arg != m_opt.pass)) {
                reply(s, "530 Login incorrect.");
                return;
            }
            s->loggedIn = true;
            reply(s, "230 Login successful.");
            return;
        }
        if (cmd == u"QUIT") {
            reply(s, "221 Goodbye.");
            s->ctrl->disconnectFromHost();
            return;
        }
        if (!s->loggedIn) {
            reply(s, "530 Please login with USER and PASS.");
            return;
        }
        // A path outside the root is refused before any command looks at it.
        static const QSet<QString> pathCommands{QStringLiteral("CWD"),  QStringLiteral("SIZE"), QStringLiteral("MDTM"), QStringLiteral("MKD"),
                                                QStringLiteral("RMD"),  QStringLiteral("DELE"), QStringLiteral("RNFR"), QStringLiteral("RNTO"),
                                                QStringLiteral("LIST"), QStringLiteral("NLST"), QStringLiteral("MLSD"), QStringLiteral("RETR"),
                                                QStringLiteral("STOR"), QStringLiteral("APPE")};
        if (pathCommands.contains(cmd) && local(s, arg).isEmpty()) {
            s->rest = 0;
            s->renameFrom.clear();
            reply(s, "550 No such file or directory.");
            return;
        }
        if (cmd == u"SYST") {
            reply(s, "215 UNIX Type: L8");
        } else if (cmd == u"FEAT" && m_opt.ftpsrv) {
            reply(s, "211-Features:\r\n MLSD\r\n KILL\r\n MTRW\r\n SELF\r\n211 End");
        } else if (cmd == u"FEAT") {
            QByteArray f = "211-Features:\r\n";
            if (m_opt.mlsd)
                f += " MLSD\r\n MLST type*;size*;modify*;\r\n";
            if (m_opt.supportRest)
                f += " REST STREAM\r\n";
            f += " SIZE\r\n MDTM\r\n UTF8\r\n" + m_opt.extraFeatures + "211 End";
            reply(s, f);
        } else if (cmd == u"PWD") {
            reply(s, "257 \"" + s->cwd.toUtf8() + "\" is the current directory");
        } else if (cmd == u"CWD") {
            if (QFileInfo(local(s, arg)).isDir()) {
                s->cwd = virtualPath(s, arg);
                reply(s, "250 Directory successfully changed.");
            } else {
                reply(s, "550 Failed to change directory.");
            }
        } else if (cmd == u"CDUP") {
            const QString up = virtualPath(s, QStringLiteral(".."));
            s->cwd = toLocalPath(m_opt.root, QStringLiteral("/"), up).isEmpty() ? QStringLiteral("/") : up; // the top stays the top
            reply(s, "250 Directory successfully changed.");
        } else if (cmd == u"TYPE" || cmd == u"OPTS" || cmd == u"MODE" || cmd == u"STRU") {
            reply(s, "200 OK");
        } else if (cmd == u"NOOP") {
            reply(s, "200 NOOP ok.");
        } else if (cmd == u"PASV" && m_pasvDrops < m_opt.dropControlOnPasv) {
            ++m_pasvDrops; // e.g. a Wi-Fi blip before the STOR is sent
            s->inbuf.clear();
            s->ctrl->abort();
        } else if (cmd == u"PASV" && !m_opt.failFirstPasvReply.isEmpty() && !m_failedPasv) {
            m_failedPasv = true;
            reply(s, m_opt.failFirstPasvReply);
        } else if (cmd == u"PASV") {
            if (m_opt.refusePasv)
                reply(s, "502 PASV not implemented.");
            else
                openPassive(s, false);
        } else if (cmd == u"EPSV") {
            if (m_opt.refuseEpsv)
                reply(s, "502 EPSV not implemented.");
            else
                openPassive(s, true);
        } else if (cmd == u"PORT") {
            const QStringList n = arg.split(u',');
            if (n.size() != 6) {
                reply(s, "501 Bad PORT");
                return;
            }
            s->activeHost = QHostAddress(n.mid(0, 4).join(u'.'));
            s->activePort = quint16(n[4].toInt() * 256 + n[5].toInt());
            reply(s, "200 PORT command successful.");
        } else if (cmd == u"EPRT") {
            const QStringList n = arg.split(arg.left(1));
            if (n.size() < 4) {
                reply(s, "501 Bad EPRT");
                return;
            }
            s->activeHost = QHostAddress(n[2]);
            s->activePort = quint16(n[3].toInt());
            reply(s, "200 EPRT command successful.");
        } else if (cmd == u"REST") {
            if (!m_opt.supportRest) {
                reply(s, "502 REST not implemented.");
                return;
            }
            s->rest = arg.toLongLong();
            reply(s, "350 Restart position accepted.");
        } else if (cmd == u"SIZE") {
            const QFileInfo fi(local(s, arg));
            if (fi.isFile() && m_opt.ftpsrv && s->selfDecrypt && isSelfFile(fi.filePath()))
                reply(s, "213 " + QByteArray::number(fi.size() - 16)); // the "decrypted" ELF
            else if (fi.isFile())
                reply(s, "213 " + QByteArray::number(fi.size()));
            else
                reply(s, "550 Could not get file size.");
        } else if (cmd == u"MDTM") {
            const QFileInfo fi(local(s, arg));
            if (fi.isFile())
                reply(s, "213 " + fi.lastModified().toUTC().toString(QStringLiteral("yyyyMMddHHmmss")).toUtf8());
            else
                reply(s, "550 Could not get file modification time.");
        } else if (cmd == u"MKD") {
            const QString p = local(s, arg);
            if (QFileInfo::exists(p) || !QDir().mkdir(p))
                reply(s, "550 Create directory operation failed.");
            else
                reply(s, "257 \"" + virtualPath(s, arg).toUtf8() + "\" created");
        } else if (cmd == u"RMD") {
            if (QDir().rmdir(local(s, arg)))
                reply(s, "250 Remove directory operation successful.");
            else
                reply(s, "550 Remove directory operation failed.");
        } else if (cmd == u"DELE") {
            if (m_opt.refuseDeletePaths.contains(virtualPath(s, arg)))
                reply(s, "550 Permission denied.");
            else if (m_opt.refuseDeleteWhileOpen && m_openForWrite.contains(local(s, arg)))
                reply(s, "550 The process cannot access the file because it is being used by another process.");
            else if (!readOnly(s, arg) && QFileInfo(local(s, arg)).isFile() && QFile::remove(local(s, arg)))
                reply(s, "250 Delete operation successful.");
            else
                reply(s, "550 Delete operation failed.");
        } else if (cmd == u"RNFR" && m_opt.dropControlOnRnfr > 0 && ++m_rnfrSeen == m_opt.dropControlOnRnfr) {
            s->inbuf.clear();
            s->ctrl->abort();
        } else if (cmd == u"RNFR") {
            if (m_opt.renameRefused) {
                reply(s, "502 RNFR not implemented.");
            } else if (m_opt.refuseRenameFromPaths.contains(virtualPath(s, arg))) {
                reply(s, "550 Permission denied.");
            } else if (QFileInfo::exists(local(s, arg))) {
                s->renameFrom = local(s, arg);
                reply(s, "350 Ready for RNTO.");
            } else {
                reply(s, "550 RNFR command failed.");
            }
        } else if (cmd == u"RNTO") {
            const QString to = local(s, arg);
            if (QFileInfo(to).isFile())
                QFile::remove(to); // rename(2) semantics
            if (!s->renameFrom.isEmpty() && QDir().rename(s->renameFrom, to))
                reply(s, "250 Rename successful.");
            else
                reply(s, "550 Rename failed.");
            s->renameFrom.clear();
        } else if (cmd == u"SITE") {
            if (arg.compare(QLatin1String("AVAIL"), Qt::CaseInsensitive) == 0)
                reply(s, "200 123456789");
            else
                reply(s, "502 Unknown SITE command.");
        } else if (cmd == u"LIST" || cmd == u"NLST" || cmd == u"MLSD") {
            if (cmd == u"MLSD" && !m_opt.mlsd && !m_opt.ftpsrv) {
                reply(s, "500 Unknown command.");
                return;
            }
            const QString dir = local(s, arg); // the client lists the working folder: no switches, no path
            if (!QFileInfo(dir).isDir()) {
                reply(s, "550 Failed to open directory.");
                return;
            }
            QByteArray out;
            for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
                if (cmd == u"NLST")
                    out += fi.fileName().toUtf8() + "\r\n";
                else if (cmd == u"MLSD")
                    out += QStringLiteral("type=%1;size=%2;modify=%3; %4\r\n")
                               .arg(fi.isDir() ? QStringLiteral("dir") : QStringLiteral("file"))
                               .arg(fi.isDir() ? 0 : fi.size())
                               .arg(fi.lastModified().toUTC().toString(QStringLiteral("yyyyMMddHHmmss")), fi.fileName())
                               .toUtf8();
                else
                    out += listLine(fi).toUtf8() + "\r\n";
            }
            for (const QString& extra : m_opt.extraListNames) {
                if (cmd == u"MLSD")
                    out += "type=file;size=5;modify=20240101000000; " + extra.toUtf8() + "\r\n";
                else if (cmd == u"LIST")
                    out += "-rw-r--r-- 1 root wheel 5 Jan 01 00:00 " + extra.toUtf8() + "\r\n";
            }
            if (cmd == u"MLSD")
                out.prepend("type=cdir;modify=20240101000000; .\r\ntype=pdir;modify=20240101000000; ..\r\n");
            withData(s, [this, s, out] {
                reply(s, "150 Here comes the directory listing.");
                beginActive();
                s->data->write(out);
                if (m_opt.lateListReplyMs > 0 && !m_lateListDone) {
                    // The data arrives and closes at once; only the 226 is late.
                    m_lateListDone = true;
                    QTcpSocket* d = s->data;
                    s->data = nullptr;
                    m_active.fetch_sub(1);
                    QObject::connect(d, &QTcpSocket::disconnected, d, &QObject::deleteLater);
                    d->disconnectFromHost();
                    QTimer::singleShot(m_opt.lateListReplyMs, s->ctrl, [this, s] { reply(s, "226 Directory send OK."); });
                    return;
                }
                finishData(s, "226 Directory send OK.");
            });
        } else if ((cmd == u"RETR" || cmd == u"STOR" || cmd == u"APPE") && !m_opt.failFirstTransferReply.isEmpty()
                   && !m_failedTransfer) {
            m_failedTransfer = true;
            s->rest = 0;
            reply(s, m_opt.failFirstTransferReply);
        } else if (cmd == u"RETR") {
            QFile f(local(s, arg));
            if (m_opt.unreadablePaths.contains(virtualPath(s, arg))) {
                reply(s, "550 Permission denied.");
                s->rest = 0;
                return;
            }
            if (!QFileInfo(f.fileName()).isFile() || !f.open(QIODevice::ReadOnly)) {
                reply(s, "550 Failed to open file.");
                s->rest = 0;
                return;
            }
            const qint64 offset = s->rest;
            s->rest = 0;
            f.seek(std::min(offset, f.size()));
            QByteArray bytes = f.readAll();
            if (m_opt.ftpsrv && s->selfDecrypt && offset == 0 && isSelfBytes(bytes))
                bytes = QByteArray("\x7f" "ELF", 4) + bytes.mid(20); // what SELF mode sends
            const QString vpath = virtualPath(s, arg);
            if (!m_opt.shortRetrPath.isEmpty() && vpath == m_opt.shortRetrPath)
                bytes.truncate(bytes.size() / 2);
            withData(s, [this, s, bytes, vpath] { sendBytes(s, bytes, vpath); });
        } else if (cmd == u"STOR" || cmd == u"APPE") {
            const QString p = local(s, arg);
            if (readOnly(s, arg) || !QFileInfo(QFileInfo(p).absolutePath()).isDir()) {
                reply(s, "550 Permission denied.");
                s->rest = 0;
                return;
            }
            auto f = std::make_shared<QFile>(p);
            bool ok = false;
            if (cmd == u"APPE") {
                ok = f->open(QIODevice::WriteOnly | QIODevice::Append);
            } else if (s->rest > 0) {
                ok = f->open(QIODevice::ReadWrite);
                ok = ok && f->resize(s->rest) && f->seek(s->rest);
            } else {
                ok = f->open(QIODevice::WriteOnly | QIODevice::Truncate);
            }
            s->rest = 0;
            if (!ok) {
                reply(s, "553 Could not create file.");
                return;
            }
            const QString vpath = virtualPath(s, arg);
            withData(s, [this, s, f, vpath] { receiveInto(s, f, vpath); });
        } else {
            reply(s, "502 Command not implemented.");
        }
    }

    Options m_opt;
    ServerThread m_t;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    std::vector<std::shared_ptr<Session>> m_sessions;
    mutable std::mutex m_mutex;
    QStringList m_commands;
    std::atomic<int> m_active{0};
    std::atomic<int> m_maxActive{0};
    std::atomic<qint64> m_received{0};
    bool m_dropped = false;
    int m_goneLeft = 0; // connections still to refuse after a goneAfterDrop drop (-1: all)
    bool m_failedPasv = false;
    bool m_lateListDone = false;
    int m_rnfrSeen = 0;
    int m_pasvDrops = 0;
    bool m_failedTransfer = false;
    int m_corruptLeft = 1;
    QSet<QString> m_openForWrite; // local paths of STORs still receiving (server thread only)
};

// ---------------------------------------------------------------------------------------
class FakeTcpSink {
public:
    // `closeAfterBytes` >= 0: the connection is closed once that much arrived
    // (a loader that takes one small payload, or rejects what it gets).
    explicit FakeTcpSink(bool neverRead = false, qint64 closeAfterBytes = -1)
        : m_neverRead(neverRead), m_closeAfter(closeAfterBytes)
    {
        m_t.run([this] {
            m_server = new QTcpServer(m_t.ctx());
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                while (QTcpSocket* s = m_server->nextPendingConnection()) {
                    m_connections.fetch_add(1);
                    if (m_neverRead) {
                        s->setReadBufferSize(1024);
                        continue;
                    }
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                        *buf += s->readAll();
                        if (m_closeAfter >= 0 && buf->size() >= m_closeAfter)
                            s->disconnectFromHost();
                    });
                    QObject::connect(s, &QTcpSocket::disconnected, s, [this, s, buf] {
                        *buf += s->readAll();
                        std::lock_guard l(m_mutex);
                        m_received.push_back(*buf);
                        s->deleteLater();
                    });
                }
            });
            m_port = m_server->serverPort();
        });
    }
    ~FakeTcpSink()
    {
        m_t.destroyChildren();
        m_server = nullptr;
    }
    quint16 port() const { return m_port; }
    int connections() const { return m_connections.load(); }
    // Payloads of connections that have closed, in order.
    std::vector<QByteArray> received() const
    {
        std::lock_guard l(m_mutex);
        return m_received;
    }

private:
    ServerThread m_t;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    bool m_neverRead = false;
    qint64 m_closeAfter = -1;
    mutable std::mutex m_mutex;
    std::vector<QByteArray> m_received;
    std::atomic<int> m_connections{0};
};

// ---------------------------------------------------------------------------------------
class FakeLineServer {
public:
    struct Step {
        int delayMs = 0;
        QByteArray bytes;
    };
    // `scripts[i]` is played to the i-th connection (the last one repeats);
    // `closeAfter` closes the connection when its script ends.
    FakeLineServer(std::vector<std::vector<Step>> scripts, bool closeAfter)
        : m_scripts(std::move(scripts)), m_close(closeAfter)
    {
        m_t.run([this] {
            m_server = new QTcpServer(m_t.ctx());
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                while (QTcpSocket* s = m_server->nextPendingConnection()) {
                    const int n = m_connections.fetch_add(1);
                    const auto& script = m_scripts.empty() ? std::vector<Step>{}
                                                           : m_scripts[std::min<std::size_t>(n, m_scripts.size() - 1)];
                    play(s, script, 0);
                }
            });
            m_port = m_server->serverPort();
        });
    }
    ~FakeLineServer()
    {
        m_t.destroyChildren();
        m_server = nullptr;
    }
    quint16 port() const { return m_port; }

private:
    void play(QTcpSocket* s, std::vector<Step> script, std::size_t i)
    {
        if (i >= script.size()) {
            if (m_close)
                s->disconnectFromHost();
            return;
        }
        QPointer<QTcpSocket> guard(s);
        QTimer::singleShot(script[i].delayMs, m_t.ctx(), [this, guard, script, i] {
            if (!guard)
                return;
            guard->write(script[i].bytes);
            guard->flush();
            play(guard, script, i + 1);
        });
    }

    std::vector<std::vector<Step>> m_scripts;
    bool m_close;
    ServerThread m_t;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    std::atomic<int> m_connections{0};
};

// ---------------------------------------------------------------------------------------
class FakeHttpUpload {
public:
    explicit FakeHttpUpload(QByteArray response) : m_response(std::move(response))
    {
        m_t.run([this] {
            m_server = new QTcpServer(m_t.ctx());
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                while (QTcpSocket* s = m_server->nextPendingConnection()) {
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                        *buf += s->readAll();
                        const qsizetype end = buf->indexOf("\r\n\r\n");
                        if (end < 0)
                            return;
                        static const QRegularExpression cl(QStringLiteral("Content-Length: (\\d+)"),
                                                           QRegularExpression::CaseInsensitiveOption);
                        const auto m = cl.match(QString::fromLatin1(buf->left(end)));
                        const qint64 len = m.hasMatch() ? m.captured(1).toLongLong() : 0;
                        if (buf->size() < end + 4 + len)
                            return;
                        {
                            std::lock_guard l(m_mutex);
                            m_request = *buf;
                        }
                        s->write(m_response);
                        s->disconnectFromHost();
                    });
                }
            });
            m_port = m_server->serverPort();
        });
    }
    ~FakeHttpUpload()
    {
        m_t.destroyChildren();
        m_server = nullptr;
    }
    quint16 port() const { return m_port; }
    QByteArray request() const
    {
        std::lock_guard l(m_mutex);
        return m_request;
    }

private:
    QByteArray m_response;
    ServerThread m_t;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    mutable std::mutex m_mutex;
    QByteArray m_request;
};

// ---------------------------------------------------------------------------------------
// ShadowMount+ 1.7 HTTP/JSON API stand-in: every POST is answered by the
// handler (HTTP status + JSON object); requests are recorded.
class FakeHttpApi {
public:
    struct Reply {
        int status = 200;
        QJsonObject body;
    };
    using Handler = std::function<Reply(const QString& path, const QJsonObject& request)>;

    explicit FakeHttpApi(Handler handler) : m_handler(std::move(handler))
    {
        m_t.run([this] {
            m_server = new QTcpServer(m_t.ctx());
            m_server->listen(QHostAddress::LocalHost, 0);
            QObject::connect(m_server, &QTcpServer::newConnection, m_server, [this] {
                while (QTcpSocket* s = m_server->nextPendingConnection()) {
                    auto buf = std::make_shared<QByteArray>();
                    QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                        *buf += s->readAll();
                        const qsizetype end = buf->indexOf("\r\n\r\n");
                        if (end < 0)
                            return;
                        static const QRegularExpression cl(QStringLiteral("Content-Length: (\\d+)"), QRegularExpression::CaseInsensitiveOption);
                        const QString head = QString::fromLatin1(buf->left(end));
                        const auto m = cl.match(head);
                        const qint64 len = m.hasMatch() ? m.captured(1).toLongLong() : 0;
                        if (buf->size() < end + 4 + len)
                            return;
                        const QString path = head.section(u' ', 1, 1);
                        const QJsonObject req = QJsonDocument::fromJson(buf->mid(end + 4, len)).object();
                        Reply r = m_handler ? m_handler(path, req) : Reply{404, {{QStringLiteral("status"), 2}}};
                        {
                            std::lock_guard l(m_mutex);
                            m_paths << path;
                            m_requests << req;
                        }
                        const QByteArray body = QJsonDocument(r.body).toJson(QJsonDocument::Compact);
                        s->write("HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Type: application/json\r\nContent-Length: "
                                 + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    });
                }
            });
            m_port = m_server->serverPort();
        });
    }
    ~FakeHttpApi()
    {
        m_t.destroyChildren();
        m_server = nullptr;
    }
    quint16 port() const { return m_port; }
    QStringList paths() const
    {
        std::lock_guard l(m_mutex);
        return m_paths;
    }
    QList<QJsonObject> requests() const
    {
        std::lock_guard l(m_mutex);
        return m_requests;
    }

private:
    Handler m_handler;
    ServerThread m_t;
    QTcpServer* m_server = nullptr;
    quint16 m_port = 0;
    mutable std::mutex m_mutex;
    QStringList m_paths;
    QList<QJsonObject> m_requests;
};

} // namespace fake
