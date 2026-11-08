#include "app/ui/HomeKit.h"

#include <QWidget>

#include <algorithm>

namespace fw::ui {

TileGrid::TileGrid(QWidget* parent, int minColumnWidth, int maxColumns, int hSpacing, int vSpacing)
    : QLayout(parent), m_minW(minColumnWidth), m_maxCols(std::max(1, maxColumns)), m_h(hSpacing), m_v(vSpacing)
{
    setContentsMargins(0, 0, 0, 0);
}

TileGrid::~TileGrid()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void TileGrid::addItem(QLayoutItem* item) { m_items.append(item); }

QLayoutItem* TileGrid::itemAt(int index) const { return index >= 0 && index < m_items.size() ? m_items.at(index) : nullptr; }

QLayoutItem* TileGrid::takeAt(int index) { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }

int TileGrid::columnsFor(int width) const
{
    int visible = 0;
    for (QLayoutItem* it : m_items)
        if (!it->isEmpty())
            ++visible;
    if (visible == 0)
        return 1;
    const int fit = std::max(1, (width + m_h) / (m_minW + m_h));
    int cols = std::clamp(std::min(fit, visible), 1, m_maxCols);
    if (m_balanced)
        while (cols > 1 && visible % cols != 0)
            --cols;
    return cols;
}

QSize TileGrid::sizeHint() const
{
    int w = 0;
    int cols = 0;
    for (QLayoutItem* it : m_items)
        if (!it->isEmpty() && cols < m_maxCols) {
            w += std::max(m_minW, it->sizeHint().width());
            ++cols;
        }
    w += std::max(0, cols - 1) * m_h;
    return {w, heightForWidth(w)};
}

QSize TileGrid::minimumSize() const
{
    int w = 0;
    for (QLayoutItem* it : m_items)
        if (!it->isEmpty())
            w = std::max(w, it->minimumSize().width());
    return {w, 0};
}

int TileGrid::heightForWidth(int width) const { return doLayout(QRect(0, 0, width, 0), false); }

void TileGrid::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, true);
}

int TileGrid::doLayout(const QRect& rect, bool apply) const
{
    QList<QLayoutItem*> items;
    for (QLayoutItem* it : m_items)
        if (!it->isEmpty())
            items << it;
    if (items.isEmpty())
        return 0;
    const int cols = columnsFor(rect.width());
    const int colW = std::max(1, (rect.width() - (cols - 1) * m_h) / cols);
    int y = rect.y();
    for (int start = 0; start < items.size(); start += cols) {
        int rowH = 0;
        for (int i = start; i < std::min<int>(start + cols, items.size()); ++i) {
            QLayoutItem* it = items[i];
            const int h = it->hasHeightForWidth() ? it->heightForWidth(colW) : it->sizeHint().height();
            rowH = std::max({rowH, h, it->minimumSize().height()});
        }
        if (apply)
            for (int i = start; i < std::min<int>(start + cols, items.size()); ++i) {
                const int c = i - start;
                items[i]->setGeometry(QRect(rect.x() + c * (colW + m_h), y, colW, rowH));
            }
        y += rowH + m_v;
    }
    return y - m_v - rect.y();
}

} // namespace fw::ui
