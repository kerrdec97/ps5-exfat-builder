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

signals:
    void backportRequested();

private:
    void updateSelection();
    QString m_folder;
    QTreeWidget* m_list = nullptr;
    QLabel* m_detail = nullptr;
    QPushButton* m_show = nullptr;
    QString m_report;
};

} // namespace fw::pages
