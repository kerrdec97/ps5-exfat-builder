#pragma once

// Design-system tokens (docs/DESIGN_PROPOSAL.md §3). Every colour, size and
// type style the application uses lives here; the QSS stylesheet is generated
// from these values and nobody writes a hex colour anywhere else.

#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QRectF>
#include <QString>

class QApplication;

namespace fw::theme {

// ---- colour ------------------------------------------------------------------
// v5 "Nebula" palette: blue-black surfaces with a cool tint and one signature
// gradient, electric blue -> violet, for the main action, selection and
// progress. Contrast ratios (WCAG 2):
//   text on bg_2 (card):  fg_0 16.9, fg_1 14.1, fg_2 9.9, fg_3 7.2, fg_4 5.6;
//                         fg_5 3.6 is for disabled text and hints only
//   captions elsewhere:   fg_3 / fg_4 on bg_1 7.8 / 6.1, on bg_0 8.1 / 6.3,
//                         on bg_3 6.5 / 5.1, on field_bg 7.7 / 6.0
//   white on the fill gradient: accent_fill 4.7 -> accent_fill_2 5.0
//                         (hover 4.0 -> 4.2, pressed 5.6 -> 6.0)
//   white on accent / accent_2 is only 3.8 / 4.2: those two stay bright for
//   text, icons, outlines, tints and progress on dark, and never carry text.
namespace color {
inline const QColor bg_0{QStringLiteral("#06080d")}; // chrome: sidebar, header, status bar
inline const QColor bg_1{QStringLiteral("#0a0d15")}; // page
inline const QColor bg_2{QStringLiteral("#121724")}; // card
inline const QColor bg_3{QStringLiteral("#1a2030")}; // raised control, hover on a card
inline const QColor bg_4{QStringLiteral("#222a3e")}; // pressed, neutral selection, secondary button fill
inline const QColor bg_5{QStringLiteral("#2d3650")}; // track, strong fill
inline const QColor bg_2_hi{QStringLiteral("#171c28")}; // bg_2 + 2 % white: the top edge of a card
inline const QColor bg_3_hi{QStringLiteral("#1f2534")}; // bg_3 + 2 % white: the same for a hovered card
inline const QColor border_1{QStringLiteral("#1a2030")}; // hairline inside a card
inline const QColor border_2{QStringLiteral("#222a3c")}; // card outline, chrome edge
inline const QColor border_3{QStringLiteral("#343f5a")}; // control outline at rest
inline const QColor border_4{QStringLiteral("#445072")}; // control outline on hover, strong divider, popup outline
inline const QColor border_control{QStringLiteral("#5a6890")}; // field outline on hover; check box and switch outline
inline const QColor fg_0{QStringLiteral("#f6f8ff")};
inline const QColor fg_1{QStringLiteral("#dfe4f1")};
inline const QColor fg_2{QStringLiteral("#b9c1d6")};
inline const QColor fg_3{QStringLiteral("#9ba4bd")}; // captions
inline const QColor fg_4{QStringLiteral("#8590ab")}; // meta, group labels
inline const QColor fg_5{QStringLiteral("#667089")}; // disabled, hints
inline const QColor fg_6{QStringLiteral("#3f4860")};
inline const QColor field_bg{QStringLiteral("#0b0f19")}; // input well: darker than the card it sits on
inline const QColor field_fg{QStringLiteral("#dfe4f1")};
inline const QColor field_select_bg{QStringLiteral("#3d7bff")};
inline const QColor field_select_fg{QStringLiteral("#ffffff")};
inline const QColor accent{QStringLiteral("#3d7bff")};
inline const QColor accent_hover{QStringLiteral("#5289ff")};
inline const QColor accent_pressed{QStringLiteral("#2f69e8")};
inline const QColor accent_hi{QStringLiteral("#8db5ff")};
inline const QColor accent_lo{QStringLiteral("#17244a")};
inline const QColor accent_08{QStringLiteral("#101830")};
inline const QColor accent_2{QStringLiteral("#8b5cf6")};       // gradient end (violet)
inline const QColor accent_2_hover{QStringLiteral("#9a70ff")};
inline const QColor accent_2_hi{QStringLiteral("#c4b0ff")};
inline const QColor accent_glow{QStringLiteral("#3d7bff")};   // used translucent for glows
// The gradient under white text or a white icon (primary button, selected
// icon squares, check bubbles, count badges): darker than accent -> accent_2.
inline const QColor accent_fill{QStringLiteral("#2f6bf2")};
inline const QColor accent_fill_2{QStringLiteral("#7c4df0")};
inline const QColor accent_fill_hover{QStringLiteral("#3d78fb")};
inline const QColor accent_fill_2_hover{QStringLiteral("#8a5df6")};
inline const QColor accent_fill_pressed{QStringLiteral("#285fdc")};
inline const QColor accent_fill_2_pressed{QStringLiteral("#6e42dd")};
inline const QColor on_accent{QStringLiteral("#ffffff")};
inline const QColor focus{QStringLiteral("#8db5ff")};
inline const QColor success{QStringLiteral("#2ed39a")};
inline const QColor success_hi{QStringLiteral("#5fe3b4")};
inline const QColor success_bg{QStringLiteral("#0c2a22")};
inline const QColor warn{QStringLiteral("#f5b544")};
inline const QColor warn_hi{QStringLiteral("#ffc966")};
inline const QColor warn_bg{QStringLiteral("#2e2410")};
inline const QColor danger{QStringLiteral("#ff5b6b")};
inline const QColor danger_hi{QStringLiteral("#ff8a95")};
inline const QColor danger_bg{QStringLiteral("#34141c")};
// The fill of a danger button: white on it is 4.7 (hover 4.1, pressed 5.8).
inline const QColor danger_fill{QStringLiteral("#d5354a")};
inline const QColor danger_fill_hover{QStringLiteral("#e0445a")};
inline const QColor danger_fill_pressed{QStringLiteral("#bd2c40")};
inline const QColor scrim{3, 4, 9, 170}; // #030409 at 67 %
inline const QColor highlight{255, 255, 255, 10};      // 1 px inner top edge of a card
inline const QColor highlight_fill{255, 255, 255, 40}; // the same on a gradient fill
inline const QColor shadow{0, 0, 0, 255};              // used translucent under floating popups
// The Home banner's backdrop: deep blue -> violet -> page.
inline const QColor hero_0{QStringLiteral("#14224d")};
inline const QColor hero_1{QStringLiteral("#1c1745")};
inline const QColor hero_2{QStringLiteral("#101222")};
// The PS5 connect banner's backdrop: deep blue-black, a shade lighter than a
// card at the top left (fg_0 on it 16.2 to 17.6, fg_3 6.9 to 7.5).
inline const QColor console_0{QStringLiteral("#121a33")};
inline const QColor console_1{QStringLiteral("#121526")};
inline const QColor console_2{QStringLiteral("#0f121d")};
} // namespace color

// The signature gradient (accent -> accent_2), left to right, for QPainter:
// tints, outlines, progress and decoration.
QLinearGradient accentGradient(const QRectF& r, int alpha = 255);
// The same sweep in the fill tokens: wherever white sits on the gradient.
QLinearGradient accentFillGradient(const QRectF& r, int alpha = 255);
// `c` at `alpha` (0..255): tints and translucent outlines made from a token.
QColor withAlpha(QColor c, int alpha);

// Covers of games without cover art: six gradients of the brand family, so a
// library of them is not one block repeated. Every stop is dark enough for
// white initials; white on the lighter / the darker stop:
//   0 blue 5.7 / 10.3, 1 indigo 6.2 / 11.6, 2 violet 5.8 / 10.9,
//   3 magenta-violet 5.3 / 10.5, 4 teal-blue 5.1 / 9.7, 5 cyan-blue 4.9 / 9.6
namespace cover {
inline constexpr int count = 6;
struct Stops {
    QColor from, to; // top left, bottom right
};
inline const Stops stops[count] = {
    {QColor{QStringLiteral("#2a5fd6")}, QColor{QStringLiteral("#1b3a8f")}}, // blue
    {QColor{QStringLiteral("#4a4fd8")}, QColor{QStringLiteral("#2b2a8c")}}, // indigo
    {QColor{QStringLiteral("#7446dc")}, QColor{QStringLiteral("#432594")}}, // violet
    {QColor{QStringLiteral("#a03cc8")}, QColor{QStringLiteral("#5c2182")}}, // magenta-violet
    {QColor{QStringLiteral("#1a7790")}, QColor{QStringLiteral("#14486b")}}, // teal-blue
    {QColor{QStringLiteral("#1b76b6")}, QColor{QStringLiteral("#164678")}}, // cyan-blue
};
// Gradient `index` (taken modulo `count`), from the top left to the bottom
// right of whatever it fills (object coordinates).
QLinearGradient gradient(int index);
} // namespace cover

// ---- spacing, radii, sizes (px) ----------------------------------------------
namespace space {
inline constexpr int xxs = 2, xs = 4, sm = 8, md = 12, lg = 16, xl = 20, xxl = 24, xxxl = 32, huge = 48;
}
namespace radius {
inline constexpr int xs = 3, sm = 6, control = 8, md = 12, lg = 16, pill = 999;
}
namespace size {
inline constexpr int header = 52;
inline constexpr int sidebar = 244;
inline constexpr int sidebarCompact = 60;
inline constexpr int navItem = 38;
inline constexpr int control = 32;
inline constexpr int controlSm = 28;
inline constexpr int row = 36;
inline constexpr int rowCompact = 28;
inline constexpr int rowJob = 56;
inline constexpr int icon = 16;
inline constexpr int iconSm = 12;
inline constexpr int iconDialog = 20;
inline constexpr int iconEmpty = 24;
inline constexpr int check = 18;
inline constexpr int switchW = 42, switchH = 24, switchKnob = 16;
inline constexpr int progress = 8;
inline constexpr int badge = 22, badgeCount = 18;
inline constexpr int scrollBar = 10; // an 8 px handle, 2 px in from the outer edge
inline constexpr int tableHeader = 32;
inline constexpr int toolTipMax = 360;
inline constexpr int popupShadow = 12; // clear margin around a menu panel, for its painted shadow
inline constexpr int emptyTile = 44, emptyMax = 360;
inline constexpr int dialog = 480;
inline constexpr int dialogWide = 640;
inline constexpr int toast = 360;
inline constexpr int dock = 400;
inline constexpr int formMax = 760;
inline constexpr int settingsList = 200;
inline constexpr int statusBar = 30;
inline constexpr int cover = 40, coverRow = 28;
inline constexpr int focusRing = 2, focusOffset = 2;
inline constexpr int windowMinW = 1100, windowMinH = 700;
inline constexpr int dockBreakpoint = 1180;  // below: dock wraps under the form
inline constexpr int sidebarFoldBelow = 1200; // a narrower window shows the sidebar as its rail
} // namespace size

// ---- typography --------------------------------------------------------------
namespace font {
QFont display();    // 30 px 700, tabular
QFont title();      // 24 px 700
QFont stat();       // 26 px 700, tabular
QFont subtitle();   // 15/20 600
QFont bodyStrong(); // 13/18 600
QFont body();       // 13/18 400
QFont caption();    // 12/16 400
QFont chip();       // 11/14 600: badges and chips
QFont overline();   // 11/14 600 +6 % uppercase
QFont mono();       // 12/18 400 tabular
QFont monoSmall();  // 11/16 400
// Line heights for fixed-height rows. lineDisplay, lineTitle and lineStat
// still hold the heights of the earlier, smaller display styles.
inline constexpr int lineBody = 18, lineCaption = 16, lineTitle = 26, lineSubtitle = 20, lineMono = 18,
                     lineMonoSmall = 16, lineOverline = 14, lineDisplay = 32, lineStat = 28;
} // namespace font

QString hex(const QColor& c);      // "#rrggbb" (or rgba() when translucent)
QString stylesheet();              // the generated application QSS
void apply(QApplication* app);     // fonts, palette, style, QSS
QString sansFamily();              // resolved UI family
QString monoFamily();              // resolved mono family

} // namespace fw::theme
