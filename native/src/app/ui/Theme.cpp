#include "app/ui/Theme.h"

#include "app/ui/Accessible.h"
#include "app/ui/Icons.h"
#include "app/ui/Style.h"

#include "core/Tr.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStringList>
#include <QToolTip>

namespace fw::theme {

namespace {

QString pickFamily(const QStringList& candidates, const QString& fallback)
{
    const QStringList available = QFontDatabase::families();
    for (const QString& c : candidates)
        if (available.contains(c, Qt::CaseInsensitive))
            return c;
    return fallback;
}

QFont make(const QString& family, int px, QFont::Weight weight)
{
    QFont f;
    f.setFamily(family);
    f.setPixelSize(px);
    f.setWeight(weight);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setHintingPreference(QFont::PreferVerticalHinting);
    return f;
}

void tabular(QFont& f)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    f.setFeature(QFont::Tag("tnum"), 1);
#else
    Q_UNUSED(f); // Cascadia Mono / Consolas are tabular by design.
#endif
}

QLinearGradient sweep(const QRectF& r, QColor a, QColor b, int alpha)
{
    QLinearGradient g(r.topLeft(), r.topRight());
    a.setAlpha(alpha);
    b.setAlpha(alpha);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    return g;
}

} // namespace

// Scripts whose letters are taller than Segoe UI's: drawn in a fallback font,
// they would be clipped by line heights measured with Segoe UI (vowel signs
// above Devanagari and Bengali letters). For those interface languages the
// Windows font made for the script comes first; it has Latin letters too.
static QStringList scriptFamilies()
{
    const QString code = i18n::currentCode().section(u'-', 0, 0);
    static const QStringList indic = {QStringLiteral("hi"), QStringLiteral("bn"), QStringLiteral("mr"), QStringLiteral("ne"),
                                      QStringLiteral("ta"), QStringLiteral("te"), QStringLiteral("kn"), QStringLiteral("ml"),
                                      QStringLiteral("gu"), QStringLiteral("pa"), QStringLiteral("or"), QStringLiteral("si")};
    if (indic.contains(code))
        return {QStringLiteral("Nirmala UI"), QStringLiteral("Nirmala Text"), QStringLiteral("Noto Sans Devanagari")};
    return {};
}

QString sansFamily()
{
    static const QString family = pickFamily(scriptFamilies() + QStringList{QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
                                                                            QStringLiteral("Inter"), QStringLiteral("Noto Sans"),
                                                                            QStringLiteral("DejaVu Sans")},
                                             QApplication::font().family());
    return family;
}

static QString displayFamily()
{
    static const QString family = pickFamily(scriptFamilies() + QStringList{QStringLiteral("Segoe UI Variable Display"), QStringLiteral("Segoe UI"),
                                                                            QStringLiteral("Inter Display"), QStringLiteral("Inter")},
                                             sansFamily());
    return family;
}

QString monoFamily()
{
    static const QString family = pickFamily({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                                              QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Noto Sans Mono"),
                                              QStringLiteral("Liberation Mono")},
                                             QStringLiteral("monospace"));
    return family;
}

namespace font {
QFont display()
{
    QFont f = make(displayFamily(), 30, QFont::Bold);
    tabular(f);
    return f;
}
QFont title() { return make(displayFamily(), 24, QFont::Bold); }
QFont stat()
{
    QFont f = make(displayFamily(), 26, QFont::Bold);
    tabular(f);
    return f;
}
QFont subtitle() { return make(displayFamily(), 15, QFont::DemiBold); }
QFont bodyStrong() { return make(sansFamily(), 13, QFont::DemiBold); }
QFont body() { return make(sansFamily(), 13, QFont::Normal); }
QFont caption()
{
    QFont f = make(sansFamily(), 12, QFont::Normal);
    tabular(f);
    return f;
}
QFont chip()
{
    QFont f = make(sansFamily(), 11, QFont::DemiBold);
    tabular(f);
    return f;
}
QFont overline()
{
    QFont f = make(sansFamily(), 11, QFont::DemiBold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::PercentageSpacing, 106);
    return f;
}
QFont mono()
{
    QFont f = make(monoFamily(), 12, QFont::Normal);
    tabular(f);
    return f;
}
QFont monoSmall()
{
    QFont f = make(monoFamily(), 11, QFont::Normal);
    tabular(f);
    return f;
}
} // namespace font

QLinearGradient accentGradient(const QRectF& r, int alpha)
{
    return sweep(r, color::accent, color::accent_2, alpha);
}

QLinearGradient accentFillGradient(const QRectF& r, int alpha)
{
    return sweep(r, color::accent_fill, color::accent_fill_2, alpha);
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

namespace cover {
QLinearGradient gradient(int index)
{
    const Stops& s = stops[((index % count) + count) % count];
    QLinearGradient g(0, 0, 1, 1);
    g.setCoordinateMode(QGradient::ObjectMode);
    g.setColorAt(0, s.from);
    g.setColorAt(1, s.to);
    return g;
}
} // namespace cover

QString hex(const QColor& c)
{
    if (c.alpha() < 255)
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
    return c.name(QColor::HexRgb);
}

QString stylesheet()
{
    using namespace color;
    // Placeholders are substituted once; the template never contains a hex.
    // No `color` in the universal rule: a rule's colour is written into the
    // palette of every widget it matches when the widget is polished, and
    // would replace what label(), caption(), Link and setTextColor() set.
    // Plain text is fg_1 through the application palette (apply()).
    QString qss = QStringLiteral(R"QSS(
QWidget { background: transparent; }
QMainWindow, QDialog { background: {bg_1}; }
QAbstractScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QAbstractScrollArea::corner { background: transparent; }
QLabel { background: transparent; }

/* ---- buttons: plates (bg_4 and lighter), never the dark fill of a field ---- */
QPushButton {
    min-height: 32px; max-height: 32px; padding: 0 14px; border-radius: 8px;
    background: {bg_4}; border: 1px solid {border_3}; color: {fg_1}; min-width: 40px;
}
QPushButton:hover { background: {bg_5}; border-color: {border_4}; color: {fg_0}; }
QPushButton:pressed { background: {bg_3}; }
QPushButton:disabled { background: {bg_2}; border-color: {border_2}; color: {fg_5}; }
/* The fill gradient runs under the border; only its top edge is lit. */
QPushButton[variant="primary"] {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {accent_fill}, stop:1 {accent_fill_2});
    border: 1px solid transparent; border-top-color: {highlight_fill}; color: {on_accent};
}
QPushButton[variant="primary"]:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {accent_fill_hover}, stop:1 {accent_fill_2_hover});
    border-color: transparent; border-top-color: {highlight_fill};
}
QPushButton[variant="primary"]:pressed {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {accent_fill_pressed}, stop:1 {accent_fill_2_pressed});
    border-color: transparent;
}
QPushButton[variant="primary"]:disabled { background: {bg_3}; border-color: {border_2}; color: {fg_5}; }
QPushButton[variant="danger"] { background: {danger_fill}; border-color: {danger_fill}; color: {on_accent}; }
QPushButton[variant="danger"]:hover { background: {danger_fill_hover}; border-color: {danger_fill_hover}; }
QPushButton[variant="danger"]:pressed { background: {danger_fill_pressed}; border-color: {danger_fill_pressed}; }
QPushButton[variant="danger"]:disabled { background: {bg_2}; border-color: {border_2}; color: {fg_5}; }
QPushButton[variant="subtle"] { background: transparent; border-color: transparent; color: {fg_2}; }
QPushButton[variant="subtle"]:hover { background: {bg_3}; color: {fg_0}; }
QPushButton[variant="subtle"]:pressed { background: {bg_4}; }
QPushButton[variant="subtle"]:disabled { background: transparent; border-color: transparent; color: {fg_5}; }
QPushButton[size="sm"] { min-height: 26px; max-height: 26px; padding: 0 10px; border-radius: 7px; }
QPushButton[size="lg"] { min-height: 40px; max-height: 40px; padding: 0 20px; border-radius: 10px; }
QPushButton::menu-indicator { image: none; width: 0; }

QToolButton {
    min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px; padding: 0;
    border-radius: 8px; background: transparent; border: 1px solid transparent;
}
QToolButton:hover { background: {bg_3}; }
QToolButton:pressed { background: {bg_4}; }
QToolButton:checked { background: {accent_lo}; }
QToolButton[size="sm"] { min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; border-radius: 7px; }
QToolButton[size="xs"] { min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; border-radius: 6px; }
QToolButton::menu-indicator { image: none; width: 0; }

/* ---- inputs: wells (field_bg, darker than the card) -------------------------- */
QLineEdit, QAbstractSpinBox, QPlainTextEdit, QTextEdit {
    min-height: 32px; max-height: 32px; padding: 0 12px; border-radius: 8px;
    background: {field_bg}; border: 1px solid {border_3}; color: {field_fg};
    selection-background-color: {field_select_bg}; selection-color: {field_select_fg};
}
QLineEdit:hover, QAbstractSpinBox:hover { border-color: {border_control}; }
QLineEdit:focus, QAbstractSpinBox:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: {accent}; }
QPlainTextEdit, QTextEdit { max-height: 100000px; padding: 8px 12px; }
/* Live log console (JobMonitor): a terminal-like well on bg_0. */
QPlainTextEdit#fwConsole { background: {bg_0}; border: 1px solid {border_2}; border-radius: 8px; padding: 6px 8px; color: {fg_1}; }
QPlainTextEdit#fwConsole:focus { border-color: {border_3}; }
QLineEdit[error="true"] { border-color: {danger}; }
QLineEdit:read-only { border-color: {border_2}; color: {fg_2}; background: {bg_2}; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QPlainTextEdit:disabled { background: {bg_2}; border-color: {border_2}; color: {fg_5}; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; border: none; }

QComboBox {
    min-height: 32px; max-height: 32px; padding: 0 30px 0 12px; border-radius: 8px;
    background: {field_bg}; border: 1px solid {border_3}; color: {field_fg};
}
QComboBox:hover { border-color: {border_control}; }
QComboBox:focus { border-color: {accent}; }
QComboBox:disabled { background: {bg_2}; border-color: {border_2}; color: {fg_5}; }
QComboBox:on { background: {field_bg}; border-color: {accent}; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 26px; border: none; }
QComboBox::down-arrow { width: 16px; height: 16px; }
QComboBox QAbstractItemView {
    background: {bg_3}; border: 1px solid {border_4}; border-radius: 12px; padding: 6px;
    outline: 0; selection-background-color: {accent_lo}; selection-color: {fg_0}; color: {fg_1};
}
QComboBox QAbstractItemView::item { min-height: 32px; padding: 0 10px; border: none; border-radius: 7px; }
QComboBox QAbstractItemView::item:hover { background: {bg_4}; }
QComboBox QAbstractItemView::item:selected { background: {accent_lo}; color: {fg_0}; }

QCheckBox { spacing: 10px; color: {fg_1}; }
QCheckBox:disabled { color: {fg_5}; }
QCheckBox::indicator { width: 18px; height: 18px; }
QRadioButton { spacing: 10px; }

/* ---- popups: bg_3 with a border_4 outline ------------------------------------ */
/* The 12 px margin (size::popupShadow) is where the app guard paints a menu's shadow. */
QMenu { background: {bg_3}; border: 1px solid {border_4}; border-radius: 12px; padding: 6px; margin: 12px; }
QMenu::item { min-height: 32px; padding: 0 14px 0 10px; border-radius: 7px; color: {fg_1}; }
QMenu::item:selected { background: {bg_4}; color: {fg_0}; }
QMenu::item:disabled { color: {fg_5}; }
QMenu::separator { height: 1px; background: {border_3}; margin: 6px 4px; }
QMenu::icon { padding-left: 8px; }
QToolTip {
    background: {bg_4}; border: 1px solid {border_4}; border-radius: 8px; padding: 6px 10px;
    color: {fg_1}; font-size: 12px;
}

/* ---- scrollbars: an 8 px pill, 2 px in from the outer edge and both ends; the
   bar stays 10 px wide, so nothing a scroll area holds is laid out narrower -- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: {bg_5}; border-radius: 4px; margin: 2px 2px 2px 0; min-height: 32px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:vertical:pressed { background: {border_control}; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: {bg_5}; border-radius: 4px; margin: 0 2px 2px 2px; min-width: 32px; }
QScrollBar::handle:horizontal:hover, QScrollBar::handle:horizontal:pressed { background: {border_control}; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---- tables and trees ---------------------------------------------------------- */
QHeaderView { background: transparent; }
QHeaderView::section {
    background: transparent; color: {fg_4}; padding: 0 10px; min-height: 31px;
    border: none; border-bottom: 1px solid {border_2}; font-size: 12px; font-weight: 600;
}
QHeaderView::section:first { padding-left: 14px; }
QHeaderView::section:hover { color: {fg_1}; }
QHeaderView::down-arrow, QHeaderView::up-arrow { width: 12px; height: 12px; }
QTableView, QTreeView, QListView {
    background: transparent; border: none; outline: 0; gridline-color: transparent;
    selection-background-color: {accent_lo}; selection-color: {fg_0}; alternate-background-color: transparent;
}
QTableView::item, QTreeView::item, QListView::item { border: none; border-bottom: 1px solid {border_1}; padding: 0 10px; }
QTableView::item:hover, QTreeView::item:hover, QListView::item:hover { background: {bg_3}; }
QListView::item:selected { background: {accent_lo}; color: {fg_0}; }
/* The row of a table or tree is filled by FwStyle (accent_lo and the accent
   bar at its left edge); the cells stay clear over it. */
QTableView::item:selected, QTreeView::item:selected { background: transparent; color: {fg_0}; }
QTreeView::branch { background: transparent; }

/* ---- tabs (QTabBar is used only for in-page navigation): pill tabs ------------ */
QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent; border: 1px solid transparent; border-radius: 9px;
    padding: 7px 14px; margin-right: 4px; color: {fg_3}; min-height: 20px;
}
QTabBar::tab:hover { color: {fg_0}; background: {bg_3}; }
QTabBar::tab:selected { color: {fg_0}; background: {accent_lo}; border-color: {border_4}; }
QTabWidget::pane { border: none; border-top: 1px solid {border_2}; }

QSplitter::handle { background: transparent; }
QStatusBar { background: {bg_0}; }
QProgressBar { background: {bg_5}; border: none; border-radius: 4px; max-height: 8px; min-height: 8px; }
QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {accent}, stop:1 {accent_2}); border-radius: 4px; }
QFrame[role="hairline"] { background: {border_1}; max-height: 1px; min-height: 1px; border: none; }
QFrame[role="hairline2"] { background: {border_2}; max-height: 1px; min-height: 1px; border: none; }
)QSS");

    const QList<QPair<QString, QColor>> colors = {
        {QStringLiteral("bg_0"), bg_0}, {QStringLiteral("bg_1"), bg_1}, {QStringLiteral("bg_2"), bg_2},
        {QStringLiteral("bg_3"), bg_3}, {QStringLiteral("bg_4"), bg_4}, {QStringLiteral("bg_5"), bg_5},
        {QStringLiteral("border_1"), border_1}, {QStringLiteral("border_2"), border_2},
        {QStringLiteral("border_3"), border_3}, {QStringLiteral("border_4"), border_4},
        {QStringLiteral("border_control"), border_control}, {QStringLiteral("fg_0"), fg_0},
        {QStringLiteral("fg_1"), fg_1}, {QStringLiteral("fg_2"), fg_2}, {QStringLiteral("fg_3"), fg_3},
        {QStringLiteral("fg_4"), fg_4}, {QStringLiteral("fg_5"), fg_5}, {QStringLiteral("fg_6"), fg_6},
        {QStringLiteral("field_bg"), field_bg}, {QStringLiteral("field_fg"), field_fg},
        {QStringLiteral("field_select_bg"), field_select_bg}, {QStringLiteral("field_select_fg"), field_select_fg},
        {QStringLiteral("accent_hover"), accent_hover}, {QStringLiteral("accent_pressed"), accent_pressed},
        {QStringLiteral("accent_hi"), accent_hi}, {QStringLiteral("accent_lo"), accent_lo},
        {QStringLiteral("accent_2_hover"), accent_2_hover}, {QStringLiteral("accent_2_hi"), accent_2_hi},
        {QStringLiteral("accent_2"), accent_2}, {QStringLiteral("accent_08"), accent_08},
        {QStringLiteral("accent_fill_2_hover"), accent_fill_2_hover},
        {QStringLiteral("accent_fill_2_pressed"), accent_fill_2_pressed},
        {QStringLiteral("accent_fill_hover"), accent_fill_hover},
        {QStringLiteral("accent_fill_pressed"), accent_fill_pressed},
        {QStringLiteral("accent_fill_2"), accent_fill_2}, {QStringLiteral("accent_fill"), accent_fill},
        {QStringLiteral("highlight_fill"), highlight_fill},
        {QStringLiteral("accent"), accent}, {QStringLiteral("on_accent"), on_accent},
        {QStringLiteral("focus"), focus}, {QStringLiteral("success_hi"), success_hi},
        {QStringLiteral("success_bg"), success_bg}, {QStringLiteral("success"), success},
        {QStringLiteral("warn_hi"), warn_hi}, {QStringLiteral("warn_bg"), warn_bg}, {QStringLiteral("warn"), warn},
        {QStringLiteral("danger_hi"), danger_hi}, {QStringLiteral("danger_bg"), danger_bg},
        {QStringLiteral("danger_fill_hover"), danger_fill_hover},
        {QStringLiteral("danger_fill_pressed"), danger_fill_pressed}, {QStringLiteral("danger_fill"), danger_fill},
        {QStringLiteral("danger"), danger},
    };
    for (const auto& [name, c] : colors)
        qss.replace(QStringLiteral("{") + name + QStringLiteral("}"), hex(c));
    return qss;
}

void apply(QApplication* app)
{
    using namespace color;
    ui::initIcons();
    app->setStyle(new ui::FwStyle);
    ui::installAppGuard(app);
    ui::installAccessibility();
    app->setFont(font::body());

    QPalette p;
    p.setColor(QPalette::Window, bg_1);
    p.setColor(QPalette::WindowText, fg_1);
    p.setColor(QPalette::Base, bg_2);
    p.setColor(QPalette::AlternateBase, bg_2);
    p.setColor(QPalette::Text, fg_1);
    p.setColor(QPalette::Button, bg_4);
    p.setColor(QPalette::ButtonText, fg_1);
    p.setColor(QPalette::Highlight, field_select_bg);
    p.setColor(QPalette::HighlightedText, field_select_fg);
    p.setColor(QPalette::PlaceholderText, fg_4);
    p.setColor(QPalette::ToolTipBase, bg_4);
    p.setColor(QPalette::ToolTipText, fg_1);
    p.setColor(QPalette::Link, accent_hi);
    p.setColor(QPalette::LinkVisited, accent_hi);
    p.setColor(QPalette::BrightText, fg_0);
    p.setColor(QPalette::Mid, border_3);
    p.setColor(QPalette::Dark, border_2);
    p.setColor(QPalette::Light, bg_4);
    p.setColor(QPalette::Disabled, QPalette::WindowText, fg_5);
    p.setColor(QPalette::Disabled, QPalette::Text, fg_5);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, fg_5);
    p.setColor(QPalette::Disabled, QPalette::Base, bg_2);
    p.setColor(QPalette::Disabled, QPalette::Button, bg_2);
    app->setPalette(p);
    QToolTip::setPalette(p);
    app->setStyleSheet(stylesheet());
    app->setEffectEnabled(Qt::UI_AnimateMenu, false);
    app->setEffectEnabled(Qt::UI_FadeMenu, false);
    app->setEffectEnabled(Qt::UI_AnimateTooltip, false);
    app->setEffectEnabled(Qt::UI_FadeTooltip, false);
    app->setEffectEnabled(Qt::UI_AnimateCombo, false);
}

} // namespace fw::theme
