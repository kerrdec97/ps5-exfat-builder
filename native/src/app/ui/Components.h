#pragma once

// v5 page-level components, built on the design tokens (Theme.h):
//
//  * ChoiceTiles  — large selectable tiles (icon, title, a description of up
//                   to three lines, optional tag) for the one decision a page
//                   is about (image format, target firmware...). Keyboard: arrows.
//  * DropZone     — the empty state of a "pick a folder" step: dashed area,
//                   click or Enter to browse, accepts a dropped folder/file.
//  * Disclosure   — a card whose body folds away ("Advanced options"), with a
//                   summary on the right ("3 changed").
//  * HeroBanner   — the gradient welcome banner on Home.
//  * StatTile     — icon + overline + big value + caption (Home, dashboards).
//  * ActionCard   — a clickable card with an icon tile, title and text
//                   (Home quick actions, empty pages).

#include "app/ui/Widgets.h"

#include <QAbstractButton>
#include <QList>
#include <QPointer>
#include <QVariant>
#include <QWidget>

#include <vector>

class QVBoxLayout;
class QHBoxLayout;

namespace fw::ui {

class SelectionGlow;

class ChoiceTiles : public QWidget {
    Q_OBJECT
public:
    explicit ChoiceTiles(QWidget* parent = nullptr);
    void addChoice(const QString& title, const QString& description, const QString& icon, const QVariant& value,
                   const QString& tag = {});
    void setCurrentIndex(int index);
    int currentIndex() const { return m_current; }
    QVariant currentValue() const;
    void setCurrentValue(const QVariant& value);
    int count() const { return int(m_items.size()); }
    QString choiceTitle(int index) const; // "" when out of range
    void setChoiceEnabled(int index, bool enabled);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Item {
        QString title, description, icon, tag;
        QVariant value;
        bool enabled = true;
    };
    QRect tileRect(int i) const;
    int columns(int w) const;
    int tileHeight(int w) const;
    int at(const QPoint& p) const;
    std::vector<Item> m_items;
    int m_current = -1;
    motion::HoverFade* m_hover; // which tile the pointer is over; the tint fades
    SelectionGlow* m_glow;      // the selected tile's soft glow
    bool m_keyNav = false; // an arrow key chose the tile last
};

class DropZone : public QAbstractButton {
    Q_OBJECT
public:
    DropZone(const QString& icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setText(const QString& text);
    void setHighlighted(bool on); // a drag is over the page
    void setCompact(bool compact); // 96 px instead of 148 px
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {200, m_compact ? 88 : 132}; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QString m_icon, m_title, m_text;
    bool m_hl = false, m_compact = false;
    motion::Value* m_hover; // the hover glow fades
};

class Disclosure : public QWidget {
    Q_OBJECT
public:
    Disclosure(const QString& title, const QString& caption = {}, QWidget* parent = nullptr);
    QVBoxLayout* body() { return m_body; }
    void setExpanded(bool on);
    bool isExpanded() const { return m_expanded; }
    void setSummary(const QString& text, Tone tone = Tone::Neutral); // right side of the header
    void setCaption(const QString& text);
    // For a disclosure inside a card: a plain header row under a hairline
    // instead of a plate of its own (a plate in a plate has two outlines).
    void setPlain(bool plain);
    bool isPlain() const { return m_plain; }

signals:
    void toggled(bool expanded);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_header;
    QWidget* m_chevron;      // turns when the body opens
    motion::Value* m_turn;   // 0 = closed (right), 1 = open (down)
    motion::Value* m_hover;  // pointer over the header
    QLabel* m_title;
    QLabel* m_caption;
    Badge* m_summary;
    QWidget* m_bodyWidget;
    QVBoxLayout* m_body;
    bool m_expanded = false;
    bool m_plain = false;
};

class HeroBanner : public QWidget {
    Q_OBJECT
public:
    HeroBanner(const QString& overline, const QString& title, const QString& text, QWidget* parent = nullptr);
    QHBoxLayout* actions() { return m_actions; }
    void setTitle(const QString& title);
    void setText(const QString& text);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QLabel* m_title;
    QLabel* m_text;
    QHBoxLayout* m_actions;
};

class StatTile : public QWidget {
    Q_OBJECT
public:
    StatTile(const QString& icon, const QString& overline, QWidget* parent = nullptr);
    void setValue(const QString& value, Tone tone = Tone::Neutral);
    // The icon tile's tone as the state of what the tile reports: Neutral is
    // the quiet tile (nothing to say), Info the accent tile (active), Ok /
    // Warn / Err the badge tones. Without this call the tile follows setValue().
    void setIconTone(Tone tone);
    Tone iconTone() const;
    void setCaption(const QString& caption);
    QLabel* valueLabel() { return m_value; }
    QHBoxLayout* footer() { return m_footer; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void connectNotify(const QMetaMethod& signal) override; // a tile with a click handler takes keyboard focus

private:
    IconTile* m_tile;
    QLabel* m_value;
    QLabel* m_caption;
    QHBoxLayout* m_footer;
    motion::Value* m_hover; // the hover tint fades
    bool m_iconToneSet = false;
};

class ActionCard : public QAbstractButton {
    Q_OBJECT
public:
    ActionCard(const QString& icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {180, 76}; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QString m_icon, m_text;
    motion::Value* m_hover; // the hover tint fades
};

// The check mark of a selected tile: a round bubble in the fill gradient.
void paintCheckBubble(QPainter& p, const QRectF& r, qreal dpr);

// The soft glow of the selected tile of a widget that paints its tiles
// itself: the signature gradient at 18 %, fading out over 6 px. Painted (no
// graphics effect), and it lies outside the tile, so it is drawn by a widget
// of its own placed under the target (in the nearest ancestor with room for
// it) that follows the target; the target's own geometry does not change.
// The object belongs to the target.
class SelectionGlow : public QWidget {
public:
    explicit SelectionGlow(QWidget* target);
    // The tile to glow around, in the target's coordinates (an empty
    // rectangle: no glow), and its corner radius.
    void setItemRect(const QRectF& r, qreal radius);
    QRectF itemRect() const { return m_rect; }
    static constexpr int kSpread = 6;

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void follow();
    QPointer<QWidget> m_target;
    QList<QPointer<QWidget>> m_chain; // the target and its ancestors up to the glow's parent: watched
    QRectF m_rect;
    qreal m_radius = 0;
    bool m_following = false;
};

} // namespace fw::ui
