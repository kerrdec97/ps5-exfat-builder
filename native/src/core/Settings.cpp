#include "core/Settings.h"

#include "core/FileUtil.h"
#include "core/Log.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace fw {

QString settingTypeName(SettingType type)
{
    switch (type) {
    case SettingType::Bool: return QStringLiteral("bool");
    case SettingType::Int: return QStringLiteral("int");
    case SettingType::Double: return QStringLiteral("double");
    case SettingType::String: return QStringLiteral("string");
    case SettingType::Enum: return QStringLiteral("enum");
    case SettingType::DirPath: return QStringLiteral("dir");
    case SettingType::FilePath: return QStringLiteral("file");
    case SettingType::StringList: return QStringLiteral("list");
    }
    return QStringLiteral("unknown");
}

const QStringList& SettingsSchema::sectionOrder()
{
    static const QStringList order = {
        QStringLiteral("General"),     QStringLiteral("Build"),        QStringLiteral("FPKG"),
        QStringLiteral("PFS"),         QStringLiteral("PlayGo"),       QStringLiteral("Backport"),     QStringLiteral("DLC"),
        QStringLiteral("Firmware and SDK"), QStringLiteral("Tools"),   QStringLiteral("Performance"),
        QStringLiteral("Storage"),     QStringLiteral("Verification"), QStringLiteral("PS5 connection"),
        QStringLiteral("Logging"),     QStringLiteral("Advanced"),
    };
    return order;
}

void SettingsSchema::add(SettingDef def)
{
    Q_ASSERT_X(!def.key.isEmpty(), "SettingsSchema::add", "empty key");
    Q_ASSERT_X(sectionOrder().contains(def.section), "SettingsSchema::add", qPrintable(def.section));
    if (auto it = m_index.find(def.key); it != m_index.end()) {
        m_defs[it->second] = std::move(def); // re-registration replaces (tests)
        return;
    }
    m_index.emplace(def.key, m_defs.size());
    m_defs.push_back(std::move(def));
}

const SettingDef* SettingsSchema::find(const QString& key) const
{
    auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_defs[it->second];
}

std::vector<const SettingDef*> SettingsSchema::all() const
{
    std::vector<const SettingDef*> out;
    out.reserve(m_defs.size());
    for (const auto& d : m_defs)
        out.push_back(&d);
    const auto& order = sectionOrder();
    std::stable_sort(out.begin(), out.end(), [&](const SettingDef* a, const SettingDef* b) {
        const auto sa = order.indexOf(a->section), sb = order.indexOf(b->section);
        if (sa != sb)
            return sa < sb;
        return a->order < b->order;
    });
    return out;
}

std::vector<const SettingDef*> SettingsSchema::inSection(const QString& section) const
{
    std::vector<const SettingDef*> out;
    for (const auto* d : all())
        if (d->section == section)
            out.push_back(d);
    return out;
}

QStringList SettingsSchema::sections() const
{
    QStringList out;
    for (const auto& s : sectionOrder())
        if (std::any_of(m_defs.begin(), m_defs.end(), [&](const SettingDef& d) { return d.section == s; }))
            out << s;
    return out;
}

Result<QVariant> SettingsSchema::normalize(const QString& key, const QVariant& value, bool runValidator) const
{
    const SettingDef* def = find(key);
    if (!def)
        return Error(ErrorCode::NotFound, QStringLiteral("Unknown setting \"%1\".").arg(key));

    QVariant out;
    const auto bad = [&](const QString& why) {
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1: %2").arg(def->label, why));
    };

    switch (def->type) {
    case SettingType::Bool: {
        if (value.typeId() == QMetaType::Bool) {
            out = value.toBool();
        } else if (value.typeId() == QMetaType::QString) {
            const QString s = value.toString().trimmed().toLower();
            if (s == u"true" || s == u"1" || s == u"yes" || s == u"on") out = true;
            else if (s == u"false" || s == u"0" || s == u"no" || s == u"off") out = false;
            else return bad(QStringLiteral("expected on or off."));
        } else if (value.canConvert<int>()) {
            out = value.toInt() != 0;
        } else {
            return bad(QStringLiteral("expected on or off."));
        }
        break;
    }
    case SettingType::Int: {
        bool ok = false;
        double d = value.toDouble(&ok);
        if (!ok || std::floor(d) != d)
            return bad(QStringLiteral("expected a whole number."));
        if (def->maximum > def->minimum && (d < def->minimum || d > def->maximum))
            return bad(QStringLiteral("must be between %1 and %2.").arg(def->minimum).arg(def->maximum));
        out = qint64(d);
        break;
    }
    case SettingType::Double: {
        bool ok = false;
        double d = value.toDouble(&ok);
        if (!ok || !std::isfinite(d))
            return bad(QStringLiteral("expected a number."));
        if (def->maximum > def->minimum && (d < def->minimum || d > def->maximum))
            return bad(QStringLiteral("must be between %1 and %2.").arg(def->minimum).arg(def->maximum));
        out = d;
        break;
    }
    case SettingType::Enum: {
        const QString s = value.toString();
        const bool known = std::any_of(def->choices.begin(), def->choices.end(),
                                       [&](const SettingChoice& c) { return c.value == s; });
        if (!known)
            return bad(QStringLiteral("\"%1\" is not one of the available options.").arg(s));
        out = s;
        break;
    }
    case SettingType::String:
    case SettingType::DirPath:
    case SettingType::FilePath:
        if (!value.canConvert<QString>())
            return bad(QStringLiteral("expected text."));
        out = value.toString();
        break;
    case SettingType::StringList:
        out = value.toStringList();
        break;
    }

    if (runValidator && def->validator) {
        const QString why = def->validator(out);
        if (!why.isEmpty())
            return bad(why);
    }
    return out;
}

void SettingsSchema::addLegacyKey(const QString& legacyKey, const QString& key, LegacyConverter convert)
{
    m_legacy[legacyKey] = LegacyMapping{key, std::move(convert)};
}

// ---------------------------------------------------------------------------

SettingsValues::SettingsValues(std::shared_ptr<const SettingsSchema> schema, QVariantMap values, QVariantMap overrides)
    : m_schema(std::move(schema)), m_values(std::move(values)), m_overrides(std::move(overrides))
{
}

SettingsValues SettingsValues::withOverrides(const QVariantMap& extra) const
{
    SettingsValues v = *this;
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        v.m_overrides.insert(it.key(), it.value());
    return v;
}

QVariant SettingsValues::value(const QString& key) const
{
    if (auto it = m_overrides.constFind(key); it != m_overrides.cend())
        return *it;
    if (auto it = m_values.constFind(key); it != m_values.cend())
        return *it;
    if (m_schema)
        if (const auto* d = m_schema->find(key))
            return d->defaultValue;
    Logger::instance().log(LogLevel::Warning, QStringLiteral("settings"),
                           QStringLiteral("Read of undeclared setting \"%1\"").arg(key));
    return {};
}

QVariantMap SettingsValues::effective() const
{
    QVariantMap out;
    if (m_schema)
        for (const auto* d : m_schema->all())
            out.insert(d->key, value(d->key));
    return out;
}

// ---------------------------------------------------------------------------

namespace {
std::map<int, SettingsStore::Migration>& migrations()
{
    static std::map<int, SettingsStore::Migration> m;
    return m;
}
} // namespace

void SettingsStore::registerMigration(int fromVersion, Migration fn)
{
    migrations()[fromVersion] = std::move(fn);
}

SettingsStore::SettingsStore(std::shared_ptr<const SettingsSchema> schema, QString filePath, QObject* parent)
    : QObject(parent), m_schema(std::move(schema)), m_path(std::move(filePath))
{
}

Status SettingsStore::load()
{
    QVariantMap loaded;
    QJsonObject foreign;
    QStringList warnings;

    QFile f(m_path);
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read the settings file."), f.errorString());
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
        f.close();
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString aside = m_path + QStringLiteral(".corrupt-")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
            QFile::rename(m_path, aside);
            warnings << QStringLiteral("The settings file was damaged and has been set aside as %1. Defaults are in use.")
                            .arg(QFileInfo(aside).fileName());
        } else {
            QJsonObject root = doc.object();
            int version = root.value(QStringLiteral("schemaVersion")).toInt(1);
            QJsonObject values = root.value(QStringLiteral("values")).toObject();
            while (version < kSchemaVersion) {
                if (auto it = migrations().find(version); it != migrations().end())
                    it->second(values);
                ++version;
            }
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (!m_schema->contains(it.key())) {
                    // Another build of the app (a private one with more
                    // engines, a newer version) owns it: kept as it is.
                    foreign.insert(it.key(), it.value());
                    warnings << QStringLiteral("Kept the setting \"%1\" for the build that knows it.").arg(it.key());
                    continue;
                }
                auto norm = m_schema->normalize(it.key(), it.value().toVariant(), /*runValidator=*/false);
                if (!norm.ok()) {
                    warnings << QStringLiteral("Reset \"%1\" to its default: %2").arg(it.key(), norm.error().message);
                    continue;
                }
                loaded.insert(it.key(), norm.value());
            }
        }
    }

    {
        std::lock_guard lock(m_mutex);
        m_values = loaded;
        m_foreign = foreign;
        m_warnings = warnings;
    }
    for (const auto& w : warnings)
        Logger::instance().log(LogLevel::Warning, QStringLiteral("settings"), w);
    return success();
}

Status SettingsStore::save() const
{
    QJsonObject values;
    {
        std::lock_guard lock(m_mutex);
        // Settings of other builds first, so this build's own values win.
        for (auto it = m_foreign.constBegin(); it != m_foreign.constEnd(); ++it)
            values.insert(it.key(), it.value());
        for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
            values.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    root.insert(QStringLiteral("values"), values);
    return writeFileAtomic(m_path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QVariant SettingsStore::value(const QString& key) const
{
    std::lock_guard lock(m_mutex);
    if (auto it = m_values.constFind(key); it != m_values.cend())
        return *it;
    if (const auto* d = m_schema->find(key))
        return d->defaultValue;
    return {};
}

Status SettingsStore::setValue(const QString& key, const QVariant& value)
{
    auto norm = m_schema->normalize(key, value);
    if (!norm.ok())
        return norm.takeError();
    const QVariant v = norm.value();
    const SettingDef* def = m_schema->find(key);
    {
        std::lock_guard lock(m_mutex);
        if (def && v == def->defaultValue)
            m_values.remove(key); // store only non-defaults so default changes propagate
        else
            m_values.insert(key, v);
    }
    emit changed(key, v);
    return success();
}

bool SettingsStore::isDefault(const QString& key) const
{
    std::lock_guard lock(m_mutex);
    return !m_values.contains(key);
}

void SettingsStore::reset(const QString& key)
{
    {
        std::lock_guard lock(m_mutex);
        m_values.remove(key);
    }
    if (const auto* d = m_schema->find(key))
        emit changed(key, d->defaultValue);
}

void SettingsStore::resetSection(const QString& section)
{
    for (const auto* d : m_schema->inSection(section))
        reset(d->key);
    emit resetDone();
}

void SettingsStore::resetAll()
{
    QStringList keys;
    {
        std::lock_guard lock(m_mutex);
        keys = m_values.keys();
        m_values.clear();
    }
    for (const auto& k : keys)
        if (const auto* d = m_schema->find(k))
            emit changed(k, d->defaultValue);
    emit resetDone();
}

SettingsValues SettingsStore::snapshot(const QVariantMap& jobOverrides) const
{
    std::lock_guard lock(m_mutex);
    return SettingsValues(m_schema, m_values, jobOverrides);
}

Result<QVariantMap> SettingsStore::normalizeOverrides(const QVariantMap& overrides) const
{
    QVariantMap out;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        const SettingDef* def = m_schema->find(it.key());
        if (!def)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Unknown setting \"%1\".").arg(it.key()));
        if (!def->perJob)
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("\"%1\" can only be changed in Settings, not per job.").arg(def->label));
        auto norm = m_schema->normalize(it.key(), it.value());
        if (!norm.ok())
            return norm.takeError();
        out.insert(it.key(), norm.value());
    }
    return out;
}

QStringList SettingsStore::loadWarnings() const
{
    std::lock_guard lock(m_mutex);
    return m_warnings;
}

Result<int> SettingsStore::importLegacy(const QString& legacySettingsPath)
{
    QFile f(legacySettingsPath);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("The old settings file could not be opened."), f.errorString());
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return Error(ErrorCode::CorruptData, QStringLiteral("The old settings file is not valid JSON."));
    const QJsonObject o = doc.object();
    int imported = 0;
    for (const auto& [legacyKey, mapping] : m_schema->legacyKeys()) {
        if (!o.contains(legacyKey))
            continue;
        QVariant v = o.value(legacyKey).toVariant();
        if (mapping.convert) {
            auto c = mapping.convert(v);
            if (!c)
                continue;
            v = *c;
        }
        if (setValue(mapping.key, v).ok())
            ++imported;
        else
            Logger::instance().log(LogLevel::Warning, QStringLiteral("settings"),
                                   QStringLiteral("Legacy setting %1 not imported (invalid value)").arg(legacyKey));
    }
    for (const auto& importer : m_schema->legacyImporters()) {
        const QVariantMap values = importer(o);
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (setValue(it.key(), it.value()).ok())
                ++imported;
            else
                Logger::instance().log(LogLevel::Warning, QStringLiteral("settings"),
                                       QStringLiteral("Legacy value for %1 not imported (invalid value)").arg(it.key()));
        }
    }
    return imported;
}

} // namespace fw
