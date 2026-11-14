#pragma once

// The design-system component set (docs/DESIGN_PROPOSAL.md §4): small widgets
// and factories that every page composes. Nothing here knows about jobs or
// settings; see SettingRow.h, JobRows.h and LogView.h for the data-bound ones.

#include "app/ui/Theme.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTimer>
#include <QWidget>

#include <functional>

class QAbstractItemView;
class QAbstractScrollArea;
class QBoxLayout;
class QVBoxLayout;
class QHBoxLayout;
class QMimeData;

namespace fw::ui {

namespace motion {
class Value;
class HoverFade;
} // namespace motion

// ---- labels ------------------------------------------------------------------
enum class TextStyle { Display, Title, Stat, Subtitle, BodyStrong, Body, Caption, Overline, Mono, MonoSmall };
QFont fontFor(TextStyle style);
// Every label of this library shows its text literally (Qt::PlainText): titles,
// paths, file names and messages come from dumps, images, tools and servers,
// and must never be read as markup. richLabel() is for markup the app itself
// writes; escape the outside parts of it with toHtmlEscaped().
QLabel* label(const QString& text, TextStyle style, const QColor& color, QWidget* parent = nullptr);
QLabel* richLabel(const QString& html, TextStyle style, const QColor& color, QWidget* parent = nullptr);
QLabel* caption(const QString& text, QWidget* parent = nullptr);      // caption fg_3
QLabel* meta(const QString& text, QWidget* parent = nullptr);         // caption fg_4
QLabel* monoLabel(const QString& text, QWidget* parent = nullptr, const QColor& color = theme::color::fg_1);
QLabel* heading(const QString& text, TextStyle style = TextStyle::Subtitle, QWidget* parent = nullptr);
void setTextColor(QWidget* w, const QColor& color);
// Tooltip text that is shown literally: escaped, and wrapped by Qt like any
// rich-text tooltip (a plain one is a single long line). "" stays "".
QString plainTip(const QString& text);
// A word-wrapped label in nested layouts can be squeezed below the height its
// text needs once the page is taller than its scroll area (a long translation
// wraps to more lines). This keeps the label's minimum height at what its
// current width needs.
void keepWrappedHeight(QLabel* label);
// Where a drop-down menu opens under `button`: its left corner, or its
// right corner in a right-to-left window (a menu then opens towards the left).
QPoint menuAnchor(const QWidget* button, int gap = 0);
// A label that elides long text (middle for paths, right for titles).
class ElideLabel : public QLabel {
    Q_OBJECT
public:
    explicit ElideLabel(const QString& text = {}, Qt::TextElideMode mode = Qt::ElideRight, QWidget* parent = nullptr);
    void setFullText(const QString& text);
    QString fullText() const { return m_text; }
    // More than one line: the text wraps where it may and only the last line
    // is cut ("…" at its end). Default 1.
    void setMaxLines(int lines);
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void relayout();
    QString m_text;
    QString m_autoName; // the accessible name this label gave itself
    QString m_autoTip;  // the tooltip this label gave itself (the whole text while it is cut)
    Qt::TextElideMode m_mode;
    int m_maxLines = 1;
};

// A link-styled label ("Use default", "Open queue"). Emits activated().
class Link : public QLabel {
    Q_OBJECT
public:
    explicit Link(const QString& text, QWidget* parent = nullptr);
    void setText(const QString& text); // also the accessible name
signals:
    void activated();

protected:
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
};

QFrame* hairline(QWidget* parent = nullptr, bool strong = false); // border_1 (strong: border_2)
QWidget* spacer(QWidget* parent = nullptr);                        // expanding
QWidget* fixedSpacer(int w, int h, QWidget* parent = nullptr);
QHBoxLayout* hbox(QWidget* parent, int spacing = theme::space::sm, const QMargins& margins = {});
QVBoxLayout* vbox(QWidget* parent, int spacing = theme::space::md, const QMargins& margins = {});
// "label above control" form field with an optional help caption.
QWidget* field(const QString& labelText, QWidget* control, const QString& help = {}, QWidget* parent = nullptr);

// ---- buttons -----------------------------------------------------------------
enum class ButtonVariant { Secondary, Primary, Danger, Subtle };
QPushButton* button(const QString& text, ButtonVariant variant = ButtonVariant::Secondary,
                    const QString& icon = {}, QWidget* parent = nullptr);
void setButtonVariant(QPushButton* b, ButtonVariant variant);
void setButtonSmall(QPushButton* b, bool small);
void setButtonIcon(QPushButton* b, const QString& icon, const QColor& color = {});
void repolish(QWidget* w);

class IconButton : public QToolButton {
    Q_OBJECT
public:
    enum class Size { Regular, Small, Tiny }; // 32, 28, 24 (14 px icon)
    IconButton(const QString& icon, const QString& accessibleName, Size size = Size::Regular,
               QWidget* parent = nullptr);
    void setIconName(const QString& icon);
    QString iconName() const { return m_icon; }
    void setIconColor(const QColor& rest);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_icon;
    QColor m_rest, m_active, m_checked;
    Size m_size;
};

// ---- painted surfaces ----------------------------------------------------------
// Depth is painted, never a graphics effect. `r` is the outline's rectangle
// (inset by half a pixel from the widget's), antialiasing is switched on here.
// A plate lifts off what it sits on: bg_2, 2 % lighter at the top, a border_2
// outline and a faint lit top edge. `hover` 0..1 takes it one step up (bg_3,
// border_4); a bool works.
void paintPlate(QPainter& p, const QRectF& r, qreal radius, qreal hover = 0.0);
// A well sinks into a card: field_bg and a border_1 outline, nothing lit.
void paintWell(QPainter& p, const QRectF& r, qreal radius);
// A popup floats over the page: bg_3 and a border_4 outline.
void paintPopup(QPainter& p, const QRectF& r, qreal radius);
// A soft shadow around `r`, `spread` px wide and dropped by a third of that:
// for what floats in a translucent window that leaves the room.
void paintShadow(QPainter& p, const QRectF& r, qreal radius, int spread);
QColor blend(const QColor& a, const QColor& b, qreal t); // a at 0, b at 1

// ---- right to left ----------------------------------------------------------------
// Qt mirrors layouts, style-sheet widgets and the alignment of drawn text for a
// right-to-left widget (an Arabic, Persian, Hebrew or Urdu interface); custom
// painting and hand-placed geometry use these. Geometry is worked out left to
// right as before and mirrored where it is used.
// `r` in `w`'s left-to-right coordinates, as it is shown: mirrored for RTL.
QRect mirrored(const QWidget* w, const QRect& r);
QRectF mirrored(const QWidget* w, const QRectF& r);
// A point as it is shown in `w` back to left-to-right coordinates (or the other way: it is its own inverse).
QPoint mirrored(const QWidget* w, const QPoint& pt);
// For a hand-written QLayout: `r`, placed left to right inside `area`, as it
// is shown in the layout's widget (mirrored inside `area` for RTL).
QRect mirrored(const QLayout* layout, const QRect& area, const QRect& r);
// Flips the painter for a right-to-left widget, so that what follows is drawn
// mirrored: for shapes only (bars, tracks, knobs), never for text or icons.
void mirrorPainter(QPainter& p, const QWidget* w);
// Paths, URLs, addresses, IDs, hashes, versions, code and logs read left to
// right in every language. A label's text is read LTR (the label stays aligned
// with the layout); any other widget and its children are laid out LTR.
void keepLeftToRight(QWidget* w);
// drawText() flags for a path, an ID or a size painted in `w`: read left to
// right, and placed as `align` is in `w`'s direction (AlignLeft: at the right
// edge right to left).
int leftToRightText(const QWidget* w, Qt::Alignment align);
// drawText() flags for data painted in a right-to-left `w` (a game title, a
// file name, "+2"): read in the text's own direction, as a label reads it,
// and placed as `align` is right to left. Left to right: `align` unchanged.
int naturalText(const QWidget* w, Qt::Alignment align, const QString& text);
// The cells of `column` (sizes, dates, IDs, paths) read left to right; the
// column itself stays where the view's direction puts it, and its alignment
// is mirrored with the view.
void keepColumnLeftToRight(QAbstractItemView* view, int column);
// `text` as it is drawn in a right-to-left paragraph: a sign at the end of a
// Latin name ("ShadowMount+", "C#") would take the direction of the text
// around it and be drawn in front of the name ("+ShadowMount"); a
// left-to-right mark after it keeps it with the name. Labels, buttons, item
// views (ItemDelegate) and the texts this library paints apply it themselves.
QString bidiSafe(const QString& text);
// The delegate of the app's item views. The style sheet draws an item's text
// itself (its ::item rules give items a border) and never through FwStyle,
// so a right-to-left view's text is prepared here, read as a label reads it:
// text without right-to-left letters reads left to right as a whole (a
// sentence keeps its full stop at its end, a path its last backslash, a name
// its sign), other text gets bidiSafe(). FwStyle gives one to every view
// that has Qt's own delegate; the app's delegates that draw an item through
// QStyledItemDelegate derive from it.
class ItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};

// ---- cover placeholder ----------------------------------------------------------
// The cover of a game that has no cover art, the same wherever it is listed:
// one of the six theme::cover gradients with the title's initials in white
// (display font). Which gradient is decided by `id` (the title ID; the title
// where there is none): the same for an ID on every run and every machine.
// `r` is filled as it is (radius 0: a plain rectangle, for a caller that clips).
void paintCoverPlaceholder(QPainter& p, const QRectF& r, const QString& title, const QString& id, qreal radius);
int coverIndex(const QString& id);            // 0 .. theme::cover::count - 1; case and outer spaces do not matter
QString coverInitials(const QString& title);  // up to two letters or digits, "?" when the title has none

// ---- card --------------------------------------------------------------------
class Card : public QFrame {
    Q_OBJECT
public:
    enum class Variant { Regular, Table, Inset }; // §4.4
    explicit Card(QWidget* parent = nullptr, Variant variant = Variant::Regular);
    // Title row (subtitle fg_0) with optional right slot. Only for Regular.
    QHBoxLayout* setTitle(const QString& title, QWidget* rightSlot = nullptr);
    void setTitleText(const QString& title);
    QVBoxLayout* body() { return m_body; }
    // Table variant: header row (12/16 padded, hairline below), footer row.
    QHBoxLayout* header();
    QHBoxLayout* footer();
    void setDropHighlight(bool on); // dashed accent_hi inner border while dragging
    bool dropHighlight() const { return m_drop; }
    void setSelected(bool on);      // accent_lo fill (running block in the dock)

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    Variant m_variant;
    QVBoxLayout* m_outer = nullptr;
    QVBoxLayout* m_body = nullptr;
    QHBoxLayout* m_header = nullptr;
    QHBoxLayout* m_footer = nullptr;
    QWidget* m_headerWidget = nullptr;
    QWidget* m_footerWidget = nullptr;
    QLabel* m_title = nullptr;
    QHBoxLayout* m_titleRow = nullptr;
    bool m_drop = false;
    bool m_selected = false;
};

// ---- status badge --------------------------------------------------------------
enum class Tone { Ok, Warn, Err, Info, Neutral };
// The one badge / chip look, for the Badge widget and for everything that
// paints chips itself (grid tiles, choice tiles, the sidebar's counts): a pill
// with 11 px semibold text on the tone's colour as a 16 % tint with a 35 %
// outline; neutral is bg_4 / border_3 / fg_2. `r` is 22 px high (18 for a
// count). `onAccent`: white on the fill gradient instead of a tone.
void paintBadge(QPainter& p, const QRectF& r, const QString& text, Tone tone, const QString& icon = {},
                bool onAccent = false);
int badgeWidth(const QString& text, bool withIcon = false, bool count = false);
class Badge : public QWidget {
    Q_OBJECT
public:
    explicit Badge(const QString& text = {}, Tone tone = Tone::Neutral, QWidget* parent = nullptr);
    void set(const QString& text, Tone tone);
    void setText(const QString& text);
    void setTone(Tone tone);
    void setIconName(const QString& icon); // overrides the tone's default glyph (empty = none)
    void setCount(bool count);             // 18 px count variant
    // A text longer than this is cut in its middle, with the whole text as the
    // tooltip (32 characters; 0: never). The header's status chips say more.
    void setMaxLength(int chars);
    QString text() const { return m_text; }
    Tone tone() const { return m_tone; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void updateCut(); // tooltip and mouse events while the text is cut
    QString shortText() const; // the text cut to the maximum length
    QString m_text;
    Tone m_tone = Tone::Neutral;
    QString m_icon;
    bool m_iconSet = false;
    bool m_count = false;
    int m_maxChars = 32;
};
QString defaultBadgeIcon(Tone tone);
QColor toneMark(Tone tone);  // glyph / fill colour
QColor toneText(Tone tone);  // text colour
QColor toneTint(Tone tone);  // background tint (opaque, for bars)
QColor toneLine(Tone tone);  // outline of a tinted surface: the tone at 35 %

// ---- brand mark / icon tile ---------------------------------------------------
// The app's logo: a gradient (accent -> accent_2) rounded square with a glyph.
class BrandMark : public QWidget {
    Q_OBJECT
public:
    explicit BrandMark(int px = 28, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    int m_px;
};

// A rounded square holding an icon: page headers, feature cards, stat tiles.
// Default: translucent brand gradient with an accent_hi glyph; a tone tints
// it; Tone::Neutral is the quiet tile of empty states (bg_3, border_2, fg_3).
class IconTile : public QWidget {
    Q_OBJECT
public:
    IconTile(const QString& iconName, int px = 40, QWidget* parent = nullptr);
    void setIconName(const QString& iconName);
    void setTone(Tone tone);
    Tone tone() const { return m_tone; }
    bool isToned() const { return m_toned; }
    // The page header's tile: the gradient tint over a raised square with a
    // gradient outline (stronger than the default tile, which sits on cards).
    void setAccent(bool on);
    bool isAccent() const { return m_accent; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_icon;
    int m_px;
    Tone m_tone = Tone::Info;
    bool m_toned = false;
    bool m_accent = false;
};

// ---- switch --------------------------------------------------------------------
class Switch : public QAbstractButton {
    Q_OBJECT
public:
    explicit Switch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {theme::size::switchW, theme::size::switchH}; }

    // 0 = off, 1 = on, in between while the knob slides (tests).
    qreal position() const;
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    bool m_hover = false;
    motion::Value* m_position; // the knob slides, the track cross-fades
};

// ---- segmented control -----------------------------------------------------------
class Segmented : public QWidget {
    Q_OBJECT
public:
    explicit Segmented(QWidget* parent = nullptr);
    void addSegment(const QString& text, const QString& icon = {}, const QVariant& value = {});
    void setSegmentText(int index, const QString& text);
    QString segmentText(int index) const; // "" when out of range
    void setCurrentIndex(int index);
    int currentIndex() const { return m_current; }
    QVariant currentValue() const;
    int indexOfValue(const QVariant& value) const;
    void setCurrentValue(const QVariant& value);
    int count() const { return int(m_segments.size()); }
    void setSegmentEnabled(int index, bool enabled);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
    bool isAnimating() const; // the selection pill is sliding (tests)
signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Segment {
        QString text, icon;
        QVariant value;
        bool enabled = true;
    };
    int segmentAt(const QPoint& p) const;
    int segmentWidth(int i) const;
    QRectF segmentRect(int i) const;
    std::vector<Segment> m_segments;
    int m_current = -1;
    motion::HoverFade* m_hover;
    motion::Value* m_slide; // 0..1 from m_slideFrom to the current segment
    QRectF m_slideFrom;
};

// ---- progress --------------------------------------------------------------------
class ProgressBar : public QWidget {
    Q_OBJECT
public:
    enum class State { Determinate, Indeterminate, Done, Failed, Paused };
    explicit ProgressBar(QWidget* parent = nullptr);
    void setFraction(double f); // 0..1
    double fraction() const { return m_fraction; }
    void setState(State s);
    State state() const { return m_state; }
    QSize sizeHint() const override { return {120, theme::size::progress}; }
    QSize minimumSizeHint() const override { return {40, theme::size::progress}; }
    // The fill eases to a new fraction: what is painted right now (tests).
    double shownFraction() const;
    // A timer or an animation of this bar is running (tests): only while it
    // is visible and its job runs.
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void updateTimer();
    void tick();
    double m_fraction = 0;
    State m_state = State::Determinate;
    QTimer m_timer;       // frames of the sweep / the indeterminate band, and the wait between sweeps
    qint64 m_t0 = 0;
    motion::Value* m_shown; // the painted fraction
};

// Progress block: bar + percent (mono fg_0), caption line, optional stepper.
class ProgressBlock : public QWidget {
    Q_OBJECT
public:
    explicit ProgressBlock(QWidget* parent = nullptr);
    void setProgress(double fraction, bool indeterminate, const QString& detail);
    void setState(ProgressBar::State state);
    void setPhases(const QStringList& phases, int current); // -1 = none active
    void setPhasesVisible(bool visible);
    ProgressBar* bar() { return m_bar; }
    QString percentText() const { return m_percent->text(); }

private:
    void updateLabel();
    ProgressBar* m_bar;
    QLabel* m_percent;
    QLabel* m_detail;
    QWidget* m_stepper;
    bool m_indeterminate = false;
};

// Stage stepper (§4.14): done = check success_hi, current = filled circle, pending = circle.
class StageStepper : public QWidget {
    Q_OBJECT
public:
    explicit StageStepper(QWidget* parent = nullptr);
    void setStages(const QStringList& names, int current, bool failed = false);
    bool isEmpty() const { return m_names.isEmpty(); }

private:
    QStringList m_names;
    int m_current = -2;
    bool m_failed = false;
    QHBoxLayout* m_layout;
};

// ---- notification: info bar ---------------------------------------------------------
class InfoBar : public QFrame {
    Q_OBJECT
public:
    explicit InfoBar(Tone tone = Tone::Info, QWidget* parent = nullptr);
    void setTitle(const QString& title);      // body-strong (errors)
    void setMessage(const QString& message);  // body fg_1
    void setTone(Tone tone);
    void setDismissable(bool on);
    void clearLinks();
    Link* addLink(const QString& text);
signals:
    void dismissed();

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    Tone m_tone;
    QLabel* m_icon;
    QLabel* m_title;
    QLabel* m_message;
    QHBoxLayout* m_links;
    IconButton* m_dismiss;
};

// ---- empty / error / unavailable states ----------------------------------------------
// A 44 px tile, a body-strong title and a caption, centred and never wider
// than 360 px. Compact (docks, wells, lists) keeps it under about 150 px tall.
class EmptyState : public QWidget {
    Q_OBJECT
public:
    EmptyState(const QString& icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    QPushButton* addAction(const QString& text, ButtonVariant variant = ButtonVariant::Secondary);
    void setCompact(bool compact);
    bool isCompact() const { return m_compact; }
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void fitText();
    QHBoxLayout* m_actions;
    QVBoxLayout* m_layout;
    QLabel* m_title;
    QLabel* m_caption;
    bool m_compact = false;
    bool m_sized = false; // had its first resize: its width is real
};

class ErrorState : public QWidget {
    Q_OBJECT
public:
    ErrorState(const QString& title, const QString& body, const QString& technical = {}, QWidget* parent = nullptr);
    void setTechnical(const QString& line);
    void setBody(const QString& body);
    void setTitle(const QString& title);
    QPushButton* addAction(const QString& text, ButtonVariant variant = ButtonVariant::Secondary);

private:
    QLabel* m_title;
    QLabel* m_body;
    QLabel* m_tech;
    QHBoxLayout* m_actions;
};

// "This feature is not available in this build" (job kind not registered).
class UnavailableState : public QWidget {
    Q_OBJECT
public:
    explicit UnavailableState(const QString& feature, const QString& kind, QWidget* parent = nullptr);
    void setKinds(const QStringList& kinds);

private:
    QLabel* m_tech;
};

// ---- inputs --------------------------------------------------------------------------
// `mono`: the value is a path, an ID or an address, set in the mono font and
// read left to right; a placeholder that is a sentence stays in the body font
// (one that shows a value, "192.168.1.40", is set like the value).
QLineEdit* textInput(const QString& placeholder = {}, bool mono = false, QWidget* parent = nullptr);
void setInputError(QLineEdit* edit, bool error);
// A unit inside the end of a field, after the value ("GB", "s", "files"), in
// fg_4: the field keeps its width whatever the unit, so a column of fields
// ends on one line. "" removes it.
void setInputSuffix(QLineEdit* edit, const QString& suffix);
QString inputSuffix(const QLineEdit* edit);
// What the value of a right-aligned number field means, inside its start in
// fg_4 while it does not have the focus ("Automatic" beside a 0). "" removes it.
void setInputNote(QLineEdit* edit, const QString& note);
QString inputNote(const QLineEdit* edit);
// A text field that shows only part of its text (it is wider than the field)
// ends in "…" while it does not have the focus, and its tooltip is the whole
// text (both are done for every QLineEdit by the application guard).
bool isTextCut(const QLineEdit* edit);
QComboBox* dropdown(QWidget* parent = nullptr);
// Item data role of a dropdown item: the short text the closed dropdown shows
// when the item's whole text ("Title ID · PPSA01325 …") does not fit.
inline constexpr int kShortTextRole = Qt::UserRole + 0x5f5;

// A path input with Browse… (§4.7); accepts drops.
class PathField : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Directory, OpenFile, SaveFile };
    explicit PathField(Mode mode = Mode::Directory, QWidget* parent = nullptr);
    QString path() const;
    void setPath(const QString& path);
    // Shown while the field is empty; cut in the middle when the field is too
    // narrow for it (the whole text is the tooltip then).
    void setPlaceholder(const QString& text);
    QString placeholder() const { return m_placeholder; }
    void setFilter(const QString& filter) { m_filter = filter; }
    void setDialogTitle(const QString& title) { m_title = title; }
    void setMode(Mode mode) { m_mode = mode; }
    void setError(const QString& message); // empty clears
    QString error() const { return m_errorText; }
    QLineEdit* edit() { return m_edit; }
    QPushButton* browseButton() { return m_browse; }
    void setReadOnly(bool ro);
    void setStartDir(const QString& dir) { m_startDir = dir; }
signals:
    void pathChanged(const QString& path);   // on edit finished / browse / drop
    void pathEdited(const QString& path);    // every keystroke

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void browse();
    void fitPlaceholder();
    QString droppedPath(const QMimeData* mime) const;
    Mode m_mode;
    QLineEdit* m_edit;
    QPushButton* m_browse;
    QWidget* m_errorRow;
    QLabel* m_errorLabel;
    QString m_filter, m_title, m_startDir, m_errorText;
    QString m_placeholder; // the whole text
    QString m_autoTip;     // the tooltip the field gave its edit (the whole placeholder while it is cut)
    QString m_autoDescription; // and the accessible description
};

// Message row under a control: 12 px glyph + caption.
class MessageRow : public QWidget {
    Q_OBJECT
public:
    explicit MessageRow(QWidget* parent = nullptr);
    void set(const QString& text, Tone tone); // empty text hides the row
    void clear();
    QString text() const;

private:
    QLabel* m_icon;
    QLabel* m_text;
};

// ---- responsive two-column layout (form + 400 px aside) -------------------------------
// Side by side when there is room (left max `leftMax`, right fixed `rightWidth`),
// otherwise the right column wraps under the left one.
class TwoColumnLayout : public QLayout {
public:
    explicit TwoColumnLayout(QWidget* parent = nullptr, int rightWidth = theme::size::dock,
                             int leftMax = theme::size::formMax, int gap = theme::space::lg);
    ~TwoColumnLayout() override;
    void setLeft(QWidget* w);
    void setRight(QWidget* w);
    void setLeftMax(int px) { m_leftMax = px; invalidate(); }
    void setBreakpoint(int px) { m_breakpoint = px; }
    bool isStacked() const { return m_stacked; }

    void addItem(QLayoutItem* item) override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    int count() const override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

private:
    bool stackedFor(int width) const;
    QLayoutItem* m_left = nullptr;
    QLayoutItem* m_right = nullptr;
    int m_rightWidth, m_leftMax, m_gap;
    int m_breakpoint = 0; // 0 = derive from the widths
    bool m_stacked = false;
};

// Wrapping row layout for chips / action groups.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int hSpacing = theme::space::sm, int vSpacing = theme::space::sm);
    ~FlowLayout() override;
    void addItem(QLayoutItem* item) override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    int count() const override { return int(m_items.size()); }
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    Qt::Orientations expandingDirections() const override { return {}; }

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    QList<QLayoutItem*> m_items;
    int m_h, m_v;
};

// ---- scroll areas ----------------------------------------------------------------------
// The vertical scroll bar's room is kept whether there is anything to scroll
// or not (the handle shows only when there is): what the area holds, and a
// header aligned with it, do not move sideways when the bar comes and goes.
void reserveScrollBarSpace(QAbstractScrollArea* area);

// ---- misc --------------------------------------------------------------------------------
QString thinPercent(double fraction);           // "63 %" with a thin space
QString elideMiddle(const QString& s, int max); // for badges and captions
// Shows a folder / selects a file. False when neither the path nor its folder
// exists any more, or the file manager could not be started.
bool openInFileManager(const QString& path);
QString nativePath(const QString& path);

} // namespace fw::ui
