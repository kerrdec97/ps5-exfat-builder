#include "app/ui/Widgets.h"

#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "core/Tr.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
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
    auto* l = new QLabel(parent);
    l->setTextFormat(Qt::PlainText);
    l->setText(text);
    l->setFont(fontFor(style));
    setTextColor(l, color);
    l->setTextInteractionFlags(Qt::NoTextInteraction);
    return l;
}

QLabel* richLabel(const QString& html, TextStyle style, const QColor& color, QWidget* parent)
{
    QLabel* l = label({}, style, color, parent);
    l->setTextFormat(Qt::RichText);
    l->setText(html);
    return l;
}

QString plainTip(const QString& text)
{
    if (text.isEmpty())
        return text;
    return QStringLiteral("<p style='white-space:pre-wrap'>%1</p>").arg(text.toHtmlEscaped());
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
    QLabel* l = label(text, TextStyle::Mono, color, parent);
    keepLeftToRight(l);
    return l;
}

QLabel* heading(const QString& text, TextStyle style, QWidget* parent)
{
    return label(text, style, color::fg_0, parent);
}

namespace {
class WrappedHeightKeeper final : public QObject {
    bool m_pending = false;

public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if ((e->type() == QEvent::Resize || e->type() == QEvent::FontChange) && !m_pending) {
            // After the layout pass that sent this event: a minimum set while
            // the layout is being applied is not acted on.
            m_pending = true;
            QPointer<QLabel> l = static_cast<QLabel*>(watched);
            QTimer::singleShot(0, this, [this, l] {
                m_pending = false;
                if (!l)
                    return;
                const int h = l->text().isEmpty() ? 0 : std::max(0, l->heightForWidth(l->width()));
                if (h != l->minimumHeight())
                    l->setMinimumHeight(h); // only the height: the width, and so this value, stays as it is
            });
        }
        return false;
    }
};
} // namespace

QPoint menuAnchor(const QWidget* button, int gap)
{
    return button->mapToGlobal(QPoint(button->isRightToLeft() ? button->width() : 0, button->height() + gap));
}

void keepWrappedHeight(QLabel* label)
{
    if (!label)
        return;
    label->setWordWrap(true);
    label->installEventFilter(new WrappedHeightKeeper(label));
}

ElideLabel::ElideLabel(const QString& text, Qt::TextElideMode mode, QWidget* parent)
    : QLabel(parent), m_text(text), m_mode(mode)
{
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::NoTextInteraction);
    relayout();
}

void ElideLabel::setFullText(const QString& text)
{
    m_text = text;
    relayout();
}

void ElideLabel::setMaxLines(int lines)
{
    lines = std::max(1, lines);
    if (lines == m_maxLines)
        return;
    m_maxLines = lines;
    relayout();
}

namespace {
// `text` in at most `maxLines` lines of `width` px, broken where it may wrap
// (the lines joined with "\n", so the label shows exactly these); the last
// line ends in "…" when the text needs more. `cut`: it did.
QString wrapToLines(const QString& text, const QFont& font, int width, int maxLines, bool* cut)
{
    *cut = false;
    if (text.isEmpty())
        return text;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    QStringList lines;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        const int start = line.textStart(), length = line.textLength();
        if (int(lines.size()) == maxLines - 1 && start + length < text.size()) {
            const QString rest = text.mid(start);
            const QString last = QFontMetrics(font).elidedText(rest, Qt::ElideRight, width);
            *cut = last != rest;
            lines << last;
            break;
        }
        QString l = text.mid(start, length);
        while (l.endsWith(u' '))
            l.chop(1);
        lines << l;
    }
    layout.endLayout();
    return lines.join(u'\n');
}
} // namespace

QSize ElideLabel::minimumSizeHint() const
{
    return {24, QLabel::minimumSizeHint().height()};
}

void ElideLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    relayout();
}

void ElideLabel::changeEvent(QEvent* e)
{
    QLabel::changeEvent(e);
    if (e->type() == QEvent::FontChange)
        relayout(); // the text was cut for the old font
}

void ElideLabel::relayout()
{
    const QFontMetrics fm(font());
    const int w = std::max(0, width() - 2);
    bool cut = false;
    QString shown;
    if (m_maxLines > 1 && w > 10) { // not before the label has a width
        shown = wrapToLines(m_text, font(), w, m_maxLines, &cut);
    } else {
        shown = fm.elidedText(m_text, m_mode, w);
        cut = shown != m_text;
    }
    QLabel::setText(shown);
    // The whole text while it is cut, unless the owner gave the label a
    // tooltip of its own (a path with more to say than the label shows).
    if (toolTip() == m_autoTip) {
        m_autoTip = cut ? plainTip(m_text) : QString();
        setToolTip(m_autoTip);
    }
    // Assistive tools read the whole text, not the cut one (unless the owner
    // gave the label a name of its own).
    if (accessibleName() == m_autoName && m_autoName != m_text) {
        m_autoName = m_text;
        setAccessibleName(m_text);
    }
}

Link::Link(const QString& text, QWidget* parent) : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setText(text);
    setFont(font::body());
    setTextColor(this, color::accent_hi);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setProperty("fwRadius", 2);
}

void Link::setText(const QString& text)
{
    QLabel::setText(text);
    setAccessibleName(text);
}

void Link::mouseReleaseEvent(QMouseEvent* e)
{
    // The slot may delete this link (an info bar clears its links): nothing
    // touches it after the signal.
    const bool hit = e->button() == Qt::LeftButton && rect().contains(e->pos());
    QLabel::mouseReleaseEvent(e);
    if (hit) {
        e->accept(); // handled: Qt does not go on to the parent of a link that may be gone
        emit activated();
    }
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
    // Filled by the stylesheet (QFrame[role="hairline"]): the universal
    // `background: transparent` rule overrides a palette fill.
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::NoFrame);
    f->setFixedHeight(1);
    f->setProperty("role", strong ? QStringLiteral("hairline2") : QStringLiteral("hairline"));
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

// The application style sheet remembers the font a widget had when it was
// first polished and puts it back on every later polish (a repolish, the
// polish when it is first shown, a new parent); QWidget::setFont() does not
// tell it. A widget whose font follows its state (a button's variant, a field
// that turns mono for a value) has the record kept in step, so that any
// polish keeps the font it has now.
static void keepFont(QWidget* w, const QFont& f)
{
    static const char kRemembered[] = "_q_styleSheetWidgetFont"; // QStyleSheetStyle's record
    w->setFont(f);
    if (w->property(kRemembered).isValid())
        w->setProperty(kRemembered, f);
}

void repolish(QWidget* w)
{
    const bool ownFont = w->testAttribute(Qt::WA_SetFont);
    const QFont f = w->font();
    w->style()->unpolish(w);
    w->style()->polish(w);
    if (ownFont && w->font() != f)
        keepFont(w, f); // a font set since the first polish
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
    const QString name = QString::fromLatin1(variantName(variant));
    if (b->property("variant").toString() == name)
        return; // unchanged: no repolish (views call this on every progress tick)
    b->setProperty("variant", name);
    const bool filled = variant == ButtonVariant::Primary || variant == ButtonVariant::Danger;
    if (!b->property("fwIcon").toString().isEmpty())
        setButtonIcon(b, b->property("fwIcon").toString(), filled ? color::on_accent : color::fg_3);
    repolish(b);
    // After the restyle, and remembered by the style sheet: a later polish
    // would otherwise put back the font the button was made with (one made
    // Primary stayed bold as a Secondary).
    keepFont(b, filled ? font::bodyStrong() : font::body());
}

void setButtonSmall(QPushButton* b, bool small)
{
    b->setProperty("size", small ? QStringLiteral("sm") : QString());
    repolish(b);
}

void setButtonIcon(QPushButton* b, const QString& iconName, const QColor& c)
{
    if (!c.isValid() && b->property("fwIcon").toString() == iconName && (iconName.isEmpty() || !b->icon().isNull()))
        return; // the same icon for the same variant
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
    if (m_titleRow) { // called again: the same row, not a second title
        m_title->setText(title);
        if (rightSlot)
            m_titleRow->addWidget(rightSlot);
        return m_titleRow;
    }
    auto* row = new QHBoxLayout;
    m_titleRow = row;
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

QColor blend(const QColor& a, const QColor& b, qreal t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (t <= 0.0)
        return a;
    if (t >= 1.0)
        return b;
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

QRect mirrored(const QWidget* w, const QRect& r)
{
    if (!w || !w->isRightToLeft())
        return r;
    return QRect(w->width() - r.x() - r.width(), r.y(), r.width(), r.height());
}

QRectF mirrored(const QWidget* w, const QRectF& r)
{
    if (!w || !w->isRightToLeft())
        return r;
    return QRectF(w->width() - r.x() - r.width(), r.y(), r.width(), r.height());
}

QPoint mirrored(const QWidget* w, const QPoint& pt)
{
    if (!w || !w->isRightToLeft())
        return pt;
    return QPoint(w->width() - 1 - pt.x(), pt.y());
}

QRect mirrored(const QLayout* layout, const QRect& area, const QRect& r)
{
    const QWidget* w = layout ? layout->parentWidget() : nullptr;
    return QStyle::visualRect(w ? w->layoutDirection() : QGuiApplication::layoutDirection(), area, r);
}

int leftToRightText(const QWidget* w, Qt::Alignment align)
{
    const Qt::LayoutDirection dir = w ? w->layoutDirection() : QGuiApplication::layoutDirection();
    return int(QStyle::visualAlignment(dir, align)) | Qt::TextForceLeftToRight;
}

namespace {
class LeftToRightCellDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // Placed as the view's direction says, read left to right.
        option->displayAlignment = QStyle::visualAlignment(option->direction, option->displayAlignment);
        option->direction = Qt::LeftToRight;
    }
};
} // namespace

void keepColumnLeftToRight(QAbstractItemView* view, int column)
{
    if (view)
        view->setItemDelegateForColumn(column, new LeftToRightCellDelegate(view));
}

int naturalText(const QWidget* w, Qt::Alignment align, const QString& text)
{
    if (!w || !w->isRightToLeft())
        return int(align);
    return int(QStyle::visualAlignment(Qt::RightToLeft, align))
         | (text.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight);
}

QString bidiSafe(const QString& text)
{
    constexpr QChar kLrm(0x200E);
    auto isSign = [](QChar c) { return c == u'+' || c == u'#'; };
    auto isLatin = [](QChar c) { return c.isDigit() || (c.isLetter() && c.script() == QChar::Script_Latin); };
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        out += text.at(i);
        // The last sign of a run ("++") right after a Latin letter or digit.
        if (!isSign(text.at(i)) || (i + 1 < text.size() && isSign(text.at(i + 1))))
            continue;
        qsizetype start = i;
        while (start > 0 && isSign(text.at(start - 1)))
            --start;
        if (start == 0 || !isLatin(text.at(start - 1)))
            continue;
        // Followed by Latin text, or marked already: it is in a Latin run as it is.
        if (i + 1 < text.size() && (isLatin(text.at(i + 1)) || text.at(i + 1) == kLrm))
            continue;
        out += kLrm;
    }
    return out;
}

namespace {

// An item's text in a right-to-left view (ItemDelegate).
QString rightToLeftItemText(const QString& text)
{
    if (text.isEmpty())
        return text;
    const bool hasRightToLeft = std::any_of(text.cbegin(), text.cend(), [](QChar c) {
        switch (c.direction()) {
        case QChar::DirR:
        case QChar::DirAL:
        case QChar::DirRLE:
        case QChar::DirRLO:
        case QChar::DirRLI: return true;
        default: return false;
        }
    });
    if (hasRightToLeft)
        return bidiSafe(text);
    // Between two left-to-right marks nothing takes the paragraph's direction:
    // a full stop, a bracket or a sign at either end stays where it is read.
    constexpr QChar kLrm(0x200E);
    QString out = text;
    if (out.front() != kLrm)
        out.prepend(kLrm);
    if (out.size() < 2 || out.back() != kLrm)
        out.append(kLrm);
    return out;
}

} // namespace

void ItemDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    if (option->direction == Qt::RightToLeft)
        option->text = rightToLeftItemText(option->text);
}

void mirrorPainter(QPainter& p, const QWidget* w)
{
    if (!w || !w->isRightToLeft())
        return;
    p.translate(w->width(), 0);
    p.scale(-1, 1);
}

void keepLeftToRight(QWidget* w)
{
    if (!w)
        return;
    // A label keeps its place in the row (right to left it starts at the
    // right); only its text is read left to right (FwStyle::drawItemText).
    // A text box's placeholder is not one of its lines: Qt paints it in the
    // application's direction, so it still reads right to left in a
    // right-to-left language.
    if (qobject_cast<QLabel*>(w))
        w->setProperty("fwLtrText", true);
    else
        w->setLayoutDirection(Qt::LeftToRight);
}

void paintPlate(QPainter& p, const QRectF& r, qreal rad, qreal hover)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QColor base = blend(color::bg_2, color::bg_3, hover);
    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0, blend(color::bg_2_hi, color::bg_3_hi, hover));
    fill.setColorAt(0.4, base);
    fill.setColorAt(1, base);
    p.setPen(QPen(blend(color::border_2, color::border_4, hover), 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, rad, rad);
    // The lit top edge: one pixel inside the outline, fading out down the corners.
    QLinearGradient lit(QPointF(0, r.top() + 1), QPointF(0, r.top() + 1 + std::max<qreal>(rad, 2)));
    lit.setColorAt(0, color::highlight);
    lit.setColorAt(1, withAlpha(color::highlight, 0));
    p.setPen(QPen(QBrush(lit), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), std::max<qreal>(rad - 1, 0), std::max<qreal>(rad - 1, 0));
    p.restore();
}

void paintWell(QPainter& p, const QRectF& r, qreal rad)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color::border_1, 1));
    p.setBrush(color::field_bg);
    p.drawRoundedRect(r, rad, rad);
    p.restore();
}

void paintPopup(QPainter& p, const QRectF& r, qreal rad)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color::border_4, 1));
    p.setBrush(color::bg_3);
    p.drawRoundedRect(r, rad, rad);
    p.restore();
}

void paintShadow(QPainter& p, const QRectF& r, qreal rad, int spread)
{
    if (spread <= 0)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    // Rings that fade outwards (no blur): each is one pixel of the falloff.
    const QRectF base = r.translated(0, spread / 3.0);
    for (int i = 0; i < spread; ++i) {
        const qreal t = 1.0 - qreal(i) / spread;
        p.setPen(QPen(withAlpha(color::shadow, int(70 * t * t)), 1.5));
        p.drawRoundedRect(base.adjusted(-i, -i, i, i), rad + i, rad + i);
    }
    p.restore();
}

// ---- cover placeholder -----------------------------------------------------------

int coverIndex(const QString& id)
{
    // FNV-1a over the ID's UTF-16 units: the same answer on every run, machine
    // and Qt version (qHash is seeded per process and may change between versions).
    quint32 h = 2166136261u;
    for (const QChar c : id.trimmed().toUpper()) {
        h ^= c.unicode();
        h *= 16777619u;
    }
    return int(h % quint32(cover::count));
}

QString coverInitials(const QString& title)
{
    QString out;
    static const QRegularExpression separators(QStringLiteral("[\\s:\\-_]+")); // once, not per cover per paint
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

void paintCoverPlaceholder(QPainter& p, const QRectF& r, const QString& title, const QString& id, qreal rad)
{
    if (r.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(cover::gradient(coverIndex(id.isEmpty() ? title : id)));
    if (rad > 0)
        p.drawRoundedRect(r, rad, rad);
    else
        p.drawRect(r);
    // A faint sheen over the top third: it ends above the initials.
    QLinearGradient sheen(r.topLeft(), QPointF(r.left(), r.top() + r.height() * 0.34));
    sheen.setColorAt(0, withAlpha(color::on_accent, 24));
    sheen.setColorAt(1, withAlpha(color::on_accent, 0));
    p.setBrush(sheen);
    if (rad > 0)
        p.drawRoundedRect(r, rad, rad);
    else
        p.drawRect(r);
    const qreal side = std::min(r.width(), r.height());
    QFont f = font::display();
    f.setPixelSize(std::max(9, qRound(side * (side >= 96 ? 0.30 : 0.38))));
    p.setFont(f);
    p.setPen(color::on_accent);
    p.drawText(r, Qt::AlignCenter, coverInitials(title));
    p.restore();
}

void Card::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_variant == Variant::Inset) {
        paintWell(p, r, radius::control + 2);
    } else {
        paintPlate(p, r, radius::md);
        if (m_selected) {
            p.setBrush(accentGradient(r, 26));
            p.setPen(QPen(withAlpha(color::accent_hi, 90), 1));
            p.drawRoundedRect(r, radius::md, radius::md);
        }
    }
    if (m_drop) {
        QPen pen(color::accent_hi, 2, Qt::DashLine);
        pen.setDashPattern({4, 3});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(4, 4, -4, -4), radius::md, radius::md);
    }
}

// ---- badge ---------------------------------------------------------------------

// Tells assistive tools that the text a self-painted control stands for changed.
static void notifyAccessibleNameChanged(QWidget* w)
{
#if QT_CONFIG(accessibility)
    if (QAccessible::isActive()) {
        QAccessibleEvent ev(w, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&ev);
    }
#else
    Q_UNUSED(w);
#endif
}

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

// The colour a tone's tints and outlines are made of.
static QColor toneBase(Tone tone)
{
    return tone == Tone::Info ? color::accent : toneMark(tone);
}

QColor toneLine(Tone tone)
{
    return tone == Tone::Neutral ? color::border_3 : withAlpha(toneBase(tone), 89); // 35 %
}

namespace {
constexpr int kBadgePad = 8, kCountPad = 6, kBadgeIconGap = 4;
}

int badgeWidth(const QString& text, bool withIcon, bool count)
{
    int w = (count ? kCountPad : kBadgePad) * 2 + QFontMetrics(font::chip()).horizontalAdvance(text);
    if (withIcon && !count)
        w += size::iconSm + kBadgeIconGap;
    return count ? std::max(w, int(size::badgeCount)) : w;
}

void paintBadge(QPainter& p, const QRectF& rect, const QString& shownText, Tone tone, const QString& iconName, bool onAccent)
{
    const QString text = p.layoutDirection() == Qt::RightToLeft ? bidiSafe(shownText) : shownText;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fg = toneText(tone);
    if (onAccent) {
        p.setPen(Qt::NoPen);
        p.setBrush(accentFillGradient(rect));
        fg = color::on_accent;
        p.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
    } else {
        p.setPen(QPen(toneLine(tone), 1));
        p.setBrush(tone == Tone::Neutral ? color::bg_4 : withAlpha(toneBase(tone), 41)); // 16 %
        p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    }
    const bool count = rect.height() < size::badge;
    const qreal pad = count ? kCountPad : kBadgePad;
    p.setFont(font::chip());
    if (!iconName.isEmpty()) {
        const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
        // Right to left (a widget's painter) the glyph is at the right end.
        const bool rtl = p.layoutDirection() == Qt::RightToLeft;
        const int iy = int(rect.top() + (rect.height() - size::iconSm) / 2.0);
        const QRect ir(rtl ? int(rect.right() - pad) - size::iconSm : int(rect.left() + pad), iy, size::iconSm, size::iconSm);
        p.drawPixmap(ir, iconPixmap(iconName, size::iconSm, tone == Tone::Neutral && !onAccent ? color::fg_3 : fg, dpr));
        p.setPen(fg);
        if (rtl) {
            const qreal x = ir.left() - kBadgeIconGap;
            p.drawText(QRectF(rect.left(), rect.top(), x - rect.left(), rect.height()),
                       Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute
                           | (text.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight), // a name, "+2": its own direction
                       text);
        } else {
            const qreal x = ir.right() + 1 + kBadgeIconGap;
            p.drawText(QRectF(x, rect.top(), rect.right() - x, rect.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
    } else {
        p.setPen(fg);
        int flags = Qt::AlignCenter;
        if (p.layoutDirection() == Qt::RightToLeft) // "+2", "PPSA…": read in their own direction
            flags |= text.isRightToLeft() ? Qt::TextForceRightToLeft : Qt::TextForceLeftToRight;
        p.drawText(rect, flags, text);
    }
    p.restore();
}

Badge::Badge(const QString& text, Tone tone, QWidget* parent) : QWidget(parent), m_text(text), m_tone(tone)
{
    setFont(font::chip());
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setProperty("fwNoRing", true);
}

void Badge::set(const QString& text, Tone tone)
{
    if (m_text == text && m_tone == tone)
        return;
    // The width depends on the text and on whether the tone has a glyph.
    const bool resize = m_text != text || (!m_iconSet && defaultBadgeIcon(m_tone).isEmpty() != defaultBadgeIcon(tone).isEmpty());
    m_text = text;
    m_tone = tone;
    updateCut();
    if (resize)
        updateGeometry();
    update();
    notifyAccessibleNameChanged(this);
}

// A badge lets clicks through to what it sits on; only one whose text is cut
// (a long text, or a badge squeezed below its width in a narrow table cell)
// takes mouse events itself, so that its tooltip can show.
QString Badge::shortText() const
{
    return m_maxChars > 0 ? elideMiddle(m_text, m_maxChars) : m_text;
}

void Badge::setMaxLength(int chars)
{
    if (m_maxChars == chars)
        return;
    m_maxChars = chars;
    updateCut();
    updateGeometry();
    update();
}

void Badge::updateCut()
{
    const bool cut = shortText() != m_text || (isVisible() && width() < sizeHint().width());
    const QString tip = cut ? plainTip(m_text) : QString();
    if (toolTip() != tip)
        setToolTip(tip);
    setAttribute(Qt::WA_TransparentForMouseEvents, !cut);
}

void Badge::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    updateCut();
}

void Badge::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    updateCut();
}

void Badge::setText(const QString& text) { set(text, m_tone); }
void Badge::setTone(Tone tone) { set(m_text, tone); }

void Badge::setIconName(const QString& iconName)
{
    if (m_iconSet && m_icon == iconName)
        return;
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
    return {badgeWidth(shortText(), !iconName.isEmpty(), m_count), m_count ? size::badgeCount : size::badge};
}

void Badge::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QString iconName = m_count ? QString() : (m_iconSet ? m_icon : defaultBadgeIcon(m_tone));
    QString text = shortText();
    // Narrower than it asks to be (a table column too narrow for a long
    // translation): the end gives way to "…" inside the outline, instead of
    // the last letters being cut by it. The tooltip has the whole text.
    if (width() < badgeWidth(text, !iconName.isEmpty(), m_count)) {
        const int room = width() - 2 * (m_count ? kCountPad : kBadgePad) - (iconName.isEmpty() ? 0 : size::iconSm + kBadgeIconGap);
        text = QFontMetrics(font::chip()).elidedText(text, Qt::ElideRight, std::max(0, room));
    }
    paintBadge(p, QRectF(rect()), text, m_tone, iconName);
}

// ---- switch -------------------------------------------------------------------

Switch::Switch(QWidget* parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setFixedSize(size::switchW, size::switchH);
    setFocusPolicy(Qt::TabFocus);
    setProperty("fwRadius", radius::pill);
    setAttribute(Qt::WA_Hover);
    m_position = new motion::Value(this, 0.0, motion::base);
    connect(this, &QAbstractButton::toggled, this, [this](bool on) { m_position->animateTo(on ? 1.0 : 0.0); });
}

qreal Switch::position() const { return m_position->value(); }
bool Switch::isAnimating() const { return m_position->isRunning(); }

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
    mirrorPainter(p, this); // right to left, as Windows does it: on is the knob at the left
    const qreal t = m_position->value(); // 0 = off .. 1 = on
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal rad = r.height() / 2;
    QColor knob;
    if (!isEnabled()) {
        p.setPen(QPen(color::border_3, 1));
        p.setBrush(color::bg_2);
        p.drawRoundedRect(r, rad, rad);
        knob = color::fg_5;
    } else {
        // The off track with the on track over it: they cross-fade while the knob slides.
        if (t < 1.0) {
            p.setPen(QPen(m_hover ? color::fg_5 : color::border_control, 1));
            p.setBrush(color::field_bg);
            p.drawRoundedRect(r, rad, rad);
        }
        if (t > 0.0) {
            p.setOpacity(t);
            p.setPen(Qt::NoPen);
            p.setBrush(accentFillGradient(r, m_hover ? 235 : 255));
            p.drawRoundedRect(r, rad, rad);
            p.setOpacity(1.0);
        }
        knob = blend(color::fg_3, color::on_accent, t);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    const qreal off = 3, on = width() - 3 - size::switchKnob;
    p.drawEllipse(QRectF(off + (on - off) * t, (height() - size::switchKnob) / 2.0, size::switchKnob, size::switchKnob));
}

// ---- segmented ----------------------------------------------------------------

Segmented::Segmented(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFont(font::body());
    m_hover = new motion::HoverFade(this);
    m_slide = new motion::Value(this, 1.0, motion::base);
}

bool Segmented::isAnimating() const { return m_slide->isRunning(); }

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
    if (index < 0 || index >= count() || m_segments[size_t(index)].text == text)
        return;
    m_segments[size_t(index)].text = text;
    updateGeometry();
    update();
}

QString Segmented::segmentText(int index) const
{
    return index >= 0 && index < count() ? m_segments[size_t(index)].text : QString();
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
    // The pill slides from where it is now (maybe half way) to the new segment.
    const QRectF target = segmentRect(m_current);
    const qreal t = m_slide->value();
    m_slideFrom = t >= 1.0 ? target
                           : QRectF(m_slideFrom.x() + (target.x() - m_slideFrom.x()) * t, target.y(),
                                    m_slideFrom.width() + (target.width() - m_slideFrom.width()) * t, target.height());
    m_current = index;
    m_slide->jumpTo(0.0);
    m_slide->animateTo(1.0);
    update();
#if QT_CONFIG(accessibility)
    if (QAccessible::isActive()) {
        QAccessibleValueChangeEvent ev(this, segmentText(index));
        QAccessible::updateAccessibility(&ev);
    }
#endif
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
    return {w, 34};
}

QRectF Segmented::segmentRect(int index) const
{
    if (index < 0 || index >= count())
        return {};
    int x = 3;
    for (int i = 0; i < index; ++i)
        x += segmentWidth(i) + 2;
    return mirrored(this, QRectF(x, 3, segmentWidth(index), height() - 6)); // right to left the first is at the right
}

int Segmented::segmentAt(const QPoint& shown) const
{
    const QPoint pt = mirrored(this, shown);
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
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::control + 1, radius::control + 1);
    // The selection pill, on its way to the current segment or on it.
    if (m_current >= 0 && m_current < count()) {
        const QRectF to = segmentRect(m_current);
        const qreal t = m_slide->value();
        const QRectF pill = t >= 1.0 ? to
                                     : QRectF(m_slideFrom.x() + (to.x() - m_slideFrom.x()) * t, to.y(),
                                              m_slideFrom.width() + (to.width() - m_slideFrom.width()) * t, to.height());
        p.setPen(QPen(withAlpha(color::accent_hi, 80), 1));
        p.setBrush(accentGradient(pill, 70));
        p.drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), radius::sm, radius::sm);
    }
    int x = 3;
    for (int i = 0; i < count(); ++i) {
        const auto& s = m_segments[size_t(i)];
        const int w = segmentWidth(i);
        const QRect r = mirrored(this, QRect(x, 3, w, height() - 6));
        const bool sel = i == m_current;
        const qreal hov = s.enabled && !sel ? m_hover->amount(i) : 0.0;
        if (hov > 0.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(color::bg_3, int(255 * hov)));
            p.drawRoundedRect(r, radius::sm, radius::sm);
        }
        const QColor fg = !s.enabled ? color::fg_5 : sel ? color::fg_0 : blend(color::fg_3, color::fg_1, hov);
        p.setPen(fg);
        p.setFont(sel ? font::bodyStrong() : font::body());
        int tx = x + 10;
        if (!s.icon.isEmpty()) {
            p.drawPixmap(mirrored(this, QRect(tx, r.center().y() - size::icon / 2, size::icon, size::icon)),
                         iconPixmap(s.icon, size::icon, fg, devicePixelRatioF()));
            tx += size::icon + 6;
        }
        p.drawText(mirrored(this, QRect(tx, r.y(), w - (tx - x), r.height())), naturalText(this, Qt::AlignVCenter | Qt::AlignLeft, s.text),
                   isRightToLeft() ? bidiSafe(s.text) : s.text);
        x += w + 2;
    }
}

void Segmented::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    const int i = segmentAt(e->pos());
    if (i >= 0 && m_segments[size_t(i)].enabled)
        setCurrentIndex(i);
}

void Segmented::mouseMoveEvent(QMouseEvent* e) { m_hover->setHovered(segmentAt(e->pos())); }

void Segmented::leaveEvent(QEvent*) { m_hover->setHovered(-1); }

void Segmented::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
        int i = m_current;
        const int step = (e->key() == Qt::Key_Left) != isRightToLeft() ? -1 : 1; // the arrow's way on screen
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

namespace {
constexpr int kFrameMs = 16;
constexpr int kSweepPeriodMs = 2400; // one highlight sweep every 2.4 s ...
constexpr int kSweepMs = 900;        // ... that takes 0.9 s to cross the fill
constexpr int kBandPeriodMs = 1400;  // the indeterminate band, edge to edge
} // namespace

ProgressBar::ProgressBar(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(size::progress);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_shown = new motion::Value(this, 0.0, motion::slow);
    connect(&m_timer, &QTimer::timeout, this, &ProgressBar::tick);
}

double ProgressBar::shownFraction() const { return m_shown->value(); }
bool ProgressBar::isAnimating() const { return m_timer.isActive() || m_shown->isRunning(); }

void ProgressBar::setFraction(double f)
{
    m_fraction = std::clamp(f, 0.0, 1.0);
    // Forwards the fill eases; back (another job in the same bar) it jumps.
    if (m_fraction < m_shown->value())
        m_shown->jumpTo(m_fraction);
    else
        m_shown->animateTo(m_fraction);
    updateTimer();
    update();
}

void ProgressBar::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    updateTimer();
    update();
}

// The timer runs only for a visible bar whose job runs: every frame for the
// indeterminate band; for the sweep 0.9 s out of every 2.4 s, with one long
// sleep in between. Never when motion is off.
void ProgressBar::updateTimer()
{
    const bool band = m_state == State::Indeterminate;
    const bool sweep = m_state == State::Determinate && m_fraction > 0.0 && m_fraction < 1.0;
    if ((band || sweep) && isVisible() && motion::enabled()) {
        if (!m_timer.isActive()) {
            m_t0 = QDateTime::currentMSecsSinceEpoch();
            m_timer.start(kFrameMs);
        }
    } else if (m_timer.isActive()) {
        m_timer.stop();
        update(); // without the highlight
    }
}

void ProgressBar::tick()
{
    if (window()->isMinimized()) { // nothing to animate for a minimised window
        m_timer.start(500);
        return;
    }
    if (m_state == State::Determinate) {
        const qint64 t = (QDateTime::currentMSecsSinceEpoch() - m_t0) % kSweepPeriodMs;
        if (t >= kSweepMs) {
            m_timer.start(int(kSweepPeriodMs - t)); // sleep until the next sweep
            update();
            return;
        }
    }
    if (m_timer.interval() != kFrameMs)
        m_timer.start(kFrameMs);
    update();
}

void ProgressBar::showEvent(QShowEvent*) { updateTimer(); }
void ProgressBar::hideEvent(QHideEvent*) { m_timer.stop(); }

void ProgressBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    mirrorPainter(p, this); // right to left the fill grows from the right, band and sweep travel leftwards
    p.setPen(Qt::NoPen);
    p.setBrush(color::bg_5);
    const qreal rad = height() / 2.0;
    p.drawRoundedRect(rect(), rad, rad);
    QColor fill = color::accent;
    double frac = m_shown->value();
    const bool gradient = m_state == State::Determinate || m_state == State::Indeterminate;
    switch (m_state) {
    case State::Done: fill = color::success; frac = 1.0; break;
    case State::Failed: fill = color::danger; break;
    case State::Paused: fill = color::fg_4; break;
    case State::Indeterminate: {
        // A gradient band, 30 % wide, that travels left to right (and stands
        // in the middle while motion is off). It is cut at the track's ends.
        const double segW = width() * 0.30;
        const double t = m_timer.isActive()
                             ? std::fmod((QDateTime::currentMSecsSinceEpoch() - m_t0) / double(kBandPeriodMs), 1.0) : 0.5;
        const double x = -segW + t * (width() + segW);
        const double left = std::max(x, 0.0), right = std::min(x + segW, double(width()));
        if (right - left >= 1.0) {
            p.setBrush(accentGradient(QRectF(x, 0, segW, height())));
            p.drawRoundedRect(QRectF(left, 0, right - left, height()), rad, rad);
        }
        return;
    }
    case State::Determinate: break;
    }
    if (frac > 0) {
        const QRectF fr(0, 0, std::max(double(size::progress), width() * frac), height());
        if (gradient)
            p.setBrush(accentGradient(QRectF(0, 0, width(), height())));
        else
            p.setBrush(fill);
        p.drawRoundedRect(fr, rad, rad);
        // While the job runs: a soft highlight that crosses the filled part.
        if (m_state == State::Determinate && m_timer.isActive()) {
            const qint64 t = (QDateTime::currentMSecsSinceEpoch() - m_t0) % kSweepPeriodMs;
            if (t < kSweepMs) {
                const double bandW = std::max(32.0, fr.width() * 0.35);
                const double x = -bandW + (double(t) / kSweepMs) * (fr.width() + bandW);
                QLinearGradient light(QPointF(x, 0), QPointF(x + bandW, 0));
                light.setColorAt(0, withAlpha(color::on_accent, 0));
                light.setColorAt(0.5, withAlpha(color::on_accent, 64));
                light.setColorAt(1, withAlpha(color::on_accent, 0));
                p.setBrush(light);
                p.drawRoundedRect(fr, rad, rad); // the fill's own shape: nothing spills past its round end
            }
        }
    }
}

ProgressBlock::ProgressBlock(QWidget* parent) : QWidget(parent)
{
    auto* l = vbox(this, space::sm);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::md);
    m_bar = new ProgressBar(this);
    m_percent = monoLabel(thinPercent(0.0), this, color::fg_0);
    m_percent->setMinimumWidth(QFontMetrics(font::mono()).horizontalAdvance(thinPercent(1.0)));
    row->addWidget(m_bar, 1);
    row->addWidget(m_percent);
    l->addLayout(row);
    m_detail = label({}, TextStyle::Caption, color::fg_2, this);
    m_detail->setWordWrap(true);
    l->addWidget(m_detail);
    m_stepper = new StageStepper(this);
    l->addWidget(m_stepper);
    m_stepper->hide();
}

void ProgressBlock::setProgress(double fraction, bool indeterminate, const QString& detail)
{
    m_bar->setFraction(fraction);
    m_indeterminate = indeterminate;
    if (m_bar->state() == ProgressBar::State::Determinate || m_bar->state() == ProgressBar::State::Indeterminate)
        m_bar->setState(indeterminate ? ProgressBar::State::Indeterminate : ProgressBar::State::Determinate);
    updateLabel();
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
}

void ProgressBlock::setState(ProgressBar::State state)
{
    m_bar->setState(state);
    updateLabel();
}

// The text right of the bar, from the bar's state and fraction: the same
// whether setProgress() or setState() was called last.
void ProgressBlock::updateLabel()
{
    const QString pct = thinPercent(m_bar->fraction());
    QString text;
    QColor c = color::fg_0;
    bool plainPercent = false;
    switch (m_bar->state()) {
    case ProgressBar::State::Done:
        text = FW_TR("Done");
        c = color::success_hi;
        break;
    case ProgressBar::State::Failed:
        text = m_indeterminate ? FW_TR("Failed") : FW_TR("Failed at %1").arg(pct);
        c = color::danger_hi;
        break;
    case ProgressBar::State::Paused:
        text = m_indeterminate ? FW_TR("Paused") : FW_TR("Paused · %1").arg(pct);
        c = color::fg_3;
        break;
    case ProgressBar::State::Indeterminate: break;
    case ProgressBar::State::Determinate:
        text = pct;
        plainPercent = true;
        break;
    }
    m_percent->setText(text);
    m_percent->setVisible(!text.isEmpty());
    // A bare percentage keeps the width of "100 %", so the bar does not change length.
    m_percent->setMinimumWidth(plainPercent ? QFontMetrics(font::mono()).horizontalAdvance(thinPercent(1.0)) : 0);
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
    if (names == m_names && current == m_current && failed == m_failed && (m_layout->count() > 0 || names.isEmpty()))
        return; // the dock calls this on every progress tick
    m_names = names;
    m_current = current;
    m_failed = failed;
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
            iconName = failed ? QStringLiteral("error") : QStringLiteral("circle_filled");
            c = failed ? color::danger : color::fg_0;
            f = font::bodyStrong();
            f.setPixelSize(12);
        }
        auto* ic = new QLabel(w);
        ic->setPixmap(iconPixmap(iconName, size::iconSm, c, devicePixelRatioF()));
        ic->setFixedSize(size::iconSm, size::iconSm);
        // Stage names are the engines' English: shown translated.
        auto* t = label(i18n::translate(names.at(i)), TextStyle::Caption, i == current && !failed ? color::fg_0 : c, w);
        t->setFont(f);
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
    m_dismiss = new IconButton(QStringLiteral("close"), FW_TR("Dismiss"), IconButton::Size::Tiny, this);
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
    setTextColor(m_title, tone == Tone::Err ? color::danger_hi : color::fg_0); // whichever of title and tone is set first
    update();
}

void InfoBar::setDismissable(bool on) { m_dismiss->setVisible(on); }

void InfoBar::clearLinks()
{
    // deleteLater: a link's own activated() may be what clears the links.
    while (m_links->count() > 1) {
        QLayoutItem* it = m_links->takeAt(0);
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        }
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
    p.setPen(QPen(toneLine(m_tone), 1));
    p.setBrush(toneTint(m_tone));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
}

// ---- states ---------------------------------------------------------------------

EmptyState::EmptyState(const QString& iconName, const QString& title, const QString& text, QWidget* parent)
    : QWidget(parent)
{
    m_layout = vbox(this, 0, QMargins(space::xxl, space::xxxl, space::xxl, space::xxxl));
    m_layout->setAlignment(Qt::AlignHCenter);
    // In a place taller than it needs (a list's whole area) it stays one
    // block in the middle, not spread out over the height.
    m_layout->addStretch(1);
    auto* tile = new IconTile(iconName, size::emptyTile, this);
    tile->setTone(Tone::Neutral);
    m_layout->addWidget(tile, 0, Qt::AlignHCenter);
    m_layout->addSpacing(space::md);
    m_title = heading(title, TextStyle::BodyStrong, this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    m_layout->addWidget(m_title, 0, Qt::AlignHCenter);
    m_caption = caption(text, this);
    m_caption->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_caption->setContentsMargins(0, space::xs, 0, 0);
    m_caption->setVisible(!text.isEmpty());
    m_layout->addWidget(m_caption, 0, Qt::AlignHCenter);
    auto* actions = new QWidget(this);
    actions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_actions = hbox(actions, space::sm, QMargins(0, space::md, 0, 0));
    m_layout->addWidget(actions, 0, Qt::AlignHCenter);
    actions->hide();
    m_layout->addStretch(1);
    fitText();
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
    m_compact = compact;
    const int v = compact ? space::lg : space::xxxl;
    m_layout->setContentsMargins(space::xxl, v, space::xxl, v);
    fitText();
}

// A word-wrapped label in a centred column gets the height of the width it
// would like, not of the width it has: `l` is given both here. True when its
// height changed.
static bool fitWrapped(QLabel* l, int width)
{
    // (A label without text has no height for a width: it takes no room.)
    const QSize s(width, l->text().isEmpty() ? 0 : std::max(0, l->heightForWidth(width)));
    if (l->minimumSize() == s && l->maximumSize() == s)
        return false;
    const bool taller = l->maximumHeight() != s.height();
    l->setFixedSize(s);
    return taller;
}

void EmptyState::fitText()
{
    // Before the first layout the widget has no width to go by: the full
    // text width is assumed, which is what nearly every place gives it.
    const QMargins m = m_layout->contentsMargins();
    const int w = m_sized ? std::clamp(width() - m.left() - m.right(), 160, int(size::emptyMax)) : int(size::emptyMax);
    const bool a = fitWrapped(m_title, w);
    const bool b = fitWrapped(m_caption, w);
    if (a || b)
        updateGeometry(); // the height changed: the parent's layout has to know now, not after this one's own pass
}

void EmptyState::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    m_sized = true;
    fitText();
}

QSize EmptyState::minimumSizeHint() const
{
    // The text follows the width (fitText()): the labels' current width is no minimum.
    const QMargins m = m_layout->contentsMargins();
    const QSize s = QWidget::minimumSizeHint();
    return {std::min(s.width(), 160 + m.left() + m.right()), s.height()};
}

ErrorState::ErrorState(const QString& title, const QString& body, const QString& technical, QWidget* parent)
    : QWidget(parent)
{
    auto* l = vbox(this, space::md);
    auto* row = new QHBoxLayout;
    row->setSpacing(space::md);
    row->setAlignment(Qt::AlignTop);
    auto* tile = new IconTile(QStringLiteral("error"), size::emptyTile, this);
    tile->setTone(Tone::Err);
    row->addWidget(tile, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout;
    col->setSpacing(4);
    m_title = heading(title, TextStyle::BodyStrong, this);
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
    auto* l = vbox(this, space::xs, QMargins(space::xxl, space::xxxl, space::xxl, space::xxxl));
    l->setAlignment(Qt::AlignHCenter);
    auto* tile = new IconTile(QStringLiteral("package"), size::emptyTile, this);
    tile->setTone(Tone::Neutral);
    l->addWidget(tile, 0, Qt::AlignHCenter);
    auto* t = heading(FW_TR("%1 is not available in this build").arg(feature), TextStyle::BodyStrong, this);
    t->setAlignment(Qt::AlignCenter);
    t->setWordWrap(true);
    l->addSpacing(space::sm);
    l->addWidget(t, 0, Qt::AlignHCenter);
    auto* c = caption(FW_TR("The engine that does this work is not part of this build. The form stays usable; nothing is queued until the engine is present."), this);
    c->setAlignment(Qt::AlignCenter);
    l->addWidget(c, 0, Qt::AlignHCenter);
    m_tech = label(FW_TR("job kind %1 is not registered").arg(kind), TextStyle::Mono, color::fg_4, this);
    m_tech->setAlignment(Qt::AlignCenter);
    m_tech->setWordWrap(true);
    l->addWidget(m_tech, 0, Qt::AlignHCenter);
    for (QLabel* wrapped : {t, c, m_tech})
        fitWrapped(wrapped, size::emptyMax);
}

void UnavailableState::setKinds(const QStringList& kinds)
{
    m_tech->setText(kinds.size() == 1 ? FW_TR("job kind %1 is not registered").arg(kinds.first())
                                      : FW_TR("job kinds %1 are not registered").arg(kinds.join(QStringLiteral(", "))));
    fitWrapped(m_tech, size::emptyMax);
}

// ---- inputs ---------------------------------------------------------------------

QLineEdit* textInput(const QString& placeholder, bool mono, QWidget* parent)
{
    auto* e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setProperty("mono", mono);
    if (mono) {
        keepLeftToRight(e); // paths, IDs, keys, addresses
        // The value in the mono font; a placeholder that is a sentence
        // ("Choose a folder…", "Default") stays in the body font while the
        // field is empty. One that shows a value ("192.168.1.40",
        // "01.000.000") is set like the value.
        auto fit = [e] {
            static const QRegularExpression valueSign(QStringLiteral("[0-9/\\\\.:_-]"));
            const QString hint = e->placeholderText();
            const bool specimen = !hint.isEmpty() && !hint.contains(u' ') && hint.contains(valueSign);
            const QFont want = e->text().isEmpty() && !specimen ? font::body() : font::mono();
            if (e->font() != want)
                keepFont(e, want);
        };
        QObject::connect(e, &QLineEdit::textChanged, e, fit);
        fit();
    }
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

void setInputSuffix(QLineEdit* edit, const QString& suffix)
{
    if (!edit)
        return;
    edit->setProperty("fwSuffix", suffix);
    // The room it takes at the end of the field (the start right to left);
    // the stylesheet's padding stays around both.
    const int w = suffix.isEmpty() ? 0 : QFontMetrics(edit->font()).horizontalAdvance(suffix) + space::xs;
    QMargins m = edit->textMargins();
    m.setLeft(edit->isRightToLeft() ? w : 0);
    m.setRight(edit->isRightToLeft() ? 0 : w);
    edit->setTextMargins(m);
    edit->update();
}

QString inputSuffix(const QLineEdit* edit) { return edit ? edit->property("fwSuffix").toString() : QString(); }

void setInputNote(QLineEdit* edit, const QString& note)
{
    if (!edit || edit->property("fwNote").toString() == note)
        return;
    edit->setProperty("fwNote", note);
    edit->setAccessibleDescription(note);
    edit->update();
}

QString inputNote(const QLineEdit* edit) { return edit ? edit->property("fwNote").toString() : QString(); }

bool isTextCut(const QLineEdit* edit)
{
    if (!edit)
        return false;
    const QString shown = edit->displayText();
    if (shown.isEmpty())
        return false;
    QStyleOptionFrame opt;
    opt.initFrom(edit);
    const QRect contents = edit->style()->subElementRect(QStyle::SE_LineEditContents, &opt, edit);
    const QMargins tm = edit->textMargins();
    const int room = contents.width() - tm.left() - tm.right() - 5; // QLineEdit's own 2 px margins and the cursor
    return QFontMetrics(edit->font()).horizontalAdvance(shown) > room;
}

QComboBox* dropdown(QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->setFocusPolicy(Qt::StrongFocus); // not WheelFocus: the wheel scrolls the page (see installAppGuard)
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
    m_edit = textInput({}, true, this); // left to right (a mono input); the row around it follows the layout
    m_browse = button(FW_TR("Browse…"), ButtonVariant::Secondary, QStringLiteral("folder"), this);
    row->addWidget(m_edit, 1);
    row->addWidget(m_browse);
    l->addLayout(row);
    m_errorRow = new QWidget(this);
    auto* er = hbox(m_errorRow, 6);
    m_errorLabel = label({}, TextStyle::Caption, color::danger_hi, m_errorRow);
    m_errorLabel->setWordWrap(true);
    auto* ic = new QLabel(m_errorRow);
    ic->setPixmap(iconPixmap(QStringLiteral("error"), size::iconSm, color::danger, devicePixelRatioF()));
    ic->setFixedSize(size::iconSm, QFontMetrics(font::caption()).height()); // level with the first line of the message
    ic->setAlignment(Qt::AlignCenter);
    m_errorLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    er->addWidget(ic, 0, Qt::AlignTop);
    er->addWidget(m_errorLabel, 1);
    l->addWidget(m_errorRow);
    m_errorRow->hide();
    setFocusProxy(m_edit);
    m_edit->installEventFilter(this); // its width decides how much of the placeholder shows
    connect(m_browse, &QPushButton::clicked, this, &PathField::browse);
    connect(m_edit, &QLineEdit::editingFinished, this, [this] { emit pathChanged(path()); });
    connect(m_edit, &QLineEdit::textEdited, this, [this](const QString&) { emit pathEdited(path()); });
}

QString PathField::path() const
{
    QString t = m_edit->text().trimmed();
    // Explorer's "Copy as path" puts the path in quotes.
    if (t.size() >= 2 && t.startsWith(u'"') && t.endsWith(u'"'))
        t = t.mid(1, t.size() - 2).trimmed();
    return QDir::fromNativeSeparators(t);
}

void PathField::setPath(const QString& p)
{
    const QString native = QDir::toNativeSeparators(p);
    if (m_edit->text() == native)
        return;
    m_edit->setText(native);
    m_edit->setCursorPosition(0);
    emit pathChanged(path());
}

void PathField::setPlaceholder(const QString& text)
{
    m_placeholder = text;
    fitPlaceholder();
}

// The placeholder as far as it fits, cut in the middle. While it is cut the
// whole text is the edit's tooltip (unless the owner gave the edit one), and
// it stays what assistive tools read.
void PathField::fitPlaceholder()
{
    QStyleOptionFrame opt;
    opt.initFrom(m_edit);
    const QRect contents = m_edit->style()->subElementRect(QStyle::SE_LineEditContents, &opt, m_edit);
    const QMargins tm = m_edit->textMargins();
    const int room = contents.width() - tm.left() - tm.right() - 6; // the edit's own 2 px margins and the cursor
    const QString shown = room > 0 ? QFontMetrics(m_edit->font()).elidedText(m_placeholder, Qt::ElideMiddle, room) : m_placeholder;
    if (m_edit->placeholderText() != shown)
        m_edit->setPlaceholderText(shown);
    const bool cut = shown != m_placeholder;
    if (m_edit->toolTip() == m_autoTip) {
        m_autoTip = cut ? plainTip(m_placeholder) : QString();
        m_edit->setToolTip(m_autoTip);
    }
    if (m_edit->accessibleDescription().isEmpty() || m_edit->accessibleDescription() == m_autoDescription) {
        m_autoDescription = cut ? m_placeholder : QString();
        m_edit->setAccessibleDescription(m_autoDescription);
    }
}

bool PathField::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_edit && !m_placeholder.isEmpty()
        && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
        fitPlaceholder();
    return QWidget::eventFilter(watched, event);
}

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
    QPointer<PathField> self(this);
    QString chosen;
    switch (m_mode) {
    case Mode::Directory:
        chosen = QFileDialog::getExistingDirectory(window(), m_title.isEmpty() ? FW_TR("Choose a folder") : m_title, start);
        break;
    case Mode::OpenFile:
        chosen = QFileDialog::getOpenFileName(window(), m_title.isEmpty() ? FW_TR("Choose a file") : m_title, start, m_filter);
        break;
    case Mode::SaveFile:
        chosen = QFileDialog::getSaveFileName(window(), m_title.isEmpty() ? FW_TR("Save as") : m_title, start, m_filter);
        break;
    }
    if (self && !chosen.isEmpty()) // the form can be rebuilt while the dialog is open
        setPath(chosen);
}

// What a drop on this field would set: a local folder for a folder field, a
// local file for a file field; "" for anything else (web links, the wrong kind).
QString PathField::droppedPath(const QMimeData* mime) const
{
    if (!mime || !mime->hasUrls() || m_edit->isReadOnly())
        return {};
    const QList<QUrl> urls = mime->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return {};
    const QString local = urls.first().toLocalFile();
    const QFileInfo fi(local);
    if (local.isEmpty() || !fi.exists())
        return {};
    return fi.isDir() == (m_mode == Mode::Directory) ? local : QString();
}

void PathField::dragEnterEvent(QDragEnterEvent* e)
{
    if (!droppedPath(e->mimeData()).isEmpty())
        e->acceptProposedAction();
}

void PathField::dropEvent(QDropEvent* e)
{
    const QString local = droppedPath(e->mimeData());
    if (local.isEmpty())
        return;
    setPath(local);
    e->acceptProposedAction();
}

MessageRow::MessageRow(QWidget* parent) : QWidget(parent)
{
    auto* h = hbox(this, 6);
    m_icon = new QLabel(this);
    // As tall as one line of the caption beside it, the glyph in its middle:
    // level with the first line however tall the row gets.
    m_icon->setFixedSize(size::iconSm, QFontMetrics(font::caption()).height());
    m_icon->setAlignment(Qt::AlignCenter);
    m_text = label({}, TextStyle::Caption, color::fg_3, this);
    m_text->setWordWrap(true);
    m_text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
        if (m_right) // stacked: the right column is laid out at the full width (setGeometry)
            h += (m_left ? m_gap : 0) + (m_right->hasHeightForWidth() ? m_right->heightForWidth(width) : m_right->sizeHint().height());
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
            m_left->setGeometry(mirrored(this, rect, QRect(rect.x(), y, rect.width(), h)));
            y += h + m_gap;
        }
        if (m_right) {
            const int w = rect.width();
            const int h = m_right->hasHeightForWidth() ? m_right->heightForWidth(w) : m_right->sizeHint().height();
            m_right->setGeometry(mirrored(this, rect, QRect(rect.x(), y, w, h)));
        }
        return;
    }
    if (m_left) {
        const int lw = std::min(m_leftMax, rect.width() - (m_right ? m_rightWidth + m_gap : 0));
        const int h = m_left->hasHeightForWidth() ? m_left->heightForWidth(lw) : m_left->sizeHint().height();
        // Right to left the main column is at the right, the side column at the left.
        m_left->setGeometry(mirrored(this, rect, QRect(rect.x(), rect.y(), lw, std::max(h, m_left->minimumSize().height()))));
        if (m_right) {
            const int rx = rect.x() + lw + m_gap;
            const int rh = m_right->hasHeightForWidth() ? m_right->heightForWidth(m_rightWidth) : m_right->sizeHint().height();
            m_right->setGeometry(mirrored(this, rect, QRect(rx, rect.y(), m_rightWidth, std::max(rh, m_right->minimumSize().height()))));
        }
    } else if (m_right) {
        m_right->setGeometry(mirrored(this, rect, QRect(rect.x(), rect.y(), m_rightWidth, m_right->sizeHint().height())));
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
        if (it->isEmpty())
            continue; // a hidden chip takes no room and no gap
        const QSize s = it->sizeHint();
        int nextX = x + s.width() + m_h;
        if (nextX - m_h > r.right() + 1 && lineH > 0) {
            x = r.x();
            y += lineH + m_v;
            nextX = x + s.width() + m_h;
            lineH = 0;
        }
        if (!testOnly)
            it->setGeometry(mirrored(this, r, QRect(QPoint(x, y), s))); // right to left a line fills from the right
        x = nextX;
        lineH = std::max(lineH, s.height());
    }
    return y + lineH - r.y() + contentsMargins().top() + contentsMargins().bottom();
}

// ---- scroll areas -------------------------------------------------------------------

void reserveScrollBarSpace(QAbstractScrollArea* area)
{
    if (!area)
        return;
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    QScrollBar* bar = area->verticalScrollBar();
    // Nothing to scroll: the bar keeps its room, without a handle (the
    // stylesheet's QScrollBar[fwIdle="true"]).
    auto sync = [bar] {
        const bool idle = bar->maximum() <= bar->minimum();
        if (bar->property("fwIdle").toBool() == idle && bar->property("fwIdle").isValid())
            return;
        bar->setProperty("fwIdle", idle);
        repolish(bar);
        bar->update();
    };
    QObject::connect(bar, &QScrollBar::rangeChanged, bar, sync);
    sync();
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

bool openInFileManager(const QString& path)
{
    if (path.isEmpty())
        return false;
    const QFileInfo fi(path);
#ifdef Q_OS_WIN
    if (fi.exists() && !fi.isDir())
        return QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(fi.absoluteFilePath())});
#endif
    // A file that was moved away: its folder is the next best thing to show.
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (!QFileInfo(dir).isDir())
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// ---- brand mark / icon tile ------------------------------------------------------

BrandMark::BrandMark(int px, QWidget* parent) : QWidget(parent), m_px(px)
{
    setFixedSize(px, px);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void BrandMark::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // A white glyph sits on it: the fill tokens.
    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0, color::accent_fill);
    g.setColorAt(1, color::accent_fill_2);
    p.setPen(QPen(withAlpha(color::on_accent, 45), 1));
    p.setBrush(g);
    const qreal rad = m_px * 0.3;
    p.drawRoundedRect(r, rad, rad);
    // Gloss on the upper half.
    QLinearGradient gloss(r.topLeft(), QPointF(r.left(), r.center().y()));
    gloss.setColorAt(0, withAlpha(color::on_accent, 40));
    gloss.setColorAt(1, withAlpha(color::on_accent, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(gloss);
    p.drawRoundedRect(r, rad, rad);
    const int is = int(m_px * 0.58);
    p.drawPixmap(QRect((m_px - is) / 2, (m_px - is) / 2, is, is),
                 iconPixmap(QStringLiteral("layers"), is, color::on_accent, devicePixelRatioF()));
}

IconTile::IconTile(const QString& iconName, int px, QWidget* parent) : QWidget(parent), m_icon(iconName), m_px(px)
{
    setFixedSize(px, px);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void IconTile::setIconName(const QString& iconName)
{
    m_icon = iconName;
    update();
}

void IconTile::setTone(Tone tone)
{
    if (m_toned && m_tone == tone)
        return;
    m_tone = tone;
    m_toned = true;
    update();
}

void IconTile::setAccent(bool on)
{
    if (m_accent == on)
        return;
    m_accent = on;
    update();
}

void IconTile::paintEvent(QPaintEvent*)
{
    if (m_icon.isEmpty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal rad = m_px * 0.28;
    QColor glyph = color::accent_hi;
    if (m_toned && m_tone == Tone::Neutral) {
        p.setPen(QPen(color::border_2, 1));
        p.setBrush(color::bg_3);
        glyph = color::fg_3;
    } else if (m_toned && m_tone != Tone::Info) {
        QColor tint = toneMark(m_tone);
        QColor fillc = tint;
        fillc.setAlpha(34);
        tint.setAlpha(70);
        p.setPen(QPen(tint, 1));
        p.setBrush(fillc);
        glyph = toneText(m_tone);
    } else if (m_accent) {
        // The page's own mark: a raised square under the signature gradient
        // as a 22 % tint, outlined by the gradient at 45 %.
        p.setPen(Qt::NoPen);
        p.setBrush(color::bg_3);
        p.drawRoundedRect(r, rad, rad);
        p.setPen(QPen(QBrush(accentGradient(r, 115)), 1));
        p.setBrush(accentGradient(r, 56));
    } else {
        QLinearGradient g(r.topLeft(), r.bottomRight());
        QColor a = color::accent, b = color::accent_2;
        a.setAlpha(58);
        b.setAlpha(40);
        g.setColorAt(0, a);
        g.setColorAt(1, b);
        p.setPen(QPen(withAlpha(color::accent_hi, 70), 1));
        p.setBrush(g);
    }
    p.drawRoundedRect(r, rad, rad);
    const int is = std::max(14, int(m_px * 0.46));
    p.drawPixmap(QRect((m_px - is) / 2, (m_px - is) / 2, is, is), iconPixmap(m_icon, is, glyph, devicePixelRatioF()));
}

} // namespace fw::ui
