#pragma once

// Small UI-owned JSON stores under Paths::dataDir():
//   library.json  — scan locations and the last scan's games (Library/Backport)
//   payloads.json — the user's payload list (PS5 connect › Payloads)
// They are not settings (no schema, no per-job meaning); they are the app's
// own records, like history.

#include "app/services/GameInfo.h"
#include "core/Settings.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <vector>

namespace fw::ui {

struct LibraryGame {
    GameSummary info;
    QString status;      // "Backported" (in English, translated where shown) or empty
    QDateTime lastSeen;
    // backport.scan results (empty when not scanned by the backport engine)
    QStringList issues;  // "severity: message"
    bool canRun = true;
    QString plan;
};

// The scan folders are the "Library folders" setting (library.folders): the
// Library page, Settings › General › Library and a version 4 import show and
// change one list. library.json keeps a copy (an older version reads it); at
// start the folders of both are taken together, and an import adds its
// folders to them. Reset section and Reset all leave them (keptOnReset).
class LibraryStore : public QObject {
    Q_OBJECT
public:
    // Bound to the running app's settings (AppContext), when there are any.
    explicit LibraryStore(QObject* parent = nullptr);
    // Bound to `settings`; with nullptr the folders live in library.json only.
    explicit LibraryStore(SettingsStore* settings, QObject* parent = nullptr);
    QStringList scanFolders() const { return m_folders; }
    void setScanFolders(const QStringList& folders);
    void addScanFolder(const QString& folder);
    void removeScanFolder(const QString& folder);
    const std::vector<LibraryGame>& games() const { return m_games; }
    void setGames(std::vector<LibraryGame> games);   // replaces (after a scan)
    void upsertGame(const GameSummary& info, const QString& status = {});
    void setBackportInfo(const QString& folder, const QStringList& issues, bool canRun, const QString& plan);
    void removeGame(const QString& folder);
    void renameGame(const QString& folder, const QString& newFolder);
    QDateTime lastScan() const { return m_lastScan; }
    void setLastScan(const QDateTime& t);
    bool load();
    bool save() const;
signals:
    void changed();

private:
    void bindFolderSetting(SettingsStore* settings);
    QStringList settingFolders() const;
    void takeFolderSetting();  // the setting changed: the folders follow it
    void writeFolderSetting(); // the folders changed: the setting follows them

    QString m_path;
    QStringList m_folders;
    std::vector<LibraryGame> m_games;
    QDateTime m_lastScan;
    QPointer<SettingsStore> m_settings;
    bool m_writingSetting = false;
};

struct PayloadEntry {
    QString name;
    QString path;
    QString notes;
    QDateTime lastSent;
};

class PayloadStore : public QObject {
    Q_OBJECT
public:
    explicit PayloadStore(QObject* parent = nullptr);
    const std::vector<PayloadEntry>& entries() const { return m_entries; }
    void add(const PayloadEntry& e);
    void update(int index, const PayloadEntry& e);
    void remove(int index);
    void markSent(const QString& path);
    bool load();
    bool save() const;
signals:
    void changed();

private:
    QString m_path;
    std::vector<PayloadEntry> m_entries;
};

} // namespace fw::ui
