#include "app/pages/ps5/ConsoleHero.h"

#include "app/AppContext.h"
#include "app/pages/ps5/FindConsole.h"
#include "app/ui/Icons.h"

#include <QGridLayout>
#include <QIntValidator>
#include <QPainter>
#include <QPainterPath>

namespace fw::pages {

using namespace ui;
using namespace theme;

// ---- ServiceChip -----------------------------------------------------------------------

ServiceChip::ServiceChip(const QString& key, const QString& icon, const QString& name, QWidget* parent)
    : QAbstractButton(parent), m_key(key), m_icon(icon), m_name(name)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAccessibleName(name);
    setAttribute(Qt::WA_Hover);
}

void ServiceChip::setState(Tone tone, const QString& detail, const QString& tooltip)
{
    m_tone = tone;
    m_detail = detail;
    const QString word = tone == Tone::Ok ? QStringLiteral("Running") : tone == Tone::Warn ? QStringLiteral("Not answering")
                       : tone == Tone::Err ? QStringLiteral("Problem") : tone == Tone::Info ? QStringLiteral("Checking") : QStringLiteral("Not checked");
    setToolTip(tooltip.isEmpty() ? QStringLiteral("%1: %2").arg(m_name, word) : QStringLiteral("%1: %2\n%3").arg(m_name, word, tooltip));
    setAccessibleDescription(word + QStringLiteral(" · ") + detail);
    updateGeometry();
    update();
}

QSize ServiceChip::sizeHint() const
{
    const QFontMetrics fn(font::bodyStrong()), fd(font::caption());
    const int w = 12 + 8 + 6 + size::icon + 8 + fn.horizontalAdvance(m_name) + (m_detail.isEmpty() ? 0 : 8 + fd.horizontalAdvance(m_detail)) + 14;
    return {w, 34};
}

void ServiceChip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(m_hover ? color::border_4 : color::border_3, 1));
    p.setBrush(m_hover ? color::bg_4 : color::bg_3);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    // State dot (with a soft halo when running).
    const QColor dot = m_tone == Tone::Neutral ? color::fg_5 : toneMark(m_tone);
    const QPointF c(r.left() + 14, r.center().y());
    if (m_tone == Tone::Ok) {
        QColor halo = dot;
        halo.setAlpha(60);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(c, 6.5, 6.5);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(c, 3.5, 3.5);
    int x = int(r.left()) + 26;
    p.drawPixmap(QRect(x, int(r.center().y()) - size::icon / 2, size::icon, size::icon),
                 iconPixmap(m_icon, size::icon, m_tone == Tone::Neutral ? color::fg_4 : color::fg_2, devicePixelRatioF()));
    x += size::icon + 8;
    p.setFont(font::bodyStrong());
    p.setPen(m_tone == Tone::Neutral ? color::fg_3 : color::fg_1);
    const QFontMetrics fn(font::bodyStrong());
    p.drawText(QRect(x, 0, fn.horizontalAdvance(m_name) + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, m_name);
    x += fn.horizontalAdvance(m_name) + 8;
    if (!m_detail.isEmpty()) {
        p.setFont(font::caption());
        p.setPen(m_tone == Tone::Warn ? color::warn_hi : m_tone == Tone::Err ? color::danger_hi : color::fg_3);
        p.drawText(QRect(x, 0, width() - x - 10, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(font::caption()).elidedText(m_detail, Qt::ElideRight, width() - x - 10));
    }
    if (hasFocus()) {
        p.setPen(QPen(color::focus, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), r.height() / 2 - 1, r.height() / 2 - 1);
    }
}

void ServiceChip::enterEvent(QEnterEvent* e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void ServiceChip::leaveEvent(QEvent* e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

// ---- ConsoleHero ------------------------------------------------------------------------

ConsoleHero::ConsoleHero(AppContext& ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx)
{
    setObjectName(QStringLiteral("ps5ConsoleHero"));
    setAttribute(Qt::WA_StyledBackground, false);
    auto* outer = vbox(this, space::lg, QMargins(space::xl, space::xl, space::xl, space::lg));

    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(space::lg);
    m_grid->setVerticalSpacing(space::md);
    outer->addLayout(m_grid);

    // Identity: tile, name + state, one line of detail.
    m_identity = new QWidget(this);
    auto* ih = hbox(m_identity, space::lg);
    m_tile = new IconTile(QStringLiteral("ps5"), 56, m_identity);
    ih->addWidget(m_tile, 0, Qt::AlignVCenter);
    auto* names = new QWidget(m_identity);
    auto* nl = vbox(names, space::xs);
    auto* titleRow = new QWidget(names);
    auto* th = hbox(titleRow, space::md);
    m_title = label(QStringLiteral("PlayStation 5"), TextStyle::Title, color::fg_0, titleRow);
    th->addWidget(m_title);
    m_badge = new Badge(QStringLiteral("Not connected"), Tone::Neutral, titleRow);
    m_badge->setIconName(QStringLiteral("plug"));
    th->addWidget(m_badge, 0, Qt::AlignVCenter);
    th->addStretch(1);
    nl->addWidget(titleRow);
    m_subtitle = new ElideLabel({}, Qt::ElideRight, names);
    m_subtitle->setFont(font::body());
    setTextColor(m_subtitle, color::fg_3);
    nl->addWidget(m_subtitle);
    ih->addWidget(names, 1);

    // Address, port, find, connect.
    m_controls = new QWidget(this);
    auto* ch = hbox(m_controls, space::sm);
    m_host = textInput(QStringLiteral("192.168.1.40"), true, m_controls);
    m_host->setMinimumWidth(150);
    m_host->setMaximumWidth(200);
    m_host->setAccessibleName(QStringLiteral("Console IP"));
    m_host->setToolTip(QStringLiteral("The console's address (PS5 Settings › Network › View connection status)"));
    m_port = textInput(QStringLiteral("2121"), true, m_controls);
    m_port->setFixedWidth(72);
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setAccessibleName(QStringLiteral("FTP port"));
    m_port->setToolTip(QStringLiteral("FTP port: ftpsrv 2121, etaHEN 1337, zftpd 2120"));
    m_find = new FindConsoleButton(ctx, m_controls);
    m_connect = button(QStringLiteral("Connect"), ButtonVariant::Primary, QStringLiteral("plug"), m_controls);
    m_connect->setMinimumWidth(124);
    ch->addWidget(m_host, 1);
    ch->addWidget(m_port);
    ch->addWidget(m_find);
    ch->addWidget(m_connect);

    // Services.
    auto* rule = hairline(this);
    outer->addWidget(rule);
    auto* services = new QWidget(this);
    auto* sh = hbox(services, space::md);
    auto* overline = label(QStringLiteral("SERVICES"), TextStyle::Overline, color::fg_4, services);
    overline->setContentsMargins(0, 10, 0, 0);
    sh->addWidget(overline, 0, Qt::AlignTop);
    m_chipRow = new QWidget(services);
    auto* flow = new FlowLayout(m_chipRow, space::sm, space::sm);
    const std::tuple<const char*, const char*, const char*> chips[] = {
        {"ftp", "server", "FTP"},
        {"klog", "klog", "Kernel log"},
        {"payload", "rocket", "Payload loader"},
        {"shadowmount", "layers", "ShadowMount+"},
        {"installer", "package", "Installer"},
    };
    for (const auto& [key, icon, name] : chips) {
        auto* c = new ServiceChip(QLatin1String(key), QLatin1String(icon), QString::fromUtf8(name), m_chipRow);
        c->setState(Tone::Neutral, QStringLiteral("—"));
        flow->addWidget(c);
        m_chips.insert(QLatin1String(key), c);
        connect(c, &QAbstractButton::clicked, this, [this, k = QString::fromLatin1(key)] { emit chipClicked(k); });
    }
    sh->addWidget(m_chipRow, 1);
    m_check = new IconButton(QStringLiteral("refresh"), QStringLiteral("Check the services again"), IconButton::Size::Small, services);
    m_check->setToolTip(QStringLiteral("Check the kernel log, installer and ShadowMount+ ports again"));
    connect(m_check, &QToolButton::clicked, this, &ConsoleHero::checkServicesRequested);
    sh->addWidget(m_check, 0, Qt::AlignTop);
    outer->addWidget(services);
    relayout();
}

void ConsoleHero::relayout()
{
    m_grid->removeWidget(m_identity);
    m_grid->removeWidget(m_controls);
    if (m_narrow) {
        m_grid->addWidget(m_identity, 0, 0);
        m_grid->addWidget(m_controls, 1, 0, Qt::AlignLeft);
        m_grid->setColumnStretch(1, 0);
    } else {
        m_grid->addWidget(m_identity, 0, 0);
        m_grid->addWidget(m_controls, 0, 1, Qt::AlignVCenter | Qt::AlignRight);
        m_grid->setColumnStretch(0, 1);
    }
}

void ConsoleHero::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const bool narrow = width() < 960;
    if (narrow != m_narrow) {
        m_narrow = narrow;
        relayout();
    }
}

QSize ConsoleHero::minimumSizeHint() const
{
    return {420, QWidget::minimumSizeHint().height()};
}

void ConsoleHero::setState(State state, const QString& detail)
{
    m_state = state;
    switch (state) {
    case State::Offline:
        m_badge->set(QStringLiteral("Not connected"), Tone::Neutral);
        m_badge->setIconName(QStringLiteral("plug"));
        m_tile->setTone(Tone::Info);
        break;
    case State::Connecting:
        m_badge->set(QStringLiteral("Connecting…"), Tone::Info);
        m_badge->setIconName(QStringLiteral("loader"));
        m_tile->setTone(Tone::Info);
        break;
    // Every state sets its own glyph: the badge keeps the last one it was given.
    case State::Connected:
        m_badge->set(QStringLiteral("Connected"), Tone::Ok);
        m_badge->setIconName(defaultBadgeIcon(Tone::Ok));
        m_tile->setTone(Tone::Ok);
        break;
    case State::Failed:
        m_badge->set(QStringLiteral("Not reachable"), Tone::Err);
        m_badge->setIconName(defaultBadgeIcon(Tone::Err));
        m_tile->setTone(Tone::Err);
        break;
    }
    m_badge->setToolTip(detail);
    // The one button: Connect, Cancel (an attempt can take the whole connect timeout) or Disconnect.
    const bool busy = state == State::Connecting;
    const bool on = state == State::Connected;
    m_connect->setText(on ? QStringLiteral("Disconnect") : busy ? QStringLiteral("Cancel") : QStringLiteral("Connect"));
    setButtonVariant(m_connect, on || busy ? ButtonVariant::Secondary : ButtonVariant::Primary);
    setButtonIcon(m_connect, on ? QStringLiteral("unplug") : busy ? QStringLiteral("close") : QStringLiteral("plug"));
    m_connect->setEnabled(true);
    update();
}

void ConsoleHero::setSubtitle(const QString& text)
{
    m_subtitle->setFullText(text);
    m_subtitle->setToolTip(text);
}

QString ConsoleHero::subtitle() const
{
    return m_subtitle->fullText();
}

void ConsoleHero::setChip(const QString& key, Tone tone, const QString& detail, const QString& tooltip)
{
    if (auto* c = m_chips.value(key))
        c->setState(tone, detail, tooltip);
}

void ConsoleHero::setChecking(bool on)
{
    m_check->setEnabled(!on);
    m_check->setIconName(on ? QStringLiteral("loader") : QStringLiteral("refresh"));
}

void ConsoleHero::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(r, radius::lg, radius::lg);
    p.setClipPath(clip);
    // Deep blue-black with the brand's light behind the console tile.
    QLinearGradient base(r.topLeft(), r.bottomRight());
    base.setColorAt(0, color::console_0);
    base.setColorAt(0.5, color::console_1);
    base.setColorAt(1, color::console_2);
    p.fillRect(r, base);
    auto blob = [&](QPointF c, qreal rad, QColor col) {
        QRadialGradient g(c, rad);
        g.setColorAt(0, col);
        col.setAlpha(0);
        g.setColorAt(1, col);
        p.fillRect(r, g);
    };
    const bool on = m_state == State::Connected;
    blob(QPointF(r.left() + 60, r.top() + 50), 220, on ? withAlpha(color::success, 34) : withAlpha(color::accent, 60));
    blob(QPointF(r.right() - 180, r.top() - 30), 260, withAlpha(color::accent_2, 40));
    // A large, faint console glyph on the right.
    const int gs = int(std::min(r.height() * 1.1, 200.0));
    p.setOpacity(0.06);
    p.drawPixmap(QRect(int(r.right() - gs - 24), int(r.top() - gs * 0.12), gs, gs), iconPixmap(QStringLiteral("ps5"), gs, color::fg_0, devicePixelRatioF()));
    p.setOpacity(1);
    // The signature gradient along the top edge.
    p.fillRect(QRectF(r.left(), r.top(), r.width(), 2), accentGradient(QRectF(r.left(), r.top(), r.width(), 2), on ? 230 : 140));
    p.setClipping(false);
    p.setPen(QPen(withAlpha(color::accent_hi, 46), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, radius::lg, radius::lg);
}

} // namespace fw::pages
