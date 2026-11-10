#pragma once

// A game / image collection for the Library: a cover-art grid or a dense list
// of the same items, with multi-selection (click, Ctrl/Shift-click, the check
// bubble, Ctrl+A, Space), activation (double-click / Enter) and a context menu
// request. Painted in one widget (no item widgets), so thousands of entries
// stay cheap; it reports its height for a width, so it sits in a page's scroll
// area and grows with its content.
//
// Covers: the item's cover file, cut to a square; without one, a tile in the
// signature gradient with the title's initials.

#include "app/ui/Widgets.h"

#include <QSet>

#include <vector>

namespace fw::ui {

struct GridItem {
    QString key;          // unique: the folder or file path
    QString title;
    QString subtitle;     // "PPSA01325 · v01.002.000"
    QString size;         // "48.5 GB"
    QString path;         // shown in the list (middle-elided) and the tooltip
    QString coverPath;    // "" = a generated tile
    QString icon = QStringLiteral("game"); // on generated tiles, next to the initials
    QStringList chips;    // "Folder", "exFAT", ... (the first chip is neutral when `firstChipNeutral`)
    bool firstChipNeutral = false;
    QString badge;        // list only, e.g. "Not backported"
    Tone badgeTone = Tone::Neutral;
    QString tooltip;
};

class LibraryGrid : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Grid, List };
    explicit LibraryGrid(QWidget* parent = nullptr);

    void setItems(std::vector<GridItem> items); // keeps the selection of keys still present
    const std::vector<GridItem>& items() const { return m_items; }
    int count() const { return int(m_items.size()); }
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    // The list's column titles (title, ID/subtitle, size, chips, badge).
    void setListHeaders(const QStringList& headers);

    QStringList selectedKeys() const; // in item order
    void setSelectedKeys(const QStringList& keys);
    void selectAll();
    void clearSelection();
    QString currentKey() const;
    // How many cover files were opened and decoded so far, by any grid (tests).
    static int coverDecodeCount();

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void selectionChanged();
    void activated(const QString& key);
    void contextRequested(const QString& key, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool event(QEvent* e) override;

private:
    int columns(int w) const;
    int tileWidth(int w) const;
    QRect itemRect(int i) const;
    QRect checkRect(int i) const;
    int itemAt(const QPoint& p) const;
    void setCurrent(int i, bool ensureVisible);
    void paintTile(QPainter& p, int i, const QRect& r);
    void paintRow(QPainter& p, int i, const QRect& r);
    void paintHeader(QPainter& p);
    void paintCover(QPainter& p, const GridItem& it, const QRectF& r, qreal radius, bool roundBottom);
    void paintCheck(QPainter& p, const QRectF& r, bool on, bool round);
    qreal paintChips(QPainter& p, const GridItem& it, qreal x, qreal y, qreal maxRight);
    bool listWide() const { return width() >= 900; }

    std::vector<GridItem> m_items;
    QSet<QString> m_selected;
    QStringList m_headers;
    Mode m_mode = Mode::Grid;
    int m_current = -1;
    int m_anchor = -1;
    motion::HoverFade* m_hover; // the item under the pointer; its tint fades
    bool m_keyNav = false; // the keyboard moved the current item last: show where it is
};

} // namespace fw::ui
