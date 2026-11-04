#pragma once

// In-process stand-ins for the console's network services, for the ps5 tests:
//
//  * FakeFtpServer  — the FTP subset the client uses, serving a local folder;
//                     LIST in three styles, MLSD, PASV/EPSV/PORT/EPRT, REST,
//                     throttling, a one-time drop and a stall for fault tests.
//  * FakeTcpSink    — accepts connections and records every byte (payloads).
//  * FakeLineServer — sends scripted text per connection (klogsrv).
//  * FakeHttpUpload — records one HTTP request and answers it (DPI).
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
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
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
    }
    ~ServerThread()
    {
        run([this] { delete m_ctx; });
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

inline QString toLocalPath(const QString& root, const QString& cwd, const QString& arg)
{
    QString p = arg.trimmed();
    if (!p.startsWith(u'/'))
        p = cwd + u'/' + p;
    p = QDir::cleanPath(p);
    if (!p.startsWith(u'/'))
        p.prepend(u'/');
    return QDir::cleanPath(root + p);
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
    int connections() const { return m_connections.load(); }
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
    };

    void log(const QString& c)
    {
        std::lock_guard l(m_mutex);
        m_commands << c;
    }
    void reply(Session* s, const QByteArray& line) { s->ctrl->write(line + "\r\n"); }

    void startSession(QTcpSocket* sock)
    {
        auto s = std::make_shared<Session>();
        s->ctrl = sock;
        sock->setParent(m_t.ctx());
        m_sessions.push_back(s);
        m_connections.fetch_add(1);
        if (!m_opt.silentGreeting)
            reply(s.get(), "220 FakePS5 FTP ready");
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
    void sendBytes(Session* s, const QByteArray& bytes)
    {
        reply(s, "150 Opening BINARY mode data connection");
        beginActive();
        auto pos = std::make_shared<qint64>(0);
        auto timer = new QTimer(m_t.ctx());
        timer->setInterval(m_opt.throttleBytesPerSec > 0 ? 20 : 0);
        QObject::connect(timer, &QTimer::timeout, timer, [this, s, bytes, pos, timer] {
            QTcpSocket* d = s->data;
            if (!d) {
                timer->deleteLater();
                return;
            }
            if (d->bytesToWrite() > 256 * 1024)
                return;
            if (m_opt.stallAfterBytes >= 0 && *pos >= m_opt.stallAfterBytes)
                return; // stall forever
            qint64 n = m_opt.throttleBytesPerSec > 0 ? std::max<qint64>(1, m_opt.throttleBytesPerSec / 50) : 256 * 1024;
            if (m_opt.dropAfterBytes >= 0 && !m_dropped)
                n = std::min(n, std::max<qint64>(0, m_opt.dropAfterBytes - *pos));
            if (m_opt.stallAfterBytes >= 0)
                n = std::min(n, m_opt.stallAfterBytes - *pos);
            n = std::min(n, bytes.size() - *pos);
            if (n > 0) {
                d->write(bytes.constData() + *pos, n);
                *pos += n;
            }
            if (m_opt.dropAfterBytes >= 0 && !m_dropped && *pos >= m_opt.dropAfterBytes && *pos < bytes.size()) {
                m_dropped = true;
                timer->stop();
                timer->deleteLater();
                d->abort();
                d->deleteLater();
                s->data = nullptr;
                m_active.fetch_sub(1);
                reply(s, "426 Connection closed; transfer aborted.");
                return;
            }
            if (*pos >= bytes.size()) {
                timer->stop();
                timer->deleteLater();
                finishData(s, "226 Transfer complete");
            }
        });
        timer->start();
    }

    // Client -> server, appended to `file` (opened by the caller).
    void receiveInto(Session* s, std::shared_ptr<QFile> file)
    {
        reply(s, "150 Ok to send data");
        beginActive();
        QTcpSocket* d = s->data;
        auto got = std::make_shared<qint64>(0);
        auto done = std::make_shared<bool>(false);
        if (m_opt.throttleBytesPerSec > 0 || m_opt.stallAfterBytes >= 0)
            d->setReadBufferSize(64 * 1024);
        auto pump = [this, s, d, file, got, done] {
            if (*done)
                return;
            qint64 budget = m_opt.throttleBytesPerSec > 0 ? std::max<qint64>(1, m_opt.throttleBytesPerSec / 50) : (1LL << 40);
            if (m_opt.stallAfterBytes >= 0)
                budget = std::min(budget, m_opt.stallAfterBytes - *got);
            if (m_opt.dropAfterBytes >= 0 && !m_dropped)
                budget = std::min(budget, m_opt.dropAfterBytes - *got);
            if (budget > 0 && d->bytesAvailable() > 0) {
                const QByteArray b = d->read(std::min(budget, d->bytesAvailable()));
                file->write(b);
                file->flush();
                *got += b.size();
                m_received.fetch_add(b.size());
            }
            if (m_opt.dropAfterBytes >= 0 && !m_dropped && *got >= m_opt.dropAfterBytes) {
                m_dropped = true;
                *done = true;
                file->close();
                d->abort();
                s->data = nullptr;
                m_active.fetch_sub(1);
                d->deleteLater();
                reply(s, "426 Connection closed; transfer aborted.");
                return;
            }
            if (d->state() != QAbstractSocket::ConnectedState && d->bytesAvailable() == 0) {
                *done = true;
                file->close();
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

    void handle(Session* s, const QString& line)
    {
        const qsizetype sp = line.indexOf(u' ');
        const QString cmd = (sp < 0 ? line : line.left(sp)).toUpper();
        const QString arg = sp < 0 ? QString() : line.mid(sp + 1);
        log(cmd == u"PASS" ? QStringLiteral("PASS ****") : line);

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
        if (cmd == u"SYST") {
            reply(s, "215 UNIX Type: L8");
        } else if (cmd == u"FEAT") {
            QByteArray f = "211-Features:\r\n";
            if (m_opt.mlsd)
                f += " MLSD\r\n MLST type*;size*;modify*;\r\n";
            if (m_opt.supportRest)
                f += " REST STREAM\r\n";
            f += " SIZE\r\n MDTM\r\n UTF8\r\n211 End";
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
            s->cwd = virtualPath(s, QStringLiteral(".."));
            reply(s, "250 Directory successfully changed.");
        } else if (cmd == u"TYPE" || cmd == u"OPTS" || cmd == u"MODE" || cmd == u"STRU") {
            reply(s, "200 OK");
        } else if (cmd == u"NOOP") {
            reply(s, "200 NOOP ok.");
        } else if (cmd == u"PASV") {
            if (m_opt.refusePasv)
                reply(s, "502 PASV not implemented.");
            else
                openPassive(s, false);
        } else if (cmd == u"EPSV") {
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
            if (fi.isFile())
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
            if (!readOnly(s, arg) && QFileInfo(local(s, arg)).isFile() && QFile::remove(local(s, arg)))
                reply(s, "250 Delete operation successful.");
            else
                reply(s, "550 Delete operation failed.");
        } else if (cmd == u"RNFR") {
            if (m_opt.renameRefused) {
                reply(s, "502 RNFR not implemented.");
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
            if (cmd == u"MLSD" && !m_opt.mlsd) {
                reply(s, "500 Unknown command.");
                return;
            }
            const QString dir = local(s, arg.startsWith(u'-') ? QString() : arg);
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
            if (cmd == u"MLSD")
                out.prepend("type=cdir;modify=20240101000000; .\r\ntype=pdir;modify=20240101000000; ..\r\n");
            withData(s, [this, s, out] {
                reply(s, "150 Here comes the directory listing.");
                beginActive();
                s->data->write(out);
                finishData(s, "226 Directory send OK.");
            });
        } else if (cmd == u"RETR") {
            QFile f(local(s, arg));
            if (!QFileInfo(f.fileName()).isFile() || !f.open(QIODevice::ReadOnly)) {
                reply(s, "550 Failed to open file.");
                s->rest = 0;
                return;
            }
            const qint64 offset = s->rest;
            s->rest = 0;
            f.seek(std::min(offset, f.size()));
            const QByteArray bytes = f.readAll();
            withData(s, [this, s, bytes] { sendBytes(s, bytes); });
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
            withData(s, [this, s, f] { receiveInto(s, f); });
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
    std::atomic<int> m_connections{0};
    std::atomic<int> m_active{0};
    std::atomic<int> m_maxActive{0};
    std::atomic<qint64> m_received{0};
    bool m_dropped = false;
};

// ---------------------------------------------------------------------------------------
class FakeTcpSink {
public:
    explicit FakeTcpSink(bool neverRead = false) : m_neverRead(neverRead)
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
                    QObject::connect(s, &QTcpSocket::readyRead, s, [s, buf] { *buf += s->readAll(); });
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
    int connections() const { return m_connections.load(); }

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

} // namespace fake
