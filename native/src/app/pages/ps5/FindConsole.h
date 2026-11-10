#pragma once

// "Find my PS5" (legacy Auto-detect IP, LEGACY_UI_INVENTORY §3): a button in
// the PS5 connect header. It sweeps this PC's /24 networks for an FTP server
// on the known ports (ps5::discoverConsoles on a worker thread, cancellable)
// and lists what answered in a sheet; the console the user picks is saved as
// ps5.consoleIp and ps5.ftpPort, which the page's address fields follow.

#include "app/ui/Widgets.h"
#include "connect/ps5/Discovery.h"
#include "core/Cancellation.h"

#include <QFuture>
#include <QTableWidget>

#include <optional>
#include <vector>

namespace fw {
class AppContext;
}
namespace fw::ui {
class ProgressBar;
class Sheet;
} // namespace fw::ui

namespace fw::pages {

// The sheet's body: progress while searching, then the consoles found.
class FindConsolePanel : public QWidget {
    Q_OBJECT
public:
    explicit FindConsolePanel(QWidget* parent = nullptr);
    ~FindConsolePanel() override; // cancels a running search and waits for it

    void start(const ps5::DiscoveryOptions& options);
    void cancel();
    bool isRunning() const { return m_running; }
    const std::vector<ps5::DiscoveredConsole>& results() const { return m_results; }
    std::optional<ps5::DiscoveredConsole> selected() const;
    QTableWidget* table() { return m_table; }
    QString statusText() const;
    // An example result (screenshots); nothing is probed.
    void showSpecimen();
signals:
    void finished(int found);
    void selectionChanged(bool any);
    void activated(); // double-click or Enter on a result

private:
    void onProgress(int generation, int done, int total, int found);
    void onFinished(int generation, const Result<std::vector<ps5::DiscoveredConsole>>& result);
    void setResults(std::vector<ps5::DiscoveredConsole> results);
    void fitArea();

public:
    // The result list's height: its rows, between these (three rows, five rows).
    static constexpr int kAreaMin = 148, kAreaMax = 220;
    QWidget* resultArea() const { return m_area; }

private:
    QWidget* m_area;
    ui::ProgressBar* m_bar;
    QLabel* m_status;
    QTableWidget* m_table;
    ui::EmptyState* m_empty;
    QPushButton* m_again;
    std::vector<ps5::DiscoveredConsole> m_results;
    ps5::DiscoveryOptions m_options;
    CancellationSource m_cancel;
    QFuture<void> m_future;
    QString m_scope; // "192.168.1.0/24" or the given addresses
    int m_generation = 0;
    bool m_running = false;
};

class FindConsoleButton : public QPushButton {
    Q_OBJECT
public:
    explicit FindConsoleButton(AppContext& ctx, QWidget* parent = nullptr);
    // Tests: probe these hosts and ports instead of this PC's networks.
    void setDiscoveryOptions(const ps5::DiscoveryOptions& options) { m_options = options; }
    // Opens the sheet and waits. Returns the console the user chose, after
    // saving its address and FTP port to the settings.
    std::optional<ps5::DiscoveredConsole> run();
    // The sheet with an example result, open without waiting (screenshots).
    ui::Sheet* openSpecimen();
signals:
    void consolePicked(const QString& host, int port);

private:
    ui::Sheet* makeSheet(FindConsolePanel*& panel, QPushButton*& use);
    AppContext& m_ctx;
    ps5::DiscoveryOptions m_options;
};

} // namespace fw::pages
