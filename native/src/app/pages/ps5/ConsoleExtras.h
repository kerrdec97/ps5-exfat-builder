#pragma once

// Smaller PS5 connect tools, each a ConsoleToolTab:
//
//   * SmpLogTab      (Logs › ShadowMount+ log, "smplog") — debug.log of the
//     current or previous run, through the ShadowMount+ 1.7 API when it
//     answers (a bounded tail), else over FTP.
//   * CrashDumpsTab  (Logs › Crash dumps, "crashdumps") — queues
//     ps5.crashdumps.capture and shows its state.
//   * InstallTab     (Console tools › Install package, "install") — any .pkg
//     through etaHEN's Direct Package Installer V2 (ps5.dpi.install).

#include "app/pages/ps5/ConsoleToolTab.h"

class QPlainTextEdit;
class QSpinBox;

namespace fw::ui {
class DropZone;
class Segmented;
class PathField;
} // namespace fw::ui

namespace fw::pages {

class SmpLogTab : public ConsoleToolTab {
    Q_OBJECT
public:
    SmpLogTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;
    void fetch();
    QString text() const;
    QString sourceText() const;

private:
    void showText(const QString& text, const QString& source);
    ui::Segmented* m_which;
    QPlainTextEdit* m_view;
    QLabel* m_source;
    ui::Badge* m_badge;
    QPushButton* m_refresh;
    bool m_fetchPending = false; // "This run / Previous run" changed while a load ran
    quint64 m_fetchGen = 0;      // the newest load; an older one's answer is not shown
};

class CrashDumpsTab : public ConsoleToolTab {
    Q_OBJECT
public:
    CrashDumpsTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;
    void start();
    QString jobId() const { return m_jobId; }
    bool isWatching() const { return m_watching; } // its job waits or runs

private:
    void onJob(const QString& id);
    void setWatching(bool on);
    bool m_watching = false;
    ui::PathField* m_folder;
    QSpinBox* m_minutes;
    QPushButton* m_start;
    QPushButton* m_stop;
    ui::MessageRow* m_status;
    QString m_jobId;
};

class InstallTab : public ConsoleToolTab {
    Q_OBJECT
public:
    InstallTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;
    void setPackage(const QString& path);
    QString package() const { return m_package; }
    void install();
    void testInstaller();

private:
    QString m_package;
    ui::DropZone* m_drop;
    QWidget* m_strip;
    QLabel* m_name;
    QLabel* m_meta;
    QPushButton* m_install;
    ui::MessageRow* m_status;
};

} // namespace fw::pages
