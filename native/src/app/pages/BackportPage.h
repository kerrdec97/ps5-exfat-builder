#pragma once

// Backport (Backport artboard): Game library table · Target/Options/Folders
// (schema-driven, section "Backport") · Activity. Scans go through
// backport.scan or library.scan; folders can also be added by hand.
// Per game (row menu): SDK fix on a copy or in place (backport.sdkFix), undo
// the last SDK fix, read-only audio report (backport.audioScan), open the
// last run report. "Open report" in Activity opens the newest report of a
// backport, SDK fix or audio report. AMPR/PlayGo pins (backport.pinProviders)
// and shortcuts to the PS5 connect page (payloads, kernel log).

#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/services/GameInfo.h"
#include "app/ui/LogView.h"

#include <QHash>
#include <QTableWidget>

namespace fw::pages {

class GameTable : public QTableWidget {
    Q_OBJECT
public:
    explicit GameTable(bool selectable, QWidget* parent = nullptr);
    void setGames(const std::vector<ui::LibraryGame>& games, const QString& filter);
    QStringList checkedFolders() const;
    QString currentFolder() const;
    void setAllChecked(bool on);
    int gameCount() const { return m_count; }
    void setStatusText(const QString& folder, const QString& text, ui::Tone tone);
    static QPixmap coverPixmap(const QString& coverPath, int px);
signals:
    void checkedChanged();
    void contextRequested(const QString& folder, const QPoint& globalPos);
    void activatedFolder(const QString& folder);

private:
    bool m_selectable;
    int m_count = 0;
};

class BackportPage : public Page {
    Q_OBJECT
public:
    BackportPage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, ui::LibraryStore& library,
                 QWidget* parent = nullptr);
    bool hasPrimaryAction() const override { return true; }
    void primaryAction() override;
    void refresh() override;
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    QString statusContext() const override;
    void setDragHighlight(bool on) override;
    // Screenshots: "report" (the Open report link after a run), "pins" (the
    // AMPR/PlayGo card at the end of the options).
    QStringList demoStates() const override { return {QStringLiteral("report"), QStringLiteral("pins")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

private:
    void rebuildTable();
    void updateCounts();
    void addFolders();
    void addGameFolder(const QString& folder);
    void scan();
    void restoreOriginals();
    void onJobFinished(const QString& id);
    void fixSdk(const QString& folder, bool inPlace);
    void undoSdkFix(const QString& folder);
    void audioReport(const QString& folder);
    void pinProviders();
    void openReport(const QString& htmlPath);
    void refreshProviderBar();
    Result<QString> enqueueBackportJob(const QString& kind, const QJsonObject& params, const QString& notStarted);

    JobQueueModel& m_model;
    ui::LibraryStore& m_library;
    ui::GameInfoReader* m_reader;
    ui::Card* m_tableCard;
    QLabel* m_counts;
    QLineEdit* m_search;
    QLabel* m_locations;
    GameTable* m_table;
    ui::EmptyState* m_empty;
    ui::UnavailableState* m_unavailable;
    QLabel* m_footerLeft;
    QLabel* m_footerRight;
    QPushButton* m_backport;
    QPushButton* m_restore;
    ui::JobOptionsPanel* m_options;
    ui::ActivityWell* m_activity;
    ui::Link* m_openReport;
    ui::InfoBar* m_providers;
    QString m_scanJobId;
    QStringList m_pendingAdds;
    QString m_lastReport;               // newest run report (HTML) of this session
    QHash<QString, QString> m_reports;  // game folder → its newest report
};

} // namespace fw::pages
