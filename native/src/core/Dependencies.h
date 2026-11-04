#pragma once

// Dependency detection ("Components & plugins" in the Settings > Tools page).
//
// Every external tool or runtime the app can use registers a probe. Probes run
// off the UI thread and report what was found, where, which version, and what
// the user can do about it. Nothing is downloaded or bundled: Sony tools are
// always user-supplied.

#include <QJsonObject>
#include <QMetaType>
#include <QString>

#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace fw {

class SettingsValues;

enum class DependencyStatus { Ready, Missing, Outdated, Error, NotApplicable };

QString dependencyStatusName(DependencyStatus status);

struct DependencyInfo {
    QString id;          // "sony.pubcmd", "ufs2tool", "dotnet", ...
    QString name;        // "Sony Publishing Tools (prospero-pub-cmd)"
    QString purpose;     // what it is used for
    DependencyStatus status = DependencyStatus::Missing;
    QString version;
    QString path;
    QString detail;      // what was checked / why it failed
    QString howToFix;    // user-facing next step
    bool required = false; // required for the app to start (none are today)
    QString settingKey;  // the setting that holds its path, if any

    QJsonObject toJson() const;
};

class DependencyRegistry {
public:
    using Probe = std::function<DependencyInfo(const SettingsValues&)>;

    void add(const QString& id, const QString& name, const QString& purpose, Probe probe);
    std::vector<QString> ids() const;

    // Runs one probe (blocking; call from a worker thread).
    DependencyInfo probe(const QString& id, const SettingsValues& settings) const;
    std::vector<DependencyInfo> probeAll(const SettingsValues& settings) const;

    // Last results (cached by probe()).
    DependencyInfo last(const QString& id) const;
    std::vector<DependencyInfo> lastAll() const;

private:
    struct Entry {
        QString id, name, purpose;
        Probe probe;
    };
    std::vector<Entry> m_entries;
    mutable std::mutex m_mutex;
    mutable std::map<QString, DependencyInfo> m_last;
};

} // namespace fw

Q_DECLARE_METATYPE(fw::DependencyInfo)
