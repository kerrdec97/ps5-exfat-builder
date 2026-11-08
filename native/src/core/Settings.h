#pragma once

// Settings architecture.
//
//  * SettingsSchema — every setting the app supports, declared by the module
//    that consumes it (key, section, type, default, range/choices, help text,
//    whether a job may override it, which dependency it needs, which other
//    setting enables it). The Settings page is generated from the schema, so a
//    control cannot exist without a consumer.
//  * SettingsStore  — persisted global defaults (JSON, atomic save, schema
//    version + migrations, legacy Python settings import). Thread-safe.
//  * SettingsValues — an immutable snapshot of the store merged with per-job
//    overrides. Engines only ever read a snapshot, so editing settings while a
//    job runs never changes that job.

#include "core/Result.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fw {

enum class SettingType { Bool, Int, Double, String, Enum, DirPath, FilePath, StringList };

QString settingTypeName(SettingType type);

struct SettingChoice {
    QString value;
    QString label;
    QString description;
};

struct SettingDef {
    QString key;         // "fpkg.compressionLevel" — module prefix + camelCase name
    QString section;     // one of SettingsSchema::sectionOrder()
    QString group;       // sub-heading inside the section
    QString label;       // sentence case
    QString description; // what it does, when to change it
    SettingType type = SettingType::Bool;
    QVariant defaultValue;
    double minimum = 0;  // numeric range, used when maximum > minimum
    double maximum = 0;
    QString unit;        // "MiB", "threads", "%"
    QList<SettingChoice> choices; // Enum
    QString fileFilter;  // FilePath dialogs, e.g. "Executables (*.exe)"
    bool perJob = false;          // may be overridden for a single job
    bool advanced = false;        // hidden behind "Show advanced"
    bool restartRequired = false;
    QString enabledWhenKey;       // setting that must equal enabledWhenValue
    QVariant enabledWhenValue = true;
    QString dependencyId;         // DependencyRegistry id this setting relies on
    int order = 0;                // position inside the section
    // Extra validation; return an empty string when the value is acceptable.
    std::function<QString(const QVariant&)> validator;
};

class SettingsSchema {
public:
    // Canonical section order of the Settings page.
    static const QStringList& sectionOrder();

    void add(SettingDef def);
    const SettingDef* find(const QString& key) const;
    bool contains(const QString& key) const { return find(key) != nullptr; }
    std::vector<const SettingDef*> all() const;                 // section order, then order
    std::vector<const SettingDef*> inSection(const QString& section) const;
    QStringList sections() const;                               // only sections with settings

    // Coerces `value` to the setting's type. Returns an error message for
    // values that cannot be accepted (wrong type, out of range, unknown choice,
    // custom validator failure).
    // `runValidator` = false applies only type/range/choice checks: used when
    // loading, so e.g. a folder on an unplugged drive is kept, not reset.
    Result<QVariant> normalize(const QString& key, const QVariant& value, bool runValidator = true) const;

    // Legacy (Python app) key mapping for importLegacy().
    using LegacyConverter = std::function<std::optional<QVariant>(const QVariant&)>;
    void addLegacyKey(const QString& legacyKey, const QString& key, LegacyConverter convert = {});
    struct LegacyMapping {
        QString key;
        LegacyConverter convert;
    };
    const std::map<QString, LegacyMapping>& legacyKeys() const { return m_legacy; }

    // Whole-file legacy importers, for settings that combine several legacy
    // keys (e.g. merging two folder lists). Return new-key → value.
    using LegacyImporter = std::function<QVariantMap(const QJsonObject& legacyFile)>;
    void addLegacyImporter(LegacyImporter importer) { m_importers.push_back(std::move(importer)); }
    const std::vector<LegacyImporter>& legacyImporters() const { return m_importers; }

private:
    std::vector<SettingDef> m_defs;
    std::map<QString, std::size_t> m_index;
    std::map<QString, LegacyMapping> m_legacy;
    std::vector<LegacyImporter> m_importers;
};

class SettingsValues {
public:
    SettingsValues() = default;
    SettingsValues(std::shared_ptr<const SettingsSchema> schema, QVariantMap values, QVariantMap overrides);

    QVariant value(const QString& key) const;
    bool boolean(const QString& key) const { return value(key).toBool(); }
    qint64 integer(const QString& key) const { return value(key).toLongLong(); }
    double real(const QString& key) const { return value(key).toDouble(); }
    QString string(const QString& key) const { return value(key).toString(); }
    QStringList stringList(const QString& key) const { return value(key).toStringList(); }

    bool isOverridden(const QString& key) const { return m_overrides.contains(key); }
    const QVariantMap& overrides() const { return m_overrides; }
    QVariantMap effective() const; // every schema key → effective value
    // A copy with `extra` added to the per-job overrides (a composite job's
    // child that must run with some settings forced).
    SettingsValues withOverrides(const QVariantMap& extra) const;

private:
    std::shared_ptr<const SettingsSchema> m_schema;
    QVariantMap m_values;    // stored globals (non-default only)
    QVariantMap m_overrides; // per-job
};

class SettingsStore : public QObject {
    Q_OBJECT
public:
    static constexpr int kSchemaVersion = 1;

    SettingsStore(std::shared_ptr<const SettingsSchema> schema, QString filePath, QObject* parent = nullptr);

    const SettingsSchema& schema() const { return *m_schema; }
    std::shared_ptr<const SettingsSchema> schemaPtr() const { return m_schema; }
    QString filePath() const { return m_path; }

    // Loads and migrates. Invalid or unknown entries are dropped with a warning
    // (see loadWarnings()); a corrupt file is renamed to *.corrupt-<time> and
    // defaults are used.
    Status load();
    Status save() const;

    QVariant value(const QString& key) const;
    Status setValue(const QString& key, const QVariant& value);
    bool isDefault(const QString& key) const;
    void reset(const QString& key);
    void resetSection(const QString& section);
    void resetAll();

    SettingsValues snapshot(const QVariantMap& jobOverrides = {}) const;
    // Validates a set of per-job overrides against the schema (perJob only).
    Result<QVariantMap> normalizeOverrides(const QVariantMap& overrides) const;

    QStringList loadWarnings() const;

    // Imports the legacy Python app's settings.json using the schema's legacy
    // key table. Returns the number of imported values.
    Result<int> importLegacy(const QString& legacySettingsPath);

    // Migration hook: transforms a raw values object from `fromVersion` to
    // `fromVersion + 1`.
    using Migration = std::function<void(QJsonObject& values)>;
    static void registerMigration(int fromVersion, Migration fn);

signals:
    void changed(const QString& key, const QVariant& value);
    void resetDone();

private:
    std::shared_ptr<const SettingsSchema> m_schema;
    QString m_path;
    mutable std::mutex m_mutex;
    QVariantMap m_values;
    QStringList m_warnings;
};

} // namespace fw
