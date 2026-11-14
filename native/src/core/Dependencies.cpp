#include "core/Dependencies.h"

#include "core/Log.h"
#include "core/Settings.h"
#include "core/Tr.h"

#include <exception>

namespace fw {

QString dependencyStatusName(DependencyStatus status)
{
    switch (status) {
    case DependencyStatus::Ready: return QStringLiteral("Ready");
    case DependencyStatus::Missing: return QStringLiteral("Missing");
    case DependencyStatus::Outdated: return QStringLiteral("Outdated");
    case DependencyStatus::Error: return QStringLiteral("Error");
    case DependencyStatus::NotApplicable: return QStringLiteral("Not needed");
    }
    return QStringLiteral("Unknown");
}

QJsonObject DependencyInfo::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("status"), dependencyStatusName(status));
    if (!version.isEmpty()) o.insert(QStringLiteral("version"), version);
    if (!path.isEmpty()) o.insert(QStringLiteral("path"), path);
    if (!detail.isEmpty()) o.insert(QStringLiteral("detail"), detail);
    if (!howToFix.isEmpty()) o.insert(QStringLiteral("howToFix"), howToFix);
    return o;
}

void DependencyRegistry::add(const QString& id, const QString& name, const QString& purpose, Probe probe)
{
    std::lock_guard lock(m_mutex);
    for (auto& e : m_entries) {
        if (e.id == id) {
            e = Entry{id, name, purpose, std::move(probe)};
            return;
        }
    }
    m_entries.push_back(Entry{id, name, purpose, std::move(probe)});
}

std::vector<QString> DependencyRegistry::ids() const
{
    std::lock_guard lock(m_mutex);
    std::vector<QString> out;
    for (const auto& e : m_entries)
        out.push_back(e.id);
    return out;
}

DependencyInfo DependencyRegistry::probe(const QString& id, const SettingsValues& settings) const
{
    Entry entry;
    {
        std::lock_guard lock(m_mutex);
        bool found = false;
        for (const auto& e : m_entries)
            if (e.id == id) {
                entry = e;
                found = true;
            }
        if (!found) {
            DependencyInfo info;
            info.id = id;
            info.status = DependencyStatus::Error;
            info.detail = FW_TR("No probe registered.");
            return info;
        }
    }
    DependencyInfo info;
    try {
        info = entry.probe(settings);
    } catch (const std::exception& ex) {
        info.status = DependencyStatus::Error;
        info.detail = QString::fromUtf8(ex.what());
    } catch (...) {
        info.status = DependencyStatus::Error;
        info.detail = FW_TR("The check failed unexpectedly.");
    }
    info.id = entry.id;
    if (info.name.isEmpty())
        info.name = entry.name;
    if (info.purpose.isEmpty())
        info.purpose = entry.purpose;
    Logger::instance().log(LogLevel::Debug, QStringLiteral("deps"),
                           QStringLiteral("%1: %2").arg(info.name, dependencyStatusName(info.status)), {}, info.toJson());
    {
        std::lock_guard lock(m_mutex);
        m_last[info.id] = info;
    }
    return info;
}

std::vector<DependencyInfo> DependencyRegistry::probeAll(const SettingsValues& settings) const
{
    std::vector<DependencyInfo> out;
    for (const auto& id : ids())
        out.push_back(probe(id, settings));
    return out;
}

DependencyInfo DependencyRegistry::last(const QString& id) const
{
    std::lock_guard lock(m_mutex);
    if (auto it = m_last.find(id); it != m_last.end())
        return it->second;
    DependencyInfo info;
    info.id = id;
    info.status = DependencyStatus::Missing;
    info.detail = FW_TR("Not checked yet.");
    return info;
}

std::vector<DependencyInfo> DependencyRegistry::lastAll() const
{
    std::vector<DependencyInfo> out;
    for (const auto& id : ids())
        out.push_back(last(id));
    return out;
}

} // namespace fw
