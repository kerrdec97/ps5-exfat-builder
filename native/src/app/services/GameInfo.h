#pragma once

// Lightweight, UI-side game folder summary for the Source cards and manual
// library rows: reads sce_sys/param.json and totals the tree on a worker
// thread (QtConcurrent) and posts the result back. The gameinfo engine owns
// the full metadata pipeline; this exists so a page can show "Ghost of Yotei ·
// PPSA01325 · 48.2 GB · 18,442 files" the moment a folder is chosen.
//
// A folder that is not a game itself (a wrapper such as "D:\Dumps\Spider-Man"
// holding "PPSA01234-app\eboot.bin") is searched for game folders below it
// (gameinfo::findGameRoots: depth 5, at most 4,000 folders), and the result
// lists them in GameSummary::gameRoots so the page can pick or offer them.
// Its files are not counted, so choosing a drive root never walks the drive.

#include "core/Cancellation.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace fw::ui {

struct GameSummary {
    QString folder;
    QString title;
    QString titleId;     // PPSA01325
    QString contentId;   // UP9000-PPSA01325_00-...
    QString version;     // 01.000.000
    QString sdk;         // "9.00" when known
    QString requiredFw;  // "4.50" (requiredSystemSoftwareVersion) when known
    QString platform;    // "PS4" / "PS5" (library.scan)
    QString kind;        // "Game" / "Patch" / "Add-on"
    QString coverPath;   // normalised cover PNG (library.scan)
    qint64 bytes = 0;
    qint64 files = 0;
    bool isGame = false; // has sce_sys/param.json or eboot.bin
    bool exists = false;
    QString error;
    QStringList gameRoots;      // not a game: folders holding eboot.bin below it (absolute)
    bool rootsTruncated = false; // the search stopped at its folder limit
    bool cancelled = false;      // a newer request replaced this one
    QString metaLine() const; // "PPSA01325 · v01.000.000 · 48.2 GB · 18,442 files · SDK 9.00"
    QJsonObject toJson() const;
    static GameSummary fromJson(const QJsonObject& o);
};

// One reader per page; summarize() cancels the previous request (its worker
// stops at the next file instead of finishing a stale walk).
class GameInfoReader : public QObject {
    Q_OBJECT
public:
    explicit GameInfoReader(QObject* parent = nullptr);
    ~GameInfoReader() override;
    void summarize(const QString& folder, bool countFiles = true);
    void cancel();
    bool isBusy() const { return m_busy; }
    // Not a game folder: search below it for game folders (GameSummary::gameRoots)
    // instead of counting its files. Off by default.
    void setFindGameRoots(bool on) { m_findRoots = on; }
signals:
    void ready(const fw::ui::GameSummary& summary);
    // While counting files: at most every 250 ms.
    void progress(qint64 files, qint64 bytes);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_findRoots = false;
    std::shared_ptr<CancellationSource> m_cancel;
};

// Synchronous helper (worker threads and tests): never throws. `onProgress`
// is called from the calling thread at most every 250 ms while counting.
// `findRoots`: see GameInfoReader::setFindGameRoots.
GameSummary readGameSummary(const QString& folder, bool countFiles, const CancellationToken& token = {},
                            const std::function<void(qint64 files, qint64 bytes)>& onProgress = {},
                            bool findRoots = false);

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::GameSummary)
