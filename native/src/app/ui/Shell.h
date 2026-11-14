#pragma once

// Shell chrome: Sidebar/NavItem (§4.1), AppHeader and StatusBar (§4.2),
// PageHeader (§4.3).

#include "app/ui/Widgets.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHash>
#include <QWidget>

namespace fw::ui {

class NavItem : public QAbstractButton {
    Q_OBJECT
public:
    NavItem(const QString& key, const QString& iconName, const QString& text, QWidget* parent = nullptr);
    QString key() const { return m_key; }
    void setCompact(bool compact);
    void setCount(int count); // 0 hides the badge
    QSize sizeHint() const override;
    // The 3 px selection bar: where it sits in this item, and how it looks.
    // A Sidebar draws one bar for all its items (it slides between them) and
    // tells them not to draw their own.
    QRect barRect() const;
    static void paintSelectionBar(QPainter& p, const QRectF& r);
    void setExternalBar(bool external);
    // While something is dragged over the window that a drop would open on
    // this item's page: a dashed accent outline, as on a drop zone.
    void setDropTarget(bool on);
    bool isDropTarget() const { return m_dropTarget; }
signals:
    void moveFocusRequested(int delta); // Up / Down arrows

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QString m_key, m_icon;
    bool m_compact = false;
    bool m_externalBar = false;
    bool m_dropTarget = false;
    int m_count = 0;
    motion::Value* m_hover; // the hover tint fades
};

class Sidebar : public QWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget* parent = nullptr);
    void addGroup(const QString& overline);
    NavItem* addItem(const QString& key, const QString& iconName, const QString& text);
    void addStretch();
    NavItem* addFooterItem(const QString& key, const QString& iconName, const QString& text);
    void setVersionText(const QString& text);
    void setCurrent(const QString& key);
    QString current() const;
    void setCompact(bool compact);
    bool isCompact() const { return m_compact; }
    void setCount(const QString& key, int count);
    QStringList keys() const;
    void focusNeighbour(NavItem* from, int delta);
    NavItem* item(const QString& key) const { return m_items.value(key); }
    void focusCurrent();
signals:
    void selected(const QString& key);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void syncBar(bool animate);
    QVBoxLayout* m_layout;
    QButtonGroup* m_group;
    QHash<QString, NavItem*> m_items;
    QList<QLabel*> m_groupLabels;
    QLabel* m_version;
    QWidget* m_bar;         // the selection bar, over the items
    motion::Value* m_barY;  // where it is on its way to the current item
    bool m_compact = false;
};

class AppHeader : public QWidget {
    Q_OBJECT
public:
    explicit AppHeader(QWidget* parent = nullptr);
    IconButton* menuButton() { return m_menu; }
    IconButton* helpButton() { return m_help; }
    Badge* queueChip() { return m_queueChip; }
    Badge* consoleChip() { return m_consoleChip; }
    void setQueueChip(const QString& text, Tone tone, bool visible);
    void setConsoleChip(const QString& text, Tone tone, bool visible);
    void setVersion(const QString& version); // pill next to the product name
signals:
    void queueChipClicked();
    void consoleChipClicked();

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    IconButton* m_menu;
    IconButton* m_help;
    Badge* m_queueChip;
    Badge* m_consoleChip;
    QWidget* m_queueChipWrap;
    QWidget* m_consoleChipWrap;
    Badge* m_versionPill = nullptr;
};

class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget* parent = nullptr);
    void setStatus(const QString& text, Tone tone);  // left: mark + text
    void setFreeSpace(const QString& text);          // mono-sm
    void setContext(const QString& text);            // right

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QLabel* m_mark;
    QLabel* m_text;
    QLabel* m_free;
    ElideLabel* m_context;
};

class PageHeader : public QWidget {
    Q_OBJECT
public:
    PageHeader(const QString& title, const QString& caption, QWidget* parent = nullptr);
    void setCaption(const QString& caption);
    QString caption() const; // the whole sentence, also while it is cut
    void setTitle(const QString& title);
    void setIcon(const QString& iconName); // gradient tile left of the title
    void addAction(QWidget* w);
    QHBoxLayout* actions() { return m_actions; }
    // The width of the page's content when it has one (form + dock: 1176 px):
    // in a wider window the buttons end where the content ends, not at the
    // window's edge. 0 (default): the full width.
    void setContentWidth(int px);
    int contentWidth() const { return m_contentWidth; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void fitTail();
    IconTile* m_icon;
    QLabel* m_title;
    ElideLabel* m_caption;
    QHBoxLayout* m_actions;
    QHBoxLayout* m_row = nullptr;
    QSpacerItem* m_tail = nullptr; // after the buttons: the width past the content
    int m_contentWidth = 0;
};

} // namespace fw::ui
