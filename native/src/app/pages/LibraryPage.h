#pragma once

// Library: tabs Games (dumps found in the scan locations: build, package,
// backport, rename, show in folder) and Languages (the language stripper:
// scan, keep/remove per language, backup, restore).

#include "app/pages/BackportPage.h"
#include "app/pages/DlcEmuEditor.h"
#include "app/services/AppStores.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>

namespace fw::pages {

class LibraryPage : public Page {
    Q_OBJECT
public:
    LibraryPage(AppContext& ctx, JobQueueModel& queueModel, ui::LibraryStore& library, QWidget* parent = nullptr);
    void refresh() override;
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    QString statusContext() const override;
    QStringList demoStates() const override { return {QStringLiteral("languages"), QStringLiteral("dlc")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

private:
    // Games
    void rebuildGames();
    void addFolders();
    void scan();
    void renameGame(const QString& folder);
    void renameDumps();
    void showRenamePreview(const QJsonArray& items, const QString& preset);
    void applyRename(const QJsonArray& items);
    void gameMenu(const QString& folder, const QPoint& pos);
    // Languages
    void languageScan();
    void rebuildLanguages();
    void applyQuickKeep(const QStringList& keepCodes);
    void stripLanguages();
    void restoreLanguages();
    void updateLanguageSummary();
    void onJobFinished(const QString& id);

    ui::LibraryStore& m_library;
    ui::GameInfoReader* m_reader;
    QTabBar* m_tabs;
    QStackedWidget* m_stack;
    // games
    ui::Card* m_gamesCard;
    QLabel* m_gamesCount;
    QLineEdit* m_search;
    QWidget* m_locationsRow;
    QHBoxLayout* m_locationsLayout;
    GameTable* m_table;
    ui::EmptyState* m_empty;
    QLabel* m_footer;
    QString m_scanJobId;
    QStringList m_pendingAdds;
    QString m_renamePreviewId, m_renameApplyId, m_renameUndoId;
    QString m_renameSource, m_renamePreset, m_renameDest;
    // languages
    ui::PathField* m_langFolder;
    QCheckBox* m_langBatch;
    QPushButton* m_langScan;
    ui::UnavailableState* m_langUnavailable;
    QTableWidget* m_langTable;
    ui::EmptyState* m_langEmpty;
    QWidget* m_chips;
    QLabel* m_langSummary;
    QPushButton* m_langStrip;
    QPushButton* m_langRestore;
    DlcEmuEditor* m_dlc;
    QString m_langJobId;
    QJsonArray m_langGames; // last scan result
};

} // namespace fw::pages
