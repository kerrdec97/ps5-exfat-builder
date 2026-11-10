#include "app/ui/Shell.h"

#include "app/ui/Icons.h"
#include "app/ui/Motion.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QVBoxLayout>

namespace fw::ui {

using namespace theme;

namespace {

// The sidebar's one selection bar: it slides from item to item.
class SelectionBar : public QWidget {
public:
    explicit SelectionBar(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setProperty("fwNoRing", true);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        NavItem::paintSelectionBar(p, QRectF(rect()));
    }
};

} // namespace

// ---- nav item ------------------------------------------------------------------

NavItem::NavItem(const QString& key, const QString& iconName, const QString& text, QWidget* parent)
    : QAbstractButton(parent), m_key(key), m_icon(iconName)
{
    setText(text);
    setCheckable(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setAccessibleName(text);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(size::navItem);
    setProperty("fwRadius", radius::control);
    m_hover = new motion::Value(this, 0.0, motion::fast);
}

void NavItem::enterEvent(QEnterEvent* e)
{
    m_hover->animateTo(1.0);
    QAbstractButton::enterEvent(e);
}

void NavItem::leaveEvent(QEvent* e)
{
    m_hover->animateTo(0.0);
    QAbstractButton::leaveEvent(e);
}

void NavItem::setCompact(bool compact)
{
    m_compact = compact;
    setToolTip(compact ? text() : QString());
    updateGeometry();
    update();
}

void NavItem::setCount(int count)
{
    m_count = count;
    update();
}

QSize NavItem::sizeHint() const
{
    return {m_compact ? 44 : 224, size::navItem};
}

QRect NavItem::barRect() const { return QRect(1, (height() - 18) / 2, 3, 18); }

void NavItem::paintSelectionBar(QPainter& p, const QRectF& r)
{
    QLinearGradient bar(r.topLeft(), r.bottomLeft());
    bar.setColorAt(0, color::accent_hi);
    bar.setColorAt(1, color::accent_2_hi);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(bar);
    p.drawRoundedRect(r, r.width() / 2.0, r.width() / 2.0);
    p.restore();
}

void NavItem::setExternalBar(bool external)
{
    m_externalBar = external;
    update();
}

void NavItem::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool sel = isChecked();
    const bool down = isDown();
    if (!underMouse() && m_hover->target() > 0.0)
        m_hover->animateTo(0.0); // the pointer left without a leave event (the item was hidden under it)
    const qreal hover = m_hover->value(); // fades in and out
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (sel) {
        // Selected: the signature gradient as an 18 % tint, and the bar.
        p.setPen(Qt::NoPen);
        p.setBrush(accentGradient(r, 46));
        p.drawRoundedRect(r, radius::control, radius::control);
        if (!m_externalBar)
            paintSelectionBar(p, barRect());
    } else if (down || hover > 0.0) {
        p.setPen(Qt::NoPen);
        p.setBrush(down ? color::bg_3 : withAlpha(color::bg_3, int(153 * hover))); // hover: bg_3 at 60 %
        p.drawRoundedRect(r, radius::control, radius::control);
    }
    const QColor textColor = sel ? color::fg_0 : blend(color::fg_2, color::fg_0, hover);
    const int ix = m_compact ? (width() - size::icon) / 2 : 14;
    const QRect iconRect(ix, (height() - size::icon) / 2, size::icon, size::icon);
    if (sel)
        p.drawPixmap(iconRect, iconPixmap(m_icon, size::icon, color::accent_hi, devicePixelRatioF()));
    else
        drawIconFade(p, iconRect, m_icon, size::icon, color::fg_4, color::fg_0, hover, devicePixelRatioF());
    if (!m_compact) {
        p.setPen(textColor);
        p.setFont(sel ? font::bodyStrong() : font::body());
        int right = width() - 10;
        if (m_count > 0) {
            const QString n = QString::number(m_count);
            const int bw = std::max(20, badgeWidth(n, false, true));
            const QRect br(width() - 10 - bw, (height() - size::badgeCount) / 2, bw, size::badgeCount);
            paintBadge(p, br, n, Tone::Info, {}, true);
            right = br.left() - 6;
            p.setPen(textColor);
            p.setFont(sel ? font::bodyStrong() : font::body());
        }
        const QRect tr(14 + size::icon + 12, 0, right - (14 + size::icon + 12), height());
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(text(), Qt::ElideRight, tr.width()));
    } else if (m_count > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(color::accent_hi);
        p.drawEllipse(QRect(width() - 13, 8, 7, 7));
    }
}

void NavItem::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        emit moveFocusRequested(e->key() == Qt::Key_Down ? 1 : -1);
        return;
    }
    QAbstractButton::keyPressEvent(e);
}

// ---- sidebar -------------------------------------------------------------------

Sidebar::Sidebar(QWidget* parent) : QWidget(parent)
{
    setFixedWidth(size::sidebar);
    setProperty("fwNoRing", true);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 1, 0); // the right border, painted in paintEvent
    outer->setSpacing(0);
    auto* inner = new QWidget(this);
    m_layout = vbox(inner, 3, QMargins(space::md, space::md, space::md, space::md));
    outer->addWidget(inner, 1);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    m_version = label({}, TextStyle::Caption, color::fg_4, this);
    m_version->setContentsMargins(14, 10, 14, 2);
    connect(m_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* b) {
        if (auto* n = qobject_cast<NavItem*>(b))
            emit selected(n->key());
    });
    // One selection bar for all items, over them: it slides to the item
    // that becomes current.
    m_bar = new SelectionBar(inner);
    m_barY = new motion::Value(inner, 0.0, motion::base);
    m_barY->onChange([this] { m_bar->move(m_bar->x(), qRound(m_barY->value())); });
}

void Sidebar::syncBar(bool animate)
{
    NavItem* cur = m_items.value(current());
    if (!cur || cur->isHidden()) {
        m_bar->hide();
        return;
    }
    const QRect r = cur->barRect().translated(cur->pos());
    const bool slide = animate && !m_bar->isHidden();
    m_bar->resize(r.size());
    if (slide) {
        m_bar->move(r.x(), m_bar->y());
        m_barY->animateTo(r.y());
    } else {
        m_barY->jumpTo(r.y());
        m_bar->move(r.topLeft());
    }
    m_bar->raise();
    m_bar->show();
}

bool Sidebar::eventFilter(QObject* watched, QEvent* event)
{
    // An item moved or changed size (first layout, compact mode): the bar follows at once.
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && qobject_cast<NavItem*>(watched))
        syncBar(false);
    return QWidget::eventFilter(watched, event);
}

void Sidebar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), color::bg_0);
    QLinearGradient glow(QPointF(0, 0), QPointF(0, 260));
    QColor a = color::accent;
    a.setAlpha(16);
    glow.setColorAt(0, a);
    glow.setColorAt(1, Qt::transparent);
    p.fillRect(QRect(0, 0, width(), 260), glow);
    p.fillRect(QRect(width() - 1, 0, 1, height()), color::border_2);
}

void Sidebar::addGroup(const QString& overline)
{
    auto* l = label(overline, TextStyle::Overline, color::fg_4, this);
    l->setContentsMargins(14, 16, 14, 6);
    m_groupLabels << l;
    m_layout->addWidget(l);
}

NavItem* Sidebar::addItem(const QString& key, const QString& iconName, const QString& text)
{
    auto* item = new NavItem(key, iconName, text, this);
    m_items.insert(key, item);
    m_group->addButton(item);
    m_layout->addWidget(item);
    connect(item, &NavItem::moveFocusRequested, this, [this, item](int delta) { focusNeighbour(item, delta); });
    item->setExternalBar(true);
    item->installEventFilter(this);
    connect(item, &QAbstractButton::toggled, this, [this](bool on) {
        if (on)
            syncBar(true);
    });
    return item;
}

void Sidebar::addStretch() { m_layout->addStretch(1); }

NavItem* Sidebar::addFooterItem(const QString& key, const QString& iconName, const QString& text)
{
    NavItem* item = addItem(key, iconName, text);
    m_layout->addWidget(m_version);
    return item;
}

void Sidebar::setVersionText(const QString& text) { m_version->setText(text); }

void Sidebar::setCurrent(const QString& key)
{
    if (auto* it = m_items.value(key))
        it->setChecked(true);
}

QString Sidebar::current() const
{
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
        if (it.value()->isChecked())
            return it.key();
    return {};
}

void Sidebar::setCompact(bool compact)
{
    m_compact = compact;
    setFixedWidth(compact ? size::sidebarCompact : size::sidebar);
    for (auto* it : m_items)
        it->setCompact(compact);
    for (auto* l : m_groupLabels)
        l->setVisible(!compact);
    m_version->setVisible(!compact);
}

void Sidebar::setCount(const QString& key, int count)
{
    if (auto* it = m_items.value(key))
        it->setCount(count);
}

QStringList Sidebar::keys() const
{
    QStringList out;
    for (int i = 0; i < m_layout->count(); ++i)
        if (auto* n = qobject_cast<NavItem*>(m_layout->itemAt(i)->widget()))
            out << n->key();
    return out;
}

void Sidebar::focusNeighbour(NavItem* from, int delta)
{
    const QStringList order = keys();
    const int i = order.indexOf(from->key());
    if (i < 0 || order.isEmpty())
        return;
    const int j = (i + delta + order.size()) % order.size();
    if (auto* it = m_items.value(order[j])) {
        window()->setAttribute(Qt::WA_KeyboardFocusChange, true);
        it->setFocus(Qt::TabFocusReason);
    }
}

void Sidebar::focusCurrent()
{
    if (auto* it = m_items.value(current()))
        it->setFocus(Qt::TabFocusReason);
}

// ---- app header ------------------------------------------------------------------

AppHeader::AppHeader(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(size::header);
    setProperty("fwNoRing", true);
    auto* h = hbox(this, space::sm, QMargins(space::sm, 0, space::md, 0));
    m_menu = new IconButton(QStringLiteral("sidebar"), QStringLiteral("Toggle sidebar"), IconButton::Size::Regular, this);
    h->addWidget(m_menu);
    auto* brand = new QWidget(this);
    auto* bl = hbox(brand, 10, QMargins(4, 0, 0, 0));
    bl->addWidget(new BrandMark(28, brand), 0, Qt::AlignVCenter);
    auto* name = label(QStringLiteral("exFAT Image Builder"), TextStyle::BodyStrong, color::fg_0, brand);
    bl->addWidget(name, 0, Qt::AlignVCenter);
    m_versionPill = new Badge(QString(), Tone::Neutral, brand);
    m_versionPill->setIconName(QString());
    m_versionPill->hide();
    bl->addWidget(m_versionPill, 0, Qt::AlignVCenter);
    h->addWidget(brand);
    h->addStretch(1);
    m_consoleChipWrap = new QWidget(this);
    auto* cw = hbox(m_consoleChipWrap, 0);
    m_consoleChip = new Badge(QStringLiteral("PS5 not connected"), Tone::Neutral, m_consoleChipWrap);
    m_consoleChip->setIconName(QStringLiteral("plug"));
    cw->addWidget(m_consoleChip);
    m_consoleChipWrap->setCursor(Qt::PointingHandCursor);
    m_consoleChipWrap->installEventFilter(this);
    m_consoleChipWrap->setToolTip(QStringLiteral("Open PS5 connect"));
    h->addWidget(m_consoleChipWrap);
    m_queueChipWrap = new QWidget(this);
    auto* qw = hbox(m_queueChipWrap, 0);
    m_queueChip = new Badge(QStringLiteral("Queue idle"), Tone::Neutral, m_queueChipWrap);
    m_queueChip->setIconName(QStringLiteral("queue"));
    qw->addWidget(m_queueChip);
    m_queueChipWrap->setCursor(Qt::PointingHandCursor);
    m_queueChipWrap->installEventFilter(this);
    m_queueChipWrap->setToolTip(QStringLiteral("Open queue"));
    // Both chips are buttons: Tab reaches them, Space and Enter press them.
    for (QWidget* chip : {m_consoleChipWrap, m_queueChipWrap}) {
        chip->setFocusPolicy(Qt::TabFocus);
        chip->setProperty("fwRole", QStringLiteral("button")); // before the name, see Accessible.h
        chip->setProperty("fwRadius", radius::pill);
        chip->setAccessibleName(chip->toolTip());
    }
    h->addWidget(m_queueChipWrap);
    m_help = new IconButton(QStringLiteral("help"), QStringLiteral("Help"), IconButton::Size::Regular, this);
    h->addWidget(m_help);
}

bool AppHeader::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && (watched == m_queueChipWrap || watched == m_consoleChipWrap)) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Space || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !ke->isAutoRepeat()) {
            if (watched == m_queueChipWrap)
                emit queueChipClicked();
            else
                emit consoleChipClicked();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && (watched == m_queueChipWrap || watched == m_consoleChipWrap)) {
        auto* me = static_cast<QMouseEvent*>(event);
        auto* chip = static_cast<QWidget*>(watched);
        if (me->button() == Qt::LeftButton && chip->rect().contains(me->position().toPoint())) {
            // Nothing after the signal: a slot may replace the window's content.
            if (watched == m_queueChipWrap)
                emit queueChipClicked();
            else
                emit consoleChipClicked();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AppHeader::setQueueChip(const QString& text, Tone tone, bool visible)
{
    m_queueChip->set(text, tone);
    m_queueChip->setIconName(tone == Tone::Info ? QStringLiteral("clock") : QStringLiteral("queue"));
    m_queueChipWrap->setVisible(visible);
}

void AppHeader::setConsoleChip(const QString& text, Tone tone, bool visible)
{
    m_consoleChip->set(text, tone);
    m_consoleChip->setIconName(tone == Tone::Ok ? QStringLiteral("success") : QStringLiteral("plug"));
    m_consoleChipWrap->setVisible(visible);
}

void AppHeader::setVersion(const QString& version)
{
    m_versionPill->set(version, Tone::Neutral);
    m_versionPill->setVisible(!version.isEmpty());
}

void AppHeader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), color::bg_0);
    // A faint brand glow under the logo; the edge to the page is border_2,
    // tinted by the gradient where the glow meets it.
    QRadialGradient glow(QPointF(70, height() / 2.0), 220);
    glow.setColorAt(0, withAlpha(color::accent, 28));
    glow.setColorAt(1, Qt::transparent);
    p.fillRect(rect(), glow);
    const QRect edge(0, height() - 1, width(), 1);
    p.fillRect(edge, color::border_2);
    QLinearGradient tint(QPointF(0, 0), QPointF(std::min(width(), 480), 0));
    tint.setColorAt(0, withAlpha(color::accent, 90));
    tint.setColorAt(0.5, withAlpha(color::accent_2, 45));
    tint.setColorAt(1, withAlpha(color::accent_2, 0));
    p.fillRect(edge, tint);
}

// ---- status bar -------------------------------------------------------------------

StatusBar::StatusBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(size::statusBar);
    setProperty("fwNoRing", true);
    auto* h = hbox(this, space::lg, QMargins(space::lg, 0, space::lg, 0));
    auto* left = new QWidget(this);
    auto* ll = hbox(left, 6);
    m_mark = new QLabel(left);
    m_mark->setFixedSize(size::iconSm, size::iconSm);
    m_text = label({}, TextStyle::Caption, color::fg_3, left);
    ll->addWidget(m_mark);
    ll->addWidget(m_text);
    h->addWidget(left);
    m_free = label({}, TextStyle::MonoSmall, color::fg_3, this);
    h->addWidget(m_free);
    h->addStretch(1);
    m_context = new ElideLabel({}, Qt::ElideMiddle, this);
    m_context->setFont(font::caption());
    setTextColor(m_context, color::fg_3);
    m_context->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_context->setMaximumWidth(720);
    h->addWidget(m_context, 1);
    setStatus(QStringLiteral("Ready"), Tone::Ok);
}

void StatusBar::setStatus(const QString& text, Tone tone)
{
    m_text->setText(text);
    const QString iconName = tone == Tone::Info ? QStringLiteral("circle_filled") : tone == Tone::Neutral ? QStringLiteral("circle") : defaultBadgeIcon(tone);
    m_mark->setPixmap(iconPixmap(iconName, size::iconSm, toneMark(tone), devicePixelRatioF()));
}

void StatusBar::setFreeSpace(const QString& text)
{
    m_free->setText(text);
    m_free->setVisible(!text.isEmpty());
}

void StatusBar::setContext(const QString& text) { m_context->setFullText(text); }

void StatusBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), color::bg_0);
    p.fillRect(QRect(0, 0, width(), 1), color::border_2);
}

// ---- page header --------------------------------------------------------------------

PageHeader::PageHeader(const QString& title, const QString& captionText, QWidget* parent) : QWidget(parent)
{
    auto* h = hbox(this, space::lg, QMargins(0, 0, 0, space::xs));
    h->setAlignment(Qt::AlignTop);
    m_icon = new IconTile(QString(), 46, this);
    m_icon->hide();
    h->addWidget(m_icon, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(3);
    m_title = heading(title, TextStyle::Title, this);
    m_caption = label(captionText, TextStyle::Body, color::fg_3, this);
    m_caption->setWordWrap(true);
    m_caption->setMaximumWidth(760);
    col->addWidget(m_title);
    col->addWidget(m_caption);
    h->addLayout(col, 1);
    auto* actions = new QWidget(this);
    m_actions = hbox(actions, space::sm);
    h->addWidget(actions, 0, Qt::AlignTop);
}

void PageHeader::setCaption(const QString& text)
{
    m_caption->setText(text);
    m_caption->setVisible(!text.isEmpty());
}

void PageHeader::setTitle(const QString& title) { m_title->setText(title); }

void PageHeader::setIcon(const QString& iconName)
{
    m_icon->setIconName(iconName);
    m_icon->setVisible(!iconName.isEmpty());
}

void PageHeader::addAction(QWidget* w) { m_actions->addWidget(w); }

} // namespace fw::ui
