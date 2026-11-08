#include "connect/ps5/Endpoint.h"

#include "connect/ps5/Module.h"
#include "core/Settings.h"

#include <algorithm>

namespace fw::ps5 {

namespace {

quint16 port(const SettingsValues& s, const QString& key, quint16 fallback)
{
    const qint64 v = s.integer(key);
    return v >= 1 && v <= 65535 ? quint16(v) : fallback;
}

} // namespace

ConsoleEndpoint endpointFromSettings(const SettingsValues& s)
{
    ConsoleEndpoint ep;
    ep.host = s.string(keys::kConsoleIp).trimmed();
    ep.ftpPort = port(s, keys::kFtpPort, 2121);
    ep.payloadPort = port(s, keys::kPayloadPort, 9021);
    ep.shadowMountPort = port(s, keys::kShadowMountPort, 9021);
    ep.microMountPort = port(s, keys::kMicroMountPort, 9021);
    ep.klogPort = port(s, keys::kKlogPort, 3232);
    ep.dpiPort = port(s, keys::kDpiPort, 12800);
    ep.smpApiPort = port(s, keys::kSmpApiPort, 10101);
    ep.connectTimeoutMs = int(std::clamp<qint64>(s.integer(keys::kConnectTimeout), 1, 300)) * 1000;
    ep.idleTimeoutMs = int(std::clamp<qint64>(s.integer(keys::kIdleTimeout), 1, 3600)) * 1000;
    ep.parallelTransfers = int(std::clamp<qint64>(s.integer(keys::kParallelTransfers), 1, 8));
    ep.passive = s.boolean(keys::kPassiveMode);
    const QString games = s.string(keys::kGamesFolder).trimmed();
    if (!games.isEmpty())
        ep.gamesFolder = games;
    return ep;
}

} // namespace fw::ps5
