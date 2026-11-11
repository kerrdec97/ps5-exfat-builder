#include "app/ui/LibraryGrid.h"

#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "app/ui/Style.h"
#include "core/Tr.h"

#include <QContextMenuEvent>
#include <QFileInfo>
#include <QHelpEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QCache>
#include <QPointer>
#include <QScrollArea>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace fw::ui {

using namespace theme;

namespace {

constexpr int kMinTile = 168;
constexpr int kGap = 16;
constexpr int kText = 84;    // under the cover: title, subtitle, chips
constexpr int kHeader = size::tableHeader; // list
constexpr int kRow = 56;
constexpr int kListCover = 40;
constexpr int kChipGap = 4;

QFont headerFont()
{
    QFont f = font::caption();
    f.setWeight(QFont::DemiBold);
    return f;
}

// Covers are read from disk once per file: a thumbnail (short side at most
// kThumb px, decoded at that size) is kept per path, and the tiles are cut
// from it, so resizing the window, hovering and repainting never touch the
// disk again. A file that cannot be read is remembered too, until the items
// are set again: a missing cover on an unplugged drive or a sleeping share is
// not opened on every repaint. The caches have their own budget; Qt's shared
// 10 MB pixmap cache holds about twenty covers at 200 % scaling.
constexpr int kThumb = 512;

struct CoverCache {
    QCache<QString, QImage> thumbs{64 * 1024};  // cost in KiB
    QCache<QString, QPixmap> tiles{128 * 1024};
    QSet<QString> failed;
    int decodes = 0;
};

CoverCache& covers()
{
    static CoverCache c;
    return c;
}

int costKiB(qsizetype bytes) { return int(std::max<qsizetype>(1, bytes / 1024)); }

const QImage* coverThumb(const QString& path)
{
    CoverCache& c = covers();
    if (const QImage* hit = c.thumbs.object(path))
        return hit;
    if (c.failed.contains(path))
        return nullptr;
    ++c.decodes;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && std::min(full.width(), full.height()) > kThumb) {
        const qreal f = qreal(kThumb) / std::min(full.width(), full.height());
        reader.setScaledSize(QSize(std::max(1, qRound(full.width() * f)), std::max(1, qRound(full.height() * f))));
    }
    QImage img = reader.read();
    if (img.isNull()) {
        c.failed.insert(path);
        return nullptr;
    }
    auto* thumb = new QImage(std::move(img));
    if (!c.thumbs.insert(path, thumb, costKiB(thumb->sizeInBytes())))
        return nullptr; // larger than the whole budget: insert() deleted it
    return c.thumbs.object(path);
}

// The cover file cut to w×h device pixels.
QPixmap coverPixmap(const QString& path, const QSize& px)
{
    if (path.isEmpty() || px.isEmpty())
        return {};
    CoverCache& c = covers();
    const QString key = QStringLiteral("%1|%2x%3").arg(path).arg(px.width()).arg(px.height());
    if (const QPixmap* hit = c.tiles.object(key))
        return *hit;
    const QImage* thumb = coverThumb(path);
    if (!thumb)
        return {};
    QImage img = thumb->scaled(px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    img = img.copy((img.width() - px.width()) / 2, (img.height() - px.height()) / 2, px.width(), px.height());
    const QPixmap pm = QPixmap::fromImage(img);
    c.tiles.insert(key, new QPixmap(pm), costKiB(img.sizeInBytes()));
    return pm;
}

} // namespace

LibraryGrid::LibraryGrid(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setProperty("fwNoRing", true); // the current item shows the focus itself
    setAccessibleName(FW_TR("Items"));
    m_headers = {FW_TR("Title"), FW_TR("ID · version"), FW_TR("Size"), FW_TR("Formats"), FW_TR("Status")};
    m_hover = new motion::HoverFade(this, [this](int i) {
        if (i < count())
            update(itemRect(i).adjusted(-4, -4, 4, 4)); // one tile, not the grid
    });
}

int LibraryGrid::coverDecodeCount() { return covers().decodes; }

void LibraryGrid::setItems(std::vector<GridItem> items)
{
    covers().failed.clear(); // a rescan: covers that could not be read get another try
    const QString cur = currentKey();
    m_items = std::move(items);
    QSet<QString> keep;
    m_current = -1;
    for (int i = 0; i < count(); ++i) {
        if (m_selected.contains(m_items[size_t(i)].key))
            keep.insert(m_items[size_t(i)].key);
        if (m_items[size_t(i)].key == cur)
            m_current = i;
    }
    const bool changed = keep != m_selected;
    m_selected = keep;
    m_hover->reset();
    m_anchor = std::min(m_anchor, count() - 1);
    updateGeometry();
    update();
    if (changed)
        emit selectionChanged();
}

void LibraryGrid::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    updateGeometry();
    update();
}

void LibraryGrid::setListHeaders(const QStringList& headers)
{
    m_headers = headers;
    update();
}

QStringList LibraryGrid::selectedKeys() const
{
    QStringList out;
    for (const GridItem& it : m_items)
        if (m_selected.contains(it.key))
            out << it.key;
    return out;
}

void LibraryGrid::setSelectedKeys(const QStringList& keys)
{
    QSet<QString> s;
    for (const GridItem& it : m_items)
        if (keys.contains(it.key))
            s.insert(it.key);
    if (s == m_selected)
        return;
    m_selected = s;
    update();
    emit selectionChanged();
}

void LibraryGrid::selectAll()
{
    QStringList all;
    for (const GridItem& it : m_items)
        all << it.key;
    setSelectedKeys(all);
}

void LibraryGrid::clearSelection() { setSelectedKeys({}); }

QString LibraryGrid::currentKey() const { return m_current >= 0 && m_current < count() ? m_items[size_t(m_current)].key : QString(); }

// ---- geometry ----------------------------------------------------------------------------

int LibraryGrid::columns(int w) const { return std::max(1, (w + kGap) / (kMinTile + kGap)); }

int LibraryGrid::tileWidth(int w) const
{
    const int cols = columns(w);
    return (w - (cols - 1) * kGap) / cols;
}

int LibraryGrid::heightForWidth(int w) const
{
    if (m_items.empty())
        return 0;
    if (m_mode == Mode::List)
        return kHeader + count() * kRow;
    const int cols = columns(w);
    const int rows = (count() + cols - 1) / cols;
    return rows * (tileWidth(w) + kText) + (rows - 1) * kGap;
}

QSize LibraryGrid::sizeHint() const { return {800, heightForWidth(800)}; }

QSize LibraryGrid::minimumSizeHint() const { return {kMinTile, 0}; }

QRect LibraryGrid::itemRect(int i) const
{
    if (m_mode == Mode::List)
        return QRect(0, kHeader + i * kRow, width(), kRow);
    const int cols = columns(width());
    const int tw = tileWidth(width());
    return mirrored(this, QRect((i % cols) * (tw + kGap), (i / cols) * (tw + kText + kGap), tw, tw + kText)); // RTL: first at the right
}

// Inside an item everything is worked out left to right in the item's mirror
// image (itemRect() as it would be left to right) and mirrored where it is
// drawn: right to left the check box, the cover and the columns start at the right.
QRect LibraryGrid::checkRect(int i) const
{
    const QRect r = mirrored(this, itemRect(i));
    if (m_mode == Mode::List)
        return mirrored(this, QRect(r.left() + 10, r.center().y() - 9, 18, 18));
    return mirrored(this, QRect(r.left() + 10, r.top() + 10, 24, 24));
}

int LibraryGrid::itemAt(const QPoint& p) const
{
    if (m_mode == Mode::List) {
        if (p.y() < kHeader)
            return -1;
        const int i = (p.y() - kHeader) / kRow;
        return i >= 0 && i < count() ? i : -1;
    }
    for (int i = 0; i < count(); ++i)
        if (itemRect(i).contains(p))
            return i;
    return -1;
}

void LibraryGrid::setCurrent(int i, bool ensureVisible)
{
    if (i < 0 || i >= count())
        return;
    m_current = i;
    setAccessibleDescription(m_items[size_t(i)].title); // the items are painted: say which one is current
    update();
    if (!ensureVisible)
        return;
    for (QWidget* w = parentWidget(); w; w = w->parentWidget())
        if (auto* sa = qobject_cast<QScrollArea*>(w)) {
            const QRect r = itemRect(i);
            const QPoint c = mapTo(sa->widget(), r.center());
            sa->ensureVisible(c.x(), c.y(), 0, r.height() / 2 + kGap);
            break;
        }
}

// ---- painting ----------------------------------------------------------------------------

void LibraryGrid::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_mode == Mode::List)
        paintHeader(p);
    for (int i = 0; i < count(); ++i) {
        const QRect r = itemRect(i);
        if (!r.intersects(e->rect()))
            continue;
        if (m_mode == Mode::List)
            paintRow(p, i, r);
        else
            paintTile(p, i, r);
    }
}

void LibraryGrid::paintCover(QPainter& p, const GridItem& it, const QRectF& r, qreal rad, bool roundBottom)
{
    QPainterPath clip;
    if (roundBottom) {
        clip.addRoundedRect(r, rad, rad);
    } else {
        clip.moveTo(r.left(), r.bottom());
        clip.lineTo(r.left(), r.top() + rad);
        clip.quadTo(r.left(), r.top(), r.left() + rad, r.top());
        clip.lineTo(r.right() - rad, r.top());
        clip.quadTo(r.right(), r.top(), r.right(), r.top() + rad);
        clip.lineTo(r.right(), r.bottom());
        clip.closeSubpath();
    }
    p.save();
    p.setClipPath(clip);
    const qreal dpr = devicePixelRatioF();
    const QPixmap pm = coverPixmap(it.coverPath, (r.size() * dpr).toSize());
    if (!pm.isNull()) {
        p.drawPixmap(r, pm, QRectF(QPointF(0, 0), QSizeF(pm.size())));
    } else {
        // A generated cover (the clip above gives it its corners).
        paintCoverPlaceholder(p, r, it.title, it.coverId.isEmpty() ? (it.title.isEmpty() ? it.key : it.title) : it.coverId, 0);
        if (!roundBottom && r.width() > 100) {
            QColor faint = color::on_accent;
            faint.setAlpha(150);
            p.drawPixmap(QRectF(isRightToLeft() ? r.right() - 30 : r.left() + 12, r.bottom() - 30, 18, 18).toRect(),
                         iconPixmap(it.icon, 18, faint, dpr));
        }
    }
    p.restore();
}

void LibraryGrid::paintCheck(QPainter& p, const QRectF& r, bool on, bool round)
{
    const qreal rad = round ? r.width() / 2 : 5;
    p.setPen(Qt::NoPen);
    if (on) {
        p.setBrush(accentFillGradient(r));
        p.drawRoundedRect(r, rad, rad);
        p.drawPixmap(QRectF(r.center().x() - 7, r.center().y() - 7, 14, 14).toRect(), iconPixmap(QStringLiteral("check"), 14, color::on_accent, devicePixelRatioF()));
    } else {
        QColor fill = color::bg_0;
        fill.setAlpha(round ? 150 : 255);
        p.setBrush(fill);
        p.setPen(QPen(round ? color::fg_2 : color::border_control, 1.5));
        p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), rad, rad);
    }
}

qreal LibraryGrid::paintChips(QPainter& p, const GridItem& it, qreal x, qreal y, qreal maxRight)
{
    // `y` is the top of the 22 px chips.
    const auto chip = [&](const QString& t, bool neutral) {
        const qreal w = badgeWidth(t);
        paintBadge(p, mirrored(this, QRectF(x, y, w, size::badge)), t, neutral ? Tone::Neutral : Tone::Info);
        x += w + kChipGap;
    };
    for (int c = 0; c < it.chips.size(); ++c) {
        const qreal w = badgeWidth(it.chips[c]);
        const int rest = int(it.chips.size()) - c - 1;
        const qreal more = rest > 0 ? badgeWidth(QStringLiteral("+%1").arg(rest)) + kChipGap : 0;
        if (x + w + more > maxRight) {
            // Not all fit: a "+N" chip says how many are left out.
            const QString plus = QStringLiteral("+%1").arg(it.chips.size() - c);
            if (x + badgeWidth(plus) <= maxRight)
                chip(plus, true);
            break;
        }
        chip(it.chips[c], c == 0 && it.firstChipNeutral);
    }
    return x;
}

void LibraryGrid::paintTile(QPainter& p, int i, const QRect& rect)
{
    const GridItem& it = m_items[size_t(i)];
    const QRectF tile = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const QRectF r = mirrored(this, tile); // left to right, mirrored as it is drawn
    const auto vis = [this](const QRectF& x) { return mirrored(this, x); };
    const bool sel = m_selected.contains(it.key);
    const qreal hov = m_hover->amount(i); // 0..1: the hover tint fades
    // Card.
    paintPlate(p, tile, radius::md, hov);
    // Cover.
    const QRectF cover(r.left() + 1, r.top() + 1, r.width() - 2, r.width() - 2);
    paintCover(p, it, vis(cover), radius::md - 1, false);
    // Text.
    const qreal x = r.left() + 12;
    const qreal w = r.width() - 24;
    qreal y = cover.bottom() + 10;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(vis(QRectF(x, y, w, 18)), naturalText(this, Qt::AlignLeft | Qt::AlignVCenter, it.title),
               QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(w)));
    y += 20;
    p.setFont(font::caption());
    p.setPen(color::fg_3);
    p.drawText(vis(QRectF(x, y, w, 16)), naturalText(this, Qt::AlignLeft | Qt::AlignVCenter, it.subtitle),
               QFontMetrics(font::caption()).elidedText(it.subtitle, Qt::ElideRight, int(w)));
    y += 20;
    qreal sizeW = 0;
    if (!it.size.isEmpty()) {
        p.setFont(font::caption());
        sizeW = QFontMetrics(font::caption()).horizontalAdvance(it.size);
        p.setPen(color::fg_4);
        p.drawText(vis(QRectF(r.right() - 12 - sizeW, y, sizeW, size::badge)), leftToRightText(this, Qt::AlignRight | Qt::AlignVCenter), it.size);
    }
    paintChips(p, it, x, y, r.right() - 12 - sizeW - 6);
    // Selection.
    if (sel) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(accentGradient(tile, 240)), 2));
        p.drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
    }
    if (sel || i == m_hover->hovered() || !m_selected.isEmpty())
        paintCheck(p, checkRect(i), sel, true);
    // Keyboard focus only, and inside the tile: outside it the grid's own
    // edge would cut the ring of the first row and of the outer columns.
    if (i == m_current && hasFocus() && (m_keyNav || hasKeyboardFocus(this))) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color::focus, 2));
        p.drawRoundedRect(tile.adjusted(2.5, 2.5, -2.5, -2.5), radius::md - 3, radius::md - 3);
    }
}

void LibraryGrid::paintHeader(QPainter& p)
{
    p.setFont(headerFont());
    p.setPen(color::fg_4);
    const qreal y = 0;
    const qreal titleX = 10 + 18 + 14 + kListCover + 12;
    const bool wide = listWide();
    const bool badges = std::any_of(m_items.begin(), m_items.end(), [](const GridItem& g) { return !g.badge.isEmpty(); });
    qreal right = width() - 12;
    const auto col = [&](const QString& t, qreal w) {
        right -= w;
        p.drawText(mirrored(this, QRectF(right, y, w, kHeader - 1)), Qt::AlignLeft | Qt::AlignVCenter, t);
        right -= 12;
    };
    if (badges && wide && m_headers.size() > 4)
        col(m_headers[4], 140);
    if (m_headers.size() > 3)
        col(m_headers[3], wide ? 170 : 120);
    if (m_headers.size() > 2)
        col(m_headers[2], 80);
    if (wide && m_headers.size() > 1)
        col(m_headers[1], 170);
    if (!m_headers.isEmpty())
        p.drawText(mirrored(this, QRectF(titleX, y, right - titleX, kHeader - 1)), Qt::AlignLeft | Qt::AlignVCenter, m_headers[0]);
    p.setPen(QPen(color::border_2, 1));
    p.drawLine(QPointF(0, kHeader - 0.5), QPointF(width(), kHeader - 0.5));
}

void LibraryGrid::paintRow(QPainter& p, int i, const QRect& rect)
{
    const GridItem& it = m_items[size_t(i)];
    const QRectF r = mirrored(this, QRectF(rect)); // left to right, mirrored as it is drawn (a row is the full width)
    const auto vis = [this](const QRectF& x) { return mirrored(this, x); };
    const bool sel = m_selected.contains(it.key);
    const qreal hov = m_hover->amount(i);
    if (sel || hov > 0.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? color::accent_lo : withAlpha(color::bg_3, int(255 * hov)));
        p.drawRoundedRect(r.adjusted(0, 2, 0, -2), radius::sm, radius::sm);
    }
    paintCheck(p, checkRect(i), sel, false);
    const QRectF cover(r.left() + 10 + 18 + 14, r.center().y() - kListCover / 2.0, kListCover, kListCover);
    paintCover(p, it, vis(cover), radius::sm, true);
    const bool wide = listWide();
    const bool badges = std::any_of(m_items.begin(), m_items.end(), [](const GridItem& g) { return !g.badge.isEmpty(); });
    qreal right = r.right() - 12;
    const auto colRect = [&](qreal w) {
        right -= w;
        const QRectF c(right, r.top(), w, r.height());
        right -= 12;
        return c;
    };
    if (badges && wide) {
        const QRectF c = colRect(140);
        if (!it.badge.isEmpty()) {
            const int full = badgeWidth(it.badge);
            const qreal bw = std::min(c.width(), qreal(full));
            const QString shown = bw < full ? QFontMetrics(font::chip()).elidedText(it.badge, Qt::ElideRight, int(bw - 16)) : it.badge;
            paintBadge(p, vis(QRectF(c.left(), c.center().y() - size::badge / 2.0, bw, size::badge)), shown, it.badgeTone);
        }
    }
    {
        const QRectF c = colRect(wide ? 170 : 120);
        paintChips(p, it, c.left(), c.center().y() - size::badge / 2.0, c.right());
    }
    {
        const QRectF c = colRect(80);
        p.setFont(font::mono());
        p.setPen(color::fg_3);
        p.drawText(vis(c), leftToRightText(this, Qt::AlignLeft | Qt::AlignVCenter), it.size);
    }
    if (wide) {
        const QRectF c = colRect(170);
        p.setFont(font::mono());
        p.setPen(color::fg_1);
        p.drawText(vis(c), leftToRightText(this, Qt::AlignLeft | Qt::AlignVCenter),
                   QFontMetrics(font::mono()).elidedText(it.subtitle, Qt::ElideRight, int(c.width())));
    }
    const qreal tx = cover.right() + 12;
    const qreal tw = right - tx;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(vis(QRectF(tx, r.center().y() - 18, tw, 18)), naturalText(this, Qt::AlignLeft | Qt::AlignVCenter, it.title),
               QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(tw)));
    const QString second = wide ? it.path : it.subtitle;
    p.setFont(wide ? font::monoSmall() : font::caption());
    p.setPen(color::fg_4);
    p.drawText(vis(QRectF(tx, r.center().y() + 1, tw, 16)), wide ? leftToRightText(this, Qt::AlignLeft | Qt::AlignVCenter) : int(Qt::AlignLeft | Qt::AlignVCenter),
               QFontMetrics(p.font()).elidedText(second, wide ? Qt::ElideMiddle : Qt::ElideRight, int(tw)));
    if (i == m_current && hasFocus() && (m_keyNav || hasKeyboardFocus(this))) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color::focus, 2));
        p.drawRoundedRect(r.adjusted(1, 2, -1, -2), radius::sm, radius::sm);
    }
}

// ---- interaction ---------------------------------------------------------------------------

void LibraryGrid::mousePressEvent(QMouseEvent* e)
{
    m_keyNav = false;
    setFocus(Qt::MouseFocusReason);
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint pos = e->position().toPoint();
    const int i = itemAt(pos);
    const QSet<QString> before = m_selected;
    if (i < 0) {
        if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
            m_selected.clear();
    } else {
        const QString& key = m_items[size_t(i)].key;
        if (checkRect(i).contains(pos) || (e->modifiers() & Qt::ControlModifier)) {
            if (m_selected.contains(key))
                m_selected.remove(key);
            else
                m_selected.insert(key);
            m_anchor = i;
        } else if ((e->modifiers() & Qt::ShiftModifier) && m_anchor >= 0) {
            m_selected.clear();
            for (int k = std::min(m_anchor, i); k <= std::max(m_anchor, i); ++k)
                m_selected.insert(m_items[size_t(k)].key);
        } else {
            m_selected = {key};
            m_anchor = i;
        }
        setCurrent(i, false);
    }
    update();
    if (before != m_selected)
        emit selectionChanged();
}

void LibraryGrid::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    const int i = itemAt(pos);
    if (e->button() == Qt::LeftButton && i >= 0 && !checkRect(i).contains(pos))
        emit activated(m_items[size_t(i)].key);
}

void LibraryGrid::mouseMoveEvent(QMouseEvent* e)
{
    const int i = itemAt(e->position().toPoint());
    if (i != m_hover->hovered()) {
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        m_hover->setHovered(i); // repaints the tile it left and the one it entered
    }
}

void LibraryGrid::leaveEvent(QEvent*) { m_hover->setHovered(-1); }

void LibraryGrid::contextMenuEvent(QContextMenuEvent* e)
{
    // The Menu key and Shift+F10 report the middle of this (tall, scrolled)
    // widget: the menu is for the current item then, not for what lies there.
    const bool keyboard = e->reason() == QContextMenuEvent::Keyboard;
    const int i = keyboard ? m_current : itemAt(e->pos());
    if (i < 0 || i >= count())
        return;
    const QString key = m_items[size_t(i)].key;
    const QPoint globalPos = keyboard ? mapToGlobal(itemRect(i).center()) : e->globalPos();
    QPointer<LibraryGrid> self(this);
    if (!m_selected.contains(key)) {
        m_selected = {key};
        m_anchor = i;
        update();
        emit selectionChanged();
        if (!self)
            return;
    }
    setCurrent(i, keyboard);
    emit contextRequested(key, globalPos);
}

void LibraryGrid::keyPressEvent(QKeyEvent* e)
{
    if (m_items.empty()) {
        QWidget::keyPressEvent(e);
        return;
    }
    const int cols = m_mode == Mode::List ? 1 : columns(width());
    int next = m_current < 0 ? 0 : m_current;
    m_keyNav = true;
    switch (e->key()) {
    case Qt::Key_Right: // left and right as they are on screen
    case Qt::Key_Left:
        if ((e->key() == Qt::Key_Right) != isRightToLeft())
            next = m_current < 0 ? 0 : m_current + 1;
        else
            next = m_current - 1;
        break;
    case Qt::Key_Down: next = m_current < 0 ? 0 : m_current + cols; break;
    case Qt::Key_Up: next = m_current - cols; break;
    case Qt::Key_Home: next = 0; break;
    case Qt::Key_End: next = count() - 1; break;
    case Qt::Key_Space:
        if (m_current >= 0) {
            const QString& key = m_items[size_t(m_current)].key;
            if (m_selected.contains(key))
                m_selected.remove(key);
            else
                m_selected.insert(key);
            m_anchor = m_current; // Shift+arrow extends from here
            update();
            emit selectionChanged();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_current >= 0)
            emit activated(m_items[size_t(m_current)].key);
        return;
    case Qt::Key_Escape:
        if (!m_selected.isEmpty()) {
            clearSelection();
            return;
        }
        QWidget::keyPressEvent(e);
        return;
    case Qt::Key_A:
        if (e->modifiers() & Qt::ControlModifier) {
            selectAll();
            return;
        }
        QWidget::keyPressEvent(e);
        return;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    next = std::clamp(next, 0, count() - 1);
    if (e->modifiers() & Qt::ShiftModifier) {
        if (m_anchor < 0)
            m_anchor = std::max(0, m_current);
        m_selected.clear();
        for (int k = std::min(m_anchor, next); k <= std::max(m_anchor, next); ++k)
            m_selected.insert(m_items[size_t(k)].key);
        setCurrent(next, true);
        emit selectionChanged();
        return;
    }
    m_anchor = next; // a later Shift+arrow extends from where the keyboard is
    setCurrent(next, true);
}

bool LibraryGrid::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = itemAt(he->pos());
        if (i >= 0) {
            const GridItem& it = m_items[size_t(i)];
            QStringList lines{it.title};
            if (!it.subtitle.isEmpty())
                lines << it.subtitle;
            if (!it.chips.isEmpty())
                lines << it.chips.join(QStringLiteral(" · "));
            if (!it.path.isEmpty())
                lines << it.path;
            if (!it.tooltip.isEmpty())
                lines << it.tooltip;
            QToolTip::showText(he->globalPos(), plainTip(lines.join(u'\n')), this, itemRect(i));
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(e);
}

} // namespace fw::ui
