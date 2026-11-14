# Design v5 — "Nebula"

v5 replaces the flat GitHub-style look of 5.0 previews with a calmer, more
confident interface: blue-black surfaces, one signature gradient
(accent `#3d7bff` → accent_2 `#8b5cf6`), larger radii and far less text on
screen. Tokens live in `src/app/ui/Theme.h`; pages take their colours from
there (the console card of PS5 connect and the payload list still paint a few
gradient colours of their own).

## Principles

1. **One decision per card.** A card answers one question ("Which dump?",
   "Which format?", "Where to save?"). Its title is that question's noun.
2. **Say it once, short.** Page captions are one sentence. Field help is at
   most one short sentence; anything longer goes in a tooltip (setting rows do
   this automatically: `SettingRow::conciseHelp`). No paragraphs inside forms
   (the Build page keeps two: the Backport note and the unpack caption).
3. **Defaults first, power second.** Options most people never change go in a
   folded `Disclosure` ("Advanced options"); `JobFormPage::addJobOptionsCard`
   already does this for per-job settings. The header summary says how many
   differ from defaults.
4. **Big targets for the main choice.** Use `ChoiceTiles` for 2–4 mutually
   exclusive options that matter (format, target), `Segmented` only for small
   view toggles, `QComboBox` for long lists. The output format of a form
   (Build › Format, Image tools › Convert to) is `pages::OptionTiles`: icon on
   top, and a last row with fewer tiles shares the whole width.
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
| `ChoiceTiles` | Large exclusive choices: `addChoice(title, description, icon, value, tag)`. The chosen tile's check sits on the icon's centre line, 14 px in from the end, as in `pages::OptionTiles`. |
| `DropZone` | Folder/file pick + drop target; emits `clicked()`; `setHighlighted` while dragging. |
| `IconTile` | Rounded icon square (page headers, strips, stats); `setTone`. |
| `HeroBanner` | Home welcome banner with `actions()`. |
| `StatTile` | Overline + big value + caption, clickable. |
| `ActionCard` | Clickable icon + title + 2-line text (quick actions). |
| `Badge`, `InfoBar`, `EmptyState`, `ErrorState`, `ProgressBlock`, `Switch`, `Segmented`, `PathField`, `IconButton`, `button(...)` | as before. |

## Layout

* Page = `PageHeader` (icon tile, title 24/700, one-line caption that ends in
  "…" with a tooltip where the window is too narrow, actions on the right) +
  content. Job pages: form column (max 760) + 400 px Jobs dock, which stays
  in view while the form scrolls; in a wider window the header's actions end
  where the dock does.
* Every page keeps the scroll bar's 10 px: nothing moves sideways between
  pages or when a page starts to scroll.
* Under 1200 px the sidebar shows its rail; the user's choice returns when
  the window is wide again.
* Spacing: 16 between cards, 12 inside, 24 page margins.
* Primary action: top-right of the page header, once per page.
* Setting rows: every control of a column ends on one line (units inside the
  number field, dropdowns 160–320 px); a 0 that means "Automatic" or "Off"
  says so in the field; the help is one sentence and never ends inside a
  word or a bracket.
* Information sheets (a result, a preview): Close is secondary and the
  default button; actions on the content ("Load more", "Copy report") sit at
  the start of the button row.
* Nothing is cut without saying so: a cut text field, badge or caption ends
  in "…" and its tooltip has the whole text. A badge keeps to 32 characters;
  the header's status chips ("1 running · 0 queued · then sleep") are whole
  up to 64.
* Header actions keep their place: a button with nothing to do rests
  disabled with its reason as the tooltip rather than going away (a tab
  shows only the buttons that act on it); the ⋯ overflow menu is always last.
* An icon button beside labelled buttons has their plate and outline
  (`pages::outlineIconButton`, the `QToolButton[outline="true"]` rule); two
  buttons next to each other never share a glyph.
* One way back: while the job monitor shows, "New build" is its footer
  button only, not a second one in the page header.
* Every card of a numbered form carries its step badge, or none does.
* Empty is never blank: a list or log with nothing in it says so (empty
  state or placeholder); a list is as tall as its rows, within limits.
* Tables: header in fg_4 600, row hover bg_3, selection accent_lo, 8 px
  between an item's icon and its text. A sorted column's chevron follows its
  label (4 px after it, before it right to left), never the far end of a
  wide column. Trees show a folder's chevron (the style's branch indicator:
  no `QTreeView::branch` rule in the style sheet). A selected tree row is
  filled across its cells as a table row is (FwStyle fills only the branch
  area of a tree, so the cells take `accent_lo` from the style sheet).
* Buttons: `setButtonVariant()` also sets the variant's font (600 for
  Primary and Danger); it holds through later restyles, so a page never sets
  a button's font itself.
* Durations ("3 min 10 s", "~2 min") are made of the translated unit texts.
* In-page tabs (`QTabBar`) render as pills.
* Right to left: a Latin name that ends in a sign stays whole
  ("ShadowMount+"); paths and IDs read left to right.
* Fonts: Segoe UI; Japanese, Chinese and Korean add their Windows UI font
  after it; the status bar is one font with tabular figures.
