#pragma once

// The result of an image.compare job as a sheet (Extract & verify › Compare):
// what was compared, the counts per difference kind and the differences
// grouped by kind, read from JobOutcome::details (shape in
// services/JobSubmit.h). "Copy report" puts a plain-text report on the
// clipboard (the legacy Compare wrote one next to image A).

#include "app/ui/Sheet.h"

#include <QJsonObject>
#include <QTreeWidget>

namespace fw::pages {

class CompareResultSheet : public ui::Sheet {
    Q_OBJECT
public:
    CompareResultSheet(QWidget* host, const QJsonObject& details, const QString& summary, const QStringList& warnings = {});

    QTreeWidget* list() { return m_list; }
    // Group headings in order, e.g. "Only in A (missing from B) · 1".
    QStringList groupTitles() const;
    QString report() const { return m_report; }

    // The plain-text report (also what Copy report copies).
    static QString reportText(const QJsonObject& details, const QString& summary, const QStringList& warnings);

private:
    QTreeWidget* m_list = nullptr;
    QString m_report;
};

} // namespace fw::pages
