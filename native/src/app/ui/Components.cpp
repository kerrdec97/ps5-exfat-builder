#include "app/ui/Components.h"

#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "app/ui/Style.h"

#include <QAbstractScrollArea>
#include <QAccessible>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QScopeGuard>
#include <QTextLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fw::ui {

using namespace theme;

namespace {

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

} // namespace

void paintCheckBubble(QPainter& p, const QRectF& r, qreal dpr)
{
    p.setPen(Qt::NoPen);
    p.setBrush(accentFillGradient(r));
    p.drawEllipse(r);
    const int is = int(r.width() * 0.62);
    p.drawPixmap(QRectF(r.center().x() - is / 2.0, r.center().y() - is / 2.0, is, is).toRect(),
                 iconPixmap(QStringLiteral("check"), is, color::on_accent, dpr));
}

// ---- SelectionGlow ----------------------------------------------------------------------

SelectionGlow::SelectionGlow(QWidget* target) : QWidget(target->parentWidget()), m_target(target)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setProperty("fwNoRing", true);
    // It dies with its target, wherever the target is moved to.
    connect(target, &QObject::destroyed, this, &QObject::deleteLater);
    follow();
}

void SelectionGlow::setItemRect(const QRectF& r, qreal radius)
{
    if (r == m_rect && qFuzzyCompare(radius + 1.0, m_radius + 1.0))
        return;
    m_rect = r;
    m_radius = radius;
    update();
}

void SelectionGlow::follow()
{
    // (The target may be gone while this object still waits for its own deletion.)
    if (m_following || !m_target)
        return;
    m_following = true;
    const auto done = qScopeGuard([this] { m_following = false; });
    // The glow needs room around the target. A tile widget often fills its
    // parent to the edge (a plain container of a layout), so the glow lives
    // in the nearest ancestor that has the room, under the branch the target
    // is in; the containers in between paint nothing.
    QWidget* host = m_target->parentWidget();
    QWidget* branch = m_target;
    QList<QPointer<QWidget>> chain{m_target};
    while (host) {
        chain << host;
        const QRect r = QRect(m_target->mapTo(host, QPoint(0, 0)), m_target->size()).adjusted(-kSpread, -kSpread, kSpread, kSpread);
        QWidget* up = host->parentWidget();
        const auto* area = up ? qobject_cast<QAbstractScrollArea*>(up->parentWidget()) : nullptr;
        if (host->rect().contains(r) || !up || host->isWindow() || (area && area->viewport() == up))
            break; // room enough, or as far up as it may go (a window, the content of a scroll area)
        branch = host;
        host = up;
    }
    // Whatever moves, resizes, hides or re-parents anything on the way up moves the glow.
    for (const QPointer<QWidget>& w : std::as_const(m_chain))
        if (w && !chain.contains(w))
            w->removeEventFilter(this);
    for (const QPointer<QWidget>& w : std::as_const(chain))
        if (!m_chain.contains(w))
            w->installEventFilter(this);
    m_chain = chain;
    if (!host)
        return;
    if (parentWidget() != host)
        setParent(host);
    setGeometry(QRect(m_target->mapTo(host, QPoint(0, 0)), m_target->size()).adjusted(-kSpread, -kSpread, kSpread, kSpread));
    stackUnder(branch);
    setVisible(m_target->isVisibleTo(host));
}

bool SelectionGlow::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
    case QEvent::ParentChange:
        if (watched != this)
            follow();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void SelectionGlow::paintEvent(QPaintEvent*)
{
    if (m_rect.isEmpty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    // Rings that fade outwards from the tile's edge (18 % there), one per pixel.
    const QRectF r = m_rect.translated(kSpread, kSpread);
    for (int i = 0; i < kSpread; ++i) {
        const qreal t = 1.0 - qreal(i) / kSpread;
        p.setPen(QPen(QBrush(accentGradient(r, int(46 * t * t))), 1.5));
        p.drawRoundedRect(r.adjusted(-i - 0.5, -i - 0.5, i + 0.5, i + 0.5), m_radius + i, m_radius + i);
    }
}

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
    m_hover = new motion::HoverFade(this);
    m_glow = new SelectionGlow(this);
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
    return mirrored(this, QRect(col * (w + kTileGap), row * (kTileH + kTileGap), w, kTileH)); // RTL: the first at the right
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
    // The tile's inside is worked out left to right and mirrored as it is drawn.
    const auto vis = [this](const QRectF& x) { return mirrored(this, x); };
    for (int i = 0; i < count(); ++i) {
        const Item& it = m_items[size_t(i)];
        const QRectF tile = QRectF(tileRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        const QRectF r = vis(tile);
        const bool sel = i == m_current;
        const qreal hov = it.enabled && !sel ? m_hover->amount(i) : 0.0;
        if (sel) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_2);
            p.drawRoundedRect(tile, radius::md, radius::md);
            p.setBrush(accentGradient(tile, 34));
            p.drawRoundedRect(tile, radius::md, radius::md);
            QPen pen(QBrush(accentGradient(tile, 230)), 1.5);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
        } else {
            p.setPen(QPen(blend(color::border_3, color::border_4, hov), 1));
            p.setBrush(blend(color::field_bg, color::bg_3, hov));
            p.drawRoundedRect(tile, radius::md, radius::md);
        }
        const QColor fgTitle = !it.enabled ? color::fg_5 : color::fg_0;
        const QColor fgText = !it.enabled ? color::fg_6 : color::fg_3;
        // Icon square.
        const QRectF ir(r.left() + 14, r.top() + 14, 34, 34);
        p.setPen(Qt::NoPen);
        if (sel) {
            p.setBrush(accentFillGradient(vis(ir)));
        } else {
            p.setBrush(color::bg_4);
        }
        p.drawRoundedRect(vis(ir), 9, 9);
        if (!it.icon.isEmpty())
            p.drawPixmap(vis(QRectF(ir.center().x() - 9, ir.center().y() - 9, 18, 18)).toRect(),
                         iconPixmap(it.icon, 18, sel ? color::on_accent : (it.enabled ? color::fg_2 : color::fg_5), dpr));
        // Title (+ tag).
        const qreal tx = ir.right() + 12;
        qreal right = r.right() - 14;
        if (sel)
            right -= 26;
        p.setFont(font::bodyStrong());
        const QFontMetrics tfm(font::bodyStrong());
        QString title = it.title;
        const int tagW = it.tag.isEmpty() ? 0 : badgeWidth(it.tag);
        title = tfm.elidedText(title, Qt::ElideRight, int(right - tx - (tagW ? tagW + 8 : 0)));
        p.setPen(fgTitle);
        const QRectF titleR(tx, ir.top(), right - tx, 20);
        p.drawText(vis(titleR), Qt::AlignLeft | Qt::AlignVCenter, title);
        if (tagW)
            paintBadge(p, vis(QRectF(tx + tfm.horizontalAdvance(title) + 8, ir.top() - 1, tagW, size::badge)), it.tag,
                       it.enabled ? Tone::Info : Tone::Neutral);
        // Description: up to three lines under the title, aligned with it.
        p.setFont(font::caption());
        p.setPen(fgText);
        const qreal dw = r.right() - 14 - tx;
        qreal y = ir.top() + 23;
        for (const QString& l : wrapLines(it.description, font::caption(), int(dw), 3)) {
            p.drawText(vis(QRectF(tx, y, dw, 16)), Qt::AlignLeft | Qt::AlignVCenter, l);
            y += 16;
        }
        if (sel)
            paintCheckBubble(p, vis(QRectF(r.right() - 32, r.top() + 12, 20, 20)), dpr);
        // Keyboard focus only, and inside the tile: outside it the widget's
        // own edge would cut the ring.
        if (sel && hasFocus() && (m_keyNav || hasKeyboardFocus(this))) {
            p.setPen(QPen(color::focus, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tile.adjusted(2.5, 2.5, -2.5, -2.5), radius::md - 3, radius::md - 3);
        }
    }
    // The selected tile's soft glow lies outside the tile: its own widget
    // draws it, around wherever the tile was just painted.
    m_glow->setItemRect(m_current >= 0 && m_current < count() ? QRectF(tileRect(m_current)).adjusted(0.5, 0.5, -0.5, -0.5) : QRectF(),
                        radius::md);
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
    if (i != m_hover->hovered()) {
        setCursor(i >= 0 && m_items[size_t(i)].enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        m_hover->setHovered(i);
    }
}

void ChoiceTiles::leaveEvent(QEvent*) { m_hover->setHovered(-1); }

void ChoiceTiles::keyPressEvent(QKeyEvent* e)
{
    const int k = e->key();
    if (k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down) {
        const bool back = k == Qt::Key_Up || (k == Qt::Key_Left && !isRightToLeft()) || (k == Qt::Key_Right && isRightToLeft());
        const int step = back ? -1 : 1; // left and right as they are on screen
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
    m_hover = new motion::Value(this, 0.0, motion::fast);
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
    m_hover->animateTo(1.0);
    QAbstractButton::enterEvent(e);
}

void DropZone::leaveEvent(QEvent* e)
{
    m_hover->animateTo(0.0);
    QAbstractButton::leaveEvent(e);
}

void DropZone::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    // 0 = at rest .. 1 = hovered, pressed or a drag over the page.
    const qreal active = m_hl || isDown() ? 1.0 : m_hover->value();
    p.setPen(Qt::NoPen);
    p.setBrush(color::field_bg);
    p.drawRoundedRect(r, radius::md, radius::md);
    if (active > 0.0) {
        QRadialGradient glow(r.center(), r.width() * 0.6);
        glow.setColorAt(0, withAlpha(color::accent, int((m_hl ? 46 : 26) * active)));
        glow.setColorAt(1, Qt::transparent);
        p.setBrush(glow);
        p.drawRoundedRect(r, radius::md, radius::md);
    }
    QPen pen(m_hl ? color::accent_hi : blend(color::border_4, color::accent, active), m_hl ? 2 : 1.4, Qt::DashLine);
    pen.setDashPattern({5, 4});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);

    const qreal dpr = devicePixelRatioF();
    // The icon square: bg_4, with the fill gradient fading in over it.
    const auto iconSquare = [&](const QRectF& ir, qreal rad, int glyph) {
        p.setPen(Qt::NoPen);
        p.setBrush(color::bg_4);
        p.drawRoundedRect(ir, rad, rad);
        if (active > 0.0) {
            p.setOpacity(active);
            p.setBrush(accentFillGradient(ir));
            p.drawRoundedRect(ir, rad, rad);
            p.setOpacity(1.0);
        }
        drawIconFade(p, QRectF(ir.center().x() - glyph / 2.0, ir.center().y() - glyph / 2.0, glyph, glyph).toRect(), m_icon,
                     glyph, color::fg_2, color::on_accent, active, dpr);
    };
    if (m_compact) {
        const QRectF ir(r.left() + 22, r.center().y() - 20, 40, 40);
        iconSquare(mirrored(this, ir), 11, 20);
        const qreal tx = ir.right() + 16;
        p.setFont(font::bodyStrong());
        p.setPen(color::fg_0);
        p.drawText(mirrored(this, QRectF(tx, r.center().y() - 19, r.right() - tx - 16, 20)), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setFont(font::caption());
        p.setPen(color::fg_3);
        p.drawText(mirrored(this, QRectF(tx, r.center().y() + 1, r.right() - tx - 16, 18)), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font::caption()).elidedText(m_text, Qt::ElideRight, int(r.right() - tx - 16)));
    } else {
        const QRectF ir(r.center().x() - 24, r.top() + 24, 48, 48);
        iconSquare(ir, 13, 24);
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

namespace {

// The disclosure's chevron: right when closed (left right to left), down when
// open, turning in between.
class Chevron : public QWidget {
public:
    explicit Chevron(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(size::icon, size::icon);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    motion::Value* turn = nullptr; // 0 = closed .. 1 = open

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const qreal t = turn ? turn->value() : 0.0;
        const qreal dpr = devicePixelRatioF();
        const bool rtl = isRightToLeft();
        const QString closed = rtl ? QStringLiteral("chevron_left") : QStringLiteral("chevron_right");
        if (t <= 0.0 || t >= 1.0) { // at rest: the icon itself, not a rotated one
            p.drawPixmap(rect(), iconPixmap(t >= 1.0 ? QStringLiteral("chevron_down") : closed, size::icon, color::fg_3, dpr));
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(width() / 2.0, height() / 2.0);
        p.rotate((rtl ? -90.0 : 90.0) * t);
        p.translate(-width() / 2.0, -height() / 2.0);
        p.drawPixmap(rect(), iconPixmap(closed, size::icon, color::fg_3, dpr));
    }
};

} // namespace

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
    auto* chevron = new Chevron(m_header);
    m_chevron = chevron;
    m_turn = new motion::Value(chevron, 0.0, motion::fast);
    chevron->turn = m_turn;
    m_hover = new motion::Value(this, 0.0, motion::fast);
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

void Disclosure::setPlain(bool plain)
{
    if (plain == m_plain)
        return;
    m_plain = plain;
    // Inside a card it is a row of that card, under a hairline: no plate of
    // its own, no side padding, a quieter title.
    m_header->layout()->setContentsMargins(plain ? QMargins(0, space::md, 0, space::sm) : QMargins(space::lg, 14, space::lg, 14));
    m_header->setProperty("fwRadius", plain ? radius::sm : radius::md);
    m_title->setFont(plain ? font::bodyStrong() : font::subtitle());
    m_body->setContentsMargins(plain ? QMargins(0, 0, 0, 0) : QMargins(space::lg, 0, space::lg, space::lg));
    update();
}

void Disclosure::setExpanded(bool on)
{
    m_expanded = on;
    m_bodyWidget->setVisible(on); // at once: only the chevron turns
    m_turn->animateTo(on ? 1.0 : 0.0);
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
        if (event->type() == QEvent::Enter)
            m_hover->animateTo(1.0);
        else if (event->type() == QEvent::Leave)
            m_hover->animateTo(0.0);
    }
    return QWidget::eventFilter(watched, event);
}

void Disclosure::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_plain) {
        // A row of the card it sits in: only a hairline above it.
        p.fillRect(QRectF(0, 0, width(), 1), color::border_1);
        return;
    }
    paintPlate(p, r, radius::md, m_expanded ? 0.0 : m_hover->value());
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
    mirrorPainter(p, this); // light and glyph only, no text: right to left they face the other way
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(r, radius::lg, radius::lg);
    p.setClipPath(clip);
    QLinearGradient base(r.topLeft(), r.bottomRight());
    base.setColorAt(0, color::hero_0);
    base.setColorAt(0.55, color::hero_1);
    base.setColorAt(1, color::hero_2);
    p.fillRect(r, base);
    // Soft light blobs.
    auto blob = [&](QPointF c, qreal radiusPx, QColor col) {
        QRadialGradient g(c, radiusPx);
        g.setColorAt(0, col);
        col.setAlpha(0);
        g.setColorAt(1, col);
        p.fillRect(r, g);
    };
    blob(QPointF(r.right() - 120, r.top() + 10), 260, withAlpha(color::accent_2, 90));
    blob(QPointF(r.right() - 330, r.bottom() + 40), 220, withAlpha(color::accent, 80));
    blob(QPointF(r.left() + 40, r.top() - 40), 200, withAlpha(color::accent, 50));
    // Decorative stacked-layers glyph, large and faint.
    const int gs = int(std::min(r.height() * 1.15, 230.0));
    p.setOpacity(0.10);
    p.drawPixmap(QRect(int(r.right() - gs - 30), int(r.center().y() - gs / 2.0), gs, gs),
                 iconPixmap(QStringLiteral("layers"), gs, color::fg_0, devicePixelRatioF()));
    p.setOpacity(1);
    p.setClipping(false);
    p.setPen(QPen(withAlpha(color::accent_hi, 50), 1));
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
    m_hover = new motion::Value(this, 0.0, motion::fast);
}

void StatTile::setValue(const QString& value, Tone tone)
{
    m_value->setText(value);
    setTextColor(m_value, tone == Tone::Neutral ? color::fg_0 : toneText(tone));
    if (m_iconToneSet)
        return; // the page decides the tile's tone itself
    if (tone == Tone::Neutral || tone == Tone::Info)
        m_tile->setTone(Tone::Info);
    else
        m_tile->setTone(tone);
}

void StatTile::setIconTone(Tone tone)
{
    m_iconToneSet = true;
    m_tile->setTone(tone);
}

Tone StatTile::iconTone() const { return m_tile->isToned() ? m_tile->tone() : Tone::Info; }

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
    m_hover->animateTo(1.0);
    QWidget::enterEvent(e);
}

void StatTile::leaveEvent(QEvent* e)
{
    m_hover->animateTo(0.0);
    QWidget::leaveEvent(e);
}

void StatTile::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintPlate(p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, m_hover->value());
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
    m_hover = new motion::Value(this, 0.0, motion::fast);
}

QSize ActionCard::sizeHint() const { return {260, 84}; }

void ActionCard::enterEvent(QEnterEvent* e)
{
    m_hover->animateTo(1.0);
    QAbstractButton::enterEvent(e);
}

void ActionCard::leaveEvent(QEvent* e)
{
    m_hover->animateTo(0.0);
    QAbstractButton::leaveEvent(e);
}

void ActionCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal active = isDown() ? 1.0 : m_hover->value(); // 0 = at rest .. 1 = hovered or pressed
    paintPlate(p, r, radius::md, active);
    if (active > 0.0) {
        p.setPen(QPen(QBrush(accentGradient(r, int(170 * active))), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius::md, radius::md);
    }
    const qreal dpr = devicePixelRatioF();
    // Worked out left to right, mirrored for a right-to-left card.
    const QRectF ir = mirrored(this, QRectF(r.left() + 16, r.center().y() - 21, 42, 42));
    // The icon square: the gradient as a tint, the fill gradient fading in over it.
    p.setPen(Qt::NoPen);
    if (active < 1.0) {
        p.setBrush(accentGradient(ir, 60));
        p.drawRoundedRect(ir, 11, 11);
    }
    if (active > 0.0) {
        p.setOpacity(active);
        p.setBrush(accentFillGradient(ir));
        p.drawRoundedRect(ir, 11, 11);
        p.setOpacity(1.0);
    }
    drawIconFade(p, QRectF(ir.center().x() - 10, ir.center().y() - 10, 20, 20).toRect(), m_icon, 20, color::accent_hi,
                 color::on_accent, active, dpr);
    const qreal tx = r.left() + 16 + 42 + 14;
    const qreal tw = r.right() - 36 - tx;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(mirrored(this, QRectF(tx, r.top() + 16, tw, 20)), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font::bodyStrong()).elidedText(text(), Qt::ElideRight, int(tw)));
    p.setFont(font::caption());
    p.setPen(color::fg_3);
    const QStringList lines = wrapLines(m_text, font::caption(), int(tw), 2);
    qreal y = r.top() + 38;
    for (const QString& l : lines) {
        p.drawText(mirrored(this, QRectF(tx, y, tw, 16)), Qt::AlignLeft | Qt::AlignVCenter, l);
        y += 16;
    }
    drawIconFade(p, mirrored(this, QRectF(r.right() - 30, r.center().y() - 8, 16, 16)).toRect(),
                 isRightToLeft() ? QStringLiteral("chevron_left") : QStringLiteral("chevron_right"), 16, color::fg_5,
                 color::accent_hi, active, dpr);
}

} // namespace fw::ui
