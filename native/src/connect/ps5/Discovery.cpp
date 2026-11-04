#include "connect/ps5/Discovery.h"

#include "connect/ps5/SocketWait.h"

#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace fw::ps5 {

QStringList subnetPrefixesFor(const QList<QHostAddress>& addresses)
{
    QStringList out;
    for (const QHostAddress& a : addresses) {
        bool ok = false;
        const quint32 v = a.toIPv4Address(&ok);
        if (!ok || v == 0 || v == 0xFFFFFFFFu)
            continue;
        if ((v >> 24) == 127)       // loopback
            continue;
        if ((v >> 16) == 0xA9FEu)   // 169.254/16 link-local (no DHCP answer)
            continue;
        if ((v >> 28) >= 0xEu)      // multicast and reserved
            continue;
        const QString prefix = QStringLiteral("%1.%2.%3").arg(v >> 24).arg((v >> 16) & 0xFF).arg((v >> 8) & 0xFF);
        if (!out.contains(prefix))
            out << prefix;
    }
    return out;
}

QList<QHostAddress> localIpv4Addresses()
{
    QList<QHostAddress> out;
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        const auto f = i.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) || (f & QNetworkInterface::IsLoopBack))
            continue;
        if (i.type() == QNetworkInterface::Virtual) // VPN tunnels: the console is on the LAN
            continue;
        for (const QNetworkAddressEntry& e : i.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                out << e.ip();
    }
    return out;
}

QStringList localSubnetPrefixes()
{
    return subnetPrefixesFor(localIpv4Addresses());
}

QStringList subnetHosts(const QStringList& prefixes, const QStringList& exclude)
{
    QStringList out;
    for (const QString& p : prefixes)
        for (int i = 1; i <= 254; ++i) {
            const QString h = QStringLiteral("%1.%2").arg(p).arg(i);
            if (!exclude.contains(h))
                out << h;
        }
    return out;
}

namespace {

// One host: every port in order; the banner is read from the first open port
// that greets within bannerWaitMs.
std::optional<DiscoveredConsole> probeHost(const QString& host, const DiscoveryOptions& o, const CancellationToken& token)
{
    DiscoveredConsole c;
    c.host = host;
    for (const quint16 port : o.ports) {
        if (token.isCancelled())
            return std::nullopt;
        QTcpSocket s;
        QElapsedTimer t;
        t.start();
        if (!detail::connectSocket(s, host, port, o.connectTimeoutMs, token, QStringLiteral("FTP server")).ok())
            continue;
        c.openPorts << port;
        if (c.rttMs < 0)
            c.rttMs = int(t.elapsed());
        if (c.banner.isEmpty() && o.bannerWaitMs > 0) {
            (void)detail::waitUntil([&] { return s.canReadLine() || s.state() != QAbstractSocket::ConnectedState; },
                                    o.bannerWaitMs, token, {&s});
            if (s.canReadLine()) {
                const QString line = QString::fromUtf8(s.readLine()).trimmed();
                if (line.startsWith(QLatin1String("220"))) {
                    QString text = line.mid(3); // "220 text" or "220-text" (multi-line greeting)
                    if (text.startsWith(u' ') || text.startsWith(u'-'))
                        text.remove(0, 1);
                    c.banner = text.trimmed();
                }
            }
        }
        s.disconnectFromHost();
        if (s.state() != QAbstractSocket::UnconnectedState)
            s.abort();
    }
    if (c.openPorts.isEmpty())
        return std::nullopt;
    c.port = c.openPorts.front();
    return c;
}

quint32 sortKey(const QString& host)
{
    bool ok = false;
    const quint32 v = QHostAddress(host).toIPv4Address(&ok);
    return ok ? v : 0xFFFFFFFFu;
}

} // namespace

Result<std::vector<DiscoveredConsole>> discoverConsoles(const DiscoveryOptions& options, const CancellationToken& token,
                                                        const DiscoveryProgress& progress)
{
    QStringList hosts = options.hosts;
    if (hosts.isEmpty()) {
        const QList<QHostAddress> own = localIpv4Addresses();
        QStringList ownText;
        for (const QHostAddress& a : own)
            ownText << a.toString();
        hosts = subnetHosts(subnetPrefixesFor(own), ownText);
    }
    if (hosts.isEmpty())
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("This PC has no network connection to search. Connect it to the same network as "
                                    "the PS5, or type the console's IP address."));
    if (options.ports.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No ports to search."));
    if (token.isCancelled())
        return Error::cancelled();

    struct Shared {
        std::mutex mutex;
        std::vector<DiscoveredConsole> found;
        std::atomic<int> done{0};
        std::atomic<int> hits{0};
    };
    auto sh = std::make_shared<Shared>();
    const int total = int(hosts.size());
    {
        QThreadPool pool;
        pool.setMaxThreadCount(std::clamp(options.workers, 1, 128));
        for (const QString& h : hosts) {
            pool.start([sh, h, options, token] {
                if (!token.isCancelled()) {
                    if (auto c = probeHost(h, options, token)) {
                        std::lock_guard lock(sh->mutex);
                        sh->found.push_back(std::move(*c));
                        sh->hits.fetch_add(1);
                    }
                }
                sh->done.fetch_add(1);
            });
        }
        int reported = -1;
        while (!pool.waitForDone(25)) {
            if (token.isCancelled())
                pool.clear(); // probes not started yet are dropped; running ones stop at their next wait
            const int d = sh->done.load();
            if (progress && d != reported) {
                reported = d;
                progress(d, total, sh->hits.load());
            }
        }
    }
    if (token.isCancelled())
        return Error::cancelled();
    if (progress)
        progress(total, total, sh->hits.load());
    std::vector<DiscoveredConsole> out = std::move(sh->found);
    std::sort(out.begin(), out.end(), [](const DiscoveredConsole& a, const DiscoveredConsole& b) {
        return sortKey(a.host) != sortKey(b.host) ? sortKey(a.host) < sortKey(b.host) : a.host < b.host;
    });
    return out;
}

} // namespace fw::ps5
