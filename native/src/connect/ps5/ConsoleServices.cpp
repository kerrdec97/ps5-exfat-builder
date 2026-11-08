#include "connect/ps5/ConsoleServices.h"

#include "connect/ps5/PayloadSender.h"

namespace fw::ps5 {

ConsoleServices probeConsoleServices(const ConsoleEndpoint& ep, const ServiceProbeOptions& opt, const CancellationToken& token)
{
    ConsoleServices out;
    if (ep.host.trimmed().isEmpty())
        return out;
    auto tcp = [&](quint16 port, const QString& name, ServiceState& st) {
        st.checked = true;
        const Status s = probeTcpPort(ep.host, port, opt.connectTimeoutMs, name, token);
        st.reachable = s.ok();
        st.detail = s.ok() ? QStringLiteral("port %1").arg(port) : s.error().message;
    };
    if (opt.klog && !token.isCancelled())
        tcp(ep.klogPort, QStringLiteral("kernel log server"), out.klog);
    if (opt.installer && !token.isCancelled())
        tcp(ep.dpiPort, QStringLiteral("package installer"), out.installer);
    if (opt.shadowMount && !token.isCancelled()) {
        out.shadowMount.checked = true;
        smp::ApiOptions api;
        api.connectTimeoutMs = opt.connectTimeoutMs;
        api.replyTimeoutMs = std::max(2000, opt.connectTimeoutMs * 2);
        if (auto v = smp::version(ep.host, ep.smpApiPort, api, token); v.ok()) {
            out.shadowMount.reachable = true;
            out.shadowMount.detail = v->version;
            out.smpVersion = v.value();
        } else {
            out.shadowMount.detail = v.error().message;
        }
    }
    return out;
}

} // namespace fw::ps5
