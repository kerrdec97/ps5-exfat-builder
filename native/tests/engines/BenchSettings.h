#pragma once

// The Performance settings a benchmark runs with, resolved the way the app
// resolves them (general::workerThreads, general::ioBufferBytes) from the
// app's own defaults:
//
//   FW_BENCH_MODE        lowImpact, balanced or maximum (performance.mode)
//   FW_BENCH_THREADS     performance.workerThreads (0 = from the mode)
//   FW_BENCH_BUFFER_MIB  performance.ioBufferMiB (1-64)
//
// Unset variables keep the app's defaults. Only the thread count and the
// buffer size change what the image engines do; the priorities a mode also
// sets apply to Sony's publisher and the DLC engine, not to them.

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "engines/general/Module.h"
#include "jobs/Module.h"

#include <QStringList>

#include <memory>

namespace benchsettings {

struct Performance {
    QString mode;           // performance.mode in effect
    int threads = 1;        // general::workerThreads()
    qint64 bufferBytes = 0; // general::ioBufferBytes()
    bool given = false;     // one of the FW_BENCH_* variables was set
    QString problem;        // a value that is not valid (empty when all are)

    QString describe() const
    {
        return QStringLiteral("performance mode %1, %2 worker thread(s), %3 MiB I/O buffers")
            .arg(mode)
            .arg(threads)
            .arg(bufferBytes / (1024 * 1024));
    }
};

inline Performance fromEnvironment()
{
    using namespace fw;
    auto schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    ModuleContext ctx{*schema, deps, jobs};
    general::registerModule(ctx);

    Performance p;
    QVariantMap values;
    const QString mode = qEnvironmentVariable("FW_BENCH_MODE").trimmed();
    if (!mode.isEmpty()) {
        p.given = true;
        if (!QStringList{QStringLiteral("lowImpact"), QStringLiteral("balanced"), QStringLiteral("maximum")}.contains(mode))
            p.problem = QStringLiteral("FW_BENCH_MODE=%1 is not lowImpact, balanced or maximum").arg(mode);
        values.insert(general::keys::kPerformanceMode, mode);
    }
    const auto number = [&](const char* name, const QString& key, int lowest, int highest) {
        const QString text = qEnvironmentVariable(name).trimmed();
        if (text.isEmpty())
            return;
        p.given = true;
        bool ok = false;
        const int v = text.toInt(&ok);
        if (!ok || v < lowest || v > highest) {
            p.problem = QStringLiteral("%1=%2 is not a number from %3 to %4").arg(QLatin1String(name), text).arg(lowest).arg(highest);
            return;
        }
        values.insert(key, v);
    };
    number("FW_BENCH_THREADS", general::keys::kWorkerThreads, 0, 256);
    number("FW_BENCH_BUFFER_MIB", general::keys::kIoBufferMiB, 1, 64);

    const SettingsValues s(schema, values, {});
    p.mode = s.string(general::keys::kPerformanceMode);
    p.threads = general::workerThreads(s);
    p.bufferBytes = general::ioBufferBytes(s);
    return p;
}

} // namespace benchsettings
