#include "app/ui/LibraryGrid.h"

#include "app/ui/Icons.h"
#include "app/ui/Style.h"

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
#include <QRegularExpression>
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
constexpr int kHeader = 34;  // list
constexpr int kRow = 56;
constexpr int kListCover = 40;

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t), 1.0f);
}

QFont chipFont()
{
    QFont f = font::caption();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    return f;
}

QFont headerFont()
{
    QFont f = font::caption();
    f.setWeight(QFont::DemiBold);
    return f;
}

QString initials(const QString& title)
{
    QString out;
    static const QRegularExpression separators(QStringLiteral("[\\s:\\-_]+")); // once, not per tile per paint
    for (const QString& w : title.split(separators, Qt::SkipEmptyParts)) {
        for (const QChar c : w)
            if (c.isLetterOrNumber()) {
                out += c.toUpper();
                break;
            }
        if (out.size() == 2)
            break;
    }
    return out.isEmpty() ? QStringLiteral("?") : out;
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
    setAccessibleName(QStringLiteral("Items"));
    m_headers = {QStringLiteral("Title"), QStringLiteral("ID · version"), QStringLiteral("Size"), QStringLiteral("Formats"), QStringLiteral("Status")};
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
    m_hover = -1;
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
    return QRect((i % cols) * (tw + kGap), (i / cols) * (tw + kText + kGap), tw, tw + kText);
}

QRect LibraryGrid::checkRect(int i) const
{
    const QRect r = itemRect(i);
    if (m_mode == Mode::List)
        return QRect(r.left() + 10, r.center().y() - 9, 18, 18);
    return QRect(r.left() + 10, r.top() + 10, 24, 24);
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
        // A generated tile: the signature gradient at a per-title angle, the initials on top.
        const uint h = uint(qHash(it.title.isEmpty() ? it.key : it.title));
        const qreal angle = (h % 360) * 3.14159265358979 / 180.0;
        const QPointF c = r.center();
        const qreal len = std::max(r.width(), r.height()) * 0.75;
        QLinearGradient g(c - QPointF(std::cos(angle) * len, std::sin(angle) * len), c + QPointF(std::cos(angle) * len, std::sin(angle) * len));
        const qreal depth = 0.38 + (h % 7) * 0.05;
        g.setColorAt(0, mix(color::accent, color::bg_2, depth));
        g.setColorAt(1, mix(color::accent_2, color::bg_2, depth + 0.1));
        p.fillRect(r, g);
        QRadialGradient glow(QPointF(r.left() + r.width() * 0.25, r.top() + r.height() * 0.2), r.width() * 0.9);
        QColor hi = color::accent_hi;
        hi.setAlpha(46);
        glow.setColorAt(0, hi);
        hi.setAlpha(0);
        glow.setColorAt(1, hi);
        p.fillRect(r, glow);
        QFont f = font::display();
        f.setPixelSize(std::max(12, int(r.height() * (roundBottom ? 0.36 : 0.30))));
        f.setWeight(QFont::Bold);
        p.setFont(f);
        QColor ink = color::on_accent;
        ink.setAlpha(230);
        p.setPen(ink);
        p.drawText(r, Qt::AlignCenter, initials(it.title));
        if (!roundBottom && r.width() > 100) {
            QColor faint = color::on_accent;
            faint.setAlpha(150);
            p.drawPixmap(QRectF(r.left() + 12, r.bottom() - 30, 18, 18).toRect(), iconPixmap(it.icon, 18, faint, dpr));
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
    const QFont f = chipFont();
    const QFontMetrics fm(f);
    p.setFont(f);
    const auto chip = [&](const QString& t, bool neutral) {
        const qreal w = fm.horizontalAdvance(t) + 14;
        const QRectF cr(x, y, w, 18);
        p.setPen(Qt::NoPen);
        if (neutral) {
            p.setBrush(color::bg_4);
        } else {
            QColor tint = color::accent;
            tint.setAlpha(46);
            p.setBrush(tint);
        }
        p.drawRoundedRect(cr, 9, 9);
        p.setPen(neutral ? color::fg_2 : color::accent_hi);
        p.drawText(cr, Qt::AlignCenter, t);
        x += w + 5;
    };
    for (int c = 0; c < it.chips.size(); ++c) {
        const qreal w = fm.horizontalAdvance(it.chips[c]) + 14;
        const int rest = int(it.chips.size()) - c - 1;
        const qreal more = rest > 0 ? fm.horizontalAdvance(QStringLiteral("+%1").arg(rest)) + 19 : 0;
        if (x + w + more > maxRight) {
            // Not all fit: a "+N" chip says how many are left out.
            const QString plus = QStringLiteral("+%1").arg(it.chips.size() - c);
            if (x + fm.horizontalAdvance(plus) + 14 <= maxRight)
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
    const QRectF r = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool sel = m_selected.contains(it.key);
    const bool hov = i == m_hover;
    // Card.
    p.setPen(QPen(hov && !sel ? color::border_4 : color::border_2, 1));
    p.setBrush(hov ? color::bg_3 : color::bg_2);
    p.drawRoundedRect(r, radius::md, radius::md);
    // Cover.
    const QRectF cover(r.left() + 1, r.top() + 1, r.width() - 2, r.width() - 2);
    paintCover(p, it, cover, radius::md - 1, false);
    // Text.
    const qreal x = r.left() + 12;
    const qreal w = r.width() - 24;
    qreal y = cover.bottom() + 10;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(QRectF(x, y, w, 18), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(w)));
    y += 20;
    p.setFont(font::caption());
    p.setPen(color::fg_3);
    p.drawText(QRectF(x, y, w, 16), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font::caption()).elidedText(it.subtitle, Qt::ElideRight, int(w)));
    y += 22;
    qreal sizeW = 0;
    if (!it.size.isEmpty()) {
        p.setFont(font::caption());
        sizeW = QFontMetrics(font::caption()).horizontalAdvance(it.size);
        p.setPen(color::fg_4);
        p.drawText(QRectF(r.right() - 12 - sizeW, y, sizeW, 18), Qt::AlignRight | Qt::AlignVCenter, it.size);
    }
    paintChips(p, it, x, y, r.right() - 12 - sizeW - 6);
    // Selection.
    if (sel) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(accentGradient(r, 240)), 2));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
    }
    if (sel || hov || !m_selected.isEmpty())
        paintCheck(p, checkRect(i), sel, true);
    // Keyboard focus only, and inside the tile: outside it the grid's own
    // edge would cut the ring of the first row and of the outer columns.
    if (i == m_current && hasFocus() && (m_keyNav || hasKeyboardFocus(this))) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color::focus, 2));
        p.drawRoundedRect(r.adjusted(2.5, 2.5, -2.5, -2.5), radius::md - 3, radius::md - 3);
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
        p.drawText(QRectF(right, y, w, kHeader - 6), Qt::AlignLeft | Qt::AlignVCenter, t);
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
        p.drawText(QRectF(titleX, y, right - titleX, kHeader - 6), Qt::AlignLeft | Qt::AlignVCenter, m_headers[0]);
    p.setPen(QPen(color::border_2, 1));
    p.drawLine(QPointF(0, kHeader - 0.5), QPointF(width(), kHeader - 0.5));
}

void LibraryGrid::paintRow(QPainter& p, int i, const QRect& rect)
{
    const GridItem& it = m_items[size_t(i)];
    const QRectF r = QRectF(rect);
    const bool sel = m_selected.contains(it.key);
    const bool hov = i == m_hover;
    if (sel || hov) {
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? color::accent_lo : color::bg_3);
        p.drawRoundedRect(r.adjusted(0, 2, 0, -2), radius::sm, radius::sm);
    }
    paintCheck(p, checkRect(i), sel, false);
    const QRectF cover(r.left() + 10 + 18 + 14, r.center().y() - kListCover / 2.0, kListCover, kListCover);
    paintCover(p, it, cover, radius::sm, true);
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
            const QFont f = chipFont();
            p.setFont(f);
            const qreal bw = std::min(c.width(), qreal(QFontMetrics(f).horizontalAdvance(it.badge) + 16));
            const QRectF br(c.left(), c.center().y() - 10, bw, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(toneTint(it.badgeTone));
            p.drawRoundedRect(br, 10, 10);
            p.setPen(toneText(it.badgeTone));
            p.drawText(br, Qt::AlignCenter, QFontMetrics(f).elidedText(it.badge, Qt::ElideRight, int(bw - 12)));
        }
    }
    {
        const QRectF c = colRect(wide ? 170 : 120);
        paintChips(p, it, c.left(), c.center().y() - 9, c.right());
    }
    {
        const QRectF c = colRect(80);
        p.setFont(font::mono());
        p.setPen(color::fg_3);
        p.drawText(c, Qt::AlignLeft | Qt::AlignVCenter, it.size);
    }
    if (wide) {
        const QRectF c = colRect(170);
        p.setFont(font::mono());
        p.setPen(color::fg_1);
        p.drawText(c, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font::mono()).elidedText(it.subtitle, Qt::ElideRight, int(c.width())));
    }
    const qreal tx = cover.right() + 12;
    const qreal tw = right - tx;
    p.setFont(font::bodyStrong());
    p.setPen(color::fg_0);
    p.drawText(QRectF(tx, r.center().y() - 18, tw, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(tw)));
    const QString second = wide ? it.path : it.subtitle;
    p.setFont(wide ? font::monoSmall() : font::caption());
    p.setPen(color::fg_4);
    p.drawText(QRectF(tx, r.center().y() + 1, tw, 16), Qt::AlignLeft | Qt::AlignVCenter,
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
    if (i != m_hover) {
        const int old = m_hover;
        m_hover = i;
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (old >= 0 && old < count())
            update(itemRect(old).adjusted(-4, -4, 4, 4));
        if (i >= 0)
            update(itemRect(i).adjusted(-4, -4, 4, 4));
    }
}

void LibraryGrid::leaveEvent(QEvent*)
{
    const int old = m_hover;
    m_hover = -1;
    if (old >= 0 && old < count())
        update(itemRect(old).adjusted(-4, -4, 4, 4));
}

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
    case Qt::Key_Right: next = m_current < 0 ? 0 : m_current + 1; break;
    case Qt::Key_Left: next = m_current - 1; break;
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
