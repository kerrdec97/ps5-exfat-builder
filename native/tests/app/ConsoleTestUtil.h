#pragma once

// For the window tests that talk to the stand-in console (FakePs5.h).
//
// A page that reaches the stand-in FTP server goes on to look for the console's
// other services (kernel log, installer, payload loaders, ShadowMount+ API) on
// the same address. Their real default ports must never be contacted on the
// developer's PC, where anything may be listening: every one of them points at
// a reserved port that refuses connections, until a test sets the one it serves.

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/Module.h"
#include "core/Settings.h"
#include "support/ClosedPort.h"

#include <QString>

namespace fwtest {

// The stand-in console as an endpoint: FTP on `ftpPort`, nothing else.
inline fw::ps5::ConsoleEndpoint loopbackConsole(quint16 ftpPort)
{
    fw::ps5::ConsoleEndpoint ep;
    ep.host = QStringLiteral("127.0.0.1");
    ep.ftpPort = ftpPort;
    ep.payloadPort = ep.shadowMountPort = ep.microMountPort = refusedPort();
    ep.klogPort = ep.dpiPort = ep.smpApiPort = refusedPort();
    ep.connectTimeoutMs = 3000;
    ep.idleTimeoutMs = 5000;
    return ep;
}

// The same for pages that build their endpoint from the settings. False when a
// key was refused (renamed key, value out of range).
inline bool closeConsoleServices(fw::SettingsStore& settings)
{
    bool ok = true;
    for (const QString& key : {fw::ps5::keys::kPayloadPort, fw::ps5::keys::kShadowMountPort, fw::ps5::keys::kMicroMountPort,
                               fw::ps5::keys::kKlogPort, fw::ps5::keys::kDpiPort, fw::ps5::keys::kSmpApiPort})
        ok = settings.setValue(key, int(refusedPort())).ok() && ok;
    return ok;
}

} // namespace fwtest
