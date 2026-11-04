#include "app/ui/Style.h"

#include "app/ui/Icons.h"
#include "app/ui/Theme.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTextEdit>

namespace fw::ui {

using namespace theme;

FwStyle::FwStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

static void paintCheck(QPainter* p, const QRect& r, const QStyleOption* opt, bool radio)
{
    const bool enabled = opt->state & QStyle::State_Enabled;
    const bool on = opt->state & QStyle::State_On;
    const bool partial = opt->state & QStyle::State_NoChange;
    const bool hover = opt->state & QStyle::State_MouseOver;
    QRect box(0, 0, size::check, size::check);
    box.moveCenter(r.center());
    QColor fill = color::field_bg, border = color::border_control;
    if (!enabled) {
        fill = color::bg_2;
        border = color::border_3;
    } else if (on || partial) {
        fill = border = hover ? color::accent_hover : color::accent;
    } else if (hover) {
        border = color::border_4;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(border, 1));
    p->setBrush(fill);
    const QRectF rf = QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5);
    if (radio)
        p->drawEllipse(rf);
    else
        p->drawRoundedRect(rf, radius::sm, radius::sm);
    if (on) {
        if (radio) {
            p->setPen(Qt::NoPen);
            p->setBrush(enabled ? color::on_accent : color::fg_5);
            p->drawEllipse(rf.center(), 3.0, 3.0);
        } else {
            const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
            const QPixmap pm = iconPixmap(QStringLiteral("check"), 12, enabled ? color::on_accent : color::fg_5, dpr);
            p->drawPixmap(QRect(box.center().x() - 5, box.center().y() - 5, 12, 12), pm);
        }
    } else if (partial) {
        p->setPen(Qt::NoPen);
        p->setBrush(color::on_accent);
        p->drawRect(QRect(box.center().x() - 4, box.center().y(), 10, 2));
    }
    p->restore();
}

static void paintChevron(QPainter* p, const QRect& r, const QString& name, const QStyleOption* opt)
{
    const bool enabled = opt->state & QStyle::State_Enabled;
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const int s = std::min({r.width(), r.height(), size::icon});
    const QPixmap pm = iconPixmap(name, s, enabled ? color::fg_3 : color::fg_5, dpr);
    QRect target(0, 0, s, s);
    target.moveCenter(r.center());
    p->drawPixmap(target, pm);
}

void FwStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                            const QWidget* widget) const
{
    switch (element) {
    case PE_FrameFocusRect:
        return; // the overlay paints the design-system ring
    case PE_IndicatorCheckBox:
    case PE_IndicatorItemViewItemCheck:
        paintCheck(painter, option->rect, option, false);
        return;
    case PE_IndicatorRadioButton:
        paintCheck(painter, option->rect, option, true);
        return;
    case PE_IndicatorArrowDown:
        paintChevron(painter, option->rect, QStringLiteral("chevron_down"), option);
        return;
    case PE_IndicatorArrowUp:
        paintChevron(painter, option->rect, QStringLiteral("chevron_up"), option);
        return;
    case PE_IndicatorArrowRight:
        paintChevron(painter, option->rect, QStringLiteral("chevron_right"), option);
        return;
    case PE_IndicatorArrowLeft:
        paintChevron(painter, option->rect, QStringLiteral("chevron_left"), option);
        return;
    case PE_IndicatorHeaderArrow:
        if (const auto* h = qstyleoption_cast<const QStyleOptionHeader*>(option)) {
            const QString name = h->sortIndicator == QStyleOptionHeader::SortDown ? QStringLiteral("chevron_down")
                                                                                  : QStringLiteral("chevron_up");
            QRect r = option->rect;
            r.setSize(QSize(size::iconSm, size::iconSm));
            r.moveCenter(option->rect.center());
            const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
            painter->drawPixmap(r, iconPixmap(name, size::iconSm, color::fg_3, dpr));
        }
        return;
    case PE_IndicatorBranch: {
        if (!(option->state & State_Children))
            return;
        const QString name = (option->state & State_Open) ? QStringLiteral("chevron_down") : QStringLiteral("chevron_right");
        paintChevron(painter, option->rect, name, option);
        return;
    }
    case PE_PanelItemViewItem:
    case PE_PanelItemViewRow:
        // Hairline rows, hover and selection are painted by delegates / QSS.
        if (option->state & State_Selected) {
            painter->fillRect(option->rect, color::accent_lo);
            return;
        }
        if (option->state & State_MouseOver) {
            painter->fillRect(option->rect, color::bg_3);
            return;
        }
        return;
    case PE_FrameTabWidget:
    case PE_FrameTabBarBase:
        return;
    default:
        break;
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void FwStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                          const QWidget* widget) const
{
    if (element == CE_ItemViewItem) {
        if (const auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
            // Keep the item text colour stable when selected (accent_lo is dark).
            QStyleOptionViewItem o(*vi);
            o.palette.setColor(QPalette::HighlightedText, o.palette.color(QPalette::Text));
            QProxyStyle::drawControl(element, &o, painter, widget);
            return;
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

int FwStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent: return 10;
    case PM_FocusFrameHMargin:
    case PM_FocusFrameVMargin: return 0;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight: return size::check;
    case PM_CheckBoxLabelSpacing:
    case PM_RadioButtonLabelSpacing: return space::sm;
    case PM_MenuPanelWidth: return 1;
    case PM_MenuHMargin:
    case PM_MenuVMargin: return 4;
    case PM_SmallIconSize: return size::icon;
    case PM_ButtonIconSize: return size::icon;
    case PM_ToolBarIconSize: return size::icon;
    case PM_HeaderMarkSize: return size::iconSm;
    case PM_SplitterWidth: return space::lg;
    case PM_DefaultFrameWidth: return 0;
    case PM_TabBarTabShiftHorizontal:
    case PM_TabBarTabShiftVertical: return 0;
    case PM_ToolTipLabelFrameWidth: return 1;
    default: break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

int FwStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                       QStyleHintReturn* returnData) const
{
    switch (hint) {
    case SH_UnderlineShortcut: return 0;
    case SH_ComboBox_Popup: return 0;
    case SH_ComboBox_ListMouseTracking: return 1;
    case SH_ToolTip_WakeUpDelay: return 450;
    case SH_ToolTip_FallAsleepDelay: return 0;
    case SH_ScrollBar_Transient: return 0;
    case SH_ItemView_ShowDecorationSelected: return 1;
    case SH_Menu_SloppySubMenus: return 1;
    case SH_Menu_FlashTriggeredItem: return 0;
    case SH_Menu_FadeOutOnHide: return 0;
    case SH_DialogButtonBox_ButtonsHaveIcons: return 0;
    case SH_MessageBox_CenterButtons: return 0;
    case SH_FocusFrame_AboveWidget: return 1;
    case SH_Widget_Animate:
    case SH_Widget_Animation_Duration: return 0;
    case SH_ScrollView_FrameOnlyAroundContents: return 0;
    case SH_ComboBox_UseNativePopup: return 0;
    default: break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

QSize FwStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                const QWidget* widget) const
{
    QSize s = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_MenuItem)
        s.setHeight(std::max(s.height(), 30));
    return s;
}

void FwStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    if (auto* menu = qobject_cast<QMenu*>(widget)) {
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        menu->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        if (combo->view()) {
            combo->view()->window()->setWindowFlag(Qt::NoDropShadowWindowHint, true);
            combo->view()->setTextElideMode(Qt::ElideRight);
        }
    }
}

// ---------------------------------------------------------------------------

FocusRingOverlay::FocusRingOverlay(QWidget* window) : QWidget(window)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setProperty("fwNoRing", true);
    setGeometry(window->rect());
    window->installEventFilter(this);
    m_tick.setInterval(60);
    connect(&m_tick, &QTimer::timeout, this, &FocusRingOverlay::refresh);
    connect(qApp, &QApplication::focusChanged, this, &FocusRingOverlay::onFocusChanged);
    hide();
}

bool FocusRingOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

static bool isInput(const QWidget* w)
{
    return qobject_cast<const QLineEdit*>(w) || qobject_cast<const QComboBox*>(w)
        || qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QPlainTextEdit*>(w)
        || qobject_cast<const QTextEdit*>(w) || w->property("fwInput").toBool();
}

void FocusRingOverlay::onFocusChanged(QWidget* /*old*/, QWidget* now)
{
    m_target = nullptr;
    if (now && now->window() == parentWidget() && !now->property("fwNoRing").toBool()
        && now->focusPolicy() != Qt::NoFocus) {
        // A line edit inside a combo box / spin box: ring the composite.
        QWidget* t = now;
        if (auto* p = qobject_cast<QWidget*>(t->parent()); p && (qobject_cast<QComboBox*>(p) || qobject_cast<QAbstractSpinBox*>(p)))
            t = p;
        if (t->property("fwRingProxy").value<QWidget*>())
            t = t->property("fwRingProxy").value<QWidget*>();
        const bool keyboard = t->window()->testAttribute(Qt::WA_KeyboardFocusChange);
        if (keyboard || isInput(t))
            m_target = t;
    }
    refresh();
}

QRect FocusRingOverlay::ringRect(int* radius) const
{
    if (!m_target || !m_target->isVisible() || !parentWidget())
        return {};
    const QWidget* t = m_target;
    QRect visible = t->visibleRegion().boundingRect();
    if (visible.isEmpty())
        return {};
    const QPoint origin = t->mapTo(parentWidget(), QPoint(0, 0));
    QRect r = t->rect().translated(origin);
    // Clip to what is actually visible (scroll areas) but keep the ring outside.
    const QRect clip = visible.translated(origin);
    if (clip.height() < r.height() - 2 || clip.width() < r.width() - 2)
        r = clip;
    const QVariant rv = t->property("fwRadius");
    *radius = rv.isValid() ? rv.toInt() : radius::sm;
    if (*radius >= radius::pill)
        *radius = r.height() / 2 + size::focusOffset;
    return r.adjusted(-size::focusOffset, -size::focusOffset, size::focusOffset, size::focusOffset);
}

void FocusRingOverlay::reset()
{
    m_target = nullptr;
    if (parentWidget())
        parentWidget()->setAttribute(Qt::WA_KeyboardFocusChange, false);
    refresh();
}

void FocusRingOverlay::refresh()
{
    int radius = 0;
    const QRect r = ringRect(&radius);
    if (r.isEmpty()) {
        if (isVisible())
            hide();
        m_tick.stop();
        m_last = {};
        return;
    }
    if (!isVisible()) {
        setGeometry(parentWidget()->rect());
        raise();
        show();
        m_tick.start();
    }
    if (r != m_last) {
        m_last = r;
        update();
    }
}

void FocusRingOverlay::paintEvent(QPaintEvent*)
{
    int radius = 0;
    const QRect r = ringRect(&radius);
    if (r.isEmpty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color::focus, size::focusRing));
    p.setBrush(Qt::NoBrush);
    const qreal inset = size::focusRing / 2.0;
    p.drawRoundedRect(QRectF(r).adjusted(-inset, -inset, inset, inset), radius + size::focusOffset,
                      radius + size::focusOffset);
}

} // namespace fw::ui
