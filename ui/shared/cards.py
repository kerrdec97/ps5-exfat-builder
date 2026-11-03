"""
ui/shared/cards.py — card-style containers and status indicators.

Implements three reusable widgets matching the design system mocks:

- Card           — bordered surface with optional icon header.
                   Caller fills `card.body` with content.
- StatusPill     — small rounded label, four kinds (active/done/wait/fail).
- DetectedGameStrip — green-tinted info strip showing cover art + title +
                      ID/version/size + "✓ Detected" pill. Hidden until
                      `pack(...)` is called.

All widgets pull colors from `tkinter_theme.COLORS` and fonts from
`tkinter_theme.FONTS` — no hex literals.

Step 3 (v2.0.5): introduced for the exFAT tab refactor.
"""

import tkinter as tk
from tkinter_theme import COLORS, FONTS


# ─────────────────────────────────────────────────────────────────────────
# Card — bordered content container with optional header + body.
# ─────────────────────────────────────────────────────────────────────────
from ui.shared.widgets import Card as _XCard, Badge as _XBadge


class Card(_XCard):
    """Legacy card API on the design-system Card.

    Card(parent, title=None, subtitle=None, icon=None, with_actions=False)
    `card.body` holds content; `card.actions` (with_actions=True) is a
    footer frame for buttons. A legacy emoji `icon` becomes an icon tile.
    """

    def __init__(self, parent, title=None, subtitle=None, icon=None,
                 with_actions=False, **kwargs):
        from ui.icons import leading_icon, has_icon
        kwargs.pop('highlightbackground', None)
        kwargs.pop('highlightthickness', None)
        bg = kwargs.pop('bg', None)
        name = None
        if icon:
            name = icon if has_icon(icon) else leading_icon(icon)
        super().__init__(parent, title=title, subtitle=subtitle, icon=name,
                         actions=with_actions, **({'bg': bg} if bg else {}),
                         **kwargs)


# ─────────────────────────────────────────────────────────────────────────
# StatusPill — small rounded label showing build status per queue item.
# ─────────────────────────────────────────────────────────────────────────
class StatusPill(_XBadge):
    """Status pill (design system › StatusBadge) with the legacy API:
    StatusPill(parent, kind='active|done|wait|fail', text) and
    .set(kind, text). Leading glyphs in `text` become the badge icon."""

    _TONE = {'active': 'info', 'done': 'success', 'wait': 'neutral',
             'fail': 'danger', 'warn': 'warn'}

    def __init__(self, parent, kind='wait', text=''):
        super().__init__(parent, text=text, tone=self._TONE.get(kind, 'neutral'))
        self._kind = kind

    def set(self, kind=None, text=None, *args, **kw):
        if kind in self._TONE:
            self._kind = kind
            return super().set(text, self._TONE[kind])
        return super().set(kind if text is None else text, *args, **kw)


# ─────────────────────────────────────────────────────────────────────────
# StatPill — small monospace count + label pill for stats strips.
# Used by the Klog tab's "1,284 info · 17 warn · 3 err" header.
# Visually distinct from StatusPill: bg_3 fill, hairline border, monospace
# count emphasized + dimmer label suffix.
# ─────────────────────────────────────────────────────────────────────────
class StatPill(tk.Frame):
    """Inline stat pill: bold count + dim label, e.g. "1,284 info".

    Args:
        parent: parent widget
        kind:   one of 'info', 'warn', 'err' — controls the count color
        count:  numeric (or string) — the bold leading number
        label:  the dimmer trailing word ("info", "warn", etc.)

    Use `.set_count(n)` to update without rebuilding the widget.
    """

    _COUNT_FG = {
        'info': COLORS['fg_0'],
        'warn': COLORS['warn_hi'],
        'err':  COLORS['danger_hi'],
        'neutral': COLORS['fg_1'],
    }

    def __init__(self, parent, kind='neutral', count=0, label=''):
        bg = COLORS['bg_3']
        super().__init__(parent, bg=bg,
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
        self._kind = kind
        self._label_text = label
        count_fg = self._COUNT_FG.get(kind, COLORS['fg_1'])

        self._count_var = tk.StringVar(value=str(count))
        self._count_lbl = tk.Label(self, textvariable=self._count_var,
                                   font=(FONTS['body_strong'][0], 10),
                                   bg=bg, fg=count_fg,
                                   padx=6, pady=2)
        self._count_lbl.pack(side='left')
        if label:
            self._lbl = tk.Label(self, text=label,
                                 font=FONTS['meta'],
                                 bg=bg, fg=COLORS['fg_4'],
                                 padx=0, pady=2)
            self._lbl.pack(side='left', padx=(0, 8))

    def set_count(self, n):
        """Update the count display (e.g. on each new log line)."""
        self._count_var.set(str(n))


# ─────────────────────────────────────────────────────────────────────────
# GameCard — the library tab's per-game tile.
#
# Visual: tinted-dark cover (one of 6 hash-deterministic palettes) with a
# corner status badge (BUILT / BACKPORT / SDK warn) + CUSA-id chip in the
# bottom-left, then title + version/size meta row below. Hover lifts the
# border to accent.
#
# Used by ui/tab_library.py via the legacy `_lib_make_card` shim, which
# instantiates one GameCard per game.
# ─────────────────────────────────────────────────────────────────────────
class GameCard(tk.Frame):
    """Premium library/image card — cover-first horizontal layout.

    v3.6.0 Library polish: redesigned from a 180px vertical tile into a
    Steam/Playnite-style horizontal card. The cover dominates the left
    side; the right column carries title, id pill, meta rows with icons,
    and a status-badge row with a ⋮ overflow button.

    Backwards-compatible constructor: every pre-3.6.0 kwarg (title,
    game_id, version, size_text, status, status_text, on_click,
    on_double, on_right, cover_size) keeps its meaning. `cover_label`
    is still the async cover-art write target.

    New optional kwargs (all presentation):
        date_text:   e.g. '06/06/2026 01:20'
        folder_text: e.g. 'C:/Users/dec/Downloads'
        badges:      list of (text, scheme) pills for the bottom row;
                     scheme in {'success','purple','warn','accent'}.
                     When None, derived from `status` (legacy behavior).
        cover_badge: (text, scheme) pill overlaid top-right ON the cover
                     (e.g. ('SDK 3','purple') or ('exFAT','accent')).
        kind_text:   small text after version (e.g. 'Base Game').
        on_menu:     callable(event) for the ⋮ button (typically the
                     same context menu as on_right). The ⋮ widget is
                     exposed as `self._menu_btn` so callers that rebind
                     descendants can skip it.

    Methods:
        set_selected(bool): accent border + slightly brighter background.
    """

    _COVER_PALETTES = [
        '#1a0d2a',  # deep purple
        '#2a1a3d',  # mid purple
        '#1d1729',  # near-bg purple
        '#241540',  # bright purple
        '#0e221f',  # teal-purple
        '#240e14',  # rust-purple
    ]

    _BADGE_SCHEMES = {
        'built':    (COLORS['success'],  '#001a05'),
        'backport': (COLORS['purple'],   COLORS['fg_0']),
        'warn':     (COLORS['warn'],     '#1a0e00'),
        # pill schemes for the badges row
        'success':  (COLORS['success'],  '#001a05'),
        'purple':   (COLORS['purple'],   COLORS['fg_0']),
        'accent':   (COLORS['accent'],   COLORS['fg_0']),
    }

    def __init__(self, parent, title, game_id, version=None, size_text=None,
                 status=None, status_text=None,
                 on_click=None, on_double=None, on_right=None,
                 cover_size=180,
                 date_text=None, folder_text=None, badges=None,
                 cover_badge=None, kind_text=None, on_menu=None,
                 info_width=None):
        bg = COLORS['bg_2']
        self._bg_idle = bg
        self._bg_sel = COLORS['bg_3']
        self._idle_border = COLORS['border_2']
        self._hover_border = COLORS['accent']
        self._selected = False

        super().__init__(parent, bg=bg,
                         highlightbackground=self._idle_border,
                         highlightthickness=1,
                         cursor='hand2')

        pad = tk.Frame(self, bg=bg)
        pad.pack(fill='both', expand=True, padx=12, pady=12)

        # ── Cover slab (left, dominant) ──
        # v3.6.2: fixed 2:3 portrait (PS5-store style). cover_size is the
        # cover HEIGHT; width is locked to height * 2/3 so every card's
        # artwork area is identical regardless of the source image shape.
        cover_h = int(cover_size)
        cover_w = max(1, int(round(cover_h * 2 / 3.0)))
        self._cover_w, self._cover_h = cover_w, cover_h
        cover_bg = self._cover_for(game_id)
        cover = tk.Frame(pad, bg=cover_bg,
                         width=cover_w, height=cover_h,
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
        cover.pack(side='left')
        cover.pack_propagate(False)

        self.cover_label = tk.Label(cover,
                                    text='\U0001f3ae',
                                    font=(FONTS['body'][0],
                                          max(28, cover_h // 5)),
                                    bg=cover_bg, fg=COLORS['fg_6'])
        self.cover_label.place(relx=0.5, rely=0.5, anchor='center')

        # cover overlay badge (e.g. SDK n / format)
        if cover_badge:
            cb_text, cb_scheme = cover_badge
            cb_bg, cb_fg = self._BADGE_SCHEMES.get(
                cb_scheme, self._BADGE_SCHEMES['purple'])
            tk.Label(cover, text=' ' + cb_text + ' ',
                     font=(FONTS['body_strong'][0], 9),
                     bg=cb_bg, fg=cb_fg, padx=4, pady=1
                     ).place(relx=1.0, rely=0.0, anchor='ne', x=-6, y=6)
        elif status and status in ('built', 'backport', 'warn'):
            # legacy single-badge-on-cover behavior
            bg_b, fg_b = self._BADGE_SCHEMES[status]
            text = status_text or {'built': 'BUILT',
                                   'backport': 'BACKPORT',
                                   'warn': 'SDK'}[status]
            tk.Label(cover, text=' ' + text + ' ',
                     font=(FONTS['body_strong'][0], 9),
                     bg=bg_b, fg=fg_b, padx=4, pady=1
                     ).place(relx=1.0, rely=0.0, anchor='ne', x=-6, y=6)

        # ── Info column (right) — FIXED width so the card never grows
        # with title/metadata length (v3.6.2). Total card width is thus
        # constant: cover_w + info_width + paddings. ──
        info_width = int(info_width) if info_width else max(
            200, int(cover_h * 1.4))
        self._info_w = info_width
        info_holder = tk.Frame(pad, bg=bg, width=info_width)
        info_holder.pack(side='left', fill='y', padx=(14, 0))
        info_holder.pack_propagate(False)
        info = tk.Frame(info_holder, bg=bg)
        info.pack(fill='both', expand=True)

        display_title = title or '(unknown)'
        # Truncate to what fits ~2 lines in the fixed column. ~9px/char
        # at 12pt bold; two lines of the column width.
        _max_chars = max(16, int((info_width / 9.0) * 2))
        if len(display_title) > _max_chars:
            display_title = display_title[:_max_chars - 1].rstrip() + '\u2026'
        tk.Label(info, text=display_title,
                 font=(FONTS['body_strong'][0], 11),
                 wraplength=max(80, info_width - 4),
                 bg=bg, fg=COLORS['fg_0'], anchor='w',
                 justify='left').pack(fill='x')

        if game_id:
            idrow = tk.Frame(info, bg=bg)
            idrow.pack(fill='x', pady=(5, 0))
            tk.Label(idrow, text=' ' + game_id + ' ',
                     font=(FONTS['body_strong'][0], 9),
                     bg=COLORS['accent_15'], fg=COLORS['accent_hi'],
                     padx=6, pady=2,
                     highlightbackground=COLORS['accent_lo'],
                     highlightthickness=1).pack(side='left')

        if version or kind_text:
            vrow = tk.Frame(info, bg=bg)
            vrow.pack(fill='x', pady=(6, 0))
            parts = [t for t in (version, kind_text) if t]
            tk.Label(vrow, text='  \u00b7  '.join(parts),
                     font=FONTS['mono_sm'], bg=bg, fg=COLORS['fg_3'],
                     anchor='w').pack(side='left')

        tk.Frame(info, bg=COLORS['border_2'], height=1
                 ).pack(fill='x', pady=(10, 8))

        def _meta_row(glyph, text):
            r = tk.Frame(info, bg=bg)
            r.pack(fill='x', pady=(0, 4))
            tk.Label(r, text=glyph, font=(FONTS['body'][0], 9),
                     bg=bg, fg=COLORS['fg_5']).pack(side='left',
                                                    padx=(0, 7))
            t = text
            _mmax = max(10, int((self._info_w - 24) / 7.0))
            if len(t) > _mmax:
                t = '\u2026' + t[-(_mmax - 1):]
            tk.Label(r, text=t, font=FONTS['mono_sm'], bg=bg,
                     fg=COLORS['fg_3'], anchor='w').pack(side='left')

        if size_text:
            _meta_row('\u2b07', size_text)
        if date_text:
            _meta_row('\U0001f4c5', date_text)
        if folder_text:
            _meta_row('\U0001f4c1', folder_text)

        # ── Bottom: status badges + ⋮ overflow ──
        brow = tk.Frame(info, bg=bg)
        brow.pack(fill='x', side='bottom', pady=(8, 0))

        pills = badges
        if pills is None and status in ('built', 'backport', 'warn'):
            # legacy: derive one pill from status
            text = status_text or {'built': 'BUILT',
                                   'backport': 'BACKPORT',
                                   'warn': 'SDK'}[status]
            pills = [(text, {'built': 'success', 'backport': 'purple',
                             'warn': 'warn'}[status])]
        for text, scheme in (pills or []):
            p_bg, p_fg = self._BADGE_SCHEMES.get(
                scheme, self._BADGE_SCHEMES['purple'])
            tk.Label(brow, text=' ' + text + ' ',
                     font=(FONTS['body_strong'][0], 9),
                     bg=p_bg, fg=p_fg, padx=5, pady=2
                     ).pack(side='left', padx=(0, 5))

        self._menu_btn = None
        if on_menu:
            mb = tk.Label(brow, text='\u22ee',
                          font=(FONTS['body_strong'][0], 11),
                          bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                          padx=9, pady=1, cursor='hand2',
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
            mb.pack(side='right')
            self._menu_btn = mb

        # ── Hover & click bindings (same idiom as before) ──
        def _all_widgets():
            stack = [self]
            while stack:
                w = stack.pop()
                yield w
                stack.extend(w.winfo_children())

        for w in _all_widgets():
            w.bind('<Enter>', self._on_enter, add='+')
            w.bind('<Leave>', self._on_leave, add='+')
            if on_click:
                w.bind('<Button-1>', on_click, add='+')
            if on_double:
                w.bind('<Double-Button-1>', on_double, add='+')
            if on_right:
                w.bind('<Button-3>', on_right, add='+')

        # ⋮ gets an exclusive left-click (replaces the select bind on it)
        if self._menu_btn is not None and on_menu:
            self._menu_btn.bind('<Button-1>', on_menu)

    # ── selection (presentation-only state) ──
    def set_selected(self, selected):
        self._selected = bool(selected)
        new_bg = self._bg_sel if self._selected else self._bg_idle
        try:
            self.configure(
                highlightbackground=(COLORS['accent'] if self._selected
                                     else self._idle_border),
                highlightcolor=(COLORS['accent'] if self._selected
                                else self._idle_border),
                highlightthickness=2 if self._selected else 1,
                bg=new_bg)
        except Exception:
            pass
        # brighten/dim every descendant that carried the card background
        old_bg = self._bg_idle if self._selected else self._bg_sel
        stack = list(self.winfo_children())
        while stack:
            w = stack.pop()
            try:
                if str(w.cget('bg')).lower() == old_bg.lower():
                    w.configure(bg=new_bg)
            except Exception:
                pass
            stack.extend(w.winfo_children())

    def _cover_for(self, game_id):
        """Pick one of 6 cover palettes deterministically from game_id."""
        if not game_id:
            return self._COVER_PALETTES[0]
        digits = ''.join(c for c in game_id if c.isdigit())
        h = sum(ord(c) for c in (digits or game_id))
        return self._COVER_PALETTES[h % len(self._COVER_PALETTES)]

    def _on_enter(self, _e=None):
        if self._selected:
            return
        try:
            self.configure(highlightbackground=self._hover_border)
        except Exception:
            pass

    def _on_leave(self, _e=None):
        if self._selected:
            return
        try:
            self.configure(highlightbackground=self._idle_border)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────────
# FolderChip — the library tab's "📁 D:\PS5\Dumps  28 games  ✕" pill.
# ─────────────────────────────────────────────────────────────────────────
class FolderChip(tk.Frame):
    """Folder chip: rounded bg_3 pill with a folder icon, monospace path,
    optional meta and a remove button."""

    def __init__(self, parent, path, meta=None, on_remove=None):
        from tkinter_theme import px, surface_of
        from ui.shared.widgets import Rounded, IconLabel, IconButton
        outside = surface_of(parent)
        super().__init__(parent, bg=outside)
        bg = COLORS['bg_3']
        chip = Rounded(self, bg=bg, border=COLORS['border_3'], radius=4,
                       padding=(px(10), px(2)), surface=outside)
        chip.pack()
        IconLabel(chip, 'folder', 14, COLORS['fg_3'], bg).pack(
            side='left', padx=(0, px(6)))
        tk.Label(chip, text=path, font=FONTS['code'], bg=bg,
                 fg=COLORS['fg_1']).pack(side='left', pady=px(4))
        if meta:
            tk.Label(chip, text=meta, font=FONTS['caption'], bg=bg,
                     fg=COLORS['fg_3']).pack(side='left', padx=(px(8), 0))
        if on_remove:
            IconButton(chip, 'close', on_remove, tooltip='Remove folder',
                       size='sm', surface=bg).pack(side='left',
                                                   padx=(px(4), 0))


# ─────────────────────────────────────────────────────────────────────────
# DetectedGameStrip — green-tinted info card showing the auto-detected
# game's title, ID, version, size, and a cover-art thumbnail.
# Replaces the legacy `_info_frame` with a styled equivalent that exposes
# the same attribute names so existing callbacks keep working.
# ─────────────────────────────────────────────────────────────────────────
class DetectedGameStrip(tk.Frame):
    """Auto-detected game strip (design system › InfoBar, success tone):
    cover thumbnail, title, ID / version, size and a "Detected" badge.

    Same attributes as before: cover_label (.config(image=…, text=…)),
    title_var, id_var, ver_var, size_var. pack/pack_forget by the caller.
    """

    def __init__(self, parent, kind='success', **kwargs):
        from tkinter_theme import px, surface_of
        from ui.shared.widgets import Rounded, CoverLabel, Badge
        outside = surface_of(parent)
        super().__init__(parent, bg=outside, **kwargs)
        bg = COLORS['success_bg'] if kind == 'success' else COLORS['accent_lo']
        strip = Rounded(self, bg=bg, border=COLORS['border_2'], radius=8,
                        padding=(px(12), px(10)), surface=outside)
        strip.pack(fill='x')
        self.cover_label = CoverLabel(strip, box=48, placeholder='game')
        self.cover_label.pack(side='left', padx=(0, px(12)))
        text_col = tk.Frame(strip, bg=bg)
        text_col.pack(side='left', fill='x', expand=True)
        self.title_var = tk.StringVar()
        self.id_var = tk.StringVar()
        self.ver_var = tk.StringVar()
        self.size_var = tk.StringVar()
        tk.Label(text_col, textvariable=self.title_var,
                 font=FONTS['body_strong'], bg=bg, fg=COLORS['fg_0'],
                 anchor='w').pack(fill='x')
        meta_row = tk.Frame(text_col, bg=bg)
        meta_row.pack(fill='x', pady=(px(2), 0))
        tk.Label(meta_row, textvariable=self.id_var, font=FONTS['code'],
                 bg=bg, fg=COLORS['fg_2'], anchor='w').pack(side='left')
        tk.Label(meta_row, textvariable=self.ver_var, font=FONTS['caption'],
                 bg=bg, fg=COLORS['fg_3'], anchor='w').pack(
                     side='left', padx=(px(12), 0))
        tk.Label(text_col, textvariable=self.size_var, font=FONTS['caption'],
                 bg=bg, fg=COLORS['fg_2'], anchor='w').pack(fill='x',
                                                            pady=(px(1), 0))
        Badge(strip, 'Detected', 'success', surface=bg).pack(
            side='right', padx=(px(8), 0))


# ─────────────────────────────────────────────────────────────────────────
# SettingsCard — card for a settings group (title + optional hint header,
# rows packed directly into body with no extra padding).
# Used by ui/tab_settings.py for each section group.
# ─────────────────────────────────────────────────────────────────────────
class SettingsCard(_XCard):
    """Settings group card (Windows 11 settings pattern): subtitle-style
    title with an optional right-aligned hint or widget, then rows (use
    `settings_row`) packed into `.body`."""

    def __init__(self, parent, title, hint=None, right_widget=None):
        from tkinter_theme import px
        super().__init__(parent, padding=(px(16), px(4)))
        bg = COLORS['bg_2']
        head = tk.Frame(self, bg=bg)
        head.pack(fill='x', pady=(px(12), px(4)))
        tk.Label(head, text=title, font=FONTS['subtitle'], bg=bg,
                 fg=COLORS['fg_0'], anchor='w').pack(side='left')
        if right_widget is not None:
            try:
                right_widget.pack(side='right')
            except Exception:
                pass
        elif hint:
            tk.Label(head, text=hint, font=FONTS['caption'], bg=bg,
                     fg=COLORS['fg_3']).pack(side='right')
        self.body = tk.Frame(self, bg=bg)
        self.body.pack(fill='both', expand=True, pady=(0, px(4)))


def settings_row(parent, label, description=None, with_divider=True):
    """A settings row inside SettingsCard.body: label + description on the
    left, control area and actions area on the right, hairline between
    rows. Returns (row_frame, control_frame, actions_frame)."""
    from tkinter_theme import px
    from ui.shared.widgets import autowrap
    bg = COLORS['bg_2']
    if with_divider and getattr(parent, '_xb_rows', 0):
        tk.Frame(parent, bg=COLORS['border_1'], height=1).pack(fill='x')
    parent._xb_rows = getattr(parent, '_xb_rows', 0) + 1
    row = tk.Frame(parent, bg=bg)
    row.pack(fill='x', pady=px(12))
    row.grid_columnconfigure(0, weight=1, minsize=px(220))
    row.grid_columnconfigure(1, weight=1)
    row.grid_columnconfigure(2, weight=0)
    label_col = tk.Frame(row, bg=bg)
    label_col.grid(row=0, column=0, sticky='nw', padx=(0, px(16)))
    tk.Label(label_col, text=label, font=FONTS['body'], bg=bg,
             fg=COLORS['fg_1'], anchor='w', justify='left').pack(fill='x')
    if description:
        d = tk.Label(label_col, text=description, font=FONTS['caption'],
                     bg=bg, fg=COLORS['fg_3'], anchor='w', justify='left',
                     wraplength=px(320))
        d.pack(fill='x', pady=(px(2), 0))
    control = tk.Frame(row, bg=bg)
    control.grid(row=0, column=1, sticky='ew')
    actions = tk.Frame(row, bg=bg)
    actions.grid(row=0, column=2, sticky='e', padx=(px(8), 0))
    return row, control, actions


# ─────────────────────────────────────────────────────────────────────────
# build_subtabs — reusable sub-tab strip for tabs that host multiple
# related views (exFAT Build/Extract/Edit, Library Dumps/Images, PS5
# Mgr/FTP/Klog/Payloads/Y2JB, Settings General/Language/Help).
# Returns the activator function so other code can navigate
# programmatically.
# ─────────────────────────────────────────────────────────────────────────

def build_subtabs(parent, items, default=None):
    """Build a sub-tab strip and content frames inside `parent`.

    Args:
        parent: tk.Frame or similar — the tab content area
        items: list of (key, label, builder) tuples. `builder(frame)`
               populates the sub-tab's content. `key` is the internal
               name used by the activator. `label` is the display text.
        default: which key to activate first. Defaults to items[0][0].

    Returns:
        activator: callable(key) — switch to a given sub-tab. The
                   parent tab can store this on `app` or call it from
                   handlers that programmatically navigate.

    The strip uses the same colour tokens as the main tab bar so the
    visual hierarchy is consistent. Active sub-tab gets accent text
    on a slightly elevated background; idle is muted text on the
    sub-tab strip background.
    """
    from tkinter_theme import px
    from ui.icons import leading_icon, strip_glyphs, icon_image
    bg = COLORS['bg_1']
    sub_bg = bg

    # Design system › Tabs: page-coloured strip, fg_3 labels, the selected
    # tab fg_0 semibold over a 3px accent indicator, 1px hairline below.
    subbar = tk.Frame(parent, bg=sub_bg)
    subbar.pack(fill='x', padx=px(20), pady=(px(8), 0))

    # Per-key content frame (only one shown at a time)
    frames = {}
    builders = {}
    for key, _label, _builder in items:
        frames[key] = tk.Frame(parent, bg=bg)
        builders[key] = _builder

    state = {'name': default or items[0][0]}
    built = set()   # keys whose builder has already run (build-once)
    btns = {}

    def _ensure_built(name):
        """Lazily run a sub-panel's builder the first time it's shown.
        v3.6.3 startup optimization: previously every sub-panel was built
        eagerly at tab construction (the bulk of ps5/settings/build-legacy
        launch cost). Now only the active panel is built up front; the rest
        build on first activation and are cached. Builders are responsible
        for their own deferred work (after()/pollers), which they schedule
        from inside the builder \u2014 so nothing can poll a panel's widgets
        before it exists."""
        if name in built:
            return
        built.add(name)
        b = builders.get(name)
        if not b:
            return
        try:
            b(frames[name])
        except Exception:
            # If a builder fails, leave its frame empty — don't propagate
            # the error and break the rest of the sub-tabs.
            import traceback
            traceback.print_exc()

    def _restyle(name, hover=False):
        b = btns.get(name)
        if not b:
            return
        lbl, bar, icon = b
        on = state['name'] == name
        fg = COLORS['fg_0'] if on else (COLORS['fg_1'] if hover else COLORS['fg_3'])
        lbl.configure(fg=fg, font=FONTS['body_strong'] if on else FONTS['body'])
        if icon:
            lbl.configure(image=icon_image(icon, 16, COLORS['accent_hi'] if on else fg,
                                           sub_bg))
        bar.configure(bg=COLORS['accent'] if on else sub_bg)

    def activate(name):
        if name not in frames:
            return
        _ensure_built(name)        # build-on-first-use
        state['name'] = name
        for n in frames:
            _restyle(n)
            frames[n].pack_forget()
        frames[name].pack(fill='both', expand=True)

    for key, label, _builder in items:
        icon = leading_icon(label)
        text = strip_glyphs(label)
        wrap = tk.Frame(subbar, bg=sub_bg, cursor='hand2')
        wrap.pack(side='left', padx=(0, px(4)))
        lbl = tk.Label(wrap, text=(' ' + text) if icon else text,
                       padx=px(12), pady=px(8), bg=sub_bg, fg=COLORS['fg_3'],
                       font=FONTS['body'], cursor='hand2',
                       compound='left' if icon else 'none')
        lbl.pack()
        bar = tk.Frame(wrap, bg=sub_bg, height=px(3))
        bar.pack(fill='x', padx=px(12))
        for w in (wrap, lbl, bar):
            w.bind('<Button-1>', lambda e, n=key: activate(n))
        lbl.bind('<Enter>', lambda e, n=key: _restyle(n, True)
                 if state['name'] != n else None)
        lbl.bind('<Leave>', lambda e, n=key: _restyle(n))
        btns[key] = (lbl, bar, icon)
        _restyle(key)

    # 1px divider under the strip
    tk.Frame(parent, bg=COLORS['border_2'], height=1).pack(fill='x')

    # Only the initially-active panel is built now; activate() builds the
    # rest on first use.
    activate(state['name'])
    return activate
