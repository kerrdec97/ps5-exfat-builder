#pragma once

// DLC (the ppr-pkg Companion's DLC Converter): turn DLC INIs (dlc_emu.ini and
// other DLC lists) and DLC .pkg files into installable PS5 DLC packages.
//
//   DLC to convert   drop zone -> table of entries (icon, title, content ID,
//                    title ID, region, version, source, result). Titles and
//                    IDs can be edited in place; double-click the icon to pick
//                    icon0.png. Change region... retargets the selected rows
//                    (online lookup of the game's regional releases, or manual
//                    identifiers); Preview changes shows the plan; Save updated
//                    INIs writes "<input>-<TitleId>-updated.ini" files.
//   Save to          the output folder.
//   Options          entitlement key and PKG source passcode; the engine
//                    options live in Advanced options (Settings > DLC).
//
// The conversion runs as a dlc.convert job in the queue; the DLC engine is
// fpkg-cli (Settings > Tools).

#include "app/pages/Page.h"
#include "app/ui/Components.h"
#include "engines/dlc/DlcConvert.h"
#include "engines/dlc/DlcIni.h"
#include "engines/dlc/DlcLookup.h"
#include "core/Cancellation.h"
#include "engines/dlc/DlcPackage.h"

#include <QHash>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSet>
#include <QTableWidget>

#include <memory>
#include <optional>
#include <vector>

namespace fw::pages {

class DlcPage : public JobFormPage {
    Q_OBJECT
public:
    DlcPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    ~DlcPage() override;

    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void setDragHighlight(bool on) override;
    void navigateWith(const QVariantMap& args) override; // {"files": [...]}
    QString statusContext() const override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

    // Adds .ini / .pkg inputs (INIs synchronously, packages in the background).
    void addInputs(const QStringList& paths);
    void clearList();
    int rowCount() const { return int(m_rows.size()); }
    QTableWidget* table() const { return m_table; }
    std::vector<dlc::DlcItem> items() const; // the rows as convert items (current edits)
    QString outputFolder() const;
    void setOutputFolder(const QString& dir);
    // Retargets rows (as "Set selected targets" with a chosen release or manual identifiers).
    QString applyTarget(const QList<int>& rows, const QString& titleId, const QString& contentId, const QString& evidence,
                        const QString& market, bool addCopies);
    QString previewText(bool iniMode) const;
    std::vector<dlc::IniExportResult> saveUpdatedInis(QStringList* log = nullptr); // no dialog (tests)

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    struct Row {
        dlc::DlcItem item;
        bool isPackage = false;
        std::optional<dlc::DlcPackageInfo> package;
        QStringList warnings;
        QString result;
        QPixmap icon;
        bool iconChosen = false;
        QString iconFile;      // the 512x512 PNG written for a chosen icon
    };
    void buildListCard();
    void buildSaveCard();
    void buildOptionsCard();
    void addRow(Row row);
    void refreshRow(int row);
    void syncFromTable(int row, int column);
    void updateState();
    void browseInputs();
    void chooseIcon(int row);
    void changeRegion();
    void showPreview();
    void saveInisInteractive();
    void appendLog(const QString& line);
    QList<int> selectedRows() const;
    void discoverRegions(int row);
    void pumpLookups();
    void updateRegionLine();
    QString lookupKey(const Row& r) const;
    bool onlineLookupAllowed() const;

    QTableWidget* m_table = nullptr;
    ui::DropZone* m_drop = nullptr;
    ui::Badge* m_countBadge = nullptr;
    QWidget* m_listTools = nullptr;
    QPushButton* m_regionBtn = nullptr;
    QPushButton* m_previewBtn = nullptr;
    QPushButton* m_saveIniBtn = nullptr;
    ui::Link* m_clearLink = nullptr;
    QLabel* m_regionLine = nullptr;
    ui::MessageRow* m_loadRow = nullptr;
    ui::PathField* m_output = nullptr;
    QLineEdit* m_key = nullptr;
    QLineEdit* m_passcode = nullptr;
    ui::Badge* m_engineBadge = nullptr;
    QLabel* m_engineDetail = nullptr;
    QPlainTextEdit* m_log = nullptr;
    std::vector<Row> m_rows;
    bool m_syncing = false;
    int m_loading = 0;
    QString m_iconDir;
    // Region discovery: key "<TitleId>|<prefix>" -> result.
    QHash<QString, std::shared_ptr<dlc::DlcLookupResult>> m_lookups;
    QSet<QString> m_lookupRunning;
    QStringList m_lookupQueue;
    QSet<QString> m_loadingPaths; // packages being read (case-folded)
    // Background reads and lookups belong to the list they were started for:
    // clearing the list cancels them and drops whatever still arrives.
    CancellationSource m_work;
    quint64 m_listGen = 0;
    QString m_cacheDir;
    bool m_demo = false;
};

} // namespace fw::pages
