#include "app/ui/Widgets.h"
#include "app/i18n/I18n.h"

#include "app/ui/Icons.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fw::ui {

using namespace theme;

// ---- labels ------------------------------------------------------------------

QFont fontFor(TextStyle style)
{
    switch (style) {
    case TextStyle::Display: return font::display();
    case TextStyle::Title: return font::title();
    case TextStyle::Stat: return font::stat();
    case TextStyle::Subtitle: return font::subtitle();
    case TextStyle::BodyStrong: return font::bodyStrong();
    case TextStyle::Body: return font::body();
    case TextStyle::Caption: return font::caption();
    case TextStyle::Overline: return font::overline();
    case TextStyle::Mono: return font::mono();
    case TextStyle::MonoSmall: return font::monoSmall();
    }
    return font::body();
}

void setTextColor(QWidget* w, const QColor& color)
{
    QPalette p = w->palette();
    p.setColor(QPalette::WindowText, color);
    p.setColor(QPalette::Text, color);
    w->setPalette(p);
}

QLabel* label(const QString& text, TextStyle style, const QColor& color, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setFont(fontFor(style));
    setTextColor(l, color);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* l = label(text, TextStyle::Caption, color::fg_3, parent);
    l->setWordWrap(true);
    return l;
}

QLabel* meta(const QString& text, QWidget* parent)
{
    return label(text, TextStyle::Caption, color::fg_4, parent);
}

QLabel* monoLabel(const QString& text, QWidget* parent, const QColor& color)
{
    return label(text, TextStyle::Mono, color, parent);
}

QLabel* heading(const QString& text, TextStyle style, QWidget* parent)
{
    return label(text, style, color::fg_0, parent);
}

ElideLabel::ElideLabel(const QString& text, Qt::TextElideMode mode, QWidget* parent)
    : QLabel(parent), m_text(text), m_mode(mode)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::NoTextInteraction);
    relayout();
}

void ElideLabel::setFullText(const QString& text)
{
    m_text = text;
    setToolTip(text);
    relayout();
}

QSize ElideLabel::minimumSizeHint() const
{
    return {24, QLabel::minimumSizeHint().height()};
}

void ElideLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    relayout();
}

void ElideLabel::relayout()
{
    const QFontMetrics fm(font());
    const int w = std::max(0, width() - 2);
    const QString shown = fm.elidedText(m_text, m_mode, w);
    QLabel::setText(shown);
    setToolTip(shown != m_text ? m_text : QString());
}

Link::Link(const QString& text, QWidget* parent) : QLabel(text, parent)
{
    setFont(font::body());
    setTextColor(this, color::accent_hi);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAccessibleName(text);
    setProperty("fwRadius", 2);
}

void Link::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && rect().contains(e->pos()))
        emit activated();
    QLabel::mouseReleaseEvent(e);
}

void Link::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        emit activated();
        return;
    }
    QLabel::keyPressEvent(e);
}

QFrame* hairline(QWidget* parent, bool strong)
{
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::NoFrame);
    f->setFixedHeight(1);
    f->setAutoFillBackground(true);
    QPalette p = f->palette();
    p.setColor(QPalette::Window, strong ? color::border_2 : color::border_1);
    f->setPalette(p);
    return f;
}

QWidget* spacer(QWidget* parent)
{
    auto* w = new QWidget(parent);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return w;
}

QWidget* fixedSpacer(int w, int h, QWidget* parent)
{
    auto* s = new QWidget(parent);
    s->setFixedSize(std::max(0, w), std::max(0, h));
    return s;
}

QHBoxLayout* hbox(QWidget* parent, int spacing, const QMargins& margins)
{
    auto* l = new QHBoxLayout(parent);
    l->setSpacing(spacing);
    l->setContentsMargins(margins);
    return l;
}

QVBoxLayout* vbox(QWidget* parent, int spacing, const QMargins& margins)
{
    auto* l = new QVBoxLayout(parent);
    l->setSpacing(spacing);
    l->setContentsMargins(margins);
    return l;
}

QWidget* field(const QString& labelText, QWidget* control, const QString& help, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* l = vbox(w, space::xs);
    if (!labelText.isEmpty()) {
        auto* lab = label(labelText, TextStyle::Body, color::fg_1, w);
        if (auto* buddy = control->focusProxy() ? control->focusProxy() : control)
            lab->setBuddy(buddy);
        l->addWidget(lab);
    }
    l->addWidget(control);
    if (!help.isEmpty())
        l->addWidget(caption(help, w));
    return w;
}

// ---- buttons -----------------------------------------------------------------

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

static const char* variantName(ButtonVariant v)
{
    switch (v) {
    case ButtonVariant::Primary: return "primary";
    case ButtonVariant::Danger: return "danger";
    case ButtonVariant::Subtle: return "subtle";
    case ButtonVariant::Secondary: break;
    }
    return "secondary";
}

void setButtonVariant(QPushButton* b, ButtonVariant variant)
{
    b->setProperty("variant", QString::fromLatin1(variantName(variant)));
    b->setFont(variant == ButtonVariant::Primary || variant == ButtonVariant::Danger ? font::bodyStrong() : font::body());
    const QColor iconColor = (variant == ButtonVariant::Primary || variant == ButtonVariant::Danger) ? color::on_accent
                                                                                                     : color::fg_3;
    if (!b->property("fwIcon").toString().isEmpty())
        setButtonIcon(b, b->property("fwIcon").toString(), iconColor);
    repolish(b);
}

void setButtonSmall(QPushButton* b, bool small)
{
    b->setProperty("size", small ? QStringLiteral("sm") : QString());
    repolish(b);
}

void setButtonIcon(QPushButton* b, const QString& iconName, const QColor& c)
{
    b->setProperty("fwIcon", iconName);
    if (iconName.isEmpty()) {
        b->setIcon(QIcon());
        return;
    }
    const QString v = b->property("variant").toString();
    const QColor rest = c.isValid() ? c
                      : (v == QLatin1String("primary") || v == QLatin1String("danger")) ? color::on_accent
                                                                                        : color::fg_3;
    b->setIcon(icon(iconName, rest, rest == color::on_accent ? rest : color::fg_0, color::fg_0, color::fg_5));
    b->setIconSize(QSize(size::icon, size::icon));
}

QPushButton* button(const QString& text, ButtonVariant variant, const QString& iconName, QWidget* parent)
{
    auto* b = new QPushButton(text, parent);
    b->setCursor(Qt::ArrowCursor);
    b->setFocusPolicy(Qt::StrongFocus);
    b->setAutoDefault(false);
    b->setDefault(false);
    setButtonVariant(b, variant);
    if (!iconName.isEmpty())
        setButtonIcon(b, iconName);
    return b;
}

IconButton::IconButton(const QString& iconName, const QString& accessibleName, Size size, QWidget* parent)
    : QToolButton(parent), m_icon(iconName), m_rest(color::fg_3), m_active(color::fg_0), m_checked(color::accent_hi),
      m_size(size)
{
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setToolTip(accessibleName);
    setAccessibleName(accessibleName);
    setProperty("size", size == Size::Small ? QStringLiteral("sm") : size == Size::Tiny ? QStringLiteral("xs") : QString());
    const int px = size == Size::Regular ? 32 : size == Size::Small ? 28 : 24;
    setFixedSize(px, px);
    setIconSize(QSize(size == Size::Tiny ? 14 : theme::size::icon, size == Size::Tiny ? 14 : theme::size::icon));
}

void IconButton::setIconName(const QString& iconName)
{
    m_icon = iconName;
    update();
}

void IconButton::setIconColor(const QColor& rest)
{
    m_rest = rest;
    update();
}

void IconButton::paintEvent(QPaintEvent* e)
{
    QToolButton::paintEvent(e); // QSS background (hover/pressed/checked)
    QPainter p(this);
    const bool enabled = isEnabled();
    QColor c = m_rest;
    if (!enabled)
        c = color::fg_5;
    else if (isChecked())
        c = m_checked;
    else if (underMouse() || isDown())
        c = m_active;
    const int s = iconSize().width();
    const QPixmap pm = iconPixmap(m_icon, s, c, devicePixelRatioF());
    QRect r(0, 0, s, s);
    r.moveCenter(rect().center());
    p.drawPixmap(r, pm);
}

// ---- card --------------------------------------------------------------------

Card::Card(QWidget* parent, Variant variant) : QFrame(parent), m_variant(variant)
{
    setFrameShape(QFrame::NoFrame);
    setProperty("fwNoRing", true);
    m_outer = new QVBoxLayout(this);
    m_outer->setSpacing(0);
    if (variant == Variant::Inset)
        m_outer->setContentsMargins(space::md, space::sm, space::md, space::sm);
    else if (variant == Variant::Table)
        m_outer->setContentsMargins(0, 0, 0, 0);
    else
        m_outer->setContentsMargins(space::lg, space::lg, space::lg, space::lg);
    m_body = new QVBoxLayout;
    m_body->setSpacing(variant == Variant::Inset ? 0 : space::md);
    m_body->setContentsMargins(0, 0, 0, 0);
    m_outer->addLayout(m_body);
}

QHBoxLayout* Card::setTitle(const QString& title, QWidget* rightSlot)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(space::sm);
    row->setContentsMargins(0, 0, 0, 0);
    m_title = heading(title, TextStyle::Subtitle, this);
    row->addWidget(m_title);
    row->addStretch(1);
    if (rightSlot)
        row->addWidget(rightSlot);
    m_body->insertLayout(0, row);
    return row;
}

void Card::setTitleText(const QString& title)
{
    if (m_title)
        m_title->setText(title);
}

QHBoxLayout* Card::header()
{
    if (!m_header) {
        m_headerWidget = new QWidget(this);
        m_header = hbox(m_headerWidget, space::sm, QMargins(space::lg, space::md, space::lg, space::md));
        m_outer->insertWidget(0, m_headerWidget);
        m_outer->insertWidget(1, hairline(this, true));
    }
    return m_header;
}

QHBoxLayout* Card::footer()
{
    if (!m_footer) {
        m_outer->addWidget(hairline(this, false));
        m_footerWidget = new QWidget(this);
        m_footer = hbox(m_footerWidget, space::md, QMargins(space::lg, space::sm, space::lg, space::sm));
        m_outer->addWidget(m_footerWidget);
    }
    return m_footer;
}

void Card::setDropHighlight(bool on)
{
    if (m_drop == on)
        return;
    m_drop = on;
    update();
}

void Card::setSelected(bool on)
{
    m_selected = on;
    update();
}

void Card::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fill = m_variant == Variant::Inset ? color::bg_0 : m_selected ? color::accent_lo : color::bg_2;
    p.setPen(QPen(color::border_2, 1));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
    if (m_drop) {
        QPen pen(color::accent_hi, 2, Qt::DashLine);
        pen.setDashPattern({4, 3});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(4, 4, -4, -4), radius::md, radius::md);
    }
}

// ---- badge ---------------------------------------------------------------------

QString defaultBadgeIcon(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return QStringLiteral("success");
    case Tone::Warn: return QStringLiteral("warning");
    case Tone::Err: return QStringLiteral("error");
    case Tone::Info: return QStringLiteral("clock");
    case Tone::Neutral: break;
    }
    return {};
}

QColor toneMark(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return color::success;
    case Tone::Warn: return color::warn;
    case Tone::Err: return color::danger;
    case Tone::Info: return color::accent_hi;
    case Tone::Neutral: break;
    }
    return color::fg_3;
}

QColor toneText(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return color::success_hi;
    case Tone::Warn: return color::warn_hi;
    case Tone::Err: return color::danger_hi;
    case Tone::Info: return color::accent_hi;
    case Tone::Neutral: break;
    }
    return color::fg_2;
}

QColor toneTint(Tone tone)
{
    switch (tone) {
    case Tone::Ok: return color::success_bg;
    case Tone::Warn: return color::warn_bg;
    case Tone::Err: return color::danger_bg;
    case Tone::Info: return color::accent_lo;
    case Tone::Neutral: break;
    }
    return color::bg_3;
}

Badge::Badge(const QString& text, Tone tone, QWidget* parent) : QWidget(parent), m_text(text), m_tone(tone)
{
    setFont(font::caption());
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setProperty("fwNoRing", true);
}

void Badge::set(const QString& text, Tone tone)
{
    m_text = text;
    m_tone = tone;
    setToolTip(text.size() > 32 ? text : QString());
    updateGeometry();
    update();
}

void Badge::setText(const QString& text) { set(text, m_tone); }
void Badge::setTone(Tone tone) { set(m_text, tone); }

void Badge::setIconName(const QString& iconName)
{
    m_icon = iconName;
    m_iconSet = true;
    updateGeometry();
    update();
}

void Badge::setCount(bool count)
{
    m_count = count;
    updateGeometry();
    update();
}

QSize Badge::sizeHint() const
{
    const QString iconName = m_iconSet ? m_icon : defaultBadgeIcon(m_tone);
    const QFontMetrics fm(font());
    const QString shown = elideMiddle(m_text, 32);
    int w = (m_count ? 6 : 8) * 2 + fm.horizontalAdvance(shown);
    if (!iconName.isEmpty() && !m_count)
        w += size::iconSm + 6;
    return {w, m_count ? 18 : 20};
}

void Badge::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(toneTint(m_tone));
    p.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);
    const QString iconName = (m_iconSet ? m_icon : defaultBadgeIcon(m_tone));
    int x = m_count ? 6 : 8;
    if (!iconName.isEmpty() && !m_count) {
        const QPixmap pm = iconPixmap(iconName, size::iconSm, m_tone == Tone::Neutral ? color::fg_3 : toneText(m_tone), devicePixelRatioF());
        p.drawPixmap(QRect(x, (height() - size::iconSm) / 2, size::iconSm, size::iconSm), pm);
        x += size::iconSm + 6;
    }
    p.setPen(toneText(m_tone));
    p.setFont(font());
    // A row that is narrower than its contents (translated texts in a side card)
    // hands the badge less than its size hint: elide instead of cutting glyphs.
    const QString shown = QFontMetrics(font()).elidedText(elideMiddle(m_text, 32), Qt::ElideRight, qMax(0, width() - x - 8));
    p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, shown);
}

// ---- switch -------------------------------------------------------------------

Switch::Switch(QWidget* parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setFixedSize(size::switchW, size::switchH);
    setFocusPolicy(Qt::TabFocus);
    setProperty("fwRadius", radius::pill);
    setAttribute(Qt::WA_Hover);
}

void Switch::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void Switch::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void Switch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool on = isChecked();
    const bool enabled = isEnabled();
    QColor fill, border, knob;
    if (!enabled) {
        fill = color::bg_2;
        border = color::border_3;
        knob = color::fg_5;
    } else if (on) {
        fill = border = m_hover ? color::accent_hover : color::accent;
        knob = color::on_accent;
    } else {
        fill = color::field_bg;
        border = m_hover ? color::border_4 : color::border_control;
        knob = color::fg_3;
    }
    p.setPen(QPen(border, 1));
    p.setBrush(fill);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    const int x = on ? width() - 3 - size::switchKnob : 3;
    p.drawEllipse(QRectF(x, (height() - size::switchKnob) / 2.0, size::switchKnob, size::switchKnob));
}

// ---- segmented ----------------------------------------------------------------

Segmented::Segmented(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFont(font::body());
}

void Segmented::addSegment(const QString& text, const QString& iconName, const QVariant& value)
{
    m_segments.push_back({text, iconName, value.isValid() ? value : QVariant(text), true});
    if (m_current < 0) {
        m_current = 0;
    }
    updateGeometry();
    update();
}

void Segmented::setSegmentText(int index, const QString& text)
{
    if (index < 0 || index >= count())
        return;
    m_segments[size_t(index)].text = text;
    updateGeometry();
    update();
}

void Segmented::setSegmentEnabled(int index, bool enabled)
{
    if (index < 0 || index >= count())
        return;
    m_segments[size_t(index)].enabled = enabled;
    update();
}

void Segmented::setCurrentIndex(int index)
{
    if (index < 0 || index >= count() || index == m_current)
        return;
    m_current = index;
    update();
    emit currentChanged(index);
}

QVariant Segmented::currentValue() const
{
    return (m_current >= 0 && m_current < count()) ? m_segments[size_t(m_current)].value : QVariant();
}

int Segmented::indexOfValue(const QVariant& value) const
{
    for (int i = 0; i < count(); ++i)
        if (m_segments[size_t(i)].value == value)
            return i;
    return -1;
}

void Segmented::setCurrentValue(const QVariant& value)
{
    const int i = indexOfValue(value);
    if (i >= 0)
        setCurrentIndex(i);
}

int Segmented::segmentWidth(int i) const
{
    const QFontMetrics fm(font::bodyStrong());
    const auto& s = m_segments[size_t(i)];
    int w = 20 + fm.horizontalAdvance(s.text);
    if (!s.icon.isEmpty())
        w += size::icon + 6;
    return w;
}

QSize Segmented::sizeHint() const
{
    int w = 3; // frame + padding
    for (int i = 0; i < count(); ++i)
        w += segmentWidth(i) + (i ? 2 : 0);
    w += 3;
    return {w, 32};
}

int Segmented::segmentAt(const QPoint& pt) const
{
    int x = 3;
    for (int i = 0; i < count(); ++i) {
        const int w = segmentWidth(i);
        if (pt.x() >= x && pt.x() < x + w)
            return i;
        x += w + 2;
    }
    return -1;
}

void Segmented::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color::border_3, 1));
    p.setBrush(color::field_bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::sm, radius::sm);
    int x = 3;
    for (int i = 0; i < count(); ++i) {
        const auto& s = m_segments[size_t(i)];
        const int w = segmentWidth(i);
        const QRect r(x, 3, w, height() - 6);
        const bool sel = i == m_current;
        const bool hov = i == m_hover && s.enabled && !sel;
        if (sel || hov) {
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? color::bg_5 : color::bg_3);
            p.drawRoundedRect(r, 3, 3);
        }
        QColor fg = !s.enabled ? color::fg_5 : sel ? color::fg_0 : hov ? color::fg_1 : color::fg_3;
        p.setPen(fg);
        p.setFont(sel ? font::bodyStrong() : font::body());
        int tx = x + 10;
        if (!s.icon.isEmpty()) {
            p.drawPixmap(QRect(tx, r.center().y() - size::icon / 2, size::icon, size::icon),
                         iconPixmap(s.icon, size::icon, fg, devicePixelRatioF()));
            tx += size::icon + 6;
        }
        p.drawText(QRect(tx, r.y(), w - (tx - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft, s.text);
        x += w + 2;
    }
}

void Segmented::mousePressEvent(QMouseEvent* e)
{
    const int i = segmentAt(e->pos());
    if (i >= 0 && m_segments[size_t(i)].enabled)
        setCurrentIndex(i);
}

void Segmented::mouseMoveEvent(QMouseEvent* e)
{
    const int i = segmentAt(e->pos());
    if (i != m_hover) {
        m_hover = i;
        update();
    }
}

void Segmented::leaveEvent(QEvent*)
{
    m_hover = -1;
    update();
}

void Segmented::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
        int i = m_current;
        const int step = e->key() == Qt::Key_Left ? -1 : 1;
        for (int n = 0; n < count(); ++n) {
            i = (i + step + count()) % count();
            if (m_segments[size_t(i)].enabled) {
                setCurrentIndex(i);
                break;
            }
        }
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---- progress -----------------------------------------------------------------

ProgressBar::ProgressBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(size::progress);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void ProgressBar::setFraction(double f)
{
    m_fraction = std::clamp(f, 0.0, 1.0);
    update();
}

void ProgressBar::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    m_t0 = QDateTime::currentMSecsSinceEpoch();
    updateTimer();
    update();
}

void ProgressBar::updateTimer()
{
    if (m_state == State::Indeterminate && isVisible())
        m_timer.start();
    else
        m_timer.stop();
}

void ProgressBar::showEvent(QShowEvent*) { updateTimer(); }
void ProgressBar::hideEvent(QHideEvent*) { m_timer.stop(); }

void ProgressBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color::bg_5);
    p.drawRoundedRect(rect(), radius::xs, radius::xs);
    QColor fill = color::accent;
    double frac = m_fraction;
    switch (m_state) {
    case State::Done: fill = color::success; frac = 1.0; break;
    case State::Failed: fill = color::danger; break;
    case State::Paused: fill = color::fg_4; break;
    case State::Indeterminate: {
        const double t = std::fmod((QDateTime::currentMSecsSinceEpoch() - m_t0) / 1400.0, 1.0);
        const double segW = width() * 0.35;
        const double x = -segW + t * (width() + segW);
        p.setClipRect(rect());
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(x, 0, segW, height()), radius::xs, radius::xs);
        return;
    }
    case State::Determinate: break;
    }
    if (frac > 0) {
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(0, 0, std::max(double(size::progress), width() * frac), height()), radius::xs, radius::xs);
    }
}

ProgressBlock::ProgressBlock(QWidget* parent) : QWidget(parent)
{
    auto* l = vbox(this, space::sm);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::md);
    m_bar = new ProgressBar(this);
    m_percent = monoLabel(QStringLiteral("0 %"), this, color::fg_0);
    m_percent->setMinimumWidth(QFontMetrics(font::mono()).horizontalAdvance(QStringLiteral("100 %")));
    row->addWidget(m_bar, 1);
    row->addWidget(m_percent);
    l->addLayout(row);
    m_detail = label({}, TextStyle::Caption, color::fg_2, this);
    m_detail->setWordWrap(true);
    l->addWidget(m_detail);
    m_stepper = new StageStepper(this);
    m_stepperLayout = nullptr;
    l->addWidget(m_stepper);
    m_stepper->hide();
}

void ProgressBlock::setProgress(double fraction, bool indeterminate, const QString& detail)
{
    m_bar->setFraction(fraction);
    if (m_bar->state() == ProgressBar::State::Determinate || m_bar->state() == ProgressBar::State::Indeterminate)
        m_bar->setState(indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
    m_percent->setText(indeterminate ? QString() : thinPercent(fraction));
    m_percent->setVisible(!indeterminate);
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
}

void ProgressBlock::setState(ProgressBar::State state)
{
    m_bar->setState(state);
    QColor c = color::fg_0;
    if (state == ProgressBar::State::Done) {
        c = color::success_hi;
        m_percent->setText(tr("Done"));
    } else if (state == ProgressBar::State::Failed) {
        c = color::danger_hi;
        m_percent->setText(tr("Failed at %1").arg(thinPercent(m_bar->fraction())));
    } else if (state == ProgressBar::State::Paused) {
        c = color::fg_3;
        m_percent->setText(tr("Paused · %1").arg(thinPercent(m_bar->fraction())));
    }
    m_percent->setVisible(true);
    m_percent->setMinimumWidth(0);
    setTextColor(m_percent, c);
}

void ProgressBlock::setPhases(const QStringList& phases, int current)
{
    static_cast<StageStepper*>(m_stepper)->setStages(phases, current, m_bar->state() == ProgressBar::State::Failed);
    m_stepper->setVisible(!phases.isEmpty());
}

void ProgressBlock::setPhasesVisible(bool visible)
{
    m_stepper->setVisible(visible && !static_cast<StageStepper*>(m_stepper)->isEmpty());
}

StageStepper::StageStepper(QWidget* parent) : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setSpacing(space::md);
    m_layout->setContentsMargins(0, 0, 0, 0);
}

void StageStepper::setStages(const QStringList& names, int current, bool failed)
{
    m_names = names;
    while (QLayoutItem* it = m_layout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (int i = 0; i < names.size(); ++i) {
        auto* w = new QWidget(this);
        auto* h = hbox(w, 4);
        QString iconName = QStringLiteral("circle");
        QColor c = color::fg_3;
        QFont f = font::caption();
        if (i < current) {
            iconName = QStringLiteral("check");
            c = color::success_hi;
        } else if (i == current) {
            iconName = failed ? QStringLiteral("error") : QStringLiteral("dot");
            c = failed ? color::danger : color::fg_0;
            f = font::bodyStrong();
            f.setPixelSize(12);
        }
        auto* ic = new QLabel(w);
        ic->setPixmap(iconPixmap(iconName, size::iconSm, c, devicePixelRatioF()));
        ic->setFixedSize(size::iconSm, size::iconSm);
        auto* t = new QLabel(names.at(i), w);
        t->setFont(f);
        setTextColor(t, i == current && !failed ? color::fg_0 : c);
        h->addWidget(ic);
        h->addWidget(t);
        m_layout->addWidget(w);
    }
    m_layout->addStretch(1);
}

// ---- info bar -----------------------------------------------------------------

InfoBar::InfoBar(Tone tone, QWidget* parent) : QFrame(parent), m_tone(tone)
{
    setFrameShape(QFrame::NoFrame);
    setProperty("fwNoRing", true);
    auto* h = hbox(this, 10, QMargins(space::md, 10, space::md, 10));
    h->setAlignment(Qt::AlignTop);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(size::icon, size::icon);
    h->addWidget(m_icon, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    col->setContentsMargins(0, 0, 0, 0);
    m_title = label({}, TextStyle::BodyStrong, color::fg_0, this);
    m_title->setWordWrap(true);
    m_title->hide();
    m_message = label({}, TextStyle::Body, color::fg_1, this);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_title);
    col->addWidget(m_message);
    m_links = new QHBoxLayout;
    m_links->setSpacing(space::md);
    m_links->setContentsMargins(0, 4, 0, 0);
    m_links->addStretch(1);
    col->addLayout(m_links);
    h->addLayout(col, 1);
    m_dismiss = new IconButton(QStringLiteral("close"), tr("Dismiss"), IconButton::Size::Tiny, this);
    m_dismiss->hide();
    connect(m_dismiss, &QToolButton::clicked, this, [this] {
        hide();
        emit dismissed();
    });
    h->addWidget(m_dismiss, 0, Qt::AlignTop);
    setTone(tone);
}

void InfoBar::setTitle(const QString& title)
{
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    if (!title.isEmpty())
        setTextColor(m_title, m_tone == Tone::Err ? color::danger_hi : color::fg_0);
}

void InfoBar::setMessage(const QString& message)
{
    m_message->setText(message);
    m_message->setVisible(!message.isEmpty());
}

void InfoBar::setTone(Tone tone)
{
    m_tone = tone;
    const QString iconName = tone == Tone::Info ? QStringLiteral("info") : defaultBadgeIcon(tone);
    m_icon->setPixmap(iconPixmap(iconName.isEmpty() ? QStringLiteral("info") : iconName, size::icon, toneMark(tone),
                                 devicePixelRatioF()));
    update();
}

void InfoBar::setDismissable(bool on) { m_dismiss->setVisible(on); }

void InfoBar::clearLinks()
{
    while (m_links->count() > 1) {
        QLayoutItem* it = m_links->takeAt(0);
        delete it->widget();
        delete it;
    }
}

Link* InfoBar::addLink(const QString& text)
{
    auto* l = new Link(text, this);
    m_links->insertWidget(m_links->count() - 1, l);
    return l;
}

void InfoBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(toneTint(m_tone), 1));
    p.setBrush(toneTint(m_tone));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
}

// ---- states ---------------------------------------------------------------------

EmptyState::EmptyState(const QString& iconName, const QString& title, const QString& text, QWidget* parent)
    : QWidget(parent)
{
    m_layout = vbox(this, space::sm, QMargins(space::xxl, 40, space::xxl, 40));
    m_layout->setAlignment(Qt::AlignHCenter);
    auto* tile = new QLabel(this);
    tile->setFixedSize(56, 56);
    tile->setAlignment(Qt::AlignCenter);
    tile->setAutoFillBackground(false);
    tile->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(hex(color::bg_3)).arg(radius::md));
    tile->setPixmap(iconPixmap(iconName, size::iconEmpty, color::fg_3, devicePixelRatioF()));
    m_layout->addWidget(tile, 0, Qt::AlignHCenter);
    auto* t = heading(title, TextStyle::Subtitle, this);
    t->setAlignment(Qt::AlignCenter);
    t->setWordWrap(true);
    t->setFixedWidth(360);
    m_layout->addSpacing(4);
    m_layout->addWidget(t, 0, Qt::AlignHCenter);
    auto* c = caption(text, this);
    c->setAlignment(Qt::AlignCenter);
    c->setFixedWidth(320);
    // Word-wrapped labels of a fixed width report a one-line minimum height, so a
    // height-constrained parent (a card body that also holds a hidden table) would
    // squeeze a three-line caption (German, Japanese…) over the title. Reserve the
    // wrapped height up front; the texts never change after construction.
    t->setMinimumHeight(t->heightForWidth(360));
    c->setMinimumHeight(c->heightForWidth(320));
    m_layout->addWidget(c, 0, Qt::AlignHCenter);
    auto* actions = new QWidget(this);
    m_actions = hbox(actions, space::sm, QMargins(0, space::sm, 0, 0));
    m_layout->addWidget(actions, 0, Qt::AlignHCenter);
    actions->hide();
}

QPushButton* EmptyState::addAction(const QString& text, ButtonVariant variant)
{
    auto* b = button(text, variant, {}, this);
    m_actions->addWidget(b);
    m_actions->parentWidget()->show();
    return b;
}

void EmptyState::setCompact(bool compact)
{
    m_layout->setContentsMargins(space::xxl, compact ? 20 : 40, space::xxl, compact ? 20 : 40);
}

ErrorState::ErrorState(const QString& title, const QString& body, const QString& technical, QWidget* parent)
    : QWidget(parent)
{
    auto* l = vbox(this, space::md);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::md);
    row->setAlignment(Qt::AlignTop);
    auto* tile = new QLabel(this);
    tile->setFixedSize(40, 40);
    tile->setAlignment(Qt::AlignCenter);
    tile->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(hex(color::danger_bg)).arg(radius::md));
    tile->setPixmap(iconPixmap(QStringLiteral("error"), size::iconDialog, color::danger, devicePixelRatioF()));
    row->addWidget(tile, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(4);
    m_title = heading(title, TextStyle::Subtitle, this);
    m_title->setWordWrap(true);
    m_body = label(body, TextStyle::Body, color::fg_2, this);
    m_body->setWordWrap(true);
    m_tech = label(technical, TextStyle::Mono, color::fg_3, this);
    m_tech->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_tech->setVisible(!technical.isEmpty());
    col->addWidget(m_title);
    col->addWidget(m_body);
    col->addSpacing(4);
    col->addWidget(m_tech);
    row->addLayout(col, 1);
    l->addLayout(row);
    auto* actions = new QWidget(this);
    m_actions = hbox(actions, space::sm);
    m_actions->addStretch(1);
    l->addWidget(actions);
    actions->hide();
}

void ErrorState::setTechnical(const QString& line)
{
    m_tech->setText(line);
    m_tech->setVisible(!line.isEmpty());
}

void ErrorState::setBody(const QString& body) { m_body->setText(body); }
void ErrorState::setTitle(const QString& title) { m_title->setText(title); }

QPushButton* ErrorState::addAction(const QString& text, ButtonVariant variant)
{
    auto* b = button(text, variant, {}, this);
    m_actions->addWidget(b);
    m_actions->parentWidget()->show();
    return b;
}

UnavailableState::UnavailableState(const QString& feature, const QString& kind, QWidget* parent) : QWidget(parent)
{
    auto* l = vbox(this, space::sm, QMargins(space::xxl, 32, space::xxl, 32));
    l->setAlignment(Qt::AlignHCenter);
    auto* tile = new QLabel(this);
    tile->setFixedSize(56, 56);
    tile->setAlignment(Qt::AlignCenter);
    tile->setStyleSheet(QStringLiteral("background:%1;border-radius:%2px;").arg(hex(color::bg_3)).arg(radius::md));
    tile->setPixmap(iconPixmap(QStringLiteral("package"), size::iconEmpty, color::fg_3, devicePixelRatioF()));
    l->addWidget(tile, 0, Qt::AlignHCenter);
    auto* t = heading(tr("%1 is not available in this build").arg(feature), TextStyle::Subtitle, this);
    t->setAlignment(Qt::AlignCenter);
    t->setWordWrap(true);
    t->setFixedWidth(420);
    l->addSpacing(4);
    l->addWidget(t, 0, Qt::AlignHCenter);
    auto* c = caption(tr("The engine that does this work is not part of this build. The form stays usable; nothing is queued until the engine is present."), this);
    c->setAlignment(Qt::AlignCenter);
    c->setFixedWidth(420);
    l->addWidget(c, 0, Qt::AlignHCenter);
    m_tech = label(tr("job kind %1 is not registered").arg(kind), TextStyle::Mono, color::fg_4, this);
    m_tech->setAlignment(Qt::AlignCenter);
    m_tech->setWordWrap(true);
    m_tech->setFixedWidth(420);
    l->addWidget(m_tech, 0, Qt::AlignHCenter);
}

void UnavailableState::setKinds(const QStringList& kinds)
{
    m_tech->setText(kinds.size() == 1 ? tr("job kind %1 is not registered").arg(kinds.first())
                                      : tr("job kinds %1 are not registered").arg(kinds.join(QStringLiteral(", "))));
}

// ---- inputs ---------------------------------------------------------------------

QLineEdit* textInput(const QString& placeholder, bool mono, QWidget* parent)
{
    auto* e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setProperty("mono", mono);
    if (mono)
        e->setFont(font::mono());
    e->setFixedHeight(size::control);
    return e;
}

void setInputError(QLineEdit* edit, bool error)
{
    if (edit->property("error").toBool() == error)
        return;
    edit->setProperty("error", error);
    repolish(edit);
}

QComboBox* dropdown(QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->setFixedHeight(size::control);
    c->setMaxVisibleItems(12);
    c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    return c;
}

PathField::PathField(Mode mode, QWidget* parent) : QWidget(parent), m_mode(mode)
{
    setAcceptDrops(true);
    auto* l = vbox(this, space::xs);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::sm);
    m_edit = textInput({}, true, this);
    m_browse = button(tr("Browse…"), ButtonVariant::Secondary, QStringLiteral("folder"), this);
    row->addWidget(m_edit, 1);
    row->addWidget(m_browse);
    l->addLayout(row);
    m_errorRow = new QWidget(this);
    auto* er = hbox(m_errorRow, 6);
    m_errorLabel = label({}, TextStyle::Caption, color::danger_hi, m_errorRow);
    m_errorLabel->setWordWrap(true);
    auto* ic = new QLabel(m_errorRow);
    ic->setPixmap(iconPixmap(QStringLiteral("error"), size::iconSm, color::danger, devicePixelRatioF()));
    ic->setFixedSize(size::iconSm, size::iconSm);
    er->addWidget(ic, 0, Qt::AlignTop);
    er->addWidget(m_errorLabel, 1);
    l->addWidget(m_errorRow);
    m_errorRow->hide();
    setFocusProxy(m_edit);
    connect(m_browse, &QPushButton::clicked, this, &PathField::browse);
    connect(m_edit, &QLineEdit::editingFinished, this, [this] { emit pathChanged(path()); });
    connect(m_edit, &QLineEdit::textEdited, this, [this](const QString&) { emit pathEdited(path()); });
}

QString PathField::path() const { return QDir::fromNativeSeparators(m_edit->text().trimmed()); }

void PathField::setPath(const QString& p)
{
    const QString native = QDir::toNativeSeparators(p);
    if (m_edit->text() == native)
        return;
    m_edit->setText(native);
    m_edit->setCursorPosition(0);
    emit pathChanged(path());
}

void PathField::setPlaceholder(const QString& text) { m_edit->setPlaceholderText(text); }

void PathField::setError(const QString& message)
{
    m_errorText = message;
    m_errorLabel->setText(message);
    m_errorRow->setVisible(!message.isEmpty());
    setInputError(m_edit, !message.isEmpty());
}

void PathField::setReadOnly(bool ro)
{
    m_edit->setReadOnly(ro);
    m_browse->setVisible(!ro);
}

void PathField::browse()
{
    const QString start = !path().isEmpty() ? path() : m_startDir;
    QString chosen;
    switch (m_mode) {
    case Mode::Directory:
        chosen = QFileDialog::getExistingDirectory(window(), m_title.isEmpty() ? tr("Choose a folder") : m_title, start);
        break;
    case Mode::OpenFile:
        chosen = QFileDialog::getOpenFileName(window(), m_title.isEmpty() ? tr("Choose a file") : m_title, start, m_filter);
        break;
    case Mode::SaveFile:
        chosen = QFileDialog::getSaveFileName(window(), m_title.isEmpty() ? tr("Save as") : m_title, start, m_filter);
        break;
    }
    if (!chosen.isEmpty())
        setPath(chosen);
}

void PathField::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() && !m_edit->isReadOnly())
        e->acceptProposedAction();
}

void PathField::dropEvent(QDropEvent* e)
{
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QString local = urls.first().toLocalFile();
    if (local.isEmpty())
        return;
    setPath(local);
    e->acceptProposedAction();
}

MessageRow::MessageRow(QWidget* parent) : QWidget(parent)
{
    auto* h = hbox(this, 6);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(size::iconSm, size::iconSm);
    m_text = label({}, TextStyle::Caption, color::fg_3, this);
    m_text->setWordWrap(true);
    h->addWidget(m_icon, 0, Qt::AlignTop);
    h->addWidget(m_text, 1);
    hide();
}

void MessageRow::set(const QString& text, Tone tone)
{
    if (text.isEmpty()) {
        clear();
        return;
    }
    const QString iconName = tone == Tone::Info ? QStringLiteral("info") : tone == Tone::Neutral ? QStringLiteral("info") : defaultBadgeIcon(tone);
    m_icon->setPixmap(iconPixmap(iconName, size::iconSm, tone == Tone::Neutral ? color::fg_3 : toneMark(tone), devicePixelRatioF()));
    m_text->setText(text);
    setTextColor(m_text, tone == Tone::Neutral ? color::fg_3 : toneText(tone));
    show();
}

QString MessageRow::text() const { return m_text->text(); }

void MessageRow::clear()
{
    m_text->clear();
    hide();
}

// ---- two column layout -------------------------------------------------------------

TwoColumnLayout::TwoColumnLayout(QWidget* parent, int rightWidth, int leftMax, int gap)
    : QLayout(parent), m_rightWidth(rightWidth), m_leftMax(leftMax), m_gap(gap)
{
    setContentsMargins(0, 0, 0, 0);
}

TwoColumnLayout::~TwoColumnLayout()
{
    delete m_left;
    delete m_right;
}

void TwoColumnLayout::setLeft(QWidget* w)
{
    delete m_left;
    m_left = nullptr;
    if (w) {
        addChildWidget(w);
        m_left = new QWidgetItem(w);
    }
    invalidate();
}

void TwoColumnLayout::setRight(QWidget* w)
{
    delete m_right;
    m_right = nullptr;
    if (w) {
        addChildWidget(w);
        m_right = new QWidgetItem(w);
    }
    invalidate();
}

void TwoColumnLayout::addItem(QLayoutItem* item)
{
    if (!m_left)
        m_left = item;
    else if (!m_right)
        m_right = item;
    else
        delete item;
}

QLayoutItem* TwoColumnLayout::itemAt(int index) const
{
    if (index == 0)
        return m_left ? m_left : m_right;
    if (index == 1 && m_left)
        return m_right;
    return nullptr;
}

QLayoutItem* TwoColumnLayout::takeAt(int index)
{
    QLayoutItem* out = nullptr;
    if (index == 0 && m_left) {
        out = m_left;
        m_left = nullptr;
    } else if ((index == 0 && !m_left) || index == 1) {
        out = m_right;
        m_right = nullptr;
    }
    return out;
}

int TwoColumnLayout::count() const { return (m_left ? 1 : 0) + (m_right ? 1 : 0); }

bool TwoColumnLayout::stackedFor(int width) const
{
    if (!m_right || !m_left)
        return false;
    const int minLeft = std::max(m_left->minimumSize().width(), 560);
    const int threshold = m_breakpoint > 0 ? m_breakpoint : minLeft + m_gap + m_rightWidth;
    return width < threshold;
}

QSize TwoColumnLayout::sizeHint() const
{
    int w = 0, h = 0;
    if (m_left) {
        w += std::min(m_left->sizeHint().width(), m_leftMax);
        h = m_left->sizeHint().height();
    }
    if (m_right) {
        w += m_rightWidth + (m_left ? m_gap : 0);
        h = std::max(h, m_right->sizeHint().height());
    }
    return {w, h};
}

QSize TwoColumnLayout::minimumSize() const
{
    int w = 0, h = 0;
    if (m_left) {
        w = std::max(w, m_left->minimumSize().width());
        h += m_left->minimumSize().height();
    }
    if (m_right) {
        w = std::max(w, m_rightWidth);
        h += m_right->minimumSize().height() + (m_left ? m_gap : 0);
    }
    return {w, h};
}

int TwoColumnLayout::heightForWidth(int width) const
{
    if (stackedFor(width)) {
        int h = 0;
        if (m_left)
            h += m_left->hasHeightForWidth() ? m_left->heightForWidth(width) : m_left->sizeHint().height();
        if (m_right)
            h += m_gap + (m_right->hasHeightForWidth() ? m_right->heightForWidth(std::min(width, m_rightWidth)) : m_right->sizeHint().height());
        return h;
    }
    int h = 0;
    if (m_left) {
        const int lw = std::min(m_leftMax, width - (m_right ? m_rightWidth + m_gap : 0));
        h = m_left->hasHeightForWidth() ? m_left->heightForWidth(lw) : m_left->sizeHint().height();
    }
    if (m_right)
        h = std::max(h, m_right->hasHeightForWidth() ? m_right->heightForWidth(m_rightWidth) : m_right->sizeHint().height());
    return h;
}

void TwoColumnLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    m_stacked = stackedFor(rect.width());
    if (m_stacked) {
        int y = rect.y();
        if (m_left) {
            const int h = m_left->hasHeightForWidth() ? m_left->heightForWidth(rect.width()) : m_left->sizeHint().height();
            m_left->setGeometry(QRect(rect.x(), y, rect.width(), h));
            y += h + m_gap;
        }
        if (m_right) {
            const int w = rect.width();
            const int h = m_right->hasHeightForWidth() ? m_right->heightForWidth(w) : m_right->sizeHint().height();
            m_right->setGeometry(QRect(rect.x(), y, w, h));
        }
        return;
    }
    if (m_left) {
        const int lw = std::min(m_leftMax, rect.width() - (m_right ? m_rightWidth + m_gap : 0));
        const int h = m_left->hasHeightForWidth() ? m_left->heightForWidth(lw) : m_left->sizeHint().height();
        m_left->setGeometry(QRect(rect.x(), rect.y(), lw, std::max(h, m_left->minimumSize().height())));
        if (m_right) {
            const int rx = rect.x() + lw + m_gap;
            const int rh = m_right->hasHeightForWidth() ? m_right->heightForWidth(m_rightWidth) : m_right->sizeHint().height();
            m_right->setGeometry(QRect(rx, rect.y(), m_rightWidth, std::max(rh, m_right->minimumSize().height())));
        }
    } else if (m_right) {
        m_right->setGeometry(QRect(rect.x(), rect.y(), m_rightWidth, m_right->sizeHint().height()));
    }
}

// ---- flow layout ----------------------------------------------------------------

FlowLayout::FlowLayout(QWidget* parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* it = takeAt(0))
        delete it;
}

void FlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }
QLayoutItem* FlowLayout::itemAt(int index) const { return m_items.value(index); }
QLayoutItem* FlowLayout::takeAt(int index)
{
    return (index >= 0 && index < m_items.size()) ? m_items.takeAt(index) : nullptr;
}
QSize FlowLayout::sizeHint() const { return minimumSize(); }
QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (auto* it : m_items)
        s = s.expandedTo(it->minimumSize());
    return s + QSize(contentsMargins().left() + contentsMargins().right(), contentsMargins().top() + contentsMargins().bottom());
}
void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}
int FlowLayout::heightForWidth(int width) const { return doLayout(QRect(0, 0, width, 0), true); }
int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QRect r = rect.marginsRemoved(contentsMargins());
    int x = r.x(), y = r.y(), lineH = 0;
    for (auto* it : m_items) {
        const QSize s = it->sizeHint();
        int nextX = x + s.width() + m_h;
        if (nextX - m_h > r.right() + 1 && lineH > 0) {
            x = r.x();
            y += lineH + m_v;
            nextX = x + s.width() + m_h;
            lineH = 0;
        }
        if (!testOnly)
            it->setGeometry(QRect(QPoint(x, y), s));
        x = nextX;
        lineH = std::max(lineH, s.height());
    }
    return y + lineH - r.y() + contentsMargins().top() + contentsMargins().bottom();
}

// ---- misc -------------------------------------------------------------------------

QString thinPercent(double fraction)
{
    return QStringLiteral("%1 %").arg(int(std::floor(std::clamp(fraction, 0.0, 1.0) * 100.0 + 1e-9)));
}

QString elideMiddle(const QString& s, int max)
{
    if (s.size() <= max)
        return s;
    const int keep = (max - 1) / 2;
    return s.left(keep) + QStringLiteral("…") + s.right(max - 1 - keep);
}

QString nativePath(const QString& path) { return QDir::toNativeSeparators(path); }

void openInFileManager(const QString& path)
{
    if (path.isEmpty())
        return;
    const QFileInfo fi(path);
#ifdef Q_OS_WIN
    if (fi.exists() && !fi.isDir()) {
        QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(fi.absoluteFilePath())});
        return;
    }
#endif
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

} // namespace fw::ui
