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
class Card(tk.Frame):
    """A bordered card with optional title header.

    Visual: bg_2 fill, border_2 hairline border, optional header row showing
    an icon-tile + title + subtitle. Caller adds content to `card.body` and
    optional action buttons to `card.actions` (a separate footer strip).

    Args:
        parent: parent widget
        title: optional title shown in the header row
        subtitle: optional subtitle (one line, smaller, muted)
        icon: optional emoji shown in the icon tile to the left of the title
        with_actions: if True, creates `card.actions` (a bg_3 footer frame
            for buttons)

    Attributes:
        body: the main content frame (bg_2, padded). Caller's children go here.
        actions: footer frame (bg_3, no padding) — only present if
            with_actions=True. Pack buttons into it.

    Example:
        card = Card(parent, title='Add to queue', icon='＋')
        card.pack(fill='x', padx=18, pady=12)
        tk.Label(card.body, text='Hello').pack()
    """

    def __init__(self, parent, title=None, subtitle=None, icon=None,
                 with_actions=False, **kwargs):
        # Pop any user-supplied highlight overrides so super() doesn't collide
        hl_bg = kwargs.pop('highlightbackground', COLORS['border_2'])
        hl_th = kwargs.pop('highlightthickness', 1)
        bg = kwargs.pop('bg', COLORS['bg_2'])
        super().__init__(parent, bg=bg,
                         highlightbackground=hl_bg,
                         highlightthickness=hl_th,
                         **kwargs)

        # ── Optional header row ──
        if title is not None:
            head = tk.Frame(self, bg=bg)
            head.pack(fill='x', padx=18, pady=(14, 12))

            if icon:
                # Icon tile: small accented square holding the emoji
                ico = tk.Label(head, text=icon,
                               font=(FONTS['h2'][0], 13),
                               bg=COLORS['accent_08'],
                               fg=COLORS['accent'],
                               width=2, padx=4, pady=2)
                ico.pack(side='left', padx=(0, 12))

            text_col = tk.Frame(head, bg=bg)
            text_col.pack(side='left', fill='x', expand=True)
            tk.Label(text_col, text=title,
                     font=(FONTS['h3'][0], 11, 'bold'),
                     bg=bg, fg=COLORS['fg_0'], anchor='w'
                     ).pack(fill='x')
            if subtitle:
                tk.Label(text_col, text=subtitle,
                         font=FONTS['meta'],
                         bg=bg, fg=COLORS['fg_4'], anchor='w',
                         wraplength=900, justify='left'
                         ).pack(fill='x', pady=(1, 0))

            # Hairline divider under the header
            tk.Frame(self, bg=COLORS['border_2'], height=1
                     ).pack(fill='x')

        # ── Optional actions footer ──
        # IMPORTANT: pack actions FIRST with side='bottom' so it pins to the
        # bottom of the card regardless of how short the available height is.
        # If we packed body first with expand=True, body would eat all space
        # and the actions row would get clipped when the window is shorter
        # than the natural content height (e.g. when the OUTPUT LOG is open).
        self.actions = None
        if with_actions:
            self.actions = tk.Frame(self, bg=COLORS['bg_3'])
            self.actions.pack(side='bottom', fill='x', padx=14, pady=10)
            tk.Frame(self, bg=COLORS['border_2'], height=1
                     ).pack(side='bottom', fill='x')

        # ── Body ── (caller fills this) — packed AFTER actions so it
        # claims the remaining vertical space above the footer.
        self.body = tk.Frame(self, bg=bg)
        self.body.pack(fill='both', expand=True, padx=18, pady=14)


# ─────────────────────────────────────────────────────────────────────────
# StatusPill — small rounded label showing build status per queue item.
# ─────────────────────────────────────────────────────────────────────────
class StatusPill(tk.Label):
    """A status pill matching the design system's queue row pill.

    Four kinds:
      'active'  — accent blue, e.g. "● Building 63%"
      'done'    — green, e.g. "✓ Done"
      'wait'    — neutral grey, e.g. "Waiting"
      'fail'    — red, e.g. "✗ Failed"

    tkinter doesn't do real rounded corners on a Label, but the small
    padding + accent foreground reads as a pill in the dark UI.

    Args:
        parent: parent widget
        kind:   one of 'active' / 'done' / 'wait' / 'fail'
        text:   pill label (caller provides the leading glyph if any)
    """

    _SCHEMES = {
        'active': (COLORS['accent_08'],  COLORS['accent_hi']),
        'done':   (COLORS['success_bg'], COLORS['success_hi']),
        'wait':   (COLORS['bg_4'],       COLORS['fg_5']),
        'fail':   (COLORS['danger_bg'],  COLORS['danger_hi']),
    }

    def __init__(self, parent, kind='wait', text=''):
        bg, fg = self._SCHEMES.get(kind, self._SCHEMES['wait'])
        super().__init__(parent, text=' ' + text + ' ',
                         bg=bg, fg=fg,
                         font=(FONTS['body'][0], 9, 'bold'),
                         padx=6, pady=2, bd=0)
        self._kind = kind

    def set(self, kind, text):
        """Update the pill to a different kind + text."""
        bg, fg = self._SCHEMES.get(kind, self._SCHEMES['wait'])
        self.configure(text=' ' + text + ' ', bg=bg, fg=fg)
        self._kind = kind


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
        'info': COLORS['success_ok'],
        'warn': COLORS['warn_hi'],
        'err':  COLORS['danger_hi'],
        'neutral': COLORS['fg_1'],
    }

    def __init__(self, parent, kind='neutral', count=0, label=''):
        bg = COLORS['bg_3']
        super().__init__(parent, bg=bg,
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)
        self._kind = kind
        self._label_text = label
        count_fg = self._COUNT_FG.get(kind, COLORS['fg_1'])

        self._count_var = tk.StringVar(value=str(count))
        self._count_lbl = tk.Label(self, textvariable=self._count_var,
                                   font=(FONTS['mono_sm'][0], 10, 'bold'),
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
    """Library tab game tile.

    Args:
        parent: parent widget
        title:  display title (will be truncated to ~22 chars at render time)
        game_id: e.g. 'CUSA-12345' / 'PPSA-01325' — shown in cover badge
        version: version string, e.g. 'v1.05' — shown in meta row
        size_text: pre-formatted size, e.g. '4.2 GB' — shown in meta row
        status: one of None / 'built' / 'backport' / 'warn'
                None        — no badge
                'built'     — green BUILT pill (image is built and ready)
                'backport'  — purple BACKPORT pill (eligible for backporting)
                'warn'      — amber SDK warn pill (caller passes text)
        status_text: badge text override (e.g. 'SDK 6.20' for warn)
        on_click: callable(event) for left-click
        on_double: callable(event) for double-click (typical: add to queue)
        on_right: callable(event) for right-click (typical: context menu)

    Attributes:
        cover_label: the large emoji/image label inside the cover area —
                     the legacy code sets `cover_label.config(image=...)`
                     to apply real cover art when loaded async.
    """

    # 6 hash-mapped tinted dark backgrounds for the cover area. Tk doesn't
    # render true gradients on Frames; these solids approximate the mock's
    # `linear-gradient(135deg, ...)` averages and read as "tinted dark slab".
    # Purple-shifted to harmonize with the v3 (purple-tinted) theme.
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
    }

    def __init__(self, parent, title, game_id, version=None, size_text=None,
                 status=None, status_text=None,
                 on_click=None, on_double=None, on_right=None,
                 cover_size=180):
        bg = COLORS['bg_2']
        border = COLORS['border_2']
        hover_border = COLORS['accent']
        self._idle_border = border
        self._hover_border = hover_border

        super().__init__(parent, bg=bg,
                         highlightbackground=border,
                         highlightthickness=1,
                         cursor='hand2')

        # ── Cover area — square aspect, tinted dark fill, badges overlaid ──
        # Step 34 (v2.2.15): pack with no fill so the 180x180 frame
        # stays square. Previously fill='x' let the column width
        # stretch the frame horizontally, producing a rectangular
        # cover area while the cover image stayed 180x180 — which
        # left empty space on the right of every card.
        cover_bg = self._cover_for(game_id)
        cover = tk.Frame(self, bg=cover_bg,
                         width=cover_size, height=cover_size)
        cover.pack(padx=0, pady=0)
        cover.pack_propagate(False)

        # Center emoji as fallback cover; legacy code overrides with a
        # PhotoImage when async cover art loads.
        self.cover_label = tk.Label(cover,
                                    text='\U0001f3ae',
                                    font=(FONTS['body'][0], 30),
                                    bg=cover_bg, fg=COLORS['fg_6'])
        self.cover_label.place(relx=0.5, rely=0.5, anchor='center')

        # Status badge (top-right, if status given)
        if status and status in self._BADGE_SCHEMES:
            bg_b, fg_b = self._BADGE_SCHEMES[status]
            text = status_text or {
                'built':    'BUILT',
                'backport': 'BACKPORT',
                'warn':     'SDK',
            }[status]
            badge = tk.Label(cover, text=' ' + text + ' ',
                             font=(FONTS['mono_sm'][0], 8, 'bold'),
                             bg=bg_b, fg=fg_b,
                             padx=4, pady=1)
            badge.place(relx=1.0, rely=0.0, anchor='ne', x=-6, y=6)

        # CUSA-id chip (bottom-left)
        if game_id:
            id_chip = tk.Label(cover, text=' ' + game_id + ' ',
                               font=(FONTS['mono_sm'][0], 8),
                               bg='#000000', fg=COLORS['fg_2'],
                               padx=4, pady=0)
            id_chip.place(relx=0.0, rely=1.0, anchor='sw', x=6, y=-6)

        # ── Meta row (below cover) — title + version/size sub-row ──
        meta = tk.Frame(self, bg=bg)
        meta.pack(fill='x', padx=10, pady=(8, 9))

        # Truncate title if very long
        display_title = title or '(unknown)'
        if len(display_title) > 24:
            display_title = display_title[:22] + '\u2026'
        tk.Label(meta, text=display_title,
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=bg, fg=COLORS['fg_1'],
                 anchor='w'
                 ).pack(fill='x')

        sub = tk.Frame(meta, bg=bg)
        sub.pack(fill='x', pady=(2, 0))
        if version:
            tk.Label(sub, text=version,
                     font=FONTS['mono_sm'],
                     bg=bg, fg=COLORS['fg_4'],
                     anchor='w'
                     ).pack(side='left', padx=(0, 4))
        if size_text:
            tk.Label(sub, text=size_text,
                     font=FONTS['mono_sm'],
                     bg=bg, fg=COLORS['fg_4'],
                     anchor='e'
                     ).pack(side='right', padx=(4, 0))

        # ── Hover & click bindings ──
        # We need to bind to every descendant so clicking the cover, badge,
        # title, etc. all hit the handler — same idiom the legacy code used.
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

    def _cover_for(self, game_id):
        """Pick one of 6 cover palettes deterministically from game_id."""
        if not game_id:
            return self._COVER_PALETTES[0]
        # Sum char codes of digits in id; cheap stable hash
        digits = ''.join(c for c in game_id if c.isdigit())
        h = sum(ord(c) for c in (digits or game_id))
        return self._COVER_PALETTES[h % len(self._COVER_PALETTES)]

    def _on_enter(self, _e=None):
        try:
            self.configure(highlightbackground=self._hover_border)
        except Exception:
            pass

    def _on_leave(self, _e=None):
        try:
            self.configure(highlightbackground=self._idle_border)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────────
# FolderChip — the library tab's "📁 D:\PS5\Dumps  28 games  ✕" pill.
# ─────────────────────────────────────────────────────────────────────────
class FolderChip(tk.Frame):
    """A folder chip: dim background, monospace path, optional close button.

    Args:
        parent: parent widget
        path: the folder path text
        meta: optional small dim text (e.g. "28 games")
        on_remove: callback() fired when the ✕ is clicked. If None, no close.
    """

    def __init__(self, parent, path, meta=None, on_remove=None):
        bg = COLORS['bg_2']
        super().__init__(parent, bg=bg,
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)

        inner = tk.Frame(self, bg=bg)
        inner.pack(padx=10, pady=5)

        # Folder glyph + path (monospace)
        tk.Label(inner, text='\U0001f4c1',
                 font=(FONTS['body'][0], 10),
                 bg=bg, fg=COLORS['fg_3']
                 ).pack(side='left', padx=(0, 6))
        tk.Label(inner, text=path,
                 font=FONTS['mono_sm'],
                 bg=bg, fg=COLORS['fg_1']
                 ).pack(side='left')

        if meta:
            tk.Label(inner, text=meta,
                     font=FONTS['meta'],
                     bg=bg, fg=COLORS['fg_4']
                     ).pack(side='left', padx=(8, 0))

        if on_remove:
            # Step 41 (v2.5.7): make the ✕ unmistakable. Earlier versions
            # rendered a small ✕ that users routinely missed — multiple
            # reports of "no way to remove a scan folder" despite the
            # button being functionally present. Now: 13pt bold ✕ with
            # an always-visible dim pill background so it reads as a
            # button at rest, not just a glyph. Hover turns it red.
            x_bg = COLORS.get('bg_3', '#2a2733')
            x_lbl = tk.Label(inner, text='\u2715',
                             font=(FONTS['body'][0], 13, 'bold'),
                             bg=x_bg, fg=COLORS['fg_1'],
                             cursor='hand2', padx=8, pady=1,
                             highlightbackground=COLORS['border_3'],
                             highlightthickness=1)
            x_lbl.pack(side='left', padx=(12, 0))
            x_lbl.bind('<Button-1>', lambda e: on_remove())
            x_lbl.bind('<Enter>',
                       lambda e: x_lbl.configure(fg='#ffffff',
                                                  bg=COLORS['danger']))
            x_lbl.bind('<Leave>',
                       lambda e: x_lbl.configure(fg=COLORS['fg_1'],
                                                  bg=x_bg))


# ─────────────────────────────────────────────────────────────────────────
# DetectedGameStrip — green-tinted info card showing the auto-detected
# game's title, ID, version, size, and a cover-art thumbnail.
# Replaces the legacy `_info_frame` with a styled equivalent that exposes
# the same attribute names so existing callbacks keep working.
# ─────────────────────────────────────────────────────────────────────────
class DetectedGameStrip(tk.Frame):
    """Auto-detected game info strip, matching exfat-tab-redesign.html `.detected`.

    Visual: tinted background with a 1px accent border, cover thumbnail
    on the left, title + metadata in the middle, "✓ Detected" pill on the right.

    Args:
        parent: parent widget
        kind:   color scheme — 'success' (green, default — exFAT tab),
                'teal' (ffpkg tab), 'accent' (blue, generic)

    The widget exposes the same attributes the legacy code expected on
    `_info_frame`'s siblings, so populating logic in exfat_builder.py
    continues to work unchanged:

        cover_label  — set image via .config(image=..., text=...)
        title_var    — StringVar
        id_var       — StringVar
        ver_var      — StringVar
        size_var     — StringVar

    Lifecycle: pack/pack_forget by the caller (legacy code already does this).
    """

    _SCHEMES = {
        'success': ('success_bg', 'success', 'success_hi'),
        'teal':    ('teal_bg',    'teal',    'teal_hi'),
        'accent':  ('accent_08',  'accent',  'accent_hi'),
    }

    def __init__(self, parent, kind='success', **kwargs):
        scheme = self._SCHEMES.get(kind, self._SCHEMES['success'])
        bg_key, border_key, accent_key = scheme
        bg = COLORS[bg_key]
        border_color = COLORS[border_key]
        accent = COLORS[accent_key]

        super().__init__(parent, bg=bg,
                         highlightbackground=border_color,
                         highlightthickness=1,
                         **kwargs)

        inner = tk.Frame(self, bg=bg)
        inner.pack(fill='x', padx=12, pady=10)

        # ── Cover thumbnail (left) ──
        # 48px square. Real cover art is set via .config(image=..., text='').
        self.cover_label = tk.Label(inner, bg=COLORS['bg_5'],
                                    fg=accent,
                                    text='\U0001f3ae',
                                    font=(FONTS['body'][0], 18),
                                    width=4, height=2,
                                    relief='flat', bd=0,
                                    highlightbackground=COLORS['border_3'],
                                    highlightthickness=1)
        self.cover_label.pack(side='left', padx=(0, 12))

        # ── Title + metadata (middle, expands) ──
        text_col = tk.Frame(inner, bg=bg)
        text_col.pack(side='left', fill='x', expand=True)

        self.title_var = tk.StringVar()
        self.id_var    = tk.StringVar()
        self.ver_var   = tk.StringVar()
        self.size_var  = tk.StringVar()

        tk.Label(text_col, textvariable=self.title_var,
                 font=(FONTS['body'][0], 11, 'bold'),
                 bg=bg, fg=COLORS['fg_0'], anchor='w'
                 ).pack(fill='x')

        meta_row = tk.Frame(text_col, bg=bg)
        meta_row.pack(fill='x', pady=(2, 0))
        tk.Label(meta_row, textvariable=self.id_var,
                 font=FONTS['mono_sm'],
                 bg=bg, fg=accent, anchor='w'
                 ).pack(side='left')
        tk.Label(meta_row, textvariable=self.ver_var,
                 font=FONTS['mono_sm'],
                 bg=bg, fg=COLORS['fg_4'], anchor='w'
                 ).pack(side='left', padx=(12, 0))

        tk.Label(text_col, textvariable=self.size_var,
                 font=FONTS['meta'],
                 bg=bg, fg=accent, anchor='w'
                 ).pack(fill='x', pady=(1, 0))

        # ── "Detected" pill (right) ──
        # Use 'done' kind for success, but mock has the pill match the tab
        # accent. Keep it simple — green pill works visually for either.
        StatusPill(inner, kind='done',
                   text='\u2713 Detected').pack(side='right', padx=(8, 0))



# ─────────────────────────────────────────────────────────────────────────
# SettingsCard — card for a settings group (title + optional hint header,
# rows packed directly into body with no extra padding).
# Used by ui/tab_settings.py for each section group.
# ─────────────────────────────────────────────────────────────────────────
class SettingsCard(tk.Frame):
    """A bordered card with a header (title + optional right-aligned hint)
    and a body the caller fills with `settings_row` (or any widget).

    Args:
        parent: parent widget
        title: section title shown in the header
        hint: optional right-aligned hint text (e.g. "last test 12s ago")
        right_widget: optional widget packed to the right of the title
                      (e.g. an "+ Add console" button)

    Attributes:
        body: the content frame; rows pack into this directly with no padding
    """

    def __init__(self, parent, title, hint=None, right_widget=None):
        bg = COLORS['bg_2']
        super().__init__(parent, bg=bg,
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)

        # ── Header (title + optional hint/widget) ──
        head = tk.Frame(self, bg=bg)
        head.pack(fill='x', padx=18, pady=(14, 12))
        tk.Label(head, text=title,
                 font=(FONTS['h3'][0], 11, 'bold'),
                 bg=bg, fg=COLORS['fg_0'], anchor='w'
                 ).pack(side='left')
        if right_widget is not None:
            # The caller built this widget against a different parent;
            # we just pack it. (E.g. an "+ Add console" button.)
            try:
                right_widget.pack(side='right')
            except Exception:
                pass
        elif hint:
            tk.Label(head, text=hint,
                     font=FONTS['mono_sm'],
                     bg=bg, fg=COLORS['fg_4']
                     ).pack(side='right')

        # Hairline divider under the header
        tk.Frame(self, bg=COLORS['border_2'], height=1).pack(fill='x')

        # ── Body — rows pack here ──
        self.body = tk.Frame(self, bg=bg)
        self.body.pack(fill='both', expand=True)


def settings_row(parent, label, description=None, with_divider=True):
    """Helper to build a 3-column settings row inside a SettingsCard.body.

    Layout:
        [label + description]  [control area]  [actions area]
        ──────────────────────────────────────────────────────  (divider)

    Returns the row's `(row_frame, control_frame, actions_frame)` so the
    caller packs widgets into the latter two. The row_frame can be ignored
    in most cases.

    The control_frame and actions_frame are bare tk.Frames; pack widgets
    using `side='left'` for typical horizontal layouts.
    """
    bg = COLORS['bg_2']
    row = tk.Frame(parent, bg=bg)
    row.pack(fill='x', padx=18, pady=10)

    # Use grid for the 3 columns so labels align across rows
    row.grid_columnconfigure(0, weight=0, minsize=200)
    row.grid_columnconfigure(1, weight=1)
    row.grid_columnconfigure(2, weight=0)

    # ── Label column ──
    label_col = tk.Frame(row, bg=bg)
    label_col.grid(row=0, column=0, sticky='w', padx=(0, 16))
    tk.Label(label_col, text=label,
             font=(FONTS['body'][0], 11),
             bg=bg, fg=COLORS['fg_1'], anchor='w'
             ).pack(fill='x')
    if description:
        tk.Label(label_col, text=description,
                 font=FONTS['meta'],
                 bg=bg, fg=COLORS['fg_4'], anchor='w',
                 wraplength=220, justify='left'
                 ).pack(fill='x', pady=(2, 0))

    # ── Control column (caller fills) ──
    control = tk.Frame(row, bg=bg)
    control.grid(row=0, column=1, sticky='ew')

    # ── Actions column (caller fills) ──
    actions = tk.Frame(row, bg=bg)
    actions.grid(row=0, column=2, sticky='e', padx=(8, 0))

    # ── Optional hairline below the row ──
    if with_divider:
        tk.Frame(parent, bg=COLORS['border_1'], height=1
                 ).pack(fill='x', padx=18)

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
    sub_bg       = COLORS.get('bg_2', '#1a1426')
    sub_active   = COLORS.get('bg_3', '#251e34')
    sub_idle_fg  = COLORS.get('fg_3', '#7a7388')
    sub_hover_fg = COLORS.get('fg_1', '#e0dcec')
    accent_fg    = COLORS.get('accent', '#b07ad6')
    border       = COLORS.get('border_4', '#2c2438')
    body_font    = FONTS.get('body', ('Segoe UI', 10))
    bg           = COLORS.get('bg_1', '#0a0812')

    # Sub-tab bar at the top of `parent`
    subbar = tk.Frame(parent, bg=sub_bg)
    subbar.pack(fill='x', padx=0, pady=0)

    # Per-key content frame (only one shown at a time)
    frames = {}
    for key, _label, _builder in items:
        frames[key] = tk.Frame(parent, bg=bg)

    state = {'name': default or items[0][0]}
    btns = {}

    def _restyle(name):
        b = btns.get(name)
        if not b:
            return
        if state['name'] == name:
            b.configure(fg=accent_fg, bg=sub_active)
        else:
            b.configure(fg=sub_idle_fg, bg=sub_bg)

    def activate(name):
        if name not in frames:
            return
        state['name'] = name
        for n in frames:
            _restyle(n)
            frames[n].pack_forget()
        frames[name].pack(fill='both', expand=True)

    for key, label, _builder in items:
        btn = tk.Label(subbar, text=label, padx=18, pady=10,
                       bg=sub_bg, fg=sub_idle_fg,
                       font=body_font,
                       cursor='hand2')
        btn.pack(side='left')
        btn.bind('<Button-1>', lambda e, n=key: activate(n))
        btn.bind('<Enter>',
                 lambda e, b=btn, n=key:
                     b.configure(fg=sub_hover_fg)
                     if state['name'] != n else None)
        btn.bind('<Leave>',
                 lambda e, n=key: _restyle(n))
        btns[key] = btn

    # 1px divider under the strip
    tk.Frame(parent, bg=border, height=1).pack(fill='x')

    # Build content (after the bar so the divider sits between them)
    for key, _label, builder in items:
        try:
            builder(frames[key])
        except Exception:
            # If a builder fails, leave its frame empty — don't
            # propagate the error and break the rest of the sub-tabs.
            import traceback
            traceback.print_exc()

    activate(state['name'])
    return activate
