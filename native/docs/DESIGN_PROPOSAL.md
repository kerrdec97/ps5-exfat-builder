# Farewell Edition — design proposal for the native (Qt 6 Widgets) app

Status: proposal for owner review. Canvas with the artboards: https://claude.ai/artifact/LH716oABMWm56ShXXrFDff
(Main = Build image, Fpkg, Backport, Queue, Settings, Logs, States, Connect).
The design system README (Graphite theme) is binding; every value below is either taken from
`tkinter_theme.py` at f151289 or marked **proposed** in section 7.

Reading order for the Qt engineer: 3 (tokens) → 4 (components) → 2 (shell and pages) → 5/6.

---

## 1. Critique of the approved design (f151289)

### 1.1 Keep

| What | Why it works |
|---|---|
| Graphite ladder `bg_0…bg_5`, hairlines `border_1…4`, text ladder `fg_0…fg_6` | Every pair is contrast-checked; hierarchy comes from surfaces, not lines. Carry over unchanged. |
| One accent (#2a6de8) used only for primary action, selection, focus, progress | The single strongest "premium" signal in the system. The old purple/teal retirement was right. |
| Geometry: 32 px controls, 36 px rows, 48 px header, 232/56 px sidebar, radii 4/8/2/pill | Matches Windows 11 and reads dense without feeling cramped. |
| Status = glyph + word + tint (`success/warn/danger` triplets) | Colour-blind safe, scannable. |
| Lucide line icons, 1.75 stroke, 16/12/20/24 sizes, one icon per concept | Consistent on Win 10 and 11; no missing-glyph boxes. |
| Sidebar nav item: 36 px, `bg_3` fill, 3 px accent pill, `accent_hi` icon | Clear selected state; keep it exactly. |
| Card: `bg_2` + `border_2` + radius 8 + 16 px padding | Correct elevation model (surface step, no shadow). |
| Copy rules: sentence case, "…" only for pickers, "you" not "we", errors say what happened + what to do | Keep verbatim. |

### 1.2 Not yet premium (specifics)

| # | Problem in f151289 | Evidence | Fix in this proposal |
|---|---|---|---|
| C1 | Navigation is tool-centric, 14 items in 3 groups incl. "Advanced", "History", "Dump Rename", "Credits", "Report Issue" as top-level destinations. "Advanced" is a dumping ground; "Convert" and "Edit" overlap "Build"; "History" duplicates the queue. | `exfat_builder.py:4113-4131`, `LEGACY_UI_INVENTORY.md §1.2` | 10 destinations in 3 groups by task (§2.1). History folds into Queue (Done filter) and Home (Recent jobs). Dump rename folds into Library. Advanced dissolves into Settings sections and per-page "job settings". Report issue and Help move to the header help menu and About. |
| C2 | `AppHeader` draws the brand glyph in accent, 20 px **bold** (`header.py:49-52`), on `bg_2`, with version badge and subtitle — three rule breaks (700 weight, blue for decoration, header surface). | `ui/shared/header.py` | Header is `bg_0`, brand set in type only (`body-strong fg_0` + `body fg_3`), no badge, no credits line (credits live in About). |
| C3 | `GameHero`: 128 px cover tile + stats strip = the "oversized hero banner" and "KPI tiles" the README tells us to avoid. | `ui/shared/hero.py` | Replaced by a 60 px "Source summary" strip inside the Source card (40 px cover, title, mono meta line, status badge). |
| C4 | Status tone is derived by parsing status strings ("online", "failed"…). Fragile and untranslatable. | `ui/shared/status.py:tone_for` | Status is an enum (`Idle/Queued/Running/Paused/Done/Warning/Failed/Cancelled`) rendered by one `StatusBadge`; text comes from the enum, never the reverse. |
| C5 | Focus is removed on checkboxes/radios (`focusthickness=0`) and nav items have no keyboard focus ring. Violates "Focus: 2 px solid ring, never removed". | `tkinter_theme.py check_style` | Every focusable widget paints the 2 px `focus` ring 1 px outside its bounds (§4.0). |
| C6 | Icons are still passed as emoji at call sites and mapped by `EMOJI_ICON`. | `sidebar.py:_resolve_icon`, `exfat_builder.py:4113` | Icon names only (§5). No emoji anywhere, including log text. |
| C7 | Build has both a unified Build page and legacy per-format sub-tabs (exFAT/FFPKG/PFS) with different layouts. | `tab_exfat.py` docstring vs `tab_unibuild.py` | One Build image page; format is a segmented control; format-specific options live in a collapsible "Job settings" card. |
| C8 | Queue is "scroll-free", no reorder, no per-job cancel/retry, no job detail. | `ui/shared/queue.py` | Queue page with 56 px two-line job rows, drag handle, move up/down, cancel/retry, selected-job detail panel (Queue artboard); a 400 px Jobs dock on every job-creating page (Main/Fpkg artboards). |
| C9 | Settings (1,347 lines) has a rail but no notion of global default vs per-job override, no inline validation, no dependency status. | `ui/tab_settings.py` | Settings = defaults only; job pages override with a visible "Overridden for this job" marker and "Use default" link; inline validation under the control; dependency rows with status (§4.21–4.22). |
| C10 | Home is a launcher: "What would you like to do?" + six action cards. | `ui/tab_home.py` | Home = status strip (console, queue, disk) + Recent jobs table + one "Continue" card for the last unfinished job. No marketing question, no tiles. |
| C11 | Progress has no indeterminate state and no stage information; ETA appears in several formats. | `progress_style()` | ProgressBar with determinate/indeterminate/done/failed, a Stage stepper, one ETA format ("About 1 min left"). |
| C12 | Logs are per-page text wells with ad-hoc colouring (CRT green retired, but still no levels/sources/filters). | `log_view.py`, `tab_klog.py` | One central Logs page: structured rows (time/level/source/message), level segmented filter, source dropdown, search, Follow switch, export; per-page "Activity" wells show the last 4–6 lines and link to Logs. |
| C13 | Overline UPPERCASE used for sidebar footer "SYSTEM STATUS" with no live data. | `sidebar.py` footer | Sidebar footer is the version caption only; live status moves to the header chips and the 28 px status bar. |
| C14 | No window-level status bar in the design system, yet the app has one (28 px PS5/FTP/queue). | `LEGACY_UI_INVENTORY.md §1.1` | Status bar specified (§4.2, proposed `size-statusbar` 28). |

---

## 2. Information architecture

### 2.1 Navigation (sidebar, top to bottom)

| Group (overline) | Item | Icon | Page contents | Replaces (legacy) |
|---|---|---|---|---|
| — | Home | `home` | Status strip (console, queue, free space), Recent jobs (last 10, from history), Continue card | Home |
| WORKFLOWS | Build image | `build` | Source · Output (format segmented: exFAT / FFPKG (UFS2) / FFPFSC) · Verification and job settings · Jobs dock | Build, exFAT, FFPKG, PFS, Convert |
| WORKFLOWS | FPKG | `package` | Source (Folder / .exfat / .ffpfsc / .ffpkg / .gp5) · Package details · Compression · PlayGo and param.json · Sony SDK · Dependencies · Build · Jobs | (new, from the FPKG builder screenshots) |
| WORKFLOWS | Extract & verify | `extract` | Source image or .pkg · Destination · Verify only toggle · Jobs dock | Extract, Edit › File manager (read side) |
| WORKFLOWS | Backport | `backports` | Game library table · Target · Options · Folders · Activity | Backports |
| TOOLS | Queue | `queue` (layers) | All jobs with filters All/Running/Queued/Done/Failed, reorder, cancel, retry, job detail | Queue panel, History |
| TOOLS | PS5 connect | `ps5` | Tabs: FTP browser · Payloads · Kernel log | PS5 › FTP, Payloads, Kernel log, Manager |
| TOOLS | Library | `library` | Games (dumps + built images), covers, rename, scan locations | Library, Dump Rename |
| SYSTEM | Logs | `logs` (file-text) | Central structured log | Output log drawer, per-tab consoles |
| SYSTEM | Settings | `settings` | 13 sections (§2.3) | Settings, Advanced, Language, Help (moved) |
| (footer) | About | `credits` | Version, credits, licences, Report issue, Check for updates, Keyboard shortcuts | Credits, Report issue |

Rules
- The Queue item carries a count badge (`b-n`, 18 px) when jobs exist; it never turns blue.
- ShadowMount+, MicroMount, Y2JB: not in the first native release's nav. If kept, they are tabs of PS5 connect (Tabs component), never top-level items.
- Compact rail (56 px): icons only, label as tooltip, group labels hidden, same selection treatment.
- Selected nav persists (`ui.lastPage`); first run opens Home.

### 2.2 Shell

```
┌ Native Windows title bar (Qt default frame; no custom chrome) ────────────────────┐
│ AppHeader 48  [≡] exFAT Image Builder  Farewell Edition        [chips] [?]       │
├──────────┬────────────────────────────────────────────────────────────────────────┤
│ Sidebar  │ Page (bg_1), gutter 24 px                                              │
│ 232/56   │   PageHeader (title 20/26, caption, actions right)                     │
│ bg_0     │   Content: form column (max 760) + optional right column 400 (dock)    │
├──────────┴────────────────────────────────────────────────────────────────────────┤
│ StatusBar 28: ● state text · D: free · ····· · engine versions                   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

- Header chips: `PS5 connected · 192.168.1.40` (success badge) and `1 running · 3 queued` (info badge). Chips are read-only; clicking opens PS5 connect / Queue.
- Window minimum 1100×700; artboards drawn at 1440×920. Below 1180 px the right column wraps under the form (flex-wrap), and the sidebar auto-compacts below 1000 px.
- Keyboard: `Ctrl+B` run the current page's primary action, `Ctrl+Q` quit, `Esc` cancels a running job only after the confirm dialog, `Ctrl+L` Logs, `Ctrl+,` Settings, `F5` rescan on Library/Backport. Every shortcut must be wired or it is not listed.

### 2.3 Settings sections (left list, 200 px, 32 px rows)

General · Build · FPKG · PFS · PlayGo · Backport · Firmware & SDK · Tools · Performance · Storage · Verification · Logging · Advanced.
A section with a dependency problem shows a warn count badge in the list (see Settings artboard, "Tools 1").
Settings are *defaults*. A page can override a setting marked `perJob` in its schema; the page shows the `SettingRow` with the override marker (§4.21). "Reset section" per section; "Reset all…" confirms in a dialog.

### 2.4 Page templates

| Template | Used by | Layout |
|---|---|---|
| Form + dock | Build image, FPKG, Extract & verify | Left: cards, max 760 px. Right: 400 px column (Jobs dock, Dependencies, Build summary). |
| Table + panel | Backport, Library, PS5 connect › FTP | Left: card with toolbar + table + footer line. Right: 400 px options column (Backport) or 280 px tree (FTP). |
| List + detail | Queue | Left: filters + 56 px job rows. Right: 400 px Job details. |
| Sections | Settings | Left: 200 px section list. Right: section header + cards of SettingRows, max 760 px. |
| Full table | Logs | Filter bar, log rows, selected-line detail, footer line. |
| Specimens | (States artboard) | Reference only, not a page. |

---

## 3. Tokens

Qt naming: one header `Theme.h` with `namespace fw::theme`. Colours as `QColor` constants named exactly like the README keys (`bg_0` → `theme::bg_0`), sizes as `int` constants, type styles as `QFont` factories. The QSS stylesheet is a template with `{bg_0}`-style placeholders substituted once at start-up; nobody writes a hex in a `.qss` or `.cpp` outside `Theme.cpp`.

### 3.1 Colour (all from `tkinter_theme.py` f151289)

| Token | Hex | Use | Qt notes |
|---|---|---|---|
| `bg_0` | `#0d0f12` | Window chrome: header, sidebar, status bar, log wells, drop zones | `QPalette::Window` for the main window |
| `bg_1` | `#131519` | Content canvas | page `QWidget` autofill |
| `bg_2` | `#191c21` | Cards, tables, dialogs, disabled control fill | `QPalette::Base` for dialogs |
| `bg_3` | `#20242a` | Secondary button, selected nav, menus, hover rows, neutral badge, cover placeholder | |
| `bg_4` | `#272b32` | Hover on `bg_3`, scrollbar thumb | |
| `bg_5` | `#30353c` | Pressed, progress track, segmented selected fill | |
| `border_1` | `#1e2126` | Row dividers | |
| `border_2` | `#262a30` | Card, header, sidebar, status bar hairlines | |
| `border_3` | `#30353c` | Buttons, menus, dialogs, segmented frame, toast | |
| `border_4` | `#3c424a` | Hover borders | |
| `border_control` | `#6a717b` | Input, checkbox, switch outline (3:1) | |
| `fg_0` | `#f3f5f8` | Headings, selected nav label, primary table column | `QPalette::WindowText` for titles |
| `fg_1` | `#dce0e5` | Body, control labels | `QPalette::Text` |
| `fg_2` | `#bcc2ca` | Emphasised secondary, nav labels at rest, info-bar body | |
| `fg_3` | `#9da4ad` | Captions, help text, idle icons, table headers | |
| `fg_4` | `#8a919b` | Meta, placeholders, overlines, log timestamps (not on `bg_4`) | `QPalette::PlaceholderText` |
| `fg_5` | `#6b727c` | Disabled text and icons, sidebar version | `QPalette::Disabled` roles |
| `fg_6` | `#454b53` | Decorative only (drag handles at rest may use `fg_5`, never `fg_6` for anything readable) | |
| `field_bg` | `#1d2026` | Inputs, dropdowns, segmented track, checkbox/switch off fill | |
| `field_fg` | `#dce0e5` | Input text | |
| `field_select_bg` | `#2a6de8` | Text selection | `QPalette::Highlight` |
| `field_select_fg` | `#ffffff` | Selected text | `QPalette::HighlightedText` |
| `accent` | `#2a6de8` | Primary button, checked check/switch, progress fill, nav pill, tab indicator | |
| `accent_hover` | `#2766d9` | Primary hover | |
| `accent_pressed` | `#2259c0` | Primary pressed | |
| `accent_hi` | `#6ea8ff` | Links, selected nav icon, info glyph, running-job icon, info badge text | |
| `accent_lo` | `#122340` | Selected rows, running-job block in the Jobs dock, info bar/badge background | |
| `accent_08` | `#121a28` | Reserved (very faint tint); not used in the artboards | |
| `on_accent` | `#ffffff` | Text/icon on `accent` and `danger_fill` | |
| `focus` | `#6ea8ff` | 2 px focus ring | |
| `success` / `success_hi` / `success_bg` | `#34c48a` / `#4fd39b` / `#0f2a20` | Mark · text · tint | |
| `warn` / `warn_hi` / `warn_bg` | `#e0a030` / `#f0b443` / `#2d220c` | Mark · text · tint | |
| `danger` / `danger_hi` / `danger_bg` | `#e5484d` / `#ff7a7f` / `#33161a` | Mark · text · tint; error outline uses `danger` | |
| `danger_fill` / `danger_fill_hover` | `#c62f39` / `#b22a33` | Destructive button fill / hover (pressed: `#9e252e`, **proposed**) | |
| `scrim` | `#050608` at 60 % alpha (**proposed** alpha; the Tk token had none) | Modal backdrop | translucent overlay widget |

Contrast grounds used in the artboards (all ≥ 4.5:1): `fg_4` on `bg_0/bg_1/bg_2` · `fg_3` on `bg_2/bg_3` · `success_hi` on `success_bg` · `warn_hi` on `warn_bg` · `danger_hi` on `danger_bg` · `accent_hi` on `accent_lo` · `on_accent` on `accent` (4.7:1) · `fg_5` only for disabled.

### 3.2 Typography

Family: `Segoe UI Variable Text` → `Segoe UI` → system sans. Display: `Segoe UI Variable Display` for `display`/`title`/`stat`. Mono: `Cascadia Mono` → `Consolas`. Weight 600 only (QFont::DemiBold); never 700.

| Style | px / line | Weight | Qt (`QFont` factory) | Use |
|---|---|---|---|---|
| `display` | 26 / 32 | 600 | `font::display()` | Home title only |
| `title` | 20 / 26 | 600 | `font::title()` | Page header title, dialog title |
| `stat` | 22 / 28 | 600 | `font::stat()` | Home status numbers |
| `subtitle` | 15 / 20 | 600 | `font::subtitle()` | Card titles, empty-state title, dock title |
| `body-strong` | 13 / 18 | 600 | `font::bodyStrong()` | Primary table column, job title, primary button, selected nav/tab/segment |
| `body` | 13 / 18 | 400 | `font::body()` | Everything else |
| `caption` | 12 / 16 | 400 | `font::caption()` | Help text, meta, badges, status bar, table headers, toast body |
| `overline` | 11 / 14 | 600, +0.06 em, uppercase | `font::overline()` | Sidebar group labels, "Job settings"/"Log" group labels in detail panels |
| `mono` | 12 / 18 | 400, tabular figures | `font::mono()` | Paths, IDs, hashes, sizes in tables, log lines |
| `mono-sm` | 11 / 16 | 400 | `font::monoSmall()` | Dense log wells (Jobs dock log tail), status-bar paths |

Qt: pixel sizes via `QFont::setPixelSize`, line height via `QTextBlockFormat`/fixed widget heights; `QFont::setLetterSpacing(PercentageSpacing, 106)` for overline. Tabular figures: `QFont::setFeature("tnum", 1)` (Qt 6.7+), otherwise Cascadia Mono is already tabular.

### 3.3 Spacing, radii, sizes

| Token | px | Token | px |
|---|---|---|---|
| `space-xxs` | 2 | `radius-xs` | 2 (progress, nav pill) |
| `space-xs` | 4 | `radius-sm` | 4 (controls, nav item, segmented) |
| `space-sm` | 8 | `radius-md` | 8 (cards, dialogs, menus, toasts, log wells, drop zones) |
| `space-md` | 12 | `radius-pill` | 999 (badges, switch) |
| `space-lg` | 16 | `size-header` | 48 |
| `space-xl` | 20 | `size-sidebar` / `size-sidebar-compact` | 232 / 56 |
| `space-xxl` | 24 (page gutter) | `size-nav-item` | 36 |
| `space-xxxl` | 32 | `size-control` / `size-control-sm` | 32 / 28 |
| `space-huge` | 48 | `size-row` / `size-row-compact` | 36 / 28 |
| | | `size-row-job` | 56 (**proposed**, two-line queue row) |
| | | `size-icon` / `size-icon-sm` / `size-icon-dialog` / `size-icon-empty` | 16 / 12 / 20 / 24 |
| | | `size-check` | 18 |
| | | `size-switch` | 40 × 20, knob 12 |
| | | `size-progress` | 6 |
| | | `size-dialog` | 480 |
| | | `size-toast` | 360 |
| | | `size-dock` | 400 (**proposed**) |
| | | `size-form-max` | 760 (**proposed**, README said 720; 760 fits two 300 px fields + gap at 1440) |
| | | `size-settings-list` | 200 (**proposed**) |
| | | `size-statusbar` | 28 (**proposed**) |
| | | `size-cover` / `size-cover-row` | 40 / 28 |
| | | `size-focus-ring` | 2, offset 1 |

Layout rhythm: page header → cards `space-lg` apart; card padding `space-lg`; card title → body `space-md`; form rows `space-md`; label → field `space-xs`; buttons in a row `space-sm`; icon → label inside a button `space-sm`; table cell gap `space-sm`/`space-md`.

---

## 4. Component spec

### 4.0 Rules that apply to every component

| Rule | Spec |
|---|---|
| Focus | 2 px solid `focus`, 1 px outside the widget's rounded rect (so the ring is radius+1). Painted in `paintEvent` when `hasFocus() && focusReason is keyboard` (use `QApplication::navigationMode`/`style()->styleHint(SH_...)`; simplest: paint on `hasFocus()` always for inputs, and on keyboard focus for buttons/rows). QSS cannot draw an outside ring: use `outline` is unsupported, so subclass or a `QProxyStyle::drawPrimitive(PE_FrameFocusRect)`. |
| Hover | Surface one step lighter. No transitions; no cursor change except links (`PointingHandCursor`). |
| Disabled | `bg_2` fill, `border_2` (buttons `border_3`), `fg_5` text and icons, no hover. |
| Icons | 16 px in controls/rows, 12 px in badges and help, 20 px in dialog titles, 24 px in empty states. Rendered from SVG at device pixel ratio with `QIcon::fromTheme` disabled; colour via a tinting `QIconEngine` (`fg_3` rest, `fg_0` hover, `accent_hi` selected, status mark colours). |
| Shadows | None. QSS has no `box-shadow`; nothing is lost. Elevation is surface step + hairline. |
| Motion | None except the indeterminate progress sweep (1.4 s, linear) and nothing else animates. |
| Density | Not in scope for v1; sizes are constants so a compact mode can later swap `size-control`/`size-row`. |
| Text | Left aligned. Centre only in empty states. Ellipsis in the middle for paths (`Qt::ElideMiddle`), at the end for titles. |

QSS strategy: one generated stylesheet for `QPushButton`, `QLineEdit`, `QComboBox`, `QScrollBar`, `QMenu`, `QToolTip`, `QHeaderView`, `QTabBar`, `QCheckBox` (label only). Everything that needs an outside focus ring, a pill, a sweep or a tint is a small custom `QWidget` subclass (`fw::widgets::*`). Variants are `Q_PROPERTY`/dynamic properties (`variant="primary"`) selected in QSS with `QPushButton[variant="primary"]`; after changing a property call `style()->unpolish(w); style()->polish(w)`.

### 4.1 Sidebar (`fw::widgets::Sidebar`)

| Part | Spec |
|---|---|
| Rail | 232 px (56 compact), `bg_0`, right hairline `border_2`, padding 12 px top/bottom, 8 px sides, items 4 px apart. |
| Group label | `overline` `fg_4`, padding 12 px top, 12 px left, 4 px bottom. Hidden in compact. |
| Item | 36 px, radius 4, 16 px icon `fg_3`, label `body` `fg_2`, padding-left 12, gap 10. Optional trailing count badge (18 px tall). |
| Hover | `bg_2` fill, icon and label `fg_0`. |
| Selected | `bg_3` fill, label `body-strong` `fg_0`, icon `accent_hi`, 3 × 16 px `accent` pill, radius 2, at x = 0 vertically centred. |
| Pressed | `bg_4`. |
| Focus | ring per 4.0. Arrow keys move, Enter/Space select, `Ctrl+1…9` jump (**proposed**). |
| Footer | spacer, About item, `caption` `fg_5` "Version 4.0.0". |
| Toggle | Header `menu` icon button; state persisted `ui.sidebarCompact`. Compact items show label as tooltip after 450 ms. |
| Qt | Custom `QWidget` with a `QVBoxLayout` of `NavItem` widgets (QAbstractButton subclass, checkable, exclusive `QButtonGroup`). Not `QListView` (the pill and badge are easier painted). |

### 4.2 AppHeader and StatusBar

| Part | Spec |
|---|---|
| AppHeader | 48 px, `bg_0`, bottom hairline `border_2`, padding 0 12 0 8. Left: `IconButton menu`, brand "exFAT Image Builder" `body-strong fg_0` + "Farewell Edition" `body fg_3`, baseline aligned, gap 8. Right: status chips (StatusBadge, read-only, clickable to their page), `IconButton help` (menu: Keyboard shortcuts, Open logs folder, Report issue…, About). |
| StatusBar | 28 px, `bg_0`, top hairline `border_2`, padding 0 16, `caption fg_3`, items 16 px apart. Left: 12 px status mark + text (e.g. "Building Ghost of Yotei · 63 %", "Ready", "1 setting needs attention"); then `mono-sm` free-space; right: engine versions or context text. Never more than four items. |
| Qt | Plain `QFrame`s with `QHBoxLayout`; the window's `QStatusBar` is not used (its size grip and item borders fight the design). |

### 4.3 PageHeader

Title `title` `fg_0` (margin 0), caption `caption` `fg_3` 2 px below, max 2 lines. Actions right-aligned, 8 px apart, at most one primary. Vertical padding: 20 px above (page gutter top is 20, sides/bottom 24), 16 px below before the first card. Optional Tabs row directly under the header (PS5 connect).

### 4.4 Card (`fw::widgets::Card`)

`bg_2`, 1 px `border_2`, radius 8, padding 16, internal gap 12. Title `subtitle fg_0`; optional right slot (badge, subtle button). Variants: `table` (padding 0; header row 12/16 padded with bottom hairline; footer caption row), `inset` (log wells: `bg_0`, `border_2`, radius 8, padding 8 12). Cards never nest more than one level (inset inside card is the limit). Qt: `QFrame` with `setFrameShape(NoFrame)`, painted rounded rect; children via layouts. Rounded corners clip nothing (no `QGraphicsEffect`); inner inset wells are painted, not clipped.

### 4.5 Button (`QPushButton` + `variant` property)

| Variant | Fill | Border | Text | Hover | Pressed | Disabled |
|---|---|---|---|---|---|---|
| `secondary` (default) | `bg_3` | `border_3` | `fg_1` body | `bg_4` / `border_4` | `bg_5` | `bg_2` / `border_2` / `fg_5` |
| `primary` | `accent` | `accent` | `on_accent` body-strong | `accent_hover` | `accent_pressed` | `bg_2` / `border_2` / `fg_5` |
| `danger` | `danger_fill` | same | `on_accent` body-strong | `danger_fill_hover` | `#9e252e` (**proposed**) | as above |
| `subtle` | transparent | transparent | `fg_2` | `bg_3`, text `fg_0` | `bg_4` | `fg_5` |

Size: 32 px tall, padding 0 12, radius 4, icon 16 px left of label with 8 px gap; `sm` = 28 px, padding 0 8. Min width 64 unless icon-only. One primary per view; destructive actions are `danger` and always sit right-most in a dialog/toolbar. "…" suffix only when a picker or dialog opens. Qt: QSS for all four variants; focus ring by `QProxyStyle`.

### 4.6 IconButton (`QToolButton`, autoRaise)

32 × 32 (`sm` 28 × 28), radius 4, icon 16 px `fg_3`. Hover `bg_3` + icon `fg_0`; pressed `bg_4`; checked (toggle) `bg_3` + `accent_hi` icon; disabled `fg_5`. Always has `toolTip` and `accessibleName`. In toasts and info bars: 24 × 24 with a 14 px icon.

### 4.7 TextInput (`QLineEdit`)

32 px, radius 4, `field_bg`, 1 px `border_control`, padding 0 10, text `field_fg` body (mono for paths/IDs), placeholder `fg_4`. Hover: `border_4`→ no — inputs keep `border_control`; hover changes nothing (**decision**: inputs show state by focus/error only). Focus: ring per 4.0 (border stays). Error: border `danger`, message row under the field: 12 px `error` glyph + `caption danger_hi`, 4 px gap. Read-only: border `border_3`, text `fg_2`, no focus ring on click (still focusable). Disabled: `bg_2`, `border_3`, `fg_5`. Leading icon slot (search) 16 px `fg_3`, trailing unit caption ("%"). Path fields pair with a `Browse…` secondary button 8 px to the right; the pair is a `PathField` composite that also accepts drops.

### 4.8 Dropdown (`QComboBox`, non-editable)

Same metrics as TextInput; trailing `chevron_down` 16 px `fg_3`. Popup: `bg_3`, `border_3`, radius 8, 4 px padding, items 32 px, hover `bg_4`, selected row `accent_lo` with `fg_0` and a 16 px `check` icon. Max 12 visible items then scroll. Qt: set `view()` to a `QListView` styled in QSS; the popup frame via `QComboBox QAbstractItemView`. Option descriptions (e.g. "SDK 4 · FW 4.00+") are part of the item text, not a second line.

### 4.9 Checkbox (`QCheckBox`, custom indicator painted)

18 × 18, radius 4, `field_bg` + `border_control`; checked `accent` fill, 12 px `check` `on_accent`; indeterminate: 10 × 2 px `on_accent` bar. Hover: border `border_4` (unchecked) / `accent_hover` (checked). Label `body fg_1`, 8 px gap. Disabled: `bg_2`/`border_3`, label `fg_5`. Used for multi-select in tables (header checkbox = indeterminate) and for options inside dialogs. For on/off settings use Switch.

### 4.10 Switch (`fw::widgets::Switch`, QAbstractButton)

40 × 20 pill, knob 12 px. Off: `field_bg`, `border_control`, knob `fg_3` at x = 3. On: `accent` fill and border, knob `on_accent` at x = 23. Hover: border `border_4` / `accent_hover`. Disabled: `bg_2`/`border_3`, knob `fg_5`. No animation (knob jumps). Label lives in the SettingRow, not inside the switch; the switch exposes the row label as its accessible name.

### 4.11 Segmented control (`fw::widgets::Segmented`)

Frame: `field_bg`, 1 px `border_3`, radius 4, padding 2, segments 2 px apart. Segment: 26 px tall, padding 0 10, radius 3, text `body fg_3`, optional 16 px icon. Hover `bg_3` + `fg_1`. Selected: `bg_5`, `body-strong fg_0`, icon `fg_0`. Focus ring on the frame; Left/Right move selection. 2–5 segments; more → Dropdown. Used for: image format, source type, package type, compression preset, AMPR/PlayGo, filter sets (Queue, Logs level). Counts in filter segments are plain text ("All 7").

### 4.12 Tabs (`QTabBar` styled, or custom)

In-page navigation only (PS5 connect). 36 px tall, text `body fg_3`, padding 0 4, gap 16, bottom 2 px `accent` indicator on the selected tab (`fg_0`, `body-strong`), hover `fg_1`. Tab row has a `border_2` bottom hairline. Optional trailing badge ("live"). No close buttons, no scroll arrows (max 5 tabs).

### 4.13 Table (`QTableView` + custom delegate, or `fw::widgets::RowList` for ≤ 200 rows)

| Part | Spec |
|---|---|
| Header | 32 px, `caption fg_3`, sentence case, bottom hairline `border_2`, sortable columns show a 12 px chevron. |
| Row | 36 px, bottom hairline `border_1`, cell padding 0 8 (first 12), gap 8–12. Primary column `body-strong fg_0`; IDs/sizes/dates `mono fg_1`/`fg_3`; status column = StatusBadge; trailing 28 px `more` IconButton. |
| Hover | `bg_3`. Selected: `accent_lo` (text colours unchanged). Keyboard focus row: ring per 4.0 inset by 1. |
| Covers | 28 px rounded 4 thumbnails in table rows; placeholder tile `bg_3` with 14 px `image` icon `fg_5`. |
| Selection | Leading checkbox column (36 px) for multi-select tables (Backport, Library); header checkbox = select all / none. |
| Empty | EmptyState inside the table card body. |
| Footer | Caption row: counts left, selection summary right. |
| Width | Columns: fixed for checkbox/cover/status/actions; title column stretches; table scrolls horizontally inside the card below 760 px. |
| Qt | `QTableView` with `QHeaderView` QSS, a `QStyledItemDelegate` painting badges/covers; `setShowGrid(false)`, alternating colours off, `SelectRows`, hairlines painted by the delegate. |

### 4.14 ProgressBar (`fw::widgets::ProgressBar`)

6 px tall, radius 2, track `bg_5`, fill `accent`. Determinate: width = value; a `mono fg_0` percentage sits 12 px right ("63 %", with a thin space). Indeterminate: a 35 %-wide `accent` segment sweeps left→right every 1.4 s, linear, restarting at −35 %. Done: fill `success`, 100 %; Failed: fill `danger` frozen at the failing value; Paused: fill `fg_4` (**proposed**). Secondary line below (`caption fg_2`): "Copying files · 28.4 GB of 48.2 GB · 418 MB/s · About 1 min left". Qt: custom paint with a `QTimer` (16 ms) only while indeterminate and visible.

Stage stepper (part of the progress block): row of `caption` steps 12 px apart; done = 12 px `check` `success_hi`; current = filled `circle` glyph, `body-strong fg_0`; pending = outline `circle` `fg_3`. Optional duration after done steps ("Mount 0:04").

### 4.15 StatusBadge (`fw::widgets::Badge`)

20 px tall pill (radius 999), padding 0 8, `caption`, 12 px icon with 6 px gap. Tones: `ok` (`success_bg`/`success_hi`), `warn`, `err`, `info` (`accent_lo`/`accent_hi`), `neutral` (`bg_3`/`fg_2`). The icon is mandatory for ok/warn/err/info (check / warning / error / clock-or-info); neutral may be text-only. Text is the state word plus an optional detail: "Running · 63 %", "Queued · 2nd", "Failed: eboot.bin missing" (≤ 32 chars, else elide and tooltip). Count badge variant: 18 px, padding 0 6, neutral.

### 4.16 Notification: InfoBar and Toast

| | InfoBar (inline) | Toast (overlay) |
|---|---|---|
| Surface | tint (`accent_lo` / `warn_bg` / `danger_bg` / `success_bg`), radius 8, padding 10 12, 1 px border same as tint | `bg_3`, `border_3`, radius 8, padding 12, width 360 |
| Content | 16 px glyph (mark colour) + body `fg_1` (title `body-strong` for errors), optional links, optional 24 px dismiss | 16 px glyph + title `body-strong fg_0` + `caption fg_2` body + up to two links; 24 px dismiss |
| Placement | In the card it concerns (space error in Output card; SDK notice in Sony SDK card) | Bottom-right of the window, 16 px from the edges, stack upward, newest at the bottom, max 3 visible |
| Lifetime | Until the condition clears | success 6 s; warning/error until dismissed; hover pauses the timer |
| Qt | `QFrame` | Frameless child `QWidget` of the main window, raised, repositioned on resize; no fade. |

### 4.17 Dialog (`QDialog`, modal)

Over a `scrim` overlay (`#050608` @ 60 %) covering the main window. Dialog 480 px wide (confirm) or 640 (choose/options), `bg_2`, `border_3`, radius 8, padding 20, gap 12. Title row: 20 px icon (warning/error/info) + `title fg_0`. Body `body fg_1`; technical strings in `mono`. Optional checkbox line. Button row right-aligned, 8 px gap, 8 px extra top padding: secondary "safe" action first, primary/danger last and default-focused only when non-destructive (destructive dialogs focus the safe button). Esc = safe action. Qt: frameless `QDialog` with `Qt::Dialog | Qt::FramelessWindowHint`, the scrim is a translucent widget over the central widget; keep Windows' own title bar off dialogs (they are in-window sheets).

### 4.18 Log view (`fw::widgets::LogView`)

| Part | Spec |
|---|---|
| Rows | `mono` 12/18, min 28 px, padding 4 12, `border_1` dividers. Columns: time 88 px (`fg_4`, `HH:MM:SS.mmm`), level 92 px (12 px glyph + word, sans caption), source 120 px (`fg_3`), message (stretch, elided; full text in the detail pane). |
| Levels | Debug `fg_3` + `terminal` glyph; Info `fg_1` + `info` glyph `accent_hi`; Warning `warn_hi` text + `warning`; Error `danger_hi` text + `error`. Never colour the whole row. |
| Filter bar | Level segmented (All / Info / Warnings n / Errors n), Source dropdown, Range dropdown (This session / Today / All), search input 260 px, Follow switch. |
| Detail pane | Selected line: overline "Selected line", meta caption, full message `mono fg_1` wrapped (`pre-wrap`), links Open job / Copy line. |
| Footer | "Showing 1,284 of 18,442 lines · filtered by …" left, file path and size (`mono-sm`) right. |
| Inset wells | Pages show an "Activity" inset (`bg_0`, radius 8, 4–6 lines `mono`/`mono-sm`, timestamps `fg_4`, warnings in `warn_hi`) with "Open in Logs". |
| Qt | `QListView` over a ring-buffer model (virtualised; 200k lines), delegate paints columns; Follow = auto-scroll when the view is at the bottom. Export writes plain text with the same columns, tab-separated. |

### 4.19 Queue row and Jobs dock

Queue row (56 px, Queue page): grid `24 (grip) · 20 (state icon) · title/meta (stretch) · progress or note 220 · badge 150 · actions 100`, padding 8 12 8 8, `border_1` divider. Title `body-strong fg_0`, meta `mono fg_3` ("Build image · exFAT · D:\Dumps\PPSA01325 → D:\Images"). Grip `grip` 16 px `fg_5` (shown only on reorderable rows: queued). State icon: running = filled circle `accent_hi`; queued = outline circle `fg_3`; done = `success` check-circle; failed = `danger` x-circle; paused = `pause_circle` `fg_3`. Actions by state: running → pause, cancel, more; queued → move up, move down, remove; failed → `Retry` sm button + more; done → show in folder, more. Selected row `accent_lo`; Enter opens the detail panel. Drag-reorder with `QDrag` inside queued rows only; drop indicator = 2 px `accent` line.

Jobs dock (400 px aside on Build/FPKG/Extract): Card variant `table`. Header 12/16 "Jobs" + count badge, pause-queue and more IconButtons. Running block: `accent_lo` background, padding 12 16, title + `mono` meta, pause/cancel sm IconButtons, ProgressBar + percent, caption line, stage stepper. Queued rows 36 px: grip, "Title · ID · format · size" (title `fg_1`, rest `mono fg_3`), neutral badge, more. Footer: caption "Queue finishes in about 24 min" + link "Open queue". Collapsed when the window is narrower than 1180 px (the dock becomes a 36 px summary strip under the page header, **proposed**).

### 4.20 EmptyState and ErrorState

EmptyState (centred, inside a card or table body, padding 40 24): 56 px tile `bg_3` radius 8 with 24 px icon `fg_3`; title `subtitle fg_0`; one caption line ≤ 320 px; up to two actions (one may be primary when it is the only sensible next step). Copy: what is empty + how to fill it ("No jobs in the queue" / "Add a dump on Build image, FPKG or Backport…").

ErrorState (blocking error for a page or card): 40 px tile `danger_bg` with 20 px `error` icon `danger`; title `subtitle`; body `fg_2` says what happened and what to do; `mono caption` technical line; actions right-aligned: subtle "Open log", secondary "Check again", primary fix action. Raw exceptions never appear in the body; they go to the mono line (one line) and the Logs page.

### 4.21 SettingRow (`fw::widgets::SettingRow`)

min 36 px, padding 6 0, `border_1` divider between rows inside a card, none after the last. Left: label `body fg_1` + optional help `caption fg_3` (2 px below); right: the control (Switch, Dropdown 160–200 px, TextInput 160–360 px, Segmented, PathField 360 px). Rows align controls on the right edge. Error: control in error state + message under the control (left-aligned with it). Disabled row: label and help `fg_5`, control disabled, help explains why ("Only available when an image is mounted."). Dependent rows (`enabledWhenKey`) are disabled, not hidden.

Override marker (job pages only): after the label, a neutral badge with the 12 px `advanced` (sliders) icon and the text "Overridden for this job"; the help line becomes "Default is 128 KB. Use default" where "Use default" is a link that restores the default and removes the marker. In the Queue detail panel the marker is shortened to "Overridden". In Settings itself there is no marker; the section header caption says "n settings differ from the defaults" and "Reset section" restores them.

### 4.22 DependencyRow (`fw::widgets::DependencyRow`)

min 36 px, padding 6 0, `border_1` dividers. 16 px status icon (`success` check-circle / `warn` warning / `danger` x-circle / `fg_3` loader while probing), name `body fg_1`, detail `caption fg_3` (version, path in `mono`, or "Not found. Optional: only for SDK builds."), right: StatusBadge (Ready / Version unknown / Not found / Checking…) and, when the probe returns `howToFix`, a link ("Set path…", "Install…", "Change…"). The card header shows "n of m ready" as a badge and a subtle "Check again" button. Probes never block the UI (run via `QtConcurrent`, rows show the loader state).

### 4.23 Drop zone and Source summary

Drop zone: any job page accepts folder/file drops on its whole content area; while dragging, the Source card shows a 2 px dashed `accent_hi` inner border (radius 8) and the caption "Drop to use as source" — no separate always-visible drop well (the f151289 drop strip is removed). Source summary strip (inside the Source card): `bg_0`, `border_2`, radius 8, padding 10 12, 40 px cover tile, title `body-strong fg_0`, `mono fg_3` meta "PPSA01325 · v01.000.000 · 48.2 GB · 18,442 files · SDK 9.00", subtle "Rescan" button; status badge in the card header ("Ready to build", "Scanning…", "Not a game dump").

### 4.24 Scrollbar, Tooltip, Menu

Scrollbar: 10 px, thumb `bg_4` radius 3 with 2 px margin, hover/pressed `bg_5`, no arrows, track transparent (QSS `QScrollBar`). Tooltip: `bg_3`, `border_3`, radius 4, padding 4 8, `caption fg_1`, 450 ms delay (`QToolTip` QSS). Menu: `bg_3`, `border_3`, radius 8, padding 4, items 32 px `body fg_1`, hover `bg_4`, separator `border_2`, shortcut text `fg_3`, destructive items `danger_hi`.

---

## 5. Icons (Lucide, one per concept)

Names are the existing `ui/icons.py` names where they exist; Lucide source name in brackets when it differs. All drawn on 24 px grid, 1.75 stroke, round caps/joins.

| Concept | Name | Concept | Name |
|---|---|---|---|
| Home | `home` | Build image | `build` (hammer) |
| FPKG / package | `package` | Extract & verify | `extract` (package-open) → artboards use `shield-check` for the nav; **decision needed**: keep `extract` for parity |
| Backport | `backports` (puzzle) | Queue | `queue` (layers) |
| PS5 connect | `ps5` (gamepad-2) | Library | `library` |
| Logs | `logs` (file-text) | Settings | `settings` |
| About / credits | `credits` (heart) | Help | `help` (circle-help) |
| Report issue | `bug` | Menu / sidebar toggle | `menu` |
| More | `more` (ellipsis), `more_v` | Close / dismiss / cancel | `close` (x) |
| Folder / browse | `folder` | Show in folder | `folder_open` |
| Refresh / rescan / check again | `refresh` (refresh-cw) | Retry | `undo` (rotate-ccw) |
| Add / new | `plus` | Delete / clear | `trash` (trash-2) |
| Play / build now / start | `play` | Pause | `pause` |
| Stop | `stop` (square) | Move up / down | `arrow_up`, `arrow_down` |
| Drag handle | `grip` (grip-vertical) | Search | `search` |
| Chevrons | `chevron_down`, `chevron_right`, `chevron_up` | Copy | `copy` |
| Download | `download` | Upload / send payload | `upload` |
| Export | `download` (same glyph; label disambiguates) | Terminal / debug | `terminal` |
| Success | `success` (circle-check) | Warning | `warning` (triangle-alert) |
| Error | `error` (circle-x) | Info | `info` |
| Running / in progress | `clock` (badges) · filled circle (row state) | Pending step | `circle` |
| exFAT image / drive | `drive` (hard-drive) | FFPFSC / PFS / compressed | `compress` (archive) |
| FFPKG | `package` | Image / cover placeholder | `image` |
| Overridden setting / advanced | `advanced` (sliders-horizontal) | Verify | `verified` (shield-check) |
| Console connected | `plug` | Network / FTP | `wifi` |
| Rename / edit | `edit` (pencil) | Empty inbox | `inbox` |
| Filter | `filter` | Time / history | `history` |

Not used anywhere: emoji, Segoe Fluent glyphs, multicolour icons. Icons never carry meaning alone: nav items have labels/tooltips, badges have words, icon buttons have `accessibleName` + tooltip.

---

## 6. Copy rules (binding; extends the README)

| Rule | Example |
|---|---|
| Sentence case everywhere; UPPERCASE only overlines | "Build image", "Add to queue", WORKFLOWS |
| Buttons: verb first, ≤ 3 words, "…" only when a picker/dialog opens | "Build now", "Browse…", "Restore originals…", "Reset all…" |
| Primary verbs per page | Build image → "Build now"; FPKG → "Build package"; Extract → "Extract" / "Verify"; Backport → "Backport n games"; Queue → "Pause queue" / "Resume queue" |
| Status words (StatusBadge) | Idle, Queued, Running, Paused, Done, Verified, Warning, Failed, Cancelled, Ready, Not found, Checking… |
| Errors: what happened + what to do, no blame, no raw exception | "Not enough space on C:. This image needs 48.5 GB; 34.1 GB is free. Free 14.4 GB or choose another output folder." |
| Confirmations: title is the question, body states consequences, safe action first | "Cancel the running build?" … [Keep building] [Cancel build] |
| Numbers: one decimal + thin space + unit; thousands separators; percent with a space | 48.2 GB · 418 MB/s · 18,442 files · 63 % |
| Time left | "About 1 min left", "About 24 min", "Less than a minute left"; durations "12 min 40 s" |
| Timestamps | Logs `14:33:02.115`; tables `2026-11-04 14:36`; relative only in captions ("2 min ago") |
| Technical strings in mono | paths, title IDs, Content IDs, hashes, versions `01.000.000`, file names |
| Formats as the app names them | exFAT, FFPKG (UFS2), FFPFSC, PFS v2 / v3, FPKG, `.gp5` |
| Help text: one sentence, says what it does and when to change it | "128 KB suits most dumps. Larger clusters copy faster and waste more space on small files." |
| Never | "I", "we", "please", exclamation marks, "Oops", "Something went wrong", emoji, ALL CAPS warnings, "successfully" |
| Person | "you" / "your copy of the SDK" |

---

## 7. Proposed changes to the design system (owner decides; nothing here is applied silently)

| # | Proposal | Reason |
|---|---|---|
| P1 | Add `size-statusbar` 28 px (`bg_0`, top hairline, caption text) | The app has always had a status bar; the README omits it. |
| P2 | Add `size-dock` 400 px and the Jobs dock pattern on job-creating pages | Progress must stay visible while editing the next job; it replaces the scattered per-page progress panels. |
| P3 | Add `size-row-job` 56 px two-line row for jobs (title + meta) | A 36 px row cannot hold title, path, progress and actions legibly. |
| P4 | Raise `size-form-max` from 720 to 760 | Two 300 px fields + 12 px gap + 16 px padding ×2 = 760 at 1440. |
| P5 | Define `scrim` as `#050608` at 60 % | The token had no alpha; a solid scrim hides context. |
| P6 | Add `danger_fill_pressed` `#9e252e` and a paused progress colour (`fg_4`) | Missing states. |
| P7 | Remove the hero (`display` style stays for Home only) and the always-visible drop strip | README's own "Avoid" list. |
| P8 | Allow `accent_lo` as the running-job block background in the dock | It is selection/progress meaning, consistent with selected rows. |
| P9 | Segmented selected = `bg_5` + `fg_0` (not the accent) | Keeps the accent for the single primary action per view; selection in segmented is local. |
| P10 | Table header text: caption, sentence case (not overline) | Overline is reserved for sidebar group labels; uppercase headers read as a dashboard trope. |
| P11 | Nav keyboard shortcuts `Ctrl+1…9` | Keyboard parity with Windows apps; cheap in Qt. |
| P12 | Header chips are StatusBadges (read-only) rather than text | One component for status everywhere. |

---

## 8. Open questions for the owner

1. Extract & verify nav icon: keep `extract` (package-open) for parity, or `verified` (shield-check) as drawn?
2. ShadowMount+/MicroMount/Y2JB: tabs under PS5 connect in v1, or deferred?
3. Should "Convert" (image → image) be a mode of Build image (source = image) or of Extract & verify? Proposal: Build image accepts images as sources; the Source card's summary strip shows the detected input type.
4. Light theme: out of scope (README: no light mode). Confirm.
