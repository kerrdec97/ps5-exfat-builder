#pragma once

// Lightweight, UI-side game folder summary for the Source cards and manual
// library rows: reads sce_sys/param.json and totals the tree on a worker
// thread (QtConcurrent) and posts the result back. The gameinfo engine owns
// the full metadata pipeline; this exists so a page can show "Ghost of Yotei ·
// PPSA01325 · 48.2 GB · 18,442 files" the moment a folder is chosen.

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace fw::ui {

struct GameSummary {
    QString folder;
    QString title;
    QString titleId;     // PPSA01325
    QString contentId;   // UP9000-PPSA01325_00-...
    QString version;     // 01.000.000
    QString sdk;         // "9.00" when known
    QString requiredFw;
    qint64 bytes = 0;
    qint64 files = 0;
    bool isGame = false; // has sce_sys/param.json or eboot.bin
    bool exists = false;
    QString error;
    QString metaLine() const; // "PPSA01325 · v01.000.000 · 48.2 GB · 18,442 files · SDK 9.00"
    QJsonObject toJson() const;
    static GameSummary fromJson(const QJsonObject& o);
};

// One reader per page; summarize() cancels the previous request.
class GameInfoReader : public QObject {
    Q_OBJECT
public:
    explicit GameInfoReader(QObject* parent = nullptr);
    void summarize(const QString& folder, bool countFiles = true);
    bool isBusy() const { return m_busy; }
signals:
    void ready(const fw::ui::GameSummary& summary);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
};

// Synchronous helper (worker threads and tests): never throws.
GameSummary readGameSummary(const QString& folder, bool countFiles);
// Default output file name: "{id}_{title}_v{version}" sanitised, no extension.
QString defaultImageStem(const GameSummary& s);

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::GameSummary)
