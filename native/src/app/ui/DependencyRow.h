#pragma once

// DependencyRow (§4.22) and DependencyList: rows bound to DependencyRegistry
// results with Check again, Copy report and "how to fix".

#include "app/ui/Widgets.h"
#include "core/Dependencies.h"

#include <QHash>
#include <QWidget>

namespace fw {
class AppContext;
}

namespace fw::ui {

class DependencyRow : public QWidget {
    Q_OBJECT
public:
    explicit DependencyRow(const QString& id, const QString& name, QWidget* parent = nullptr);
    void setInfo(const DependencyInfo& info);
    void setChecking();
    QString id() const { return m_id; }
    void setDivider(bool on);
signals:
    void fixRequested(const QString& id, const QString& howToFix, const QString& settingKey);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_id;
    QLabel* m_icon;
    ElideLabel* m_name;
    ElideLabel* m_detail;
    Badge* m_badge;
    Link* m_fix;
    DependencyInfo m_info;
    bool m_divider = true;
};

// A card body listing dependencies (all, or a subset by id). Probes run on a
// worker thread through AppContext::checkDependenciesAsync().
class DependencyList : public QWidget {
    Q_OBJECT
public:
    DependencyList(AppContext& ctx, const QStringList& ids = {}, QWidget* parent = nullptr);
    int readyCount() const;
    int total() const { return int(m_rows.size()); }
    int problemCount() const; // Missing / Outdated / Error
    QString report() const;   // JSON text for the clipboard
    void refresh();           // re-read the registry's last results
    void checkAgain();
    bool isEmpty() const { return m_rows.isEmpty(); }
signals:
    void changed();
    void fixRequested(const QString& id, const QString& howToFix, const QString& settingKey);

private:
    AppContext& m_ctx;
    QStringList m_ids;
    QHash<QString, DependencyRow*> m_rows;
    QList<DependencyInfo> m_last;
};

} // namespace fw::ui
