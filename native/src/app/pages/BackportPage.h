#pragma once

// Backport (Backport artboard): Game library table · Target/Options/Folders
// (schema-driven, section "Backport") · Activity. Scans go through
// backport.scan or library.scan; folders can also be added by hand.

#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/services/GameInfo.h"
#include "app/ui/LogView.h"

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

private:
    void rebuildTable();
    void updateCounts();
    void addFolders();
    void addGameFolder(const QString& folder);
    void scan();
    void restoreOriginals();
    void onJobFinished(const QString& id);

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
    QString m_scanJobId;
    QStringList m_pendingAdds;
};

} // namespace fw::pages
