#pragma once

// The result of a tools.dump.check job as a sheet (Library › Games › Check
// dump…): what was checked, the facts (title ID, version, executables by
// class, backport state), and the findings grouped by severity, read from
// JobOutcome::details (shape in engines/tools/Module.h). A selected finding
// is shown in full with "Show in folder"; "Copy report" puts a plain-text
// report on the clipboard; "Backport…" asks the page for the Backport tab.

#include "app/ui/Sheet.h"

#include <QDateTime>
#include <QJsonObject>
#include <QTreeWidget>
#include <QVariantMap>

namespace fw::pages {

class DumpCheckSheet : public ui::Sheet {
    Q_OBJECT
public:
    // `summary`: the job's one line ("2 problems and 3 warnings in 18,442
    // files"); `title`: the game's name; `folder`: the dump that was checked.
    DumpCheckSheet(QWidget* host, const QJsonObject& details, const QString& summary, const QString& title, const QString& folder,
                   const QDateTime& checkedAt = {});

    QTreeWidget* list() { return m_list; }
    // Group headings in order: "Problems · 2", "Warnings · 3", "Notes · 4".
    QStringList groupTitles() const;
    QString report() const { return m_report; }
    QPushButton* showInFolderButton() { return m_show; }
    // The file or folder "Show in folder" opens for the selected finding ("" without a selection).
    QString selectedPath() const;

    // The plain-text report (also what Copy report copies).
    static QString reportText(const QJsonObject& details, const QString& summary, const QString& title, const QString& folder,
                              const QDateTime& checkedAt = {});
    // Sample details for screenshots.
    static QJsonObject demoDetails();
    // The button says "Restore originals…" (restoreRequested) instead of
    // "Backport…" (backportRequested): the folder carries a backport (or a
    // half-finished apply / restore) recorded in the PS5 Backport ledger with
    // its backup.
    bool offersRestore() const { return m_offersRestore; }
    static bool backportBackupFor(const QString& folder, const QString& backportState);
    // What the Backport tab is opened with for this folder (navigateWith):
    // the folder is listed, the game ticked and selected, and with `restore`
    // its restore is offered (the tab asks first).
    static QVariantMap backportArgs(const QString& folder, bool restore);

signals:
    void backportRequested();
    void restoreRequested();

private:
    void updateSelection();
    bool m_offersRestore = false;
    QString m_folder;
    QTreeWidget* m_list = nullptr;
    QLabel* m_detail = nullptr;
    QPushButton* m_show = nullptr;
    QString m_report;
};

} // namespace fw::pages
