#pragma once

// Application shell (§2.2): AppHeader · Sidebar · pages · StatusBar, the
// focus-ring overlay and toast host, navigation and shortcuts, drag and drop,
// job-finished toasts and tray notifications, the after-queue countdown,
// the update banner, crash-report banner and problem reports.

#include "app/services/AppStores.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Style.h"
#include "jobs/QueueCompletionWatcher.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>

namespace fw {

class AppContext;
class JobQueueModel;
class LogModel;

namespace pages {
class Page;
class HomePage;
class BuildImagePage;
class BackportPage;
class QueuePage;
class Ps5ConnectPage;
class LibraryPage;
class SettingsPage;
class AboutPage;
} // namespace pages

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext& ctx, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Renders every page (and key states) into `<dir>/<nn>-<name>.png`. Returns the count.
    int screenshotAllPages(const QString& dir);

    void navigate(const QString& key, const QVariantMap& args = {});
    QString currentPage() const;
    QStringList pageKeys() const { return m_order; }
    pages::Page* page(const QString& key) const { return m_pages.value(key); }
    ui::Sidebar* sidebar() { return m_sidebar; }
    ui::ToastHost* toasts() { return m_toasts; }
    ui::StatusBar* statusBarWidget() { return m_status; }
    JobQueueModel& queueModel() { return *m_queueModel; }
    LogModel& logModel() { return *m_logModel; }
    void toast(ui::Tone tone, const QString& title, const QString& body, const QList<ui::ToastHost::Action>& actions = {});
    void showShortcuts();
    // Asks, then queues the problem report. Returns true when the report job was queued.
    bool reportProblem();
    // `apiUrl` is for tests (a local server standing in for GitHub); the app
    // passes none and the job refuses any other address.
    void checkForUpdates(bool silent, const QString& apiUrl = {});
    void setSidebarCompact(bool compact);
    // The "When the queue finishes" countdown while it is open (else null).
    ui::Sheet* afterQueueCountdown() const { return m_countdown; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildPages();
    void buildShortcuts();
    void addPage(pages::Page* page, const QString& iconName, const QString& text, bool footer = false);
    void onJobFinished(const QString& id);
    void onQueueFinished(int succeeded, int failed, int cancelled);
    void onActionDue(power::Action action, int delaySeconds, int succeeded, int failed);
    QString workOutsideTheQueue() const;
    void updateStatus();
    void updateChips();
    void refreshFreeSpace();
    void showBanner(ui::InfoBar* bar);
    void checkCrashFiles();
    void onUpdateCheckFinished(const JobRecord& rec);
    void onUpdateDownloadFinished(const JobRecord& rec);
    void setUpdateBanner(ui::InfoBar* bar);
    void showUpdateAvailable();
    void showUpdateDownloaded();
    void runUpdateAction();
    void restoreWindowState();
    void saveWindowState();
    void showDemoShell(const QString& key, const QString& state); // screenshots: chips and status bar for a page's demo state
    pages::Page* currentPageWidget() const;

    AppContext& m_ctx;
    JobQueueModel* m_queueModel;
    LogModel* m_logModel;
    ui::LibraryStore* m_library;
    ui::PayloadStore* m_payloads;
    QueueCompletionWatcher* m_watcher;
    ui::AppHeader* m_header;
    ui::Sidebar* m_sidebar;
    QStackedWidget* m_stack;
    QVBoxLayout* m_banners;
    ui::StatusBar* m_status;
    ui::FocusRingOverlay* m_focusRing;
    ui::ToastHost* m_toasts;
    QSystemTrayIcon* m_tray = nullptr;
    QHash<QString, pages::Page*> m_pages;
    QStringList m_order;
    pages::Page* m_shownPage = nullptr; // the page navigate() showed last (null before the first call)
    pages::HomePage* m_home = nullptr;
    pages::QueuePage* m_queuePage = nullptr;
    pages::SettingsPage* m_settingsPage = nullptr;
    pages::AboutPage* m_aboutPage = nullptr;
    pages::Ps5ConnectPage* m_ps5Page = nullptr;
    QSet<QString> m_openFolderWhenDone;
    quint64 m_freeSpaceGen = 0; // refreshFreeSpace(): the newest probe wins
    // The app-update lifecycle (one owner: the update slots):
    //   Idle ─▶ Checking ─▶ UpToDate | Available ─▶ Downloading ─▶ Downloaded
    //                                      ▲              │ failed, refused or cancelled
    //                                      └──────────────┘
    // Available: a newer release exists; m_release says whether the app can
    // download and check it ("canDownload") or it has to be fetched from its
    // page. Downloaded: the verified program file lies in the cache folder
    // (m_downloadedUpdate) for the user to put in place; the app never starts
    // it and never replaces itself. A check from Available or Downloaded
    // returns there when it fails. A second check or download while one is
    // under way is ignored.
    enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Downloaded };
    UpdateState m_updateState = UpdateState::Idle;
    QString m_updateCheckId, m_updateDownloadId, m_reportJobId;
    QJsonObject m_release;       // the details of the last successful check
    QString m_downloadedUpdate;  // the verified file, while the state is Downloaded
    QPointer<ui::InfoBar> m_updateBanner;
    QPointer<ui::Sheet> m_countdown;
    QTimer m_freeSpaceTimer;
    bool m_userCompact = false; // the sidebar is collapsed (the header's menu button; remembered)
    bool m_narrow = false;      // the window is narrower than size::sidebarFoldBelow: the sidebar shows its rail
    bool m_closeInProgress = false; // closeEvent() is waiting for the backport engine to stop
};

} // namespace fw
