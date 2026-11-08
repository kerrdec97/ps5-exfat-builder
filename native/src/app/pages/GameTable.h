#pragma once

// The game table shared by the Library page (cover, title, ID, version, SDK,
// size, status; optional check boxes). Moved out of the old Backport page.

#include "app/services/AppStores.h"
#include "app/ui/Widgets.h"

#include <QTableWidget>

namespace fw::pages {

class GameTable : public QTableWidget {
    Q_OBJECT
public:
    explicit GameTable(bool selectable, QWidget* parent = nullptr);
    void setGames(const std::vector<ui::LibraryGame>& games, const QString& filter);
    QStringList checkedFolders() const;
    QString currentFolder() const;
    void setAllChecked(bool on);
    int gameCount() const { return m_count; }
    void setStatusText(const QString& folder, const QString& text, ui::Tone tone);
    static QPixmap coverPixmap(const QString& coverPath, int px);
signals:
    void checkedChanged();
    void contextRequested(const QString& folder, const QPoint& globalPos);
    void activatedFolder(const QString& folder);

private:
    bool m_selectable;
    int m_count = 0;
};

} // namespace fw::pages
