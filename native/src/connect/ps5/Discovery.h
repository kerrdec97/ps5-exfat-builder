#pragma once

// "Find my PS5": the legacy auto-detect IP sweep (_auto_detect_ip): every
// address .1–.254 of the PC's /24 networks is probed on the FTP ports 2121 and
// 2122 with a 0.3 s connect timeout, 50 probes at a time. A host that accepts
// a connection is reported with its open ports and, when an FTP greeting
// arrives within a moment, the server's banner.
//
// Blocking: call from a worker thread (QtConcurrent, the queue). The probes
// run on a private QThreadPool, so each QTcpSocket lives on a Qt-started
// thread (Qt's socket notifiers refuse std::thread).

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fw::ps5 {

struct DiscoveredConsole {
    QString host;
    quint16 port = 0;          // first open port in the probe order
    QList<quint16> openPorts;  // every probed port that accepted a connection
    QString banner;            // text of the FTP "220" greeting, empty if none arrived in time
    int rttMs = -1;            // time the first successful connect took
};

struct DiscoveryOptions {
    QList<quint16> ports = {2121, 2122}; // legacy order
    int connectTimeoutMs = 300;          // legacy 0.3 s
    int bannerWaitMs = 400;              // how long an open port may take to greet
    int workers = 50;                    // probes at the same time (legacy 50)
    // Hosts to probe. Empty = every .1–.254 address of localSubnetPrefixes(),
    // without this PC's own addresses. Tests pass 127.0.0.1 here.
    QStringList hosts;
};

// Called on the calling thread of discoverConsoles() as probes complete.
using DiscoveryProgress = std::function<void(int probedHosts, int totalHosts, int found)>;

// "a.b.c" prefixes of the /24 networks of the given addresses: IPv4 only,
// loopback (127/8), link-local (169.254/16), 0.0.0.0 and broadcast skipped,
// duplicates removed, order kept.
QStringList subnetPrefixesFor(const QList<QHostAddress>& addresses);

// The PC's own IPv4 addresses on interfaces that are up and running (not
// loopback), from QNetworkInterface.
QList<QHostAddress> localIpv4Addresses();

// subnetPrefixesFor(localIpv4Addresses()).
QStringList localSubnetPrefixes();

// "a.b.c.1" … "a.b.c.254" for every prefix, without the `exclude` addresses.
QStringList subnetHosts(const QStringList& prefixes, const QStringList& exclude = {});

// Probes the hosts (see DiscoveryOptions::hosts) and returns those with at
// least one open port, sorted by address. Fails with InvalidArgument when
// there is nothing to probe (no usable network interface) and Cancelled when
// `token` is cancelled.
Result<std::vector<DiscoveredConsole>> discoverConsoles(const DiscoveryOptions& options,
                                                        const CancellationToken& token = {},
                                                        const DiscoveryProgress& progress = {});

} // namespace fw::ps5
