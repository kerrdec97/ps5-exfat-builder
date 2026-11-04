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
class FpkgPage;
class ExtractVerifyPage;
class BackportPage;
class QueuePage;
class Ps5ConnectPage;
class LibraryPage;
class LogsPage;
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
    void reportProblem(const QStringList& extraFiles = {});
    void checkForUpdates(bool silent);
    void setSidebarCompact(bool compact);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildPages();
    void buildShortcuts();
    void addPage(pages::Page* page, const QString& iconName, const QString& text, bool footer = false);
    void onJobFinished(const QString& id);
    void onQueueFinished(int succeeded, int failed, int cancelled);
    void onActionDue(power::Action action, int delaySeconds, int succeeded, int failed);
    void updateStatus();
    void updateChips();
    void refreshFreeSpace();
    void showBanner(ui::InfoBar* bar);
    void checkCrashFiles();
    void onUpdateCheckFinished(const JobRecord& rec);
    void onUpdateDownloadFinished(const JobRecord& rec);
    void installUpdate(const QString& installer);
    void restoreWindowState();
    void saveWindowState();
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
    pages::HomePage* m_home = nullptr;
    pages::QueuePage* m_queuePage = nullptr;
    pages::SettingsPage* m_settingsPage = nullptr;
    pages::AboutPage* m_aboutPage = nullptr;
    pages::Ps5ConnectPage* m_ps5Page = nullptr;
    pages::LogsPage* m_logsPage = nullptr;
    QSet<QString> m_openFolderWhenDone;
    QString m_updateCheckId, m_updateDownloadId, m_reportJobId;
    QJsonObject m_release;
    QString m_pendingInstaller;
    QPointer<ui::InfoBar> m_updateBanner;
    QPointer<ui::InfoBar> m_crashBanner;
    QPointer<ui::Sheet> m_countdown;
    QTimer m_freeSpaceTimer;
    bool m_autoCompact = false;
    bool m_userCompact = false;
    bool m_dragging = false;
    bool m_closing = false;
};

} // namespace fw
