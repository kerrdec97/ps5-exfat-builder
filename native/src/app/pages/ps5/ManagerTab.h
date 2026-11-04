#pragma once

// PS5 connect › Manager (legacy PS5 › Manager game list, exfat_builder.py
// E:13196-13670, ui/tab_ps5_mgr.py): the games and images in the console's
// games folder(s) next to the images in a local folder (On PS5 / Size
// mismatch / Local only / PS5 only), free space from SITE AVAIL, and a
// file-by-file compare of a console folder with a local dump folder or an
// .exfat/.ffpkg image (connect/ps5/ConsoleGames.h).

#include "app/pages/ps5/ConsoleToolTab.h"
#include "connect/ps5/ConsoleGames.h"

#include <optional>

namespace fw::pages {

class Ps5ManagerTab : public ConsoleToolTab {
    Q_OBJECT
public:
    Ps5ManagerTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;

    // Test hooks (no dialogs).
    void setConsoleFolders(const QString& folders);
    void setLocalFolder(const QString& folder);
    void refresh();
    void compare(const QString& remoteFolder, const QString& localSource);
    void setStatusFilter(int index); // 0 all, 1 on PS5, 2 size mismatch, 3 local only, 4 PS5 only
    QTableWidget* gamesTable() { return m_games; }
    QTableWidget* compareTable() { return m_diffs; }
    const std::vector<ps5::GameRow>& rows() const { return m_rows; }
    const std::optional<ps5::CompareReport>& report() const { return m_report; }
    QString freeSpaceText() const;
    QString compareSummary() const;

private:
    void showGames();
    void showReport();
    void setStats(int remote, int local, std::optional<qint64> freeBytes, const QString& freeText, qint64 imageBytes);
    int selectedGameRow() const; // index into m_rows, or -1
    void updateActions();
    void uploadSelected();
    void compareSelected();
    void startCompare();
    void browseConsole();
    void chooseImage();

    // games
    QLineEdit* m_folders;
    ui::PathField* m_localFolder;
    QPushButton* m_refresh;
    QLabel* m_statRemote;
    QLabel* m_statLocal;
    QLabel* m_statFree;
    QLabel* m_statImages;
    QLabel* m_statRemoteCaption;
    QLabel* m_statLocalCaption;
    QLabel* m_statImagesCaption;
    QLabel* m_statFreeCaption;
    ui::Segmented* m_filter;
    QLineEdit* m_search;
    QTableWidget* m_games;
    ui::EmptyState* m_gamesEmpty;
    QLabel* m_gamesFooter;
    QPushButton* m_upload;
    QPushButton* m_compareRow;
    QPushButton* m_openLocal;
    std::vector<ps5::GameRow> m_rows;
    std::vector<int> m_shown; // table row -> m_rows index
    QStringList m_folderErrors;
    // compare
    QLineEdit* m_cmpRemote;
    QPushButton* m_cmpBrowse;
    ui::PathField* m_cmpLocal;
    QPushButton* m_cmpImage;
    QPushButton* m_cmpGo;
    QPushButton* m_cmpCancel;
    ui::Segmented* m_cmpFilter;
    QTableWidget* m_diffs;
    ui::EmptyState* m_cmpEmpty;
    QLabel* m_cmpFooter;
    std::optional<ps5::CompareReport> m_report;
    bool m_comparing = false;
};

} // namespace fw::pages
