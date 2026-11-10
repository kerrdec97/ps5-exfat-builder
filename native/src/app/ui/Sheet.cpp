#include "app/ui/Sheet.h"

#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "app/ui/Style.h"

#include <QApplication>
#include <QEvent>
#include <QAbstractSpinBox>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace fw::ui {

using namespace theme;

namespace {
// A toast's dynamic property: how many px it still is from its place while it slides in.
const char* const kSlide = "fwSlide";
} // namespace

Sheet::Sheet(QWidget* host, const QString& title, Icon icon, int width) : QWidget(host), m_width(width)
{
    setProperty("fwNoRing", true);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    m_panel = new QFrame(this);
    m_panel->setObjectName(QStringLiteral("sheetPanel"));
    m_panel->setStyleSheet(QStringLiteral("#sheetPanel{background:%1;border:1px solid %2;border-radius:%3px;}")
                               .arg(hex(color::bg_3), hex(color::border_4)).arg(radius::md));
    m_panel->setProperty("fwNoRing", true);
    auto* outer = vbox(m_panel, space::md, QMargins(space::xl, space::xl, space::xl, space::xl));

    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    if (icon != Icon::None) {
        auto* ic = new QLabel(m_panel);
        ic->setFixedSize(size::iconDialog, size::iconDialog);
        const QString name = icon == Icon::Error ? QStringLiteral("error") : icon == Icon::Warning ? QStringLiteral("warning") : QStringLiteral("info");
        const QColor c = icon == Icon::Error ? color::danger : icon == Icon::Warning ? color::warn : color::accent_hi;
        ic->setPixmap(iconPixmap(name, size::iconDialog, c, devicePixelRatioF()));
        // Level with the first line of the title, whose line is taller than the glyph.
        auto* iconColumn = new QVBoxLayout;
        iconColumn->setContentsMargins(0, (QFontMetrics(font::title()).height() - size::iconDialog + 1) / 2, 0, 0);
        iconColumn->addWidget(ic);
        iconColumn->addStretch(1);
        titleRow->addLayout(iconColumn);
    }
    m_title = heading(title, TextStyle::Title, m_panel);
    m_title->setWordWrap(true);
    titleRow->addWidget(m_title, 1);
    outer->addLayout(titleRow);

    m_body = new QVBoxLayout;
    m_body->setSpacing(space::md);
    m_body->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(m_body);

    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(space::sm);
    m_buttons->setContentsMargins(0, space::sm, 0, 0);
    m_buttons->addStretch(1);
    outer->addLayout(m_buttons);
    host->installEventFilter(this);
    m_shown = new motion::Value(this, 1.0, motion::base);
    m_shown->onChange([this] { applyShown(); });
}

// The scrim fades with m_shown; the panel rises its last 8 px and fades too.
// The fade is an opacity effect on the panel alone (a small widget, for a
// fraction of a second) that is taken off again once the sheet is open.
void Sheet::applyShown()
{
    const qreal v = m_shown->value();
    m_panel->move(m_panelRect.x(), m_panelRect.y() + qRound((1.0 - v) * 8));
    auto* fade = qobject_cast<QGraphicsOpacityEffect*>(m_panel->graphicsEffect());
    if (v < 1.0) {
        if (!fade) {
            fade = new QGraphicsOpacityEffect(m_panel);
            m_panel->setGraphicsEffect(fade);
        }
        fade->setOpacity(v);
    } else if (fade) {
        m_panel->setGraphicsEffect(nullptr);
    }
    update();
    if (m_closing && v <= 0.0)
        QMetaObject::invokeMethod(this, &Sheet::finishClose, Qt::QueuedConnection); // not from inside the animation's own step
}

void Sheet::finishClose()
{
    hide();
    deleteLater();
}

namespace {
int g_openSheets = 0;
}

int Sheet::openCount() { return g_openSheets; }

Sheet::~Sheet()
{
    if (m_counted) {
        m_counted = false;
        --g_openSheets;
    }
    if (m_loop)
        m_loop->exit(m_result);
}

void Sheet::setTitle(const QString& title) { m_title->setText(title); }

namespace {

// Names the spans this process wrote. Text from outside cannot contain it, so
// it cannot make addText() read a message as markup.
const QString& spanMarker()
{
    static const QString marker = QStringLiteral("fw%1").arg(QRandomGenerator::global()->generate64(), 0, 16);
    return marker;
}

QString spanOpen()
{
    return QStringLiteral("<span class=\"%1\" style=\"font-family:'%2';font-size:12px\">").arg(spanMarker(), monoFamily());
}

} // namespace

QLabel* Sheet::addBodyLabel(const QString& text, Qt::TextFormat format)
{
    QLabel* l = format == Qt::RichText ? richLabel(text, TextStyle::Body, color::fg_1, m_panel)
                                       : label(text, TextStyle::Body, color::fg_1, m_panel);
    l->setWordWrap(true);
    m_body->addWidget(l);
    return l;
}

QLabel* Sheet::addText(const QString& text)
{
    const bool ownMarkup = text.contains(QStringLiteral("<span class=\"%1\"").arg(spanMarker()));
    return addBodyLabel(text, ownMarkup ? Qt::RichText : Qt::PlainText);
}

QLabel* Sheet::addPlainText(const QString& text) { return addBodyLabel(text, Qt::PlainText); }
QLabel* Sheet::addRichText(const QString& html) { return addBodyLabel(html, Qt::RichText); }

QCheckBox* Sheet::addCheckbox(const QString& text)
{
    auto* c = new QCheckBox(text, m_panel);
    m_body->addWidget(c);
    return c;
}

QPushButton* Sheet::addButton(const QString& text, Role role)
{
    const ButtonVariant v = role == Role::Primary ? ButtonVariant::Primary
                          : role == Role::Destructive ? ButtonVariant::Danger : ButtonVariant::Secondary;
    auto* b = button(text, v, {}, m_panel);
    const int index = m_buttonList.size();
    m_buttonList << b;
    m_roles << role;
    m_buttons->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, index] { close(index); });
    return b;
}

QString Sheet::monoSpan(const QString& text)
{
    return spanOpen() + text.toHtmlEscaped() + QStringLiteral("</span>");
}

QString Sheet::monoLines(const QStringList& plainLines)
{
    QStringList escaped;
    escaped.reserve(plainLines.size());
    for (const QString& line : plainLines)
        escaped << line.toHtmlEscaped();
    return spanOpen() + escaped.join(QStringLiteral("<br>")) + QStringLiteral("</span>");
}

void Sheet::relayout()
{
    if (!parentWidget())
        return;
    setGeometry(parentWidget()->rect());
    const int w = std::min(m_width, width() - 2 * space::lg);
    m_panel->setFixedWidth(w);
    // The height the wrapped text needs at this width (sizeHint() is for the
    // labels' preferred width), and never taller than the window, so the
    // buttons stay on screen.
    QLayout* l = m_panel->layout();
    int h = l && l->hasHeightForWidth() ? l->totalHeightForWidth(w) : m_panel->sizeHint().height();
    h = std::min(h, std::max(l ? l->totalMinimumSize().height() : 0, height() - 2 * space::lg));
    m_panelRect = QRect((width() - w) / 2, std::max(space::lg, (height() - h) / 2), w, h);
    m_panel->setGeometry(m_panelRect.translated(0, qRound((1.0 - m_shown->value()) * 8)));
}

void Sheet::open()
{
    if (!m_counted) {
        m_counted = true;
        ++g_openSheets;
    }
    m_previousFocus = QApplication::focusWidget();
    m_previousByKeyboard = hasKeyboardFocus(m_previousFocus);
    m_closing = false;
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_shown->jumpTo(1.0); // the resting geometry first
    relayout();
    raise();
    show();
    if (motion::enabled()) { // the scrim fades in, the panel rises and fades in
        m_shown->setDuration(motion::base);
        m_shown->jumpTo(0.0);
        m_shown->animateTo(1.0);
    }
    if (QPushButton* focusTarget = defaultButton()) {
        window()->setAttribute(Qt::WA_KeyboardFocusChange, true);
        focusTarget->setFocus(Qt::TabFocusReason);
    } else {
        setFocus();
    }
}

// The safe action for destructive dialogs, the primary one otherwise.
QPushButton* Sheet::defaultButton() const
{
    const bool destructive = m_roles.contains(Role::Destructive);
    for (int i = 0; i < m_roles.size(); ++i) {
        if (destructive && m_roles[i] == Role::Safe)
            return m_buttonList[i];
        if (!destructive && m_roles[i] == Role::Primary)
            return m_buttonList[i];
    }
    return m_buttonList.isEmpty() ? nullptr : m_buttonList.last();
}

int Sheet::exec()
{
    open();
    QEventLoop loop;
    m_loop = &loop;
    m_answered = false;
    QPointer<Sheet> self(this);
    loop.exec(QEventLoop::DialogExec);
    if (!self)
        return -1; // destroyed with its host while it was open: no answer
    m_loop = nullptr;
    // The loop also ends when the application quits: that is not "the first
    // button" but no answer, i.e. the safe one.
    if (!m_answered)
        close(-1);
    return m_result;
}

void Sheet::close(int result)
{
    if (m_closing)
        return; // answered already: it is only fading away
    if (result < 0) {
        for (int i = 0; i < m_roles.size(); ++i)
            if (m_roles[i] == Role::Safe) {
                result = i;
                break;
            }
    }
    m_result = result;
    m_answered = true;
    if (m_counted) {
        m_counted = false;
        --g_openSheets;
    }
    // With motion the sheet fades away (fast) instead of vanishing: it is
    // answered and counted as closed now, lets the mouse through, and is
    // hidden and deleted when the fade ends.
    // (Not when it is closed before its first frame: there is nothing to fade.)
    const bool fade = isVisible() && motion::enabled() && m_shown->value() > 0.0;
    if (fade) {
        m_closing = true;
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_shown->setDuration(motion::fast);
        m_shown->animateTo(0.0);
    } else {
        hide();
    }
    if (m_previousFocus) // a keyboard user gets the ring back where it was
        m_previousFocus->setFocus(m_previousByKeyboard ? Qt::TabFocusReason : Qt::OtherFocusReason);
    if (m_loop)
        m_loop->exit(result);
    if (!fade)
        deleteLater();
}

void Sheet::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), withAlpha(color::scrim, qRound(color::scrim.alpha() * m_shown->value())));
}

void Sheet::resizeEvent(QResizeEvent*) { relayout(); }

void Sheet::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        close(-1);
        return;
    }
    // Enter anywhere but on a button (those press themselves) is the default
    // button. Not from a text field of a destructive sheet: its default is the
    // safe button, and Enter there would throw away what was typed.
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !e->isAutoRepeat() && isVisible()
        && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))) {
        QWidget* fw = QApplication::focusWidget();
        const bool typing = qobject_cast<QLineEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw)
                         || qobject_cast<QTextEdit*>(fw) || qobject_cast<QAbstractSpinBox*>(fw);
        QPushButton* b = defaultButton();
        if (auto* focused = qobject_cast<QPushButton*>(fw); focused && m_buttonList.contains(focused))
            b = focused;
        if (b && b->isEnabled() && !(typing && m_roles.contains(Role::Destructive))) {
            b->click();
            return;
        }
    }
    QWidget::keyPressEvent(e);
}

void Sheet::mousePressEvent(QMouseEvent* e)
{
    e->accept(); // the scrim swallows clicks
}

void Sheet::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    // Hidden itself (hide(), setVisible(false)), not with its window: it no
    // longer counts as open and a waiting exec() returns (the safe answer).
    if (!isHidden())
        return;
    if (m_counted) {
        m_counted = false;
        --g_openSheets;
    }
    if (m_loop && !m_answered)
        m_loop->exit(-1);
}

bool Sheet::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
        relayout();
    if (isVisible() && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            close(-1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool Sheet::focusNextPrevChild(bool next)
{
    // Keep Tab inside the panel.
    QWidget* cur = QApplication::focusWidget();
    QList<QWidget*> order;
    for (QWidget* w : m_panel->findChildren<QWidget*>())
        if (w->focusPolicy() & Qt::TabFocus && w->isVisibleTo(m_panel) && w->isEnabled())
            order << w;
    if (order.isEmpty())
        return true;
    int i = order.indexOf(cur);
    i = i < 0 ? 0 : (i + (next ? 1 : order.size() - 1)) % order.size();
    order[i]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
}

bool Sheet::confirm(QWidget* host, const QString& title, const QString& text, const QString& safeText,
                    const QString& actionText, bool destructive, Icon icon, const QString& checkboxText,
                    bool* checkboxValue, Qt::TextFormat format)
{
    auto* s = new Sheet(host, title, icon);
    if (format == Qt::AutoText)
        s->addText(text);
    else
        s->addBodyLabel(text, format == Qt::RichText ? Qt::RichText : Qt::PlainText);
    QPointer<QCheckBox> cb; // gone with the sheet when its host is destroyed during exec()
    if (!checkboxText.isEmpty())
        cb = s->addCheckbox(checkboxText);
    s->addButton(safeText, Role::Safe);
    s->addButton(actionText, destructive ? Role::Destructive : Role::Primary);
    const int r = s->exec();
    if (cb && checkboxValue)
        *checkboxValue = cb->isChecked();
    return r == 1;
}

void Sheet::message(QWidget* host, const QString& title, const QString& text, Icon icon, const QString& buttonText,
                    Qt::TextFormat format)
{
    auto* s = new Sheet(host, title, icon);
    if (format == Qt::AutoText)
        s->addText(text);
    else
        s->addBodyLabel(text, format == Qt::RichText ? Qt::RichText : Qt::PlainText);
    s->addButton(buttonText, Role::Primary);
    s->exec();
}

Sheet* Sheet::notify(QWidget* host, const QString& title, const QString& text, Icon icon, const QString& buttonText)
{
    auto* s = new Sheet(host, title, icon);
    s->addPlainText(text);
    s->addButton(buttonText, Role::Primary);
    s->open();
    return s;
}

// ---- toasts --------------------------------------------------------------------------

ToastHost::ToastHost(QWidget* host) : QWidget(host)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setProperty("fwNoRing", true);
    host->installEventFilter(this);
    setGeometry(host->rect());
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
}

bool ToastHost::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        relayout();
    return QWidget::eventFilter(watched, event);
}

void ToastHost::show(Tone tone, const QString& title, const QString& body, const QList<Action>& actions, int lifetimeMs)
{
    auto* t = new QFrame(parentWidget());
    t->setObjectName(QStringLiteral("toast"));
    t->setStyleSheet(QStringLiteral("#toast{background:%1;border:1px solid %2;border-radius:%3px;}")
                         .arg(hex(color::bg_3), hex(color::border_4)).arg(radius::md));
    t->setFixedWidth(size::toast);
    t->setProperty("fwNoRing", true);
    auto* h = hbox(t, 10, QMargins(space::md, space::md, space::md, space::md));
    h->setAlignment(Qt::AlignTop);
    auto* ic = new QLabel(t);
    ic->setFixedSize(size::icon, size::icon);
    const QString iconName = tone == Tone::Info ? QStringLiteral("info") : defaultBadgeIcon(tone);
    ic->setPixmap(iconPixmap(iconName.isEmpty() ? QStringLiteral("info") : iconName, size::icon, toneMark(tone), devicePixelRatioF()));
    h->addWidget(ic, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    auto* tl = label(title, TextStyle::BodyStrong, color::fg_0, t);
    tl->setWordWrap(true);
    col->addWidget(tl);
    if (!body.isEmpty()) {
        auto* bl = label(body, TextStyle::Caption, color::fg_2, t);
        bl->setWordWrap(true);
        col->addWidget(bl);
    }
    if (!actions.isEmpty()) {
        auto* links = new QHBoxLayout;
        links->setSpacing(space::md);
        links->setContentsMargins(0, 6, 0, 0);
        for (const auto& a : actions) {
            auto* l = new Link(a.text, t);
            auto fn = a.fn;
            connect(l, &Link::activated, this, [this, t, fn] {
                // The action may close the window this host belongs to.
                QPointer<ToastHost> self(this);
                if (fn)
                    fn();
                if (self)
                    remove(t);
            });
            links->addWidget(l);
        }
        links->addStretch(1);
        col->addLayout(links);
    }
    h->addLayout(col, 1);
    auto* close = new IconButton(QStringLiteral("close"), QStringLiteral("Dismiss"), IconButton::Size::Tiny, t);
    connect(close, &QToolButton::clicked, this, [this, t] { remove(t); });
    h->addWidget(close, 0, Qt::AlignTop);
    t->setAttribute(Qt::WA_Hover);
    t->adjustSize();
    m_toasts.push_back(t);
    while (m_toasts.size() > 3)
        remove(m_toasts.front());
    t->show();
    relayout();
    // It slides in 12 px from the right while it fades in. The fade is an
    // opacity effect on the toast alone, taken off again when it has arrived.
    if (motion::enabled() && t->isVisible()) {
        auto* fade = new QGraphicsOpacityEffect(t);
        fade->setOpacity(0.0);
        t->setGraphicsEffect(fade);
        t->setProperty(kSlide, 12);
        auto* in = new motion::Value(t, 0.0, motion::base);
        in->onChange([this, t, in] {
            const qreal v = in->value();
            if (auto* fx = qobject_cast<QGraphicsOpacityEffect*>(t->graphicsEffect())) {
                if (v >= 1.0)
                    t->setGraphicsEffect(nullptr);
                else
                    fx->setOpacity(v);
            }
            t->setProperty(kSlide, qRound((1.0 - v) * 12));
            if (parentWidget())
                t->move(parentWidget()->width() - space::lg - size::toast + t->property(kSlide).toInt(), t->y());
        });
        t->move(t->x() + 12, t->y());
        in->animateTo(1.0);
    }

    const int life = lifetimeMs >= 0 ? lifetimeMs : (tone == Tone::Ok || tone == Tone::Info || tone == Tone::Neutral) ? 6000 : 0;
    if (life > 0) {
        auto* timer = new QTimer(t);
        timer->setInterval(life);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, t, timer] {
            if (t->underMouse()) { // hover pauses the timer
                timer->start(1500);
                return;
            }
            remove(t);
        });
        timer->start();
    }
}

void ToastHost::remove(QWidget* toast)
{
    auto it = std::find(m_toasts.begin(), m_toasts.end(), toast);
    if (it == m_toasts.end())
        return;
    m_toasts.erase(it);
    auto* fade = qobject_cast<QGraphicsOpacityEffect*>(toast->graphicsEffect());
    // (Not when it goes before the first frame of its slide-in: nothing shows yet.)
    if (motion::enabled() && toast->isVisible() && (!fade || fade->opacity() > 0.0)) {
        // It fades out (fast) where it is, then the others close the gap. It
        // is out of the stack already and takes no more clicks.
        qDeleteAll(toast->findChildren<motion::Value*>(QString(), Qt::FindDirectChildrenOnly)); // a slide-in still running
        toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        if (!fade) {
            fade = new QGraphicsOpacityEffect(toast);
            toast->setGraphicsEffect(fade);
        }
        auto* out = new motion::Value(toast, fade->opacity(), motion::fast);
        QPointer<ToastHost> self(this);
        out->onChange([self, toast, out, fade] {
            fade->setOpacity(out->value());
            if (out->value() > 0.0)
                return;
            toast->hide();
            toast->deleteLater();
            if (self)
                self->relayout();
        });
        out->animateTo(0.0);
        return;
    }
    toast->hide();
    toast->deleteLater();
    relayout();
}

void ToastHost::clear()
{
    while (!m_toasts.empty())
        remove(m_toasts.front());
}

void ToastHost::relayout()
{
    if (!parentWidget())
        return;
    // A toast never covers an open sheet: its links would act on the page
    // behind a dialog that is still waiting for its answer.
    QWidget* sheet = nullptr;
    for (QObject* o : parentWidget()->children())
        if (auto* s = qobject_cast<Sheet*>(o); s && s->isOpen()) {
            sheet = s;
            break; // the lowest one: children are in stacking order
        }
    int y = parentWidget()->height() - space::lg;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        QWidget* t = *it;
        QLayout* l = t->layout();
        const int h = l && l->hasHeightForWidth() ? l->totalHeightForWidth(size::toast) : t->sizeHint().height();
        y -= h;
        // kSlide: how far a toast that is still sliding in is from its place.
        t->setGeometry(parentWidget()->width() - space::lg - size::toast + t->property(kSlide).toInt(), y, size::toast, h);
        if (sheet)
            t->stackUnder(sheet);
        else
            t->raise();
        y -= space::sm;
    }
}

} // namespace fw::ui
