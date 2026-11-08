#pragma once

// Library: tabs Games, Images, Languages and DLC emulator.
//
//  * Games  — the dumps found in the library folders as a cover-art grid or a
//             list (LibraryGrid): title, ID, version, size and the formats this
//             game exists in (its folder plus any image of it found in the
//             library and output folders); search, filter, sort; bulk build /
//             backport / remove; per game build, FPKG, backport, languages,
//             DLC list, rename, show in folder.
//  * Images — .exfat / .ffpkg / .ffpfs / .ffpfsc files in the library folders,
//             their sub-folders and the default output folder, with the title
//             and ID read from inside each image (services/ImageInfo); open in
//             Image tools, verify, rename (by title ID, name and version).
//  * Languages — the language stripper: scan, keep/remove per language, backup, restore.
//  * DLC emulator — the dlc_emu.ini editor (DlcEmuEditor).

#include "app/pages/DlcEmuEditor.h"
#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/services/ImageInfo.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>

namespace fw::ui {
class DropZone;
class LibraryGrid;
} // namespace fw::ui

namespace fw::pages {

class LibraryPage : public Page {
    Q_OBJECT
public:
    enum Tab { GamesTab = 0, ImagesTab, LanguagesTab, DlcTab };

    LibraryPage(AppContext& ctx, JobQueueModel& queueModel, ui::LibraryStore& library, QWidget* parent = nullptr);
    void refresh() override;
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void setDragHighlight(bool on) override;
    void navigateWith(const QVariantMap& args) override;
    QString statusContext() const override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

    void setTab(Tab tab);
    Tab tab() const { return Tab(m_tabs->currentIndex()); }
    ui::LibraryGrid* gamesView() { return m_gamesGrid; }
    ui::LibraryGrid* imagesView() { return m_imagesGrid; }
    // The image scan: library folders, their sub-folders and the default output folder.
    void scanImages();
    bool isScanningImages() const;
    const QList<ui::ImageSummary>& images() const { return m_images; }
    QStringList imageFolders() const;

    // "PPSA01325 Astro Bot (01.002.000).exfat" for preset "id" | "id-title" | "id-title-version".
    static QString imageFileName(const ui::ImageSummary& image, const QString& preset);
    // Renames image files (never over an existing file). Returns how many were renamed.
    int renameImages(const QList<QPair<QString, QString>>& renames, QStringList* skipped = nullptr);

private:
    // Games
    QWidget* buildGamesTab();
    QWidget* buildImagesTab();
    QWidget* buildLanguagesTab();
    void rebuildGames();
    void rebuildFolders();
    void updateGameSelection();
    void addFolders();
    void addFolder(const QString& dir);
    void scan();
    void renameGame(const QString& folder);
    void renameDumps();
    void showRenamePreview(const QJsonArray& items, const QString& preset);
    void applyRename(const QJsonArray& items);
    void gameMenu(const QString& folder, const QPoint& pos);
    void buildSelected();
    const std::vector<ui::LibraryGame>& games() const;
    const ui::LibraryGame* game(const QString& folder) const;
    QStringList formatsOf(const ui::GameSummary& g) const;
    // Images
    void rebuildImages();
    void updateImageSelection();
    void imageMenu(const QString& path, const QPoint& pos);
    void openInImageTools(const QString& image, const QString& action);
    void verifyImages(const QStringList& paths);
    void renameImagesSheet(const QStringList& paths);
    const ui::ImageSummary* image(const QString& path) const;
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
    ui::ImageScanner* m_scanner;
    QTabBar* m_tabs;
    QStackedWidget* m_stack;
    QWidget* m_headerActions = nullptr;

    // games
    QLineEdit* m_search;
    QComboBox* m_filter;
    QComboBox* m_sort;
    QLabel* m_gamesCount;
    ui::Segmented* m_gamesView;
    QWidget* m_folders;
    ui::Card* m_gamesSelection;
    QLabel* m_gamesSelectionText;
    ui::LibraryGrid* m_gamesGrid;
    ui::DropZone* m_gamesDrop;
    ui::EmptyState* m_gamesNoMatch;
    ui::EmptyState* m_gamesNone;
    QString m_scanJobId;
    QStringList m_pendingAdds;
    QString m_renamePreviewId, m_renameApplyId, m_renameUndoId;
    QString m_renameSource, m_renamePreset, m_renameDest;

    // images
    QLineEdit* m_imageSearch;
    QComboBox* m_imageFormat;
    QComboBox* m_imageSort;
    QLabel* m_imagesCount;
    ui::Segmented* m_imagesView;
    ui::Card* m_imagesSelection;
    QLabel* m_imagesSelectionText;
    QPushButton* m_imagesOpen;
    ui::LibraryGrid* m_imagesGrid;
    ui::EmptyState* m_imagesEmpty;
    ui::EmptyState* m_imagesNoMatch;
    QList<ui::ImageSummary> m_images;
    QString m_imageFoldersKey; // the folders the last scan covered

    // languages
    ui::PathField* m_langFolder;
    QCheckBox* m_langBatch;
    QPushButton* m_langScan;
    ui::UnavailableState* m_langUnavailable;
    ui::Card* m_langCard;
    QTableWidget* m_langTable;
    ui::EmptyState* m_langEmpty;
    QWidget* m_chips;
    QLabel* m_langSummary;
    QPushButton* m_langStrip;
    QPushButton* m_langRestore;
    DlcEmuEditor* m_dlc;
    QString m_langJobId;
    QJsonArray m_langGames; // last scan result

    // screenshots
    bool m_demo = false;
    std::vector<ui::LibraryGame> m_demoGames;
};

} // namespace fw::pages
