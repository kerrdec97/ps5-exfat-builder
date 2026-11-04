#include "app/ui/Shell.h"
#include "app/i18n/I18n.h"

#include "app/ui/Icons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace fw::ui {

using namespace theme;

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
    setProperty("fwRadius", radius::sm);
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
    return {m_compact ? 40 : 216, size::navItem};
}

void NavItem::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool sel = isChecked();
    const bool hover = underMouse();
    const bool down = isDown();
    QColor fill;
    if (down)
        fill = color::bg_4;
    else if (sel)
        fill = color::bg_3;
    else if (hover)
        fill = color::bg_2;
    if (fill.isValid()) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(rect(), radius::sm, radius::sm);
    }
    if (sel) {
        p.setBrush(color::accent);
        p.drawRoundedRect(QRect(0, (height() - 16) / 2, 3, 16), radius::xs, radius::xs);
    }
    const QColor iconColor = sel ? color::accent_hi : hover ? color::fg_0 : color::fg_3;
    const QColor textColor = sel || hover ? color::fg_0 : color::fg_2;
    const int ix = m_compact ? (width() - size::icon) / 2 : 12;
    p.drawPixmap(QRect(ix, (height() - size::icon) / 2, size::icon, size::icon),
                 iconPixmap(m_icon, size::icon, iconColor, devicePixelRatioF()));
    if (!m_compact) {
        p.setPen(textColor);
        p.setFont(sel ? font::bodyStrong() : font::body());
        int right = width() - 8;
        if (m_count > 0) {
            const QString n = QString::number(m_count);
            const QFontMetrics fm(font::caption());
            const int bw = fm.horizontalAdvance(n) + 12;
            const QRect br(width() - 8 - bw, (height() - 18) / 2, bw, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_3);
            if (sel)
                p.setBrush(color::bg_5);
            p.drawRoundedRect(br, 9, 9);
            p.setPen(color::fg_2);
            p.setFont(font::caption());
            p.drawText(br, Qt::AlignCenter, n);
            right = br.left() - 6;
            p.setPen(textColor);
            p.setFont(sel ? font::bodyStrong() : font::body());
        }
        const QRect tr(12 + size::icon + 10, 0, right - (12 + size::icon + 10), height());
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(text(), Qt::ElideRight, tr.width()));
    } else if (m_count > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(color::accent_hi);
        p.drawEllipse(QRect(width() - 12, 8, 6, 6));
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
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, color::bg_0);
    setPalette(p);
    setProperty("fwNoRing", true);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* inner = new QWidget(this);
    m_layout = vbox(inner, space::xs, QMargins(space::sm, space::md, space::sm, space::md));
    outer->addWidget(inner, 1);
    auto* line = new QFrame(this);
    line->setFixedWidth(1);
    line->setAutoFillBackground(true);
    QPalette lp = line->palette();
    lp.setColor(QPalette::Window, color::border_2);
    line->setPalette(lp);
    outer->addWidget(line);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    m_version = label({}, TextStyle::Caption, color::fg_5, this);
    m_version->setContentsMargins(12, 8, 12, 4);
    connect(m_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* b) {
        if (auto* n = qobject_cast<NavItem*>(b))
            emit selected(n->key());
    });
}

void Sidebar::addGroup(const QString& overline)
{
    auto* l = label(overline, TextStyle::Overline, color::fg_4, this);
    l->setContentsMargins(12, 12, 12, 4);
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
    m_menu = new IconButton(QStringLiteral("sidebar"), tr("Toggle sidebar"), IconButton::Size::Regular, this);
    h->addWidget(m_menu);
    auto* brand = new QWidget(this);
    auto* bl = hbox(brand, space::sm, QMargins(4, 0, 0, 0));
    bl->setAlignment(Qt::AlignBaseline);
    bl->addWidget(label(tr("exFAT Image Builder"), TextStyle::BodyStrong, color::fg_0, brand), 0, Qt::AlignBaseline);
    bl->addWidget(label(tr("Farewell Edition"), TextStyle::Body, color::fg_3, brand), 0, Qt::AlignBaseline);
    h->addWidget(brand);
    h->addStretch(1);
    m_consoleChipWrap = new QWidget(this);
    auto* cw = hbox(m_consoleChipWrap, 0);
    m_consoleChip = new Badge(tr("PS5 not connected"), Tone::Neutral, m_consoleChipWrap);
    m_consoleChip->setIconName(QStringLiteral("plug"));
    cw->addWidget(m_consoleChip);
    m_consoleChipWrap->setCursor(Qt::PointingHandCursor);
    m_consoleChipWrap->installEventFilter(this);
    m_consoleChipWrap->setToolTip(tr("Open PS5 connect"));
    h->addWidget(m_consoleChipWrap);
    m_queueChipWrap = new QWidget(this);
    auto* qw = hbox(m_queueChipWrap, 0);
    m_queueChip = new Badge(tr("Queue idle"), Tone::Neutral, m_queueChipWrap);
    m_queueChip->setIconName(QStringLiteral("queue"));
    qw->addWidget(m_queueChip);
    m_queueChipWrap->setCursor(Qt::PointingHandCursor);
    m_queueChipWrap->installEventFilter(this);
    m_queueChipWrap->setToolTip(tr("Open the queue"));
    h->addWidget(m_queueChipWrap);
    m_help = new IconButton(QStringLiteral("help"), tr("Help"), IconButton::Size::Regular, this);
    h->addWidget(m_help);
}

bool AppHeader::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (watched == m_queueChipWrap)
            emit queueChipClicked();
        else if (watched == m_consoleChipWrap)
            emit consoleChipClicked();
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

void AppHeader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), color::bg_0);
    p.fillRect(QRect(0, height() - 1, width(), 1), color::border_2);
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
    setStatus(tr("Ready"), Tone::Ok);
}

void StatusBar::setStatus(const QString& text, Tone tone)
{
    m_text->setText(text);
    const QString iconName = tone == Tone::Info ? QStringLiteral("dot") : tone == Tone::Neutral ? QStringLiteral("circle") : defaultBadgeIcon(tone);
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
    auto* h = hbox(this, space::lg, QMargins(0, 0, 0, 0));
    h->setAlignment(Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    m_title = heading(title, TextStyle::Title, this);
    m_caption = caption(captionText, this);
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

void PageHeader::addAction(QWidget* w) { m_actions->addWidget(w); }

} // namespace fw::ui
