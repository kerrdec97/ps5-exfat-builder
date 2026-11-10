#pragma once

// PS5 connect: the console card (address, ports, state, Find my PS5,
// Connect, detected services) and the tools in five groups:
//
//   Files         Browser (FTP file manager + transfer queue) · Games on the console
//   Payloads      the payload library and where to send it
//   Logs          Kernel log · ShadowMount+ log · Crash dumps
//   ShadowMount+  Settings · Fakelib · Autoloader · AMPR index (+ unplug / eject)
//   Console tools Y2JB · Install package · Diagnostics
//
// Other pages open a tool by its stable key through navigateWith({"tab": key}):
// files, ftp, manager, payloads, logs, klog, smplog, crashdumps, shadowmount,
// mount, fakelib, autoloader, ampr, tools, y2jb, install, diagnostics.
// Transfers, payloads, installs, Y2JB, Safe to unplug, eject and diagnostics
// are queue jobs.

#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/services/ConsoleSession.h"
#include "connect/ps5/ConsoleServices.h"
#include "connect/ps5/KlogClient.h"

#include <QAbstractTableModel>
#include <QFuture>
#include <QListWidget>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTableWidget>

#include <deque>
#include <memory>
#include <vector>

class QComboBox;
class QTimer;

namespace fw::ui {
class Segmented;
}

namespace fw::pages {

class AmprTab;
class AutoloaderTab;
class ConsoleHero;
class ConsoleToolTab;
class CrashDumpsTab;
class DiagnosticsTab;
class FakelibTab;
class FindConsoleButton;
class InstallTab;
class Ps5ManagerTab;
class ShadowMountTab;
class SmpLogTab;
class ToolStack;
class TransferPanel;

// Bounded kernel-log buffer for the viewer (legacy defect 4: unbounded).
class KlogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { TimeColumn, TagColumn, TextColumn, ColumnCount };
    enum Role { TagRole = Qt::UserRole + 1 };
    explicit KlogModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void append(const ps5::KlogLine& line);
    void append(const std::vector<ps5::KlogLine>& lines); // one model change for the whole batch
    void clear();
    QString allText() const;
    int warnings() const { return m_warnings; }
    int errors() const { return m_errors; }
    const ps5::KlogLine* lineAt(int row) const;

private:
    std::deque<ps5::KlogLine> m_lines;
    int m_warnings = 0, m_errors = 0;
    size_t m_max = 20000;
};

class Ps5ConnectPage : public Page {
    Q_OBJECT
public:
    Ps5ConnectPage(AppContext& ctx, JobQueueModel& queueModel, ui::PayloadStore& payloads, QWidget* parent = nullptr);
    ~Ps5ConnectPage() override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    QString host() const;
    int ftpPort() const;
    bool isReachable() const { return m_session->isConnected(); }
    // What would be lost by closing now ("" when nothing): unsaved edits in the Autoloader editor
    // and the ShadowMount+ settings form.
    QString unsavedChanges() const override;
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void setDragHighlight(bool on) override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    // Test hooks
    ui::ConsoleSession* session() { return m_session; }
    QString currentRemoteDir() const { return m_remoteDir; }
    QTableWidget* remoteTable() { return m_remote; }
    KlogModel* klogModel() { return m_klog; }
    DiagnosticsTab* diagnosticsTab() { return m_diagnostics; }
    FindConsoleButton* findConsoleButton() { return m_find; }
    ConsoleHero* hero() { return m_hero; }
    TransferPanel* transfers() { return m_transfers; }
    ShadowMountTab* shadowMountTab() { return m_smp; }
    QTabBar* groupBar() { return m_groups; }
    QString currentGroup() const;
    QString currentTool() const; // the stable key of the visible tool
    QStringList remoteNames() const; // names in the browser, in display order
    void listRemote(const QString& path);
    void uploadFiles(const QStringList& localPaths, const QString& remoteDir = {});
signals:
    void reachabilityChanged(bool reachable, const QString& host);
    // The connection's state (Offline / Connecting / Connected / Failed), for the header chip and Home.
    void connectionChanged(fw::ui::ConsoleSession::ConnectionState state, const QString& host, const QString& detail);

private:
    struct Group {
        QString key;
        QWidget* page = nullptr;
        ui::Segmented* sub = nullptr; // null for single-view groups
        ToolStack* stack = nullptr;
        QStringList toolKeys;
    };
    // header / settings
    int settingPort(const QString& key, int fallback) const;
    ps5::ConsoleEndpoint endpoint() const;
    void toggleConnection();
    // Every view of the connection (hero, chips, file manager, status) follows the session's state.
    void applyConnectionState(ui::ConsoleSession::ConnectionState state, const QString& detail);
    void updateHeroSubtitle();
    void checkServices();
    void showServices(const ps5::ConsoleServices& s);
    void updatePayloadChip();
    void showTool(const QString& key);
    void addGroup(const QString& key, const QString& title, const QString& icon,
                  const QList<std::tuple<QString, QString, QString, QWidget*>>& tools);
    // FTP
    QWidget* buildFilesBrowser(QWidget* parent);
    void onListed(const ui::RemoteListing& listing);
    // Delete / rename / new folder: `count` requests were sent; their answers are summed up in one toast.
    void beginOperations(int count, const QString& footer);
    void onOperationDone(const QString& what, bool ok, const QString& message);
    // The console the browser is connected to (the address fields when it is not connected).
    QString browserHost() const;
    int browserPort() const;
    // Names in the open folder that an upload of `localPaths` into `remoteDir` would replace.
    QStringList uploadCollisions(const QStringList& localPaths, const QString& remoteDir) const;
    void showListing(const ui::RemoteListing& listing);
    void rebuildBreadcrumb();
    void rebuildPlaces();
    void updateFooter();
    void updateActions();
    int entryIndex(int row) const;
    QStringList selectedRemotePaths() const;
    bool confirmProtected(const QString& path, const QString& action);
    void uploadDialog(bool folder);
    void downloadSelected();
    void deleteSelected();
    void renameSelected();
    // The listed name a rename to `newName` would land on ("" when it is free).
    QString renameCollision(const QString& dir, const QString& oldName, const QString& newName) const;
    void newFolder();
    void goBack();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void applyCompact(bool compact); // narrow windows: places as a menu, icon-only secondary actions
    // Payloads
    QWidget* buildPayloads(QWidget* parent);
    void rebuildPayloads();
    void showPayload(int index);
    void addOrEditPayload(int index);
    void sendPayload(int index);
    int payloadIndex(const ui::PayloadEntry& entry) const; // -1 when it is not in the list any more
    // Kernel log
    QWidget* buildKlog(QWidget* parent);
    void klogToggle();
    void klogState(ps5::KlogState state, const QString& detail);
    void klogExport();
    void klogFilterChanged();
    void updateKlogStats();
    void flushKlog();
    void klogSnapshot();
    // Y2JB
    QWidget* buildY2jb(QWidget* parent);
    QString region() const;

    ui::PayloadStore& m_payloads;
    ui::ConsoleSession* m_session;
    ConsoleHero* m_hero;
    QLineEdit* m_host;
    QLineEdit* m_ftpPort;
    QPushButton* m_connect;
    FindConsoleButton* m_find = nullptr;
    ui::ConsoleSession::ConnectionState m_lastState = ui::ConsoleSession::ConnectionState::Offline; // last state announced
    QTabBar* m_groups;
    ToolStack* m_groupStack;
    std::vector<Group> m_groupList;
    QHash<QString, QString> m_toolGroup; // tool key -> group key
    // services
    QFuture<void> m_probe;
    bool m_probing = false;
    std::vector<ps5::smp::StorageItem> m_storage;
    QString m_smpVersion;
    QString m_payloadSendId;
    QString m_payloadSendPath; // the payload that job sends
    // ftp
    QString m_remoteDir;
    ui::RemoteListing m_listing;
    QStringList m_history;
    int m_historyIndex = -1;
    int m_opsPending = 0, m_opsTotal = 0, m_opsOk = 0; // the batch of delete / rename / mkdir requests under way
    QString m_opsError;                                // the last failure of that batch
    bool m_firstListing = false;                       // the listing right after connecting
    int m_firstListingStep = 0;                        // its fallbacks: 1 = games folder, 2 = root
    QPushButton* m_emptyConnect = nullptr;
    bool m_specimenListing = false;
    ui::Card* m_filesCard = nullptr;
    QWidget* m_crumbs;
    QHBoxLayout* m_crumbLayout;
    QListWidget* m_places;
    QTableWidget* m_remote;
    QWidget* m_remoteArea;
    ui::EmptyState* m_remoteEmpty;
    QLabel* m_remoteFooter;
    QLabel* m_remoteFree;
    QLineEdit* m_filter;
    QPushButton* m_btnUpload;
    QPushButton* m_btnUploadFolder;
    QPushButton* m_btnDownload;
    QPushButton* m_btnNewFolder;
    QPushButton* m_btnRename;
    QPushButton* m_btnDelete;
    ui::IconButton* m_btnBack;
    ui::IconButton* m_btnUp;
    ui::IconButton* m_btnRefresh;
    ui::IconButton* m_btnPlaces = nullptr;
    bool m_compact = false;
    QLabel* m_destClass;
    TransferPanel* m_transfers = nullptr;
    // payloads
    QListWidget* m_payloadList;
    ui::Card* m_payloadDetail;
    QWidget* m_payloadArea;
    QLabel* m_pName;
    ui::Badge* m_pBadge;
    QLabel* m_pFile;
    QLabel* m_pNotes;
    QLabel* m_pLastSent;
    QPushButton* m_pSend;
    QPushButton* m_pEdit;
    QPushButton* m_pRemove;
    ui::EmptyState* m_payloadEmpty;
    QComboBox* m_payloadTarget;
    QLabel* m_targetHint;
    int m_currentPayload = -1;
    // klog
    KlogModel* m_klog;
    std::unique_ptr<ps5::KlogClient> m_klogClient;
    QPushButton* m_klogConnect;
    QPushButton* m_klogPause;
    ui::Badge* m_klogBadge;
    QLineEdit* m_klogFilter;
    ui::Switch* m_klogFollow;
    QTableView* m_klogView;
    QSortFilterProxyModel* m_klogProxy;
    QLabel* m_klogStats;
    bool m_klogPaused = false;
    bool m_klogLive = false;                   // the card's chip says "live"
    std::deque<ps5::KlogLine> m_klogHeld;      // received while paused
    std::vector<ps5::KlogLine> m_klogIncoming; // received since the last flush
    QTimer* m_klogFlush = nullptr;
    quint64 m_klogGen = 0; // bumped on Connect/Disconnect: calls of an earlier client are ignored
    // y2jb
    ui::Segmented* m_region;
    ui::PathField* m_pkg;
    QPushButton* m_install;
    QPushButton* m_autoBlock;
    ui::PathField* m_patchFolder;
    QPushButton* m_patchLocal;
    QLabel* m_dpiStatus;
    QLineEdit* m_firmware;
    QLabel* m_youtubeVersion;
    // y2jb: the user's own YouTube packages (and the private build's)
    struct YoutubePackage {
        QString path;
        QString version;  // "v01.000.003"
        QString titleId;  // PPSA01650
        QString firmware; // "4.03 – 12.40", "" when the version is not in the table
        bool bundled = false;
    };
    void scanYoutubePackages();
    void rebuildYoutubeList();
    void installYoutube(const QString& path);
    ui::PathField* m_ytFolder = nullptr;
    QWidget* m_ytList = nullptr;
    QPushButton* m_ytBest = nullptr;
    QLabel* m_ytHint = nullptr;
    std::vector<YoutubePackage> m_ytPackages;
    quint64 m_ytScan = 0;
    // tools
    QPointer<QWidget> m_demoSheet;
    DiagnosticsTab* m_diagnostics = nullptr;
    Ps5ManagerTab* m_manager = nullptr;
    FakelibTab* m_fakelib = nullptr;
    AutoloaderTab* m_autoloader = nullptr;
    AmprTab* m_ampr = nullptr;
    ShadowMountTab* m_smp = nullptr;
    SmpLogTab* m_smpLog = nullptr;
    CrashDumpsTab* m_crash = nullptr;
    InstallTab* m_installTab = nullptr;
};

} // namespace fw::pages
