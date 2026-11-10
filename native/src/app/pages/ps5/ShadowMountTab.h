#pragma once

// PS5 connect › ShadowMount+ › Settings: the ShadowMount+ config as a form
// built from the key schema (connect/ps5/MountConfig.h), for the 1.6 flat,
// 1.7 flat and v2 sectioned formats, plus the drive actions:
//
//   * Load reads config.ini, profiles.ini and the running version (HTTP API,
//     else the debug.log banner) and detects the format.
//   * Push writes only the keys that changed, into the file as it was loaded
//     (comments, unknown keys and [smb:*] sections stay), after checking that
//     nobody changed the file on the console since it was loaded.
//   * Safe to unplug (STOP file, confirmed from the log), Eject USB (1.7 API,
//     keeps ShadowMount+ running) and Send loader.
//   * MicroMount (archived upstream) keeps its key/value editor, folded away.

#include "app/pages/ps5/ConsoleToolTab.h"
#include "connect/ps5/MountConfig.h"

#include <QHash>
#include <QSet>

class QComboBox;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;

namespace fw::ui {
class ActionCard;
class Disclosure;
} // namespace fw::ui

namespace fw::pages {

class ShadowMountTab : public ConsoleToolTab {
    Q_OBJECT
public:
    ShadowMountTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;

    // The version the page's service check found ("" = unknown).
    void setApiVersion(const QString& version);
    void load();
    void push(bool confirmed = false);
    void safeToUnplug();
    void ejectUsb();
    void sendLoader();

    // Test hooks
    ps5::MountConfigValues loadedValues() const { return m_loaded; }
    ps5::MountConfigValues editedValues() const;
    ps5::MountConfigValues changes() const { return ps5::changedMountValues(m_loaded, editedValues()); }
    ps5::SmpConfigFormat format() const { return m_format; }
    bool hasLoaded() const { return m_hasLoaded; }
    QString statusText() const;
    void setValue(const QString& key, const QString& value);
    QString value(const QString& key) const;
    void resetToDefault(const QString& key);

signals:
    void loaded();

private:
    struct Row {
        const ps5::SmpKey* def = nullptr;
        QWidget* box = nullptr;
        QLabel* label = nullptr;
        ui::Switch* toggle = nullptr;
        QSpinBox* spin = nullptr;
        QComboBox* combo = nullptr;
        QLineEdit* edit = nullptr;
        QPlainTextEdit* text = nullptr;
        ui::IconButton* reset = nullptr;
    };
    QWidget* buildRow(const ps5::SmpKey& k, QWidget* parent);
    QString rowValue(const Row& r) const;
    void setRowValue(Row& r, const QString& v);
    void populate(const ps5::MountConfigValues& values);
    void refreshMarks();
    void setStatus(const QString& text, ui::Tone tone);
    void onStopJob(const QString& id); // the Safe to unplug job changed or ended
    void updateHeader();
    void finishLoad(const ps5::SmpRemoteState& state, const QString& consoleHost);
    QWidget* buildMicroMount(QWidget* parent);

    QHash<QString, Row> m_rows;
    QSet<QString> m_reset; // keys the user reset to the default (removed on push)
    bool m_populating = false;
    bool m_hasLoaded = false;
    QString m_loadedHost; // the console the shown config came from
    QString m_apiVersion;
    QString m_logVersion;
    ps5::MountConfigValues m_loaded;
    QString m_config, m_profiles;
    bool m_configExists = false, m_profilesExists = false;
    ps5::SmpConfigFormat m_format = ps5::SmpConfigFormat::Flat17;

    ui::IconTile* m_tile;
    QLabel* m_meta;
    ui::Badge* m_formatBadge;
    QPushButton* m_load;
    QPushButton* m_push;
    ui::MessageRow* m_status;
    ui::InfoBar* m_problems;
    ui::ActionCard* m_stop;
    ui::ActionCard* m_eject;
    ui::ActionCard* m_loader;
    QWidget* m_unknownBox;
    QLabel* m_unknownText;
    ui::Disclosure* m_unknown;
    QList<ui::Disclosure*> m_groups;
    QString m_stopJobId; // the Safe to unplug job the status line follows
    // MicroMount (archived)
    QTableWidget* m_mmTable = nullptr;
    QLabel* m_mmStatus = nullptr;
    QPushButton* m_mmPush = nullptr;
};

} // namespace fw::pages
