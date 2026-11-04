#pragma once

// Small UI-owned JSON stores under Paths::dataDir():
//   library.json  — scan locations and the last scan's games (Library/Backport)
//   payloads.json — the user's payload list (PS5 connect › Payloads)
// They are not settings (no schema, no per-job meaning); they are the app's
// own records, like history.

#include "app/services/GameInfo.h"

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <vector>

namespace fw::ui {

struct LibraryGame {
    GameSummary info;
    QString status;      // free text from the last scan / backport ("Not backported", ...)
    QDateTime lastSeen;
    // backport.scan results (empty when not scanned by the backport engine)
    QStringList issues;  // "severity: message"
    bool canRun = true;
    QString plan;
};

class LibraryStore : public QObject {
    Q_OBJECT
public:
    explicit LibraryStore(QObject* parent = nullptr);
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
    QString m_path;
    QStringList m_folders;
    std::vector<LibraryGame> m_games;
    QDateTime m_lastScan;
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
