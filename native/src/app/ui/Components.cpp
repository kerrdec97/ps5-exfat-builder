#include "app/ui/Components.h"

#include "app/ui/Icons.h"
#include "app/ui/Style.h"

#include <QAccessible>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTextLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fw::ui {

using namespace theme;

namespace {

void paintCardSurface(QPainter& p, const QRectF& r, qreal rad, bool hover = false)
{
    QLinearGradient sheen(r.topLeft(), r.bottomLeft());
    sheen.setColorAt(0, hover ? QColor(0x19, 0x1f, 0x2e) : QColor(0x15, 0x1a, 0x27));
    sheen.setColorAt(1, hover ? color::bg_3 : color::bg_2);
    p.setPen(QPen(hover ? color::border_4 : color::border_2, 1));
    p.setBrush(sheen);
    p.drawRoundedRect(r, rad, rad);
}

// Wraps `text` into at most `maxLines` lines of `width` px, eliding the last.
QStringList wrapLines(const QString& text, const QFont& font, int width, int maxLines)
{
    QStringList out;
    if (text.isEmpty() || width <= 10)
        return out;
    QTextLayout layout(text, font);
    layout.beginLayout();
    while (out.size() < maxLines) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        const int start = line.textStart();
        const int len = line.textLength();
        if (out.size() == maxLines - 1 && start + len < text.size()) {
            out << QFontMetrics(font).elidedText(text.mid(start), Qt::ElideRight, width);
            break;
        }
        out << text.mid(start, len).trimmed();
    }
    layout.endLayout();
    return out;
}

void paintCheckBubble(QPainter& p, const QRectF& r, qreal dpr)
{
    p.setPen(Qt::NoPen);
    p.setBrush(accentGradient(r));
    p.drawEllipse(r);
    const int is = int(r.width() * 0.62);
    p.drawPixmap(QRectF(r.center().x() - is / 2.0, r.center().y() - is / 2.0, is, is).toRect(),
                 iconPixmap(QStringLiteral("check"), is, color::on_accent, dpr));
}

} // namespace

// ---- ChoiceTiles ----------------------------------------------------------------------

namespace {
constexpr int kTileMinW = 200;
constexpr int kTileGap = 12;
constexpr int kTileH = 104;
} // namespace

ChoiceTiles::ChoiceTiles(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setProperty("fwNoRing", true); // the selected tile shows the focus itself
}

QString ChoiceTiles::choiceTitle(int index) const
{
    return index >= 0 && index < count() ? m_items[size_t(index)].title : QString();
}

void ChoiceTiles::addChoice(const QString& title, const QString& description, const QString& icon, const QVariant& value,
                            const QString& tag)
{
    m_items.push_back({title, description, icon, tag, value.isValid() ? value : QVariant(title), true});
    if (m_current < 0)
        m_current = 0;
    updateGeometry();
    update();
}

void ChoiceTiles::setCurrentIndex(int index)
{
    if (index < 0 || index >= count() || index == m_current)
        return;
    m_current = index;
    update();
#if QT_CONFIG(accessibility)
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent ev(this, choiceTitle(index));
        QAccessible::updateAccessibility(&ev);
    }
#endif
    emit currentChanged(index);
}

QVariant ChoiceTiles::currentValue() const
{
    return m_current >= 0 && m_current < count() ? m_items[size_t(m_current)].value : QVariant();
}

void ChoiceTiles::setCurrentValue(const QVariant& value)
{
    for (int i = 0; i < count(); ++i)
        if (m_items[size_t(i)].value == value) {
            setCurrentIndex(i);
            return;
        }
}

void ChoiceTiles::setChoiceEnabled(int index, bool enabled)
{
    if (index < 0 || index >= count())
        return;
    m_items[size_t(index)].enabled = enabled;
    update();
}

int ChoiceTiles::columns(int w) const
{
    if (count() == 0)
        return 1;
    const int fit = std::max(1, (w + kTileGap) / (kTileMinW + kTileGap));
    return std::min(count(), fit);
}

int ChoiceTiles::tileHeight(int) const { return kTileH; }

int ChoiceTiles::heightForWidth(int w) const
{
    const int cols = columns(w);
    const int rows = (count() + cols - 1) / cols;
    return rows * tileHeight(w) + std::max(0, rows - 1) * kTileGap;
}

QSize ChoiceTiles::sizeHint() const { return {count() * (kTileMinW + 40), heightForWidth(count() * (kTileMinW + 40))}; }

QSize ChoiceTiles::minimumSizeHint() const { return {kTileMinW, kTileH}; }

QRect ChoiceTiles::tileRect(int i) const
{
    const int cols = columns(width());
    const int w = (width() - (cols - 1) * kTileGap) / cols;
    const int row = i / cols, col = i % cols;
    return QRect(col * (w + kTileGap), row * (kTileH + kTileGap), w, kTileH);
}

int ChoiceTiles::at(const QPoint& p) const
{
    for (int i = 0; i < count(); ++i)
        if (tileRect(i).contains(p))
            return i;
    return -1;
}

void ChoiceTiles::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < count(); ++i) {
        const Item& it = m_items[size_t(i)];
        const QRectF r = QRectF(tileRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool sel = i == m_current;
        const bool hov = i == m_hover && it.enabled && !sel;
        if (sel) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_2);
            p.drawRoundedRect(r, radius::md, radius::md);
            p.setBrush(accentGradient(r, 34));
            p.drawRoundedRect(r, radius::md, radius::md);
            QPen pen(QBrush(accentGradient(r, 230)), 1.5);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
        } else {
            p.setPen(QPen(hov ? color::border_4 : color::border_3, 1));
            p.setBrush(hov ? color::bg_3 : color::field_bg);
            p.drawRoundedRect(r, radius::md, radius::md);
        }
        const QColor fgTitle = !it.enabled ? color::fg_5 : color::fg_0;
        const QColor fgText = !it.enabled ? color::fg_6 : color::fg_3;
        // Icon square.
        const QRectF ir(r.left() + 14, r.top() + 14, 34, 34);
        p.setPen(Qt::NoPen);
        if (sel) {
            p.setBrush(accentGradient(ir));
        } else {
            p.setBrush(color::bg_4);
        }
        p.drawRoundedRect(ir, 9, 9);
        if (!it.icon.isEmpty())
            p.drawPixmap(QRectF(ir.center().x() - 9, ir.center().y() - 9, 18, 18).toRect(),
                         iconPixmap(it.icon, 18, sel ? color::on_accent : (it.enabled ? color::fg_2 : color::fg_5), dpr));
        // Title (+ tag).
        const qreal tx = ir.right() + 12;
        qreal right = r.right() - 14;
        if (sel)
            right -= 26;
        p.setFont(font::bodyStrong());
        const QFontMetrics tfm(font::bodyStrong());
        QString title = it.title;
        int tagW = 0;
        if (!it.tag.isEmpty()) {
            const QFontMetrics cfm(font::caption());
            tagW = cfm.horizontalAdvance(it.tag) + 14;
        }
        title = tfm.elidedText(title, Qt::ElideRight, int(right - tx - (tagW ? tagW + 6 : 0)));
        p.setPen(fgTitle);
        const QRectF titleR(tx, ir.top(), right - tx, 20);
        p.drawText(titleR, Qt::AlignLeft | Qt::AlignVCenter, title);
        if (tagW) {
            const QRectF tagR(tx + tfm.horizontalAdvance(title) + 8, ir.top() + 1, tagW, 18);
            p.setPen(Qt::NoPen);
            QColor tint = color::accent;
            tint.setAlpha(46);
            p.setBrush(tint);
            p.drawRoundedRect(tagR, 9, 9);
            p.setPen(color::accent_hi);
            p.setFont(font::caption());
            p.drawText(tagR, Qt::AlignCenter, it.tag);
        }
        // Description: up to three lines under the title, aligned with it.
        p.setFont(font::caption());
        p.setPen(fgText);
        const qreal dw = r.right() - 14 - tx;
        qreal y = ir.top() + 23;
        for (const QString& l : wrapLines(it.description, font::caption(), int(dw), 3)) {
            p.drawText(QRectF(tx, y, dw, 16), Qt::AlignLeft | Qt::AlignVCenter, l);
            y += 16;
        }
        if (sel)
            paintCheckBubble(p, QRectF(r.right() - 32, r.top() + 12, 20, 20), dpr);
        // Keyboard focus only, and inside the tile: outside it the widget's
        // own edge would cut the ring.
        if (sel && hasFocus() && (m_keyNav || hasKeyboardFocus(this))) {
            p.setPen(QPen(color::focus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(2.5, 2.5, -2.5, -2.5), radius::md - 3, radius::md - 3);
        }
    }
}

void ChoiceTiles::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_keyNav = false;
    update();
    const int i = at(e->pos());
    if (i >= 0 && m_items[size_t(i)].enabled)
        setCurrentIndex(i);
}

void ChoiceTiles::mouseMoveEvent(QMouseEvent* e)
{
    const int i = at(e->pos());
    if (i != m_hover) {
        m_hover = i;
        setCursor(i >= 0 && m_items[size_t(i)].enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void ChoiceTiles::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

void ChoiceTiles::keyPressEvent(QKeyEvent* e)
{
    const int k = e->key();
    if (k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down) {
        const int step = (k == Qt::Key_Left || k == Qt::Key_Up) ? -1 : 1;
        int i = m_current;
        m_keyNav = true;
        update();
        for (int n = 0; n < count(); ++n) {
            i = (i + step + count()) % count();
            if (m_items[size_t(i)].enabled) {
                setCurrentIndex(i);
                break;
            }
        }
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---- DropZone ----------------------------------------------------------------------

DropZone::DropZone(const QString& icon, const QString& title, const QString& text, QWidget* parent)
    : QAbstractButton(parent), m_icon(icon), m_title(title), m_text(text)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(148);
    setProperty("fwRadius", radius::md);
}

void DropZone::setTitle(const QString& title)
{
    m_title = title;
    setAccessibleName(title);
    update();
}

void DropZone::setText(const QString& text)
{
    m_text = text;
    update();
}

void DropZone::setHighlighted(bool on)
{
    m_hl = on;
    update();
}

void DropZone::setCompact(bool compact)
{
    m_compact = compact;
    setFixedHeight(compact ? 96 : 148);
    update();
}

QSize DropZone::sizeHint() const { return {480, m_compact ? 96 : 148}; }

void DropZone::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void DropZone::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void DropZone::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const bool active = m_hl || m_hover || isDown();
    p.setPen(Qt::NoPen);
    p.setBrush(color::field_bg);
    p.drawRoundedRect(r, radius::md, radius::md);
    if (active) {
        QRadialGradient glow(r.center(), r.width() * 0.6);
        QColor a = color::accent;
        a.setAlpha(m_hl ? 46 : 26);
        glow.setColorAt(0, a);
        glow.setColorAt(1, QColor(0, 0, 0, 0));
        p.setBrush(glow);
        p.drawRoundedRect(r, radius::md, radius::md);
    }
    QPen pen(m_hl ? color::accent_hi : active ? color::accent : color::border_4, m_hl ? 2 : 1.4, Qt::DashLine);
    pen.setDashPattern({5, 4});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);

    const qreal dpr = devicePixelRatioF();
    if (m_compact) {
        const QRectF ir(r.left() + 22, r.center().y() - 20, 40, 40);
        p.setPen(Qt::NoPen);
        p.setBrush(active ? accentGradient(ir) : QBrush(color::bg_4));
        p.drawRoundedRect(ir, 11, 11);
        p.drawPixmap(QRectF(ir.center().x() - 10, ir.center().y() - 10, 20, 20).toRect(),
                     iconPixmap(m_icon, 20, active ? color::on_accent : color::fg_2, dpr));
        const qreal tx = ir.right() + 16;
        p.setFont(font::bodyStrong());
        p.setPen(color::fg_0);
        p.drawText(QRectF(tx, r.center().y() - 19, r.right() - tx - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setFont(font::caption());
        p.setPen(color::fg_3);
        p.drawText(QRectF(tx, r.center().y() + 1, r.right() - tx - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font::caption()).elidedText(m_text, Qt::ElideRight, int(r.right() - tx - 16)));
    } else {
        const QRectF ir(r.center().x() - 24, r.top() + 24, 48, 48);
        p.setPen(Qt::NoPen);
        p.setBrush(active ? accentGradient(ir) : QBrush(color::bg_4));
        p.drawRoundedRect(ir, 13, 13);
        p.drawPixmap(QRectF(ir.center().x() - 12, ir.center().y() - 12, 24, 24).toRect(),
                     iconPixmap(m_icon, 24, active ? color::on_accent : color::fg_2, dpr));
        p.setFont(font::bodyStrong());
        p.setPen(color::fg_0);
        p.drawText(QRectF(r.left() + 16, ir.bottom() + 12, r.width() - 32, 20), Qt::AlignCenter, m_title);
        p.setFont(font::caption());
        p.setPen(color::fg_3);
        p.drawText(QRectF(r.left() + 16, ir.bottom() + 32, r.width() - 32, 18), Qt::AlignCenter,
                   QFontMetrics(font::caption()).elidedText(m_text, Qt::ElideRight, int(r.width() - 32)));
    }
}

// ---- Disclosure ----------------------------------------------------------------------

Disclosure::Disclosure(const QString& title, const QString& captionText, QWidget* parent) : QWidget(parent)
{
    setProperty("fwNoRing", true);
    auto* outer = vbox(this, 0, QMargins(0, 0, 0, 0));
    m_header = new QWidget(this);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setFocusPolicy(Qt::StrongFocus);
    // The role first: naming a widget can already make Qt create (and keep)
    // its accessible interface. See Accessible.h.
    m_header->setProperty("fwRole", QStringLiteral("button"));
    m_header->setProperty("fwExpanded", false);
    m_header->setProperty("fwRadius", radius::md);
    m_header->setAccessibleName(title);
    m_header->installEventFilter(this);
    auto* h = hbox(m_header, space::md, QMargins(space::lg, 14, space::lg, 14));
    m_chevron = new QLabel(m_header);
    m_chevron->setFixedSize(16, 16);
    h->addWidget(m_chevron);
    auto* col = new QVBoxLayout;
    col->setSpacing(1);
    m_title = label(title, TextStyle::Subtitle, color::fg_0, m_header);
    m_caption = label(captionText, TextStyle::Caption, color::fg_3, m_header);
    m_caption->setVisible(!captionText.isEmpty());
    col->addWidget(m_title);
    col->addWidget(m_caption);
    h->addLayout(col, 1);
    m_summary = new Badge({}, Tone::Neutral, m_header);
    m_summary->setIconName(QString());
    m_summary->hide();
    h->addWidget(m_summary);
    outer->addWidget(m_header);
    m_bodyWidget = new QWidget(this);
    m_body = vbox(m_bodyWidget, space::md, QMargins(space::lg, 0, space::lg, space::lg));
    outer->addWidget(m_bodyWidget);
    setExpanded(false);
}

void Disclosure::setExpanded(bool on)
{
    m_expanded = on;
    m_bodyWidget->setVisible(on);
    m_chevron->setPixmap(iconPixmap(on ? QStringLiteral("chevron_down") : QStringLiteral("chevron_right"), 16, color::fg_3,
                                    devicePixelRatioF()));
    m_header->setProperty("fwExpanded", on);
#if QT_CONFIG(accessibility)
    if (QAccessible::isActive()) {
        QAccessible::State changed;
        changed.expanded = true;
        changed.collapsed = true;
        QAccessibleStateChangeEvent ev(m_header, changed);
        QAccessible::updateAccessibility(&ev);
    }
#endif
    update();
    emit toggled(on);
}

void Disclosure::setSummary(const QString& text, Tone tone)
{
    m_summary->set(text, tone);
    m_summary->setVisible(!text.isEmpty());
}

void Disclosure::setCaption(const QString& text)
{
    m_caption->setText(text);
    m_caption->setVisible(!text.isEmpty());
}

bool Disclosure::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_header) {
        if (event->type() == QEvent::MouseButtonRelease) {
            // The left button, released on the header (not after dragging off it).
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() != Qt::LeftButton || !m_header->rect().contains(me->position().toPoint()))
                return false;
            setExpanded(!m_expanded);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(event);
            if (k->key() == Qt::Key_Space || k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
                if (!k->isAutoRepeat()) // a held key toggles once
                    setExpanded(!m_expanded);
                return true;
            }
        }
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
            update();
    }
    return QWidget::eventFilter(watched, event);
}

void Disclosure::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    paintCardSurface(p, r, radius::md, m_header->underMouse() && !m_expanded);
    if (m_expanded) {
        p.fillRect(QRectF(r.left() + space::lg, m_header->geometry().bottom(), r.width() - 2 * space::lg, 1), color::border_1);
    }
}

// ---- HeroBanner -------------------------------------------------------------------

HeroBanner::HeroBanner(const QString& overline, const QString& title, const QString& text, QWidget* parent) : QWidget(parent)
{
    setProperty("fwNoRing", true);
    setMinimumHeight(176);
    auto* v = vbox(this, 6, QMargins(28, 26, 28, 24));
    auto* ov = label(overline, TextStyle::Overline, color::accent_hi, this);
    v->addWidget(ov);
    m_title = label(title, TextStyle::Display, color::fg_0, this);
    m_title->setWordWrap(true);
    v->addWidget(m_title);
    m_text = label(text, TextStyle::Body, color::fg_2, this);
    m_text->setWordWrap(true);
    m_text->setMaximumWidth(620);
    v->addWidget(m_text);
    v->addSpacing(10);
    auto* aw = new QWidget(this);
    m_actions = hbox(aw, space::sm);
    v->addWidget(aw, 0, Qt::AlignLeft);
}

void HeroBanner::setTitle(const QString& title) { m_title->setText(title); }
void HeroBanner::setText(const QString& text) { m_text->setText(text); }

void HeroBanner::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(r, radius::lg, radius::lg);
    p.setClipPath(clip);
    QLinearGradient base(r.topLeft(), r.bottomRight());
    base.setColorAt(0, QColor(0x14, 0x22, 0x4d));
    base.setColorAt(0.55, QColor(0x1c, 0x17, 0x45));
    base.setColorAt(1, QColor(0x10, 0x12, 0x22));
    p.fillRect(r, base);
    // Soft light blobs.
    auto blob = [&](QPointF c, qreal radiusPx, QColor col) {
        QRadialGradient g(c, radiusPx);
        g.setColorAt(0, col);
        col.setAlpha(0);
        g.setColorAt(1, col);
        p.fillRect(r, g);
    };
    blob(QPointF(r.right() - 120, r.top() + 10), 260, QColor(0x8b, 0x5c, 0xf6, 90));
    blob(QPointF(r.right() - 330, r.bottom() + 40), 220, QColor(0x3d, 0x7b, 0xff, 80));
    blob(QPointF(r.left() + 40, r.top() - 40), 200, QColor(0x3d, 0x7b, 0xff, 50));
    // Decorative stacked-layers glyph, large and faint.
    const int gs = int(std::min(r.height() * 1.15, 230.0));
    p.setOpacity(0.10);
    p.drawPixmap(QRect(int(r.right() - gs - 30), int(r.center().y() - gs / 2.0), gs, gs),
                 iconPixmap(QStringLiteral("layers"), gs, color::fg_0, devicePixelRatioF()));
    p.setOpacity(1);
    p.setClipping(false);
    p.setPen(QPen(QColor(141, 181, 255, 50), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, radius::lg, radius::lg);
}

// ---- StatTile ---------------------------------------------------------------------

StatTile::StatTile(const QString& icon, const QString& overline, QWidget* parent) : QWidget(parent)
{
    setProperty("fwNoRing", true);
    setAttribute(Qt::WA_Hover);
    auto* v = vbox(this, 6, QMargins(18, 16, 18, 16));
    auto* top = new QHBoxLayout;
    top->setSpacing(10);
    m_tile = new IconTile(icon, 32, this);
    top->addWidget(m_tile);
    top->addWidget(label(overline, TextStyle::Overline, color::fg_4, this), 1);
    setAccessibleName(overline);
    v->addLayout(top);
    m_value = label({}, TextStyle::Stat, color::fg_0, this);
    v->addWidget(m_value);
    m_caption = label({}, TextStyle::Caption, color::fg_3, this);
    m_caption->setWordWrap(true);
    v->addWidget(m_caption);
    auto* fw = new QWidget(this);
    m_footer = hbox(fw, space::sm);
    v->addWidget(fw);
    v->addStretch(1);
}

void StatTile::setValue(const QString& value, Tone tone)
{
    m_value->setText(value);
    setTextColor(m_value, tone == Tone::Neutral ? color::fg_0 : toneText(tone));
    if (tone == Tone::Neutral || tone == Tone::Info)
        m_tile->setTone(Tone::Info);
    else
        m_tile->setTone(tone);
}

void StatTile::setCaption(const QString& caption)
{
    m_caption->setText(caption);
    m_caption->setVisible(!caption.isEmpty());
}

void StatTile::mouseReleaseEvent(QMouseEvent* e)
{
    const bool hit = e->button() == Qt::LeftButton && rect().contains(e->pos());
    QWidget::mouseReleaseEvent(e);
    if (hit) {
        e->accept();
        emit clicked(); // last: a slot may rebuild the page this tile is on
    }
}

void StatTile::connectNotify(const QMetaMethod& signal)
{
    // Somebody listens for clicks: the tile is a control then, reachable with
    // Tab and pressed with Space or Enter like a button.
    if (signal == QMetaMethod::fromSignal(&StatTile::clicked) && focusPolicy() == Qt::NoFocus) {
        setFocusPolicy(Qt::TabFocus);
        setProperty("fwNoRing", false);
        setProperty("fwRadius", radius::md);
    }
    QWidget::connectNotify(signal);
}

void StatTile::keyPressEvent(QKeyEvent* e)
{
    const bool press = e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
    if (press && focusPolicy() != Qt::NoFocus && !e->isAutoRepeat()) {
        e->accept();
        emit clicked();
        return;
    }
    QWidget::keyPressEvent(e);
}

void StatTile::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void StatTile::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

void StatTile::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintCardSurface(p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, m_hover);
}

// ---- ActionCard -------------------------------------------------------------------

ActionCard::ActionCard(const QString& icon, const QString& title, const QString& text, QWidget* parent)
    : QAbstractButton(parent), m_icon(icon), m_text(text)
{
    setText(title);
    setAccessibleName(title);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(84);
    setProperty("fwRadius", radius::md);
}

QSize ActionCard::sizeHint() const { return {260, 84}; }

void ActionCard::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void ActionCard::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void ActionCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool active = m_hover || isDown();
    paintCardSurface(p, r, radius::md, active);
    if (active) {
        p.setPen(QPen(QBrush(accentGradient(r, 170)), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius::md, radius::md);
    }
    const qreal dpr = devicePixelRatioF();
    const QRectF ir(r.left() + 16, r.center().y() - 21, 42, 42);
    p.setPen(Qt::NoPen);
    p.setBrush(active ? accentGradient(ir) : accentGradient(ir, 60));
    p.drawRoundedRect(ir, 11, 11);
    p.drawPixmap(QRectF(ir.center().x() - 10, ir.center().y() - 10, 20, 20).toRect(),
                 iconPixmap(m_icon, 20, active ? color::on_accent : color::accent_hi, dpr));
    const qreal tx = ir.right() + 14;
    const qreal tw = r.right() - 36 - tx;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(QRectF(tx, r.top() + 16, tw, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font::bodyStrong()).elidedText(text(), Qt::ElideRight, int(tw)));
    p.setFont(font::caption());
    p.setPen(color::fg_3);
    const QStringList lines = wrapLines(m_text, font::caption(), int(tw), 2);
    qreal y = r.top() + 38;
    for (const QString& l : lines) {
        p.drawText(QRectF(tx, y, tw, 16), Qt::AlignLeft | Qt::AlignVCenter, l);
        y += 16;
    }
    p.drawPixmap(QRectF(r.right() - 30, r.center().y() - 8, 16, 16).toRect(),
                 iconPixmap(QStringLiteral("chevron_right"), 16, active ? color::accent_hi : color::fg_5, dpr));
}

} // namespace fw::ui
