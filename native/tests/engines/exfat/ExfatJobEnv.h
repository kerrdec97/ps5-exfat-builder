#pragma once

// A module-only registry (general + exfat) and a direct job runner for the
// exFAT job tests.

#include "core/Dependencies.h"
#include "core/FileUtil.h"
#include "core/Settings.h"
#include "engines/exfat/Module.h"
#include "engines/general/Module.h"
#include "jobs/Job.h"
#include "support/ProgressAudit.h"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>
#include <memory>

namespace exfattest {

struct JobEnv {
    std::shared_ptr<fw::SettingsSchema> schema = std::make_shared<fw::SettingsSchema>();
    fw::DependencyRegistry deps;
    fw::JobFactory jobs;
    QVariantMap defaults; // applied to every job (tests keep images small)

    JobEnv()
    {
        fw::ModuleContext ctx{*schema, deps, jobs};
        fw::general::registerModule(ctx);
        fw::exfat::registerModule(ctx);
        defaults.insert(fw::exfat::keys::kSpareSpace, QStringLiteral("fixed"));
        defaults.insert(fw::exfat::keys::kSpareSpaceMiB, 4);
        defaults.insert(fw::general::keys::kFreeSpaceMarginMiB, 16);
    }

    fw::SettingsValues settings(const QVariantMap& overrides) const
    {
        QVariantMap merged = defaults;
        for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
            auto n = schema->normalize(it.key(), it.value());
            Q_ASSERT_X(n.ok(), "JobEnv::settings", qPrintable(it.key()));
            merged.insert(it.key(), n.value());
        }
        return fw::SettingsValues(schema, {}, merged);
    }

    // Validates and runs a job synchronously, like the queue's worker does
    // (the phases and their weights declared as the queue declares them).
    // `audit` watches the progress and gets the last state at the end.
    fw::Result<fw::JobOutcome> run(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {},
                                   fw::CancellationSource* cancel = nullptr,
                                   std::function<void(const fw::ProgressSnapshot&)> sink = {},
                                   fwtest::ProgressAudit* audit = nullptr) const
    {
        auto job = jobs.create(kind, params, overrides);
        if (!job.ok())
            return job.takeError();
        const fw::SettingsValues s = settings(overrides);
        if (auto v = job.value()->validate(s); !v.ok())
            return v.takeError();
        fw::ProgressReporter reporter(
            [&](const fw::ProgressSnapshot& p) {
                if (audit)
                    audit->report(p);
                if (sink)
                    sink(p);
            },
            0);
        if (audit)
            audit->watch(&reporter);
        reporter.setPhases(job.value()->phases(), job.value()->phaseWeights(s));
        fw::TempDir temp(QStringLiteral("exfat-test"));
        fw::JobContext ctx;
        ctx.jobId = QStringLiteral("test");
        ctx.token = cancel ? cancel->token() : fw::CancellationToken();
        ctx.progress = &reporter;
        ctx.log = fw::Log(kind, QStringLiteral("test"));
        ctx.settings = s;
        ctx.tempDir = temp.path();
        auto r = job.value()->run(ctx);
        if (audit) {
            reporter.finish();
            audit->finish(reporter.snapshot(), r.ok() && r->verification != fw::Verification::Failed);
            audit->watch(nullptr);
        }
        if (!r.ok() && ctx.token.isCancelled() && !r.error().isCancelled())
            return fw::Error::cancelled();
        return r;
    }
};

inline QByteArray fileHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result();
}

} // namespace exfattest
