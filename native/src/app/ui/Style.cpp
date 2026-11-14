#include "app/ui/Style.h"

#include "app/ui/Icons.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"

#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolTip>

#include <algorithm>
#include <typeinfo>

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
    } else if (hover) {
        border = color::fg_5;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF rf = QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5);
    if (enabled && (on || partial)) {
        // A white mark sits on it: the fill gradient, not the bright accent.
        QLinearGradient g(rf.topLeft(), rf.topRight());
        g.setColorAt(0, hover ? color::accent_fill_hover : color::accent_fill);
        g.setColorAt(1, hover ? color::accent_fill_2_hover : color::accent_fill_2);
        p->setPen(Qt::NoPen);
        p->setBrush(g);
    } else {
        p->setPen(QPen(border, 1));
        p->setBrush(fill);
    }
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
        p->setBrush(enabled ? color::on_accent : color::fg_5);
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

// Where a sorted section's chevron goes. Fusion (and the style sheet, which
// places it itself once its rule gives it a size) puts it at the far end of the
// section: in a wide column whose label is at the start (the FTP browser's
// Name) it then reads as the next column's. It follows the label instead, 4 px
// after it (before it right to left); `given` when the label leaves no room
// or is not at the start.
static QRect headerArrowRect(const QStyleOptionHeader* h, const QWidget* widget, const QRect& given)
{
    const auto* view = qobject_cast<const QHeaderView*>(widget);
    if (!view || h->orientation != Qt::Horizontal || h->text.isEmpty() || h->section < 0 || !h->icon.isNull()
        || (h->textAlignment & Qt::AlignHorizontal_Mask) != Qt::AlignLeft)
        return given;
    QStyleOptionHeader section(*h);
    section.rect = QRect(view->sectionViewportPosition(h->section), 0, view->sectionSize(h->section), view->height());
    section.sortIndicator = QStyleOptionHeader::None;
    const QRect label = view->style()->subElementRect(QStyle::SE_HeaderLabel, &section, view);
    // The label's font: the QHeaderView::section rule of the style sheet (12 px, 600).
    QFont f = font::caption();
    f.setWeight(QFont::DemiBold);
    const int s = size::iconSm;
    const int textW = QFontMetrics(f).horizontalAdvance(h->text);
    if (!label.isValid() || textW + space::xs + s > label.width())
        return given;
    const int x = h->direction == Qt::RightToLeft ? label.right() + 1 - textW - space::xs - s : label.left() + textW + space::xs;
    return QRect(x, label.center().y() - s / 2, s, s);
}

QRect FwStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const
{
    const QRect r = QProxyStyle::subElementRect(element, option, widget);
    if (element == SE_HeaderArrow)
        if (const auto* h = qstyleoption_cast<const QStyleOptionHeader*>(option))
            return headerArrowRect(h, widget, r);
    return r;
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
            const QRect place = headerArrowRect(h, widget, option->rect);
            QRect r = place;
            r.setSize(QSize(size::iconSm, size::iconSm));
            r.moveCenter(place.center());
            const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
            painter->drawPixmap(r, iconPixmap(name, size::iconSm, color::fg_3, dpr));
        }
        return;
    case PE_IndicatorBranch: {
        if (!(option->state & State_Children))
            return;
        // Closed it points the way the tree reads: left in a right-to-left one.
        const QString name = (option->state & State_Open)          ? QStringLiteral("chevron_down")
                           : option->direction == Qt::RightToLeft ? QStringLiteral("chevron_left")
                                                                   : QStringLiteral("chevron_right");
        paintChevron(painter, option->rect, name, option);
        return;
    }
    case PE_PanelItemViewItem:
    case PE_PanelItemViewRow:
        // Tables and trees ask for this before every cell (and a tree for its
        // branch area); the stylesheet keeps selected cells clear over it.
        // Hairlines and hover come from delegates / QSS.
        if (option->state & State_Selected) {
            painter->fillRect(option->rect, color::accent_lo);
            // The accent bar, on the part that touches the view's left edge.
            if (element == PE_PanelItemViewRow && option->rect.left() <= 0 && option->rect.right() >= 2)
                painter->fillRect(QRect(0, option->rect.top(), 2, option->rect.height()), color::accent);
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

int FwStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent: return size::scrollBar;
    case PM_FocusFrameHMargin:
        // Item views lay out an item's icon and its text with this margin
        // (plus 1) on both sides of each: 3 gives the 8 px between an icon
        // and its label that buttons have, instead of 2 px.
        if (const auto* vi = qstyleoption_cast<const QStyleOptionViewItem*>(option);
            vi && (vi->features & QStyleOptionViewItem::HasDecoration) && !vi->icon.isNull())
            return space::xs - 1;
        return 0;
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

void FwStyle::drawItemText(QPainter* painter, const QRect& rect, int flags, const QPalette& pal, bool enabled,
                           const QString& givenText, QPalette::ColorRole textRole) const
{
    QString text = givenText;
    // The text of a dropdown that is narrower than it ends in an ellipsis
    // instead of being cut by the edge of the field (the stylesheet style
    // only clips it). An item with a short form ("Title ID, title and
    // version" for "… · PPSA01325 Astro Bot v01.003") shows that first.
    if (auto* combo = qobject_cast<QComboBox*>(dynamic_cast<QWidget*>(painter->device()))) {
        const QRect room = painter->hasClipping() ? rect.intersected(painter->clipBoundingRect().toAlignedRect()) : rect;
        const QFontMetrics fm = painter->fontMetrics();
        if (room.width() > 0 && fm.horizontalAdvance(text) > room.width()) {
            if (const QString brief = combo->currentData(kShortTextRole).toString(); !brief.isEmpty() && text == combo->currentText())
                text = brief;
            QProxyStyle::drawItemText(painter, room, flags, pal, enabled, fm.elidedText(text, Qt::ElideRight, room.width()), textRole);
            return;
        }
    }
    // A plain-text label aligns its text by the text's own direction: right to
    // left a number, a name or a path would stand at the left edge, away from
    // the rest of the row. The label's direction decides instead (the text is
    // still read its own way); a label of keepLeftToRight() is read LTR.
    if (auto* l = qobject_cast<QLabel*>(dynamic_cast<QWidget*>(painter->device()))) {
        if (l->property("fwLtrText").toBool())
            flags = (flags & ~int(Qt::TextForceRightToLeft)) | Qt::TextForceLeftToRight;
        if (l->isRightToLeft()) {
            const int h = int(QStyle::visualAlignment(Qt::RightToLeft, l->alignment()) & Qt::AlignHorizontal_Mask);
            flags = (flags & ~int(Qt::AlignHorizontal_Mask)) | h;
            if (!(flags & Qt::TextForceLeftToRight))
                text = bidiSafe(text); // "… ShadowMount+ …" in an Arabic sentence
        }
    } else if (const QWidget* w = dynamic_cast<QWidget*>(painter->device());
               painter->layoutDirection() == Qt::RightToLeft || (w && w->isRightToLeft())) {
        // Right to left (a painter takes the application's direction, not the
        // widget's): the text is placed as the widget's direction says, so a
        // header of a log kept left to right is not pushed to the right. A
        // button's or a header's text that is not right-to-left text (a name
        // such as "ShadowMount+", a number) is read left to right, as a label
        // reads it.
        const Qt::LayoutDirection dir = w ? w->layoutDirection() : painter->layoutDirection();
        const int h = int(QStyle::visualAlignment(dir, Qt::Alignment(flags & Qt::AlignHorizontal_Mask)));
        flags = (flags & ~int(Qt::AlignHorizontal_Mask)) | (h & Qt::AlignHorizontal_Mask);
        if (!(flags & (Qt::TextForceLeftToRight | Qt::TextForceRightToLeft)) && (dir == Qt::LeftToRight || !text.isRightToLeft()))
            flags |= Qt::TextForceLeftToRight;
        if (!(flags & Qt::TextForceLeftToRight))
            text = bidiSafe(text);
    }
    QProxyStyle::drawItemText(painter, rect, flags, pal, enabled, text, textRole);
}

namespace {

// A dropdown's list: Qt's own delegate still draws its separator rows.
class DropdownItemDelegate final : public ItemDelegate {
public:
    DropdownItemDelegate(QAbstractItemDelegate* qtDelegate, QObject* parent) : ItemDelegate(parent), m_qt(qtDelegate) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (m_qt && isSeparator(index))
            m_qt->paint(painter, option, index);
        else
            ItemDelegate::paint(painter, option, index);
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return m_qt && isSeparator(index) ? m_qt->sizeHint(option, index) : ItemDelegate::sizeHint(option, index);
    }

private:
    // As QComboBox::insertSeparator() marks the row.
    static bool isSeparator(const QModelIndex& index)
    {
        return index.data(Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
    }
    QPointer<QAbstractItemDelegate> m_qt;
};

// The style sheet draws an item itself and never asks this style for its
// text: Qt's own delegate of a view (a list, a tree, a table, a dropdown's
// list) makes way for the app's, which prepares it (ItemDelegate). A view
// with a delegate of its own keeps it.
void useAppItemDelegate(QAbstractItemView* view)
{
    QAbstractItemDelegate* delegate = view->itemDelegate();
    if (!delegate)
        return;
    if (typeid(*delegate) == typeid(QStyledItemDelegate))
        view->setItemDelegate(new ItemDelegate(view));
    else if (qstrcmp(delegate->metaObject()->className(), "QComboBoxDelegate") == 0)
        view->setItemDelegate(new DropdownItemDelegate(delegate, view)); // Qt's stays the view's child
}

} // namespace

// Controls whose value the mouse wheel changes.
static bool wheelChangesValue(const QWidget* w)
{
    return qobject_cast<const QComboBox*>(w) || qobject_cast<const QAbstractSpinBox*>(w)
        || (qobject_cast<const QAbstractSlider*>(w) && !qobject_cast<const QScrollBar*>(w));
}

// The style sheet has no direction: a dropdown laid out right to left is
// marked, and its rule mirrors the padding that keeps the chevron's room
// (Theme.cpp). True when the mark changed.
static bool markDropdownDirection(QComboBox* combo)
{
    const QVariant was = combo->property("fwRtl");
    const bool rtl = combo->layoutDirection() == Qt::RightToLeft;
    if (was.isValid() && was.toBool() == rtl)
        return false;
    combo->setProperty("fwRtl", rtl);
    return true;
}

void FwStyle::polish(QWidget* widget)
{
    QProxyStyle::polish(widget);
    // Qt gives these focus on a wheel turn (Qt::WheelFocus) before the wheel
    // event arrives; the app guard lets the wheel through to focused ones only.
    if (wheelChangesValue(widget) && widget->focusPolicy() == Qt::WheelFocus)
        widget->setFocusPolicy(Qt::StrongFocus);
    if (auto* menu = qobject_cast<QMenu*>(widget)) {
        menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        menu->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        // Before the style sheet reads its rules (it polishes this style first).
        markDropdownDirection(combo);
        if (combo->view()) {
            combo->view()->window()->setWindowFlag(Qt::NoDropShadowWindowHint, true);
            combo->view()->setTextElideMode(Qt::ElideRight);
        }
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(widget))
        useAppItemDelegate(view);
}

// ---------------------------------------------------------------------------

namespace {

// The widget that received the last FocusIn, and whether that was a keyboard reason.
QPointer<QWidget> g_focusIn;
bool g_focusInByKeyboard = false;

bool arrivedByKeyboard(const QWidget* focusWidget)
{
    if (!focusWidget)
        return false;
    if (g_focusIn == focusWidget && g_focusInByKeyboard)
        return true;
    return focusWidget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}

// Markup the app wrote itself: one paragraph whose content is escaped
// (plainTip(), the setting descriptions). Anything else that looks like
// markup came from outside.
bool isAppTip(const QString& tip)
{
    static const QLatin1String prefixes[] = {QLatin1String("<p style='white-space:pre-wrap'>"),
                                             QLatin1String("<p style='white-space:normal'>")};
    static const QLatin1String suffix("</p>");
    for (const QLatin1String& prefix : prefixes) {
        if (!tip.startsWith(prefix) || !tip.endsWith(suffix) || tip.size() < prefix.size() + suffix.size())
            continue;
        const QStringView inner = QStringView(tip).mid(prefix.size(), tip.size() - prefix.size() - suffix.size());
        return !inner.contains(u'<');
    }
    return false;
}

// Qt sizes a tooltip for its text alone: a long one becomes one very long
// line, or a paragraph as wide as Qt likes. A tip wider than the design's
// limit is narrowed to it afterwards and made as tall as the wrapped text
// needs (never narrower than its longest word, which cannot wrap).
void limitToolTipWidth(QWidget* w)
{
    auto* tip = qobject_cast<QLabel*>(w);
    if (!tip || w->width() <= size::toolTipMax)
        return;
    if (!tip->wordWrap())
        tip->setWordWrap(true); // a plain-text tip; Qt decides again for the next text
    const int target = std::max(size::toolTipMax, tip->minimumSizeHint().width());
    if (w->width() > target)
        w->resize(target, tip->heightForWidth(target));
}

// The visible panel of a menu: its window without the shadow's margin.
QRect menuPanel(const QMenu* menu)
{
    const int m = size::popupShadow;
    return menu->rect().adjusted(m, m, -m, -m);
}

// In the 26 px the stylesheet keeps free at the right of a dropdown
// (QComboBox::drop-down), inside its 1 px border.
void paintDropdownChevron(QComboBox* combo)
{
    const QColor c = !combo->isEnabled() ? color::fg_5
                   : combo->underMouse() || (combo->view() && combo->view()->isVisible()) ? color::fg_1 : color::fg_3;
    // The stylesheet's drop-down area is mirrored with the field: at the left right to left.
    const QRect r = mirrored(combo, QRect(combo->width() - 1 - 26 + (26 - size::icon) / 2, (combo->height() - size::icon) / 2,
                                          size::icon, size::icon));
    QPainter p(combo);
    p.drawPixmap(r, iconPixmap(QStringLiteral("chevron_down"), size::icon, c, combo->devicePixelRatioF()));
}

// The inside of a line edit's padding, where its text and a unit go.
QRect lineEditContents(const QLineEdit* edit)
{
    QStyleOptionFrame opt;
    opt.initFrom(edit);
    return edit->style()->subElementRect(QStyle::SE_LineEditContents, &opt, edit);
}

// A field whose text runs past its end while it does not have the focus,
// laid out left to right from its start (a path, a pattern list, a template;
// QLineEdit lays out such a text by its own direction, so also in a
// right-to-left window): the part that does not fit is covered and "…"
// stands in for it.
bool showsCutEnd(const QLineEdit* edit)
{
    return !edit->hasFocus() && edit->cursorPosition() == 0 && !edit->displayText().isRightToLeft()
        && (QStyle::visualAlignment(Qt::LeftToRight, edit->alignment()) & Qt::AlignLeft) && isTextCut(edit);
}

// What a line edit needs drawn over it: its unit (setInputSuffix), what its
// value means (setInputNote) and the ellipsis of a cut text.
bool needsLineEditExtras(const QLineEdit* edit)
{
    return !inputSuffix(edit).isEmpty() || (!edit->hasFocus() && !inputNote(edit).isEmpty()) || showsCutEnd(edit);
}

void paintLineEditExtras(QLineEdit* edit)
{
    QPainter p(edit);
    p.setFont(edit->font());
    const QRect contents = lineEditContents(edit);
    const QString suffix = inputSuffix(edit);
    if (!suffix.isEmpty()) {
        // In the room setInputSuffix() kept: after the value, at the field's
        // end (its start right to left).
        const int w = QFontMetrics(edit->font()).horizontalAdvance(suffix);
        const QRect r = edit->isRightToLeft() ? QRect(contents.left(), contents.top(), w + 1, contents.height())
                                              : QRect(contents.right() - w, contents.top(), w + 1, contents.height());
        p.setPen(edit->isEnabled() ? color::fg_4 : color::fg_5);
        p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextForceLeftToRight, suffix);
    }
    if (const QString note = inputNote(edit); !note.isEmpty() && !edit->hasFocus()) {
        // In the text area (inside the unit's room), on the side away from
        // the number: QLineEdit places a number by its own direction (left to
        // right, also in a right-to-left window) and the field's alignment.
        const QRect area = contents.marginsRemoved(edit->textMargins()).adjusted(2, 0, -2, 0);
        const Qt::Alignment numberSide =
            QStyle::visualAlignment(edit->text().isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight, edit->alignment()) & Qt::AlignHorizontal_Mask;
        const QFontMetrics fm(edit->font());
        const int room = area.width() - fm.horizontalAdvance(edit->displayText()) - space::md; // what the number leaves
        p.setPen(edit->isEnabled() ? color::fg_4 : color::fg_5);
        p.drawText(area, Qt::AlignVCenter | Qt::AlignAbsolute | ((numberSide & Qt::AlignRight) ? Qt::AlignLeft : Qt::AlignRight),
                   fm.elidedText(note, Qt::ElideRight, std::max(0, room)));
    }
    if (!showsCutEnd(edit))
        return;
    // At the end of the text area: inside the text margins and QLineEdit's own 2 px.
    const QRect text = contents.marginsRemoved(edit->textMargins()).adjusted(2, 0, -2, 0);
    const int ew = QFontMetrics(edit->font()).horizontalAdvance(QChar(0x2026)) + space::xs;
    const QRect cover(text.right() - ew + 1, text.top() + 1, ew + 1, text.height() - 2);
    p.fillRect(cover, !edit->isEnabled() || edit->isReadOnly() ? color::bg_2 : color::field_bg);
    p.setPen(!edit->isEnabled() ? color::fg_5 : edit->isReadOnly() ? color::fg_2 : color::field_fg);
    p.drawText(cover, Qt::AlignVCenter | Qt::AlignRight | Qt::AlignAbsolute, QString(QChar(0x2026)));
}

bool isSubMenu(const QMenu* menu)
{
    const auto* parent = qobject_cast<const QMenu*>(QApplication::activePopupWidget());
    return parent && parent != menu && parent->activeAction() == menu->menuAction();
}

class AppGuard : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::ToolTip: return toolTip(qobject_cast<QWidget*>(watched), static_cast<QHelpEvent*>(event));
        case QEvent::Wheel:
            if (auto* w = qobject_cast<QWidget*>(watched); w && wheelChangesValue(w) && !w->hasFocus()) {
                event->ignore(); // on to the parent: the page scrolls
                return true;
            }
            break;
        case QEvent::KeyPress: return keyPress(watched, static_cast<QKeyEvent*>(event));
        case QEvent::Resize:
            if (auto* w = qobject_cast<QWidget*>(watched); w && w->isWindow() && w->windowType() == Qt::ToolTip)
                limitToolTipWidth(w);
            break;
        // Menus: a translucent window with a clear margin around the panel
        // (the stylesheet's `margin`), where the soft shadow is painted.
        case QEvent::Paint:
            if (auto* menu = qobject_cast<QMenu*>(watched)) {
                QPainter p(menu); // before the menu paints its panel over it
                paintShadow(p, QRectF(menuPanel(menu)).adjusted(0.5, 0.5, -0.5, -0.5), radius::md, size::popupShadow - 2);
            } else if (auto* combo = qobject_cast<QComboBox*>(watched)) {
                // The stylesheet draws a dropdown as a field and leaves its
                // arrow area empty: the chevron that tells it from a text
                // field is painted over it, after the dropdown itself.
                watched->event(event);
                paintDropdownChevron(combo);
                return true;
            } else if (auto* edit = qobject_cast<QLineEdit*>(watched); edit && needsLineEditExtras(edit)) {
                watched->event(event);
                paintLineEditExtras(edit);
                return true;
            }
            break;
        case QEvent::LayoutDirectionChange:
            // A dropdown turned round after it was polished: its padding goes
            // with the chevron (an unpolished one is marked when it is polished).
            if (auto* combo = qobject_cast<QComboBox*>(watched);
                combo && combo->testAttribute(Qt::WA_WState_Polished) && markDropdownDirection(combo)) {
                combo->style()->unpolish(combo);
                combo->style()->polish(combo);
                combo->updateGeometry();
                combo->update();
            }
            [[fallthrough]];
        case QEvent::FontChange:
            // The unit's room goes with the field's end and the font's width.
            if (auto* edit = qobject_cast<QLineEdit*>(watched); edit && !inputSuffix(edit).isEmpty())
                setInputSuffix(edit, inputSuffix(edit));
            break;
        case QEvent::Show:
            // The panel, not the margin, goes where the menu was asked to be.
            // A submenu is placed by its first item, margin included.
            if (auto* menu = qobject_cast<QMenu*>(watched); menu && !isSubMenu(menu))
                menu->move(menu->pos() - QPoint(size::popupShadow, size::popupShadow));
            break;
        case QEvent::MouseButtonPress:
            // A click on the margin is a click beside the menu: it closes.
            if (auto* menu = qobject_cast<QMenu*>(watched);
                menu && !menuPanel(menu).contains(static_cast<QMouseEvent*>(event)->position().toPoint())) {
                for (int guard = 0; guard < 16; ++guard) {
                    auto* open = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    if (!open)
                        break;
                    open->close();
                }
                return true;
            }
            break;
        case QEvent::FocusIn:
            // Delivered before QApplication::focusChanged: see hasKeyboardFocus().
            if (auto* w = qobject_cast<QWidget*>(watched)) {
                const Qt::FocusReason reason = static_cast<QFocusEvent*>(event)->reason();
                // Back from another window or a popup: as it was before.
                if ((reason == Qt::ActiveWindowFocusReason || reason == Qt::PopupFocusReason) && g_focusIn == w)
                    break;
                g_focusIn = w;
                g_focusInByKeyboard = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
            }
            break;
        default: break;
        }
        return false;
    }

private:
    static bool toolTip(QWidget* w, QHelpEvent* e)
    {
        if (!w)
            return false;
        QString tip = w->toolTip();
        QRect rect;
        // A field that shows only part of its value: the whole value.
        if (auto* edit = qobject_cast<QLineEdit*>(w);
            tip.isEmpty() && edit && edit->echoMode() == QLineEdit::Normal && !edit->hasFocus() && isTextCut(edit)) {
            QToolTip::showText(e->globalPos(), plainTip(edit->text()), w);
            return true;
        }
        if (tip.isEmpty()) {
            // Item views take theirs from the model (Qt::ToolTipRole).
            auto* view = qobject_cast<QAbstractItemView*>(w->parentWidget());
            if (!view || view->viewport() != w)
                return false;
            const QModelIndex index = view->indexAt(e->pos());
            tip = index.data(Qt::ToolTipRole).toString();
            rect = view->visualRect(index);
        }
        if (tip.isEmpty() || !Qt::mightBeRichText(tip) || isAppTip(tip))
            return false;
        QToolTip::showText(e->globalPos(), plainTip(tip), w, rect);
        return true;
    }

    static bool keyPress(QObject* watched, QKeyEvent* e)
    {
        if ((e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter) || e->isAutoRepeat())
            return false;
        if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier))
            return false;
        auto* b = qobject_cast<QAbstractButton*>(watched);
        if (!b || !b->isEnabled())
            return false;
        // Toggles keep the platform's keys (Space); Enter is for actions.
        if (qobject_cast<QCheckBox*>(b) || qobject_cast<QRadioButton*>(b) || qobject_cast<Switch*>(b))
            return false;
        b->click();
        return true;
    }
};

} // namespace

bool hasKeyboardFocus(const QWidget* w)
{
    const QWidget* fw = QApplication::focusWidget();
    return w && fw && w->hasFocus() && arrivedByKeyboard(fw);
}

void installAppGuard(QApplication* app)
{
    static QPointer<AppGuard> guard;
    if (!app || guard)
        return;
    guard = new AppGuard(app);
    app->installEventFilter(guard);
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
        if (arrivedByKeyboard(now) || isInput(t))
            m_target = t;
    }
    // Above whatever was raised since (a sheet that just opened).
    if (m_target)
        raise();
    // Polled while there is a target: it may scroll out of view and back in.
    if (m_target && !m_tick.isActive())
        m_tick.start();
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
    *radius = rv.isValid() ? rv.toInt() : radius::control; // buttons and inputs
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
    if (!m_target) {
        if (isVisible())
            hide();
        m_tick.stop();
        m_last = {};
        return;
    }
    if (parentWidget() && parentWidget()->isMinimized())
        return; // nothing to follow; the next tick after restoring catches up
    int radius = 0;
    const QRect r = ringRect(&radius);
    if (r.isEmpty()) {
        // Scrolled out of view or hidden for now: keep watching it.
        if (isVisible())
            hide();
        m_last = {};
        return;
    }
    if (!isVisible()) {
        setGeometry(parentWidget()->rect());
        raise();
        show();
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
