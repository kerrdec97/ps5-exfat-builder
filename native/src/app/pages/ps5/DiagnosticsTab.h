#pragma once

// PS5 connect › Diagnostics (legacy FTP-18 "Connection Diagnostics"): runs the
// queue job ps5.ftp.diagnose — the checks, or the checks plus the 16 MiB
// speed test — and shows its result cells: reachable and round trip, login,
// passive and binary mode, destination, writable folder, server type, the
// greeting, the FEAT extensions, and upload/download speed. Any finished
// diagnose job is shown, also one retried from the Queue page.

#include "app/ui/Widgets.h"
#include "connect/ps5/FtpDiagnostics.h"

#include <QDateTime>
#include <QHash>
#include <QLineEdit>

#include <functional>

namespace fw {
class AppContext;
}

namespace fw::pages {

class DiagnosticsTab : public QWidget {
    Q_OBJECT
public:
    // Where the console is, as the page shows it right now.
    struct Source {
        std::function<QString()> host;   // the header's address field
        std::function<int()> port;       // the header's FTP port field
        std::function<QString()> folder; // the FTP browser's open folder
    };
    DiagnosticsTab(AppContext& ctx, Source source, QWidget* parent = nullptr);

    // Queues ps5.ftp.diagnose for the current address and folder.
    void run(bool speedTest);
    void cancel();
    bool isRunning() const { return !m_jobId.isEmpty(); }
    void showResult(const ps5::FtpDiagnosis& d, const QString& verdict, ui::Tone tone, const QString& status);
    void clearResult();
    // An example result for the screenshots (labelled as such).
    void showSpecimen();

    // Test hooks. Cell keys: reachable, rtt, login, destination, passive,
    // binary, writable, server, upload, download, readback.
    QString statusText() const;
    QString verdictText() const;
    QString cellText(const QString& key) const;
    ui::Tone cellTone(const QString& key) const;
    QString bannerText() const;
    QPushButton* runButton() { return m_run; }
    QPushButton* speedButton() { return m_speed; }
    QLineEdit* folderEdit() { return m_folder; }
    QString jobId() const { return m_jobId; }

protected:
    void showEvent(QShowEvent* e) override;

private:
    struct Cell {
        QWidget* box = nullptr;
        ui::Badge* badge = nullptr; // status cells
        QLabel* value = nullptr;    // value cells
        QLabel* detail = nullptr;
    };
    QWidget* makeCell(const QString& key, const QString& title, bool badge, QWidget* parent, ui::TextStyle valueStyle = ui::TextStyle::Mono);
    void setCell(const QString& key, const QString& text, ui::Tone tone = ui::Tone::Neutral, const QString& detail = {});
    void setStatus(const QString& text, ui::Tone tone = ui::Tone::Neutral);
    void setRunning(bool running);
    void onJobChanged(const QString& id);
    void onJobFinished(const QString& id);
    void prefillFolder();

    AppContext& m_ctx;
    Source m_src;
    QString m_jobId;
    ui::Badge* m_verdict;
    QLineEdit* m_folder;
    QLabel* m_status;
    QPushButton* m_run;
    QPushButton* m_speed;
    QPushButton* m_cancel;
    ui::Card* m_results;
    ui::InfoBar* m_problem;
    QHash<QString, Cell> m_cells;
    QLabel* m_banner;
    QWidget* m_featRow;
    ui::FlowLayout* m_feat;
    ui::Card* m_speedCard;
    QLabel* m_speedCaption;
    ui::InfoBar* m_leftover;
};

} // namespace fw::pages
