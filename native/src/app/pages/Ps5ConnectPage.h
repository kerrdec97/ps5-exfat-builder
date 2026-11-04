#pragma once

// PS5 connect (Connect artboard): console address bound to the ps5.* settings,
// a live FTP session on a worker thread, and tabs: FTP browser · Payloads ·
// Kernel log · Y2JB · Mount config · Diagnostics. Transfers, payloads, DPI
// installs, the Y2JB auto-block, "Safe to unplug" and the FTP diagnostics are
// queue jobs. "Find my PS5" in the header sweeps the network (FindConsole.h).

#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/services/ConsoleSession.h"
#include "connect/ps5/KlogClient.h"

#include <QAbstractTableModel>
#include <QListWidget>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTableWidget>

#include <deque>
#include <memory>

namespace fw::pages {

class DiagnosticsTab;
class FindConsoleButton;

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
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
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
    void listRemote(const QString& path);
signals:
    void reachabilityChanged(bool reachable, const QString& host);

private:
    // header / settings
    int settingPort(const QString& key, int fallback) const;
    void toggleConnection();
    void setConnectedUi(bool on, const QString& detail);
    // FTP
    QWidget* buildFtpTab();
    void onListed(const ui::RemoteListing& listing);
    void rebuildBreadcrumb();
    QStringList selectedRemotePaths() const;
    bool confirmProtected(const QString& path, const QString& action);
    void uploadFiles(const QStringList& localPaths);
    void downloadSelected();
    void deleteSelected();
    void renameSelected();
    void newFolder();
    // Payloads
    QWidget* buildPayloadsTab();
    void rebuildPayloads();
    void showPayload(int index);
    void addOrEditPayload(int index);
    void sendPayload(int index);
    // Kernel log
    QWidget* buildKlogTab();
    void klogToggle();
    void klogState(ps5::KlogState state, const QString& detail);
    void klogExport();
    void klogFilterChanged();
    // Y2JB
    QWidget* buildY2jbTab();
    QString region() const;
    // Mount config
    QWidget* buildMountTab();
    ps5::MountTool mountTool() const;
    void mountLoadDefaults();
    void mountLoad();
    void mountPush();
    ps5::MountConfigValues mountValuesFromTable() const;
    void mountSetValues(const ps5::MountConfigValues& values);

    ui::PayloadStore& m_payloads;
    ui::ConsoleSession* m_session;
    QLineEdit* m_host;
    QLineEdit* m_ftpPort;
    QPushButton* m_connect;
    ui::Badge* m_badge;
    QLabel* m_welcome;
    QTabBar* m_tabs;
    QStackedWidget* m_stack;
    // ftp
    QString m_remoteDir;
    ui::RemoteListing m_listing;
    QWidget* m_crumbs;
    QHBoxLayout* m_crumbLayout;
    QTableWidget* m_remote;
    ui::EmptyState* m_remoteEmpty;
    QLabel* m_remoteFooter;
    QLabel* m_remoteFree;
    QPushButton* m_btnUpload;
    QPushButton* m_btnUploadFolder;
    QPushButton* m_btnDownload;
    QPushButton* m_btnNewFolder;
    QPushButton* m_btnRename;
    QPushButton* m_btnDelete;
    ui::IconButton* m_btnUp;
    ui::IconButton* m_btnRefresh;
    QLabel* m_destClass;
    // payloads
    QListWidget* m_payloadList;
    ui::Card* m_payloadDetail;
    QLabel* m_pName;
    ui::Badge* m_pBadge;
    QLabel* m_pFile;
    QLabel* m_pNotes;
    QLabel* m_pLastSent;
    QPushButton* m_pSend;
    QPushButton* m_pEdit;
    QPushButton* m_pRemove;
    ui::EmptyState* m_payloadEmpty;
    ui::Segmented* m_payloadTarget;
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
    std::deque<ps5::KlogLine> m_klogHeld;
    // y2jb
    ui::Segmented* m_region;
    ui::PathField* m_pkg;
    QPushButton* m_install;
    QPushButton* m_autoBlock;
    ui::PathField* m_patchFolder;
    QPushButton* m_patchLocal;
    QLabel* m_dpiStatus;
    // mount
    ui::Segmented* m_tool;
    QTableWidget* m_mountTable;
    QLabel* m_mountStatus;
    QPushButton* m_mountLoad;
    QPushButton* m_mountPush;
    QPushButton* m_mountStop;
    QPushButton* m_mountSendElf;
    ui::PathField* m_mountElf;
    QPointer<QWidget> m_demoSheet;
    DiagnosticsTab* m_diagnostics = nullptr; // pages/ps5/DiagnosticsTab
    FindConsoleButton* m_find = nullptr;     // pages/ps5/FindConsole
};

} // namespace fw::pages
