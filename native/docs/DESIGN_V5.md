# Design v5 — "Nebula"

v5 replaces the flat GitHub-style look of 5.0 previews with a calmer, more
confident interface: blue-black surfaces, one signature gradient
(accent `#3d7bff` → accent_2 `#8b5cf6`), larger radii and far less text on
screen. Tokens live in `src/app/ui/Theme.h`; nobody writes a hex colour
anywhere else.

## Principles

1. **One decision per card.** A card answers one question ("Which dump?",
   "Which format?", "Where to save?"). Its title is that question's noun.
2. **Say it once, short.** Page captions are one sentence. Field help is at
   most one short sentence; anything longer goes in a tooltip (setting rows do
   this automatically: `SettingRow::conciseHelp`). No paragraphs inside forms.
3. **Defaults first, power second.** Options most people never change go in a
   folded `Disclosure` ("Advanced options"); `JobFormPage::addJobOptionsCard`
   already does this for per-job settings. The header summary says how many
   differ from defaults.
4. **Big targets for the main choice.** Use `ChoiceTiles` for 2–4 mutually
   exclusive options that matter (format, target), `Segmented` only for small
   view toggles, `QComboBox` for long lists.
5. **Empty states invite action.** A step that needs a folder starts as a
   `DropZone`; once filled it becomes an inset card with an `IconTile`, the
   item's name, a one-line meta row and `Change…`.
6. **The gradient is precious.** Only the page's primary button, selection
   states, the brand mark, progress and the Home hero use it. Everything else
   is neutral surfaces with fg_1..fg_4 text.
7. **Status is colour + glyph + word**, never colour alone (`Badge`, `Tone`).

## Components (src/app/ui)

| Component | Use |
|---|---|
| `Card` | Section container (radius 12, sheen). `setTitle(title, rightSlot)`. Inset variant for nested strips. |
| `Disclosure` | Folded section; `body()`, `setSummary(text, tone)`, `setExpanded`. |
| `ChoiceTiles` | Large exclusive choices: `addChoice(title, description, icon, value, tag)`. |
| `DropZone` | Folder/file pick + drop target; emits `clicked()`; `setHighlighted` while dragging. |
| `IconTile` | Rounded icon square (page headers, strips, stats); `setTone`. |
| `HeroBanner` | Home welcome banner with `actions()`. |
| `StatTile` | Overline + big value + caption, clickable. |
| `ActionCard` | Clickable icon + title + 2-line text (quick actions). |
| `Badge`, `InfoBar`, `EmptyState`, `ErrorState`, `ProgressBlock`, `Switch`, `Segmented`, `PathField`, `IconButton`, `button(...)` | as before. |

## Layout

* Page = `PageHeader` (icon tile, title 24/700, one-line caption, actions on
  the right) + content. Job pages: form column (max 760) + 400 px Jobs dock.
* Spacing: 16 between cards, 12 inside, 24 page margins.
* Primary action: top-right of the page header, once per page.
* Tables: header in fg_4 600, row hover bg_3, selection accent_lo.
* In-page tabs (`QTabBar`) render as pills.
