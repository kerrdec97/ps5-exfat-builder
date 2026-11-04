#pragma once

// Design-system tokens (docs/DESIGN_PROPOSAL.md §3). Every colour, size and
// type style the application uses lives here; the QSS stylesheet is generated
// from these values and nobody writes a hex colour anywhere else.

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace fw::theme {

// ---- colour ------------------------------------------------------------------
namespace color {
inline const QColor bg_0{QStringLiteral("#0d0f12")};
inline const QColor bg_1{QStringLiteral("#131519")};
inline const QColor bg_2{QStringLiteral("#191c21")};
inline const QColor bg_3{QStringLiteral("#20242a")};
inline const QColor bg_4{QStringLiteral("#272b32")};
inline const QColor bg_5{QStringLiteral("#30353c")};
inline const QColor border_1{QStringLiteral("#1e2126")};
inline const QColor border_2{QStringLiteral("#262a30")};
inline const QColor border_3{QStringLiteral("#30353c")};
inline const QColor border_4{QStringLiteral("#3c424a")};
inline const QColor border_control{QStringLiteral("#6a717b")};
inline const QColor fg_0{QStringLiteral("#f3f5f8")};
inline const QColor fg_1{QStringLiteral("#dce0e5")};
inline const QColor fg_2{QStringLiteral("#bcc2ca")};
inline const QColor fg_3{QStringLiteral("#9da4ad")};
inline const QColor fg_4{QStringLiteral("#8a919b")};
inline const QColor fg_5{QStringLiteral("#6b727c")};
inline const QColor fg_6{QStringLiteral("#454b53")};
inline const QColor field_bg{QStringLiteral("#1d2026")};
inline const QColor field_fg{QStringLiteral("#dce0e5")};
inline const QColor field_select_bg{QStringLiteral("#2a6de8")};
inline const QColor field_select_fg{QStringLiteral("#ffffff")};
inline const QColor accent{QStringLiteral("#2a6de8")};
inline const QColor accent_hover{QStringLiteral("#2766d9")};
inline const QColor accent_pressed{QStringLiteral("#2259c0")};
inline const QColor accent_hi{QStringLiteral("#6ea8ff")};
inline const QColor accent_lo{QStringLiteral("#122340")};
inline const QColor accent_08{QStringLiteral("#121a28")};
inline const QColor on_accent{QStringLiteral("#ffffff")};
inline const QColor focus{QStringLiteral("#6ea8ff")};
inline const QColor success{QStringLiteral("#34c48a")};
inline const QColor success_hi{QStringLiteral("#4fd39b")};
inline const QColor success_bg{QStringLiteral("#0f2a20")};
inline const QColor warn{QStringLiteral("#e0a030")};
inline const QColor warn_hi{QStringLiteral("#f0b443")};
inline const QColor warn_bg{QStringLiteral("#2d220c")};
inline const QColor danger{QStringLiteral("#e5484d")};
inline const QColor danger_hi{QStringLiteral("#ff7a7f")};
inline const QColor danger_bg{QStringLiteral("#33161a")};
inline const QColor danger_fill{QStringLiteral("#c62f39")};
inline const QColor danger_fill_hover{QStringLiteral("#b22a33")};
inline const QColor danger_fill_pressed{QStringLiteral("#9e252e")};
inline const QColor scrim{5, 6, 8, 153}; // #050608 at 60 %
} // namespace color

// ---- spacing, radii, sizes (px) ----------------------------------------------
namespace space {
inline constexpr int xxs = 2, xs = 4, sm = 8, md = 12, lg = 16, xl = 20, xxl = 24, xxxl = 32, huge = 48;
}
namespace radius {
inline constexpr int xs = 2, sm = 4, md = 8, pill = 999;
}
namespace size {
inline constexpr int header = 48;
inline constexpr int sidebar = 232;
inline constexpr int sidebarCompact = 56;
inline constexpr int navItem = 36;
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
inline constexpr int switchW = 40, switchH = 20, switchKnob = 12;
inline constexpr int progress = 6;
inline constexpr int dialog = 480;
inline constexpr int dialogWide = 640;
inline constexpr int toast = 360;
inline constexpr int dock = 400;
inline constexpr int formMax = 760;
inline constexpr int settingsList = 200;
inline constexpr int statusBar = 28;
inline constexpr int cover = 40, coverRow = 28;
inline constexpr int focusRing = 2, focusOffset = 1;
inline constexpr int windowMinW = 1100, windowMinH = 700;
inline constexpr int dockBreakpoint = 1180;  // below: dock wraps under the form
inline constexpr int sidebarBreakpoint = 1000; // below: sidebar auto-compacts
} // namespace size

// ---- typography --------------------------------------------------------------
namespace font {
QFont display();    // 26/32 600
QFont title();      // 20/26 600
QFont stat();       // 22/28 600
QFont subtitle();   // 15/20 600
QFont bodyStrong(); // 13/18 600
QFont body();       // 13/18 400
QFont caption();    // 12/16 400
QFont overline();   // 11/14 600 +6 % uppercase
QFont mono();       // 12/18 400 tabular
QFont monoSmall();  // 11/16 400
// Line heights that go with the styles above (for fixed-height rows).
inline constexpr int lineBody = 18, lineCaption = 16, lineTitle = 26, lineSubtitle = 20, lineMono = 18,
                     lineMonoSmall = 16, lineOverline = 14, lineDisplay = 32, lineStat = 28;
} // namespace font

QString hex(const QColor& c);      // "#rrggbb" (or rgba() when translucent)
QString stylesheet();              // the generated application QSS
void apply(QApplication* app);     // fonts, palette, style, QSS
QString sansFamily();              // resolved UI family
QString monoFamily();              // resolved mono family

} // namespace fw::theme
