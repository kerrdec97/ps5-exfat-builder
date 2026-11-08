#pragma once

// Where the console is and how to talk to it, resolved from the ps5.*
// settings (see Module.h for the keys and defaults).

#include <QString>

namespace fw {
class SettingsValues;
}

namespace fw::ps5 {

struct ConsoleEndpoint {
    QString host;
    quint16 ftpPort = 2121;
    quint16 payloadPort = 9021;      // Payloads page: elfldr (9090 is etaHEN DPI v1, not a loader)
    quint16 shadowMountPort = 9021;  // ShadowMount+ ELF
    quint16 microMountPort = 9021;   // MicroMount ELF
    quint16 klogPort = 3232;
    quint16 dpiPort = 12800;         // etaHEN Direct Package Installer V2
    quint16 smpApiPort = 10101;      // ShadowMount+ 1.7 HTTP API
    int connectTimeoutMs = 10000;
    int idleTimeoutMs = 30000;       // no progress for this long = stalled
    bool passive = true;
    int parallelTransfers = 2;
    QString gamesFolder = QStringLiteral("/data/etaHEN/games/");
};

ConsoleEndpoint endpointFromSettings(const SettingsValues& settings);

} // namespace fw::ps5
