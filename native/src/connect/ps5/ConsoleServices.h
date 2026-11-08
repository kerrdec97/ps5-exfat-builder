#pragma once

// "Which services is the console running?" for the PS5 connect page's console
// card: a quick, read-only check of the ports the tools use (research §4-§7).
//
//   * FTP          — from the open session's greeting (ftpsrv + version,
//                    etaHEN, zftpd), never re-probed here.
//   * Kernel log   — TCP connect to the klog port (klogsrv 3232, etaHEN 9081).
//   * Installer    — TCP connect to etaHEN's DPIv2 port (12800, off by default).
//   * ShadowMount+ — POST /api/v1/version on its API port (10101, 1.7+).
//
// The payload loader port is deliberately NOT probed: elfldr treats every
// connection as an incoming payload, so an empty connection could make it
// complain on the console. Its state comes from the last send instead.
//
// Blocking; run on a Qt-started thread (QtConcurrent). Probes run one after
// another with short timeouts, so a console that is off answers in a few
// seconds.

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/SmpApi.h"
#include "core/Cancellation.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace fw::ps5 {

struct ServiceState {
    bool checked = false;  // false: not probed (yet)
    bool reachable = false;
    QString detail;        // "klogsrv · 3232", "1.7beta11", why not
};

struct ConsoleServices {
    ServiceState klog;
    ServiceState installer;
    ServiceState shadowMount;
    std::optional<smp::VersionInfo> smpVersion; // when the API answered
};

struct ServiceProbeOptions {
    int connectTimeoutMs = 1500;
    bool klog = true;
    bool installer = true;
    bool shadowMount = true;
};

ConsoleServices probeConsoleServices(const ConsoleEndpoint& endpoint, const ServiceProbeOptions& options = {},
                                     const CancellationToken& token = {});

} // namespace fw::ps5
