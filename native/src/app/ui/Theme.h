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
// v5 "Nebula" palette: blue-black surfaces with a cool tint, an electric
// blue -> violet signature gradient (accent -> accent_2) for primary actions,
// selection and brand moments. Text contrast on bg_2: fg_1 14.3:1, fg_3 6.5:1,
// fg_4 5.0:1 (WCAG AA for body text down to fg_4).
namespace color {
inline const QColor bg_0{QStringLiteral("#07090f")}; // chrome: sidebar, header, status bar
inline const QColor bg_1{QStringLiteral("#0b0e17")}; // page
inline const QColor bg_2{QStringLiteral("#11151f")}; // card
inline const QColor bg_3{QStringLiteral("#171c2a")}; // raised control, hover
inline const QColor bg_4{QStringLiteral("#1e2435")}; // pressed, selected neutral
inline const QColor bg_5{QStringLiteral("#283044")}; // track, strong fill
inline const QColor border_1{QStringLiteral("#151a26")};
inline const QColor border_2{QStringLiteral("#1c2231")};
inline const QColor border_3{QStringLiteral("#262d40")};
inline const QColor border_4{QStringLiteral("#333c55")};
inline const QColor border_control{QStringLiteral("#3a4561")};
inline const QColor fg_0{QStringLiteral("#f6f8ff")};
inline const QColor fg_1{QStringLiteral("#dfe4f1")};
inline const QColor fg_2{QStringLiteral("#b9c1d6")};
inline const QColor fg_3{QStringLiteral("#919ab2")};
inline const QColor fg_4{QStringLiteral("#7c859e")};
inline const QColor fg_5{QStringLiteral("#5a627b")};
inline const QColor fg_6{QStringLiteral("#3a4258")};
inline const QColor field_bg{QStringLiteral("#0d111c")};
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
inline const QColor danger_fill{QStringLiteral("#e23d50")};
inline const QColor danger_fill_hover{QStringLiteral("#ef4a5d")};
inline const QColor danger_fill_pressed{QStringLiteral("#c73245")};
inline const QColor scrim{3, 4, 9, 170}; // #030409 at 67 %
} // namespace color

// The signature gradient (accent -> accent_2), left to right, for QPainter.
QLinearGradient accentGradient(const QRectF& r, int alpha = 255);

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
inline constexpr int progress = 6;
inline constexpr int dialog = 480;
inline constexpr int dialogWide = 640;
inline constexpr int toast = 360;
inline constexpr int dock = 400;
inline constexpr int formMax = 760;
inline constexpr int settingsList = 200;
inline constexpr int statusBar = 30;
inline constexpr int cover = 40, coverRow = 28;
inline constexpr int focusRing = 2, focusOffset = 1;
inline constexpr int windowMinW = 1100, windowMinH = 700;
inline constexpr int dockBreakpoint = 1180;  // below: dock wraps under the form
inline constexpr int sidebarBreakpoint = 1000; // below: sidebar auto-compacts
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
