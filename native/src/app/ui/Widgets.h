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
#include <QToolButton>
#include <QTimer>
#include <QWidget>

#include <functional>

class QBoxLayout;
class QVBoxLayout;
class QHBoxLayout;
class QMimeData;

namespace fw::ui {

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
// A label that elides long text (middle for paths, right for titles).
class ElideLabel : public QLabel {
    Q_OBJECT
public:
    explicit ElideLabel(const QString& text = {}, Qt::TextElideMode mode = Qt::ElideRight, QWidget* parent = nullptr);
    void setFullText(const QString& text);
    QString fullText() const { return m_text; }
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void relayout();
    QString m_text;
    Qt::TextElideMode m_mode;
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
    bool m_drop = false;
    bool m_selected = false;
};

// ---- status badge --------------------------------------------------------------
enum class Tone { Ok, Warn, Err, Info, Neutral };
class Badge : public QWidget {
    Q_OBJECT
public:
    explicit Badge(const QString& text = {}, Tone tone = Tone::Neutral, QWidget* parent = nullptr);
    void set(const QString& text, Tone tone);
    void setText(const QString& text);
    void setTone(Tone tone);
    void setIconName(const QString& icon); // overrides the tone's default glyph (empty = none)
    void setCount(bool count);             // 18 px count variant
    QString text() const { return m_text; }
    Tone tone() const { return m_tone; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_text;
    Tone m_tone = Tone::Neutral;
    QString m_icon;
    bool m_iconSet = false;
    bool m_count = false;
};
QString defaultBadgeIcon(Tone tone);
QColor toneMark(Tone tone);  // glyph / fill colour
QColor toneText(Tone tone);  // text colour
QColor toneTint(Tone tone);  // background tint

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
// Default: translucent brand gradient with an accent_hi glyph; a tone tints it.
class IconTile : public QWidget {
    Q_OBJECT
public:
    IconTile(const QString& iconName, int px = 40, QWidget* parent = nullptr);
    void setIconName(const QString& iconName);
    void setTone(Tone tone);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QString m_icon;
    int m_px;
    Tone m_tone = Tone::Info;
    bool m_toned = false;
};

// ---- switch --------------------------------------------------------------------
class Switch : public QAbstractButton {
    Q_OBJECT
public:
    explicit Switch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {theme::size::switchW, theme::size::switchH}; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    bool m_hover = false;
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
    std::vector<Segment> m_segments;
    int m_current = -1;
    int m_hover = -1;
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

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void updateTimer();
    double m_fraction = 0;
    State m_state = State::Determinate;
    QTimer m_timer;
    qint64 m_t0 = 0;
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
class EmptyState : public QWidget {
    Q_OBJECT
public:
    EmptyState(const QString& icon, const QString& title, const QString& text, QWidget* parent = nullptr);
    QPushButton* addAction(const QString& text, ButtonVariant variant = ButtonVariant::Secondary);
    void setCompact(bool compact);

private:
    QHBoxLayout* m_actions;
    QVBoxLayout* m_layout;
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
QLineEdit* textInput(const QString& placeholder = {}, bool mono = false, QWidget* parent = nullptr);
void setInputError(QLineEdit* edit, bool error);
QComboBox* dropdown(QWidget* parent = nullptr);

// A path input with Browse… (§4.7); accepts drops.
class PathField : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Directory, OpenFile, SaveFile };
    explicit PathField(Mode mode = Mode::Directory, QWidget* parent = nullptr);
    QString path() const;
    void setPath(const QString& path);
    void setPlaceholder(const QString& text);
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

private:
    void browse();
    QString droppedPath(const QMimeData* mime) const;
    Mode m_mode;
    QLineEdit* m_edit;
    QPushButton* m_browse;
    QWidget* m_errorRow;
    QLabel* m_errorLabel;
    QString m_filter, m_title, m_startDir, m_errorText;
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

// ---- misc --------------------------------------------------------------------------------
QString thinPercent(double fraction);           // "63 %" with a thin space
QString elideMiddle(const QString& s, int max); // for badges and captions
// Shows a folder / selects a file. False when neither the path nor its folder
// exists any more, or the file manager could not be started.
bool openInFileManager(const QString& path);
QString nativePath(const QString& path);

} // namespace fw::ui
