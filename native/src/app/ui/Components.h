#pragma once

// v5 page-level components, built on the design tokens (Theme.h):
//
//  * ChoiceTiles  — large selectable tiles (icon, title, one-line description,
//                   optional tag) for the one decision a page is about
//                   (image format, target firmware...). Keyboard: arrows.
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
#include <QVariant>
#include <QWidget>

#include <vector>

class QVBoxLayout;
class QHBoxLayout;

namespace fw::ui {

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
    int m_hover = -1;
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
    bool m_hl = false, m_hover = false, m_compact = false;
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

signals:
    void toggled(bool expanded);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_header;
    QLabel* m_chevron;
    QLabel* m_title;
    QLabel* m_caption;
    Badge* m_summary;
    QWidget* m_bodyWidget;
    QVBoxLayout* m_body;
    bool m_expanded = false;
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
    void setCaption(const QString& caption);
    QLabel* valueLabel() { return m_value; }
    QHBoxLayout* footer() { return m_footer; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    IconTile* m_tile;
    QLabel* m_value;
    QLabel* m_caption;
    QHBoxLayout* m_footer;
    bool m_hover = false;
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
    bool m_hover = false;
};

} // namespace fw::ui
