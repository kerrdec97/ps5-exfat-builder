#pragma once

// Small layout helpers for the Home, Queue, Logs, Settings and About pages.
//
//  * TileGrid — equal-width columns that wrap: as many columns as fit
//               `minColumnWidth` (capped at `maxColumns`), rows as tall as
//               their tallest item. Stat tiles, action cards, credits.

#include "app/ui/Theme.h"

#include <QLayout>
#include <QList>

namespace fw::ui {

class TileGrid : public QLayout {
public:
    explicit TileGrid(QWidget* parent = nullptr, int minColumnWidth = 220, int maxColumns = 4,
                      int hSpacing = theme::space::lg, int vSpacing = theme::space::lg);
    ~TileGrid() override;
    void setMinColumnWidth(int px) { m_minW = px; invalidate(); }
    void setMaxColumns(int n) { m_maxCols = n; invalidate(); }
    // Prefer a column count that fills every row (4 items: 4 or 2, never 3 + 1).
    void setBalanced(bool on) { m_balanced = on; invalidate(); }
    int columnsFor(int width) const;

    void addItem(QLayoutItem* item) override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    int count() const override { return int(m_items.size()); }
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

private:
    int doLayout(const QRect& rect, bool apply) const;
    QList<QLayoutItem*> m_items;
    int m_minW, m_maxCols, m_h, m_v;
    bool m_balanced = false;
};

} // namespace fw::ui
