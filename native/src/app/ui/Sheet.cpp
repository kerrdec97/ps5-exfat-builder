#include "app/ui/Sheet.h"
#include "app/i18n/I18n.h"

#include "app/ui/Icons.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace fw::ui {

using namespace theme;

Sheet::Sheet(QWidget* host, const QString& title, Icon icon, int width) : QWidget(host), m_width(width)
{
    setProperty("fwNoRing", true);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    m_panel = new QFrame(this);
    m_panel->setObjectName(QStringLiteral("sheetPanel"));
    m_panel->setStyleSheet(QStringLiteral("#sheetPanel{background:%1;border:1px solid %2;border-radius:%3px;}")
                               .arg(hex(color::bg_2), hex(color::border_3)).arg(radius::md));
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
        titleRow->addWidget(ic, 0, Qt::AlignTop);
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
}

Sheet::~Sheet()
{
    if (m_loop)
        m_loop->exit(m_result);
}

void Sheet::setTitle(const QString& title) { m_title->setText(title); }

QLabel* Sheet::addText(const QString& text)
{
    auto* l = label(text, TextStyle::Body, color::fg_1, m_panel);
    l->setWordWrap(true);
    l->setTextFormat(text.contains(QLatin1String("<span")) ? Qt::RichText : Qt::PlainText);
    m_body->addWidget(l);
    return l;
}

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
    return QStringLiteral("<span style=\"font-family:'%1';font-size:12px\">%2</span>").arg(monoFamily(), text.toHtmlEscaped());
}

void Sheet::relayout()
{
    if (!parentWidget())
        return;
    setGeometry(parentWidget()->rect());
    const int w = std::min(m_width, width() - 2 * space::lg);
    m_panel->setFixedWidth(w);
    m_panel->adjustSize();
    const QSize s = m_panel->sizeHint();
    m_panel->setGeometry((width() - w) / 2, std::max(space::lg, (height() - s.height()) / 2), w, s.height());
}

void Sheet::open()
{
    m_previousFocus = QApplication::focusWidget();
    relayout();
    raise();
    show();
    // Focus the safe action for destructive dialogs, the primary one otherwise.
    QPushButton* focusTarget = nullptr;
    bool destructive = false;
    for (int i = 0; i < m_roles.size(); ++i)
        if (m_roles[i] == Role::Destructive)
            destructive = true;
    for (int i = 0; i < m_roles.size(); ++i) {
        if (destructive && m_roles[i] == Role::Safe) { focusTarget = m_buttonList[i]; break; }
        if (!destructive && m_roles[i] == Role::Primary) { focusTarget = m_buttonList[i]; break; }
    }
    if (!focusTarget && !m_buttonList.isEmpty())
        focusTarget = m_buttonList.last();
    if (focusTarget) {
        window()->setAttribute(Qt::WA_KeyboardFocusChange, true);
        focusTarget->setFocus(Qt::TabFocusReason);
    } else {
        setFocus();
    }
}

int Sheet::exec()
{
    open();
    QEventLoop loop;
    m_loop = &loop;
    const int r = loop.exec(QEventLoop::DialogExec);
    m_loop = nullptr;
    return r;
}

void Sheet::close(int result)
{
    if (result < 0) {
        for (int i = 0; i < m_roles.size(); ++i)
            if (m_roles[i] == Role::Safe) {
                result = i;
                break;
            }
    }
    m_result = result;
    hide();
    if (m_previousFocus)
        m_previousFocus->setFocus(Qt::OtherFocusReason);
    if (m_loop)
        m_loop->exit(result);
    deleteLater();
}

void Sheet::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), color::scrim);
}

void Sheet::resizeEvent(QResizeEvent*) { relayout(); }

void Sheet::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        close(-1);
        return;
    }
    QWidget::keyPressEvent(e);
}

void Sheet::mousePressEvent(QMouseEvent* e)
{
    e->accept(); // the scrim swallows clicks
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
                    bool* checkboxValue)
{
    auto* s = new Sheet(host, title, icon);
    s->addText(text);
    QCheckBox* cb = nullptr;
    if (!checkboxText.isEmpty())
        cb = s->addCheckbox(checkboxText);
    s->addButton(safeText, Role::Safe);
    s->addButton(actionText, destructive ? Role::Destructive : Role::Primary);
    const int r = s->exec();
    if (cb && checkboxValue)
        *checkboxValue = cb->isChecked();
    return r == 1;
}

void Sheet::message(QWidget* host, const QString& title, const QString& text, Icon icon, const QString& buttonText)
{
    auto* s = new Sheet(host, title, icon);
    s->addText(text);
    s->addButton(buttonText, Role::Primary);
    s->exec();
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
                         .arg(hex(color::bg_3), hex(color::border_3)).arg(radius::md));
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
                if (fn)
                    fn();
                remove(t);
            });
            links->addWidget(l);
        }
        links->addStretch(1);
        col->addLayout(links);
    }
    h->addLayout(col, 1);
    auto* close = new IconButton(QStringLiteral("close"), tr("Dismiss"), IconButton::Size::Tiny, t);
    connect(close, &QToolButton::clicked, this, [this, t] { remove(t); });
    h->addWidget(close, 0, Qt::AlignTop);
    t->setAttribute(Qt::WA_Hover);
    t->adjustSize();
    m_toasts.push_back(t);
    while (m_toasts.size() > 3)
        remove(m_toasts.front());
    t->show();
    t->raise();
    relayout();

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
    int y = parentWidget()->height() - space::lg;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        QWidget* t = *it;
        t->adjustSize();
        const int h = t->sizeHint().height();
        y -= h;
        t->setGeometry(parentWidget()->width() - space::lg - size::toast, y, size::toast, h);
        t->raise();
        y -= space::sm;
    }
}

} // namespace fw::ui
