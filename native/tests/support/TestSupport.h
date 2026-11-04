#pragma once

// Shared helpers for every test executable.

#include "core/Dependencies.h"
#include "core/Settings.h"
#include "jobs/Job.h"

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace fwtest {

// Points Paths at a fresh temporary root so tests never touch real app data.
// Call from initTestCase().
void isolateAppData(QTemporaryDir& dir);

// Writes `size` deterministic pseudo-random bytes (seeded) to `path`.
bool writeRandomFile(const QString& path, qint64 size, quint32 seed);
bool writeTextFile(const QString& path, const QByteArray& text);

// A schema with every module registered (what the app uses).
struct Registry {
    std::shared_ptr<fw::SettingsSchema> schema = std::make_shared<fw::SettingsSchema>();
    fw::DependencyRegistry deps;
    fw::JobFactory jobs;
    Registry();
};

QString helperPath();
QString dataDir();

} // namespace fwtest
