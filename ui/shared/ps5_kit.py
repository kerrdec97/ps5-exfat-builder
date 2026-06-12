"""ui/shared/ps5_kit.py — shared chrome for the PS5 section.

v3.6.0 "PS5 Control Center" pass. These components let every PS5 page
(Manager, FTP, Klog, Payloads, Y2JB, ShadowMount+, MicroMount) reuse the
exact Build-tab visual language instead of inventing per-tab styles:

- ControlHero — the Build-style hero card (icon tile + title + status
                badge + stats strip + actions row). Mirrors
                ui/shared/hero.GameHero but is console/tool-centric:
                no cover-art loader, adds an `.actions` row.
- StatStrip   — just the Build hero's stats strip, standalone, for
                pages where a full hero is too heavy (Klog, FTP
                browser). Same fonts/colors as GameHero's strip.
- scroll_empty_state — wraps ui/shared/empty_state.EmptyState so it
                renders correctly inside pack/canvas scroll frames
                (EmptyState centers via place(), which collapses to
                0 height in a pack context without an explicit size).

Pure presentation: no FTP, payload, socket, or protocol code in here.
All colors/fonts come from tkinter_theme tokens — no hex literals.
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS

from ui.shared.empty_state import EmptyState

# Same badge schemes as ui/shared/hero.GameHero so the two hero types
# read as one family.
_BADGE = {
    'ready':   (COLORS['success'], '#0a0a0a'),
    'wait':    (COLORS['bg_4'],    COLORS['fg_3']),
    'busy':    (COLORS['accent'],  '#0a0a0a'),
    'warn':    (COLORS['warn'],    '#0a0a0a'),
    'fail':    (COLORS['danger'],  COLORS['fg_0']),
}


class StatStrip(tk.Frame):
    """The Build hero's stats strip as a standalone widget.

    stats: ordered list of (label, key) pairs.
    Use `.set(key, value, warn=False)` to update a cell. Values default
    to an em-dash. Cell styling is identical to GameHero's strip:
    uppercase mono label in fg_5, bold mono value in teal (warn flips
    the value to the warn token).
    """

    def __init__(self, parent, stats, bg=None, gap=26):
        bg = bg or COLORS['bg_2']
        super().__init__(parent, bg=bg)
        self._bg = bg
        self._cells = {}
        for i, (label, key) in enumerate(stats):
            cell = tk.Frame(self, bg=bg)
            cell.pack(side='left', padx=(0 if i == 0 else gap, 0))
            tk.Label(cell, text=label.upper(),
                     font=(FONTS['mono_sm'][0], 8, 'bold'), bg=bg,
                     fg=COLORS['fg_5'], anchor='w').pack(anchor='w')
            v = tk.StringVar(value='\u2014')
            val = tk.Label(cell, textvariable=v,
                           font=(FONTS['mono_sm'][0], 11, 'bold'),
                           bg=bg, fg=COLORS['teal'], anchor='w')
            val.pack(anchor='w', pady=(2, 0))
            self._cells[key] = (v, val)

    def set(self, key, value, warn=False, ok=False):
        cell = self._cells.get(key)
        if not cell:
            return
        v, val = cell
        v.set(value if value not in (None, '') else '\u2014')
        try:
            if warn:
                val.config(fg=COLORS['warn'])
            elif ok:
                val.config(fg=COLORS['success_hi'])
            else:
                val.config(fg=COLORS['teal'])
        except Exception:
            pass

    def get(self, key):
        cell = self._cells.get(key)
        return cell[0].get() if cell else ''


class ControlHero(tk.Frame):
    """Build-style hero card for PS5 tool pages.

    Layout mirrors ui/shared/hero.GameHero:

        ┌──────────────────────────────────────────────────────────┐
        │ [icon]  TITLE                          [STATUS BADGE]    │
        │  tile   subtitle (mono, muted)                            │
        │         LABEL    LABEL    LABEL    LABEL   (stats strip)  │
        │         value    value    value    value                  │
        │         [action] [action] [action] …       (actions row)  │
        └──────────────────────────────────────────────────────────┘

    Args:
        parent:  parent widget
        title:   hero title (rendered uppercase, like GameHero)
        subtitle: small mono line under the title
        stats:   ordered list of (label, key) pairs for the strip
        icon:    glyph for the icon tile
        icon_size: tile edge in px (default 64 — smaller than the
                 150px cover tile because there's no cover art here)

    Attributes:
        actions: a frame under the stats strip — caller packs themed
                 buttons (ui.shared.page_head.make_themed_button) into
                 it. Created lazily on first access via `.actions_row()`
                 so heroes without actions don't reserve the space.

    API: set_title, set_subtitle, set_badge, set_stat — same verbs as
    GameHero so call sites read identically.
    """

    def __init__(self, parent, title='', subtitle='', stats=(),
                 icon='\U0001f3ae', icon_size=64):
        super().__init__(parent, bg=COLORS['bg_2'],
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)

        pad = tk.Frame(self, bg=COLORS['bg_2'])
        pad.pack(fill='both', expand=True, padx=20, pady=16)

        # ── icon tile (accent-tinted square, like the Build cover
        #    tile family but sized for a glyph) ──
        tile = tk.Frame(pad, bg=COLORS['accent_08'], width=icon_size,
                        height=icon_size,
                        highlightbackground=COLORS['accent_15'],
                        highlightthickness=1)
        tile.pack(side='left', anchor='n')
        tile.pack_propagate(False)
        self._icon_lbl = tk.Label(tile, text=icon, bg=COLORS['accent_08'],
                                  fg=COLORS['accent'],
                                  font=(FONTS['body'][0],
                                        max(16, int(icon_size * 0.42))))
        self._icon_lbl.pack(expand=True)

        # ── right column ──
        right = tk.Frame(pad, bg=COLORS['bg_2'])
        right.pack(side='left', fill='both', expand=True, padx=(16, 0))

        title_row = tk.Frame(right, bg=COLORS['bg_2'])
        title_row.pack(fill='x')

        tcol = tk.Frame(title_row, bg=COLORS['bg_2'])
        tcol.pack(side='left', fill='x', expand=True)
        self.title_var = tk.StringVar(value=(title or '').upper())
        tk.Label(tcol, textvariable=self.title_var,
                 font=(FONTS['h2'][0], 16, 'bold'), bg=COLORS['bg_2'],
                 fg=COLORS['fg_0'], anchor='w').pack(anchor='w', fill='x')
        self.subtitle_var = tk.StringVar(value=subtitle or '')
        tk.Label(tcol, textvariable=self.subtitle_var,
                 font=(FONTS['mono_sm'][0], 9), bg=COLORS['bg_2'],
                 fg=COLORS['fg_4'], anchor='w', justify='left'
                 ).pack(anchor='w', pady=(2, 0))

        # status badge (top-right, GameHero style)
        self._badge = tk.Label(title_row, text='',
                               font=(FONTS['mono_sm'][0], 9, 'bold'),
                               bg=COLORS['bg_4'], fg=COLORS['fg_3'],
                               padx=10, pady=4)
        self._badge.pack(side='right', anchor='n')

        # stats strip
        self.strip = StatStrip(right, stats) if stats else None
        if self.strip is not None:
            self.strip.pack(fill='x', pady=(14, 0))

        self.title_row = title_row
        self._right = right
        self._actions = None
        self._extra_strips = []

    def add_strip(self, stats):
        """Add a second (or third) stats-strip row under the first.

        Returns the StatStrip. `set_stat` falls through to extra rows,
        so callers address every cell through the hero regardless of
        which row it lives on.
        """
        s = StatStrip(self._right, stats)
        s.pack(fill='x', pady=(10, 0))
        self._extra_strips.append(s)
        return s

    # ── public API (GameHero verbs) ──
    def set_title(self, title):
        self.title_var.set((title or '').upper() or '\u2014')

    def set_subtitle(self, text):
        self.subtitle_var.set(text or '')

    def set_stat(self, key, value, warn=False, ok=False):
        if self.strip is not None and key in self.strip._cells:
            self.strip.set(key, value, warn=warn, ok=ok)
            return
        for s in self._extra_strips:
            if key in s._cells:
                s.set(key, value, warn=warn, ok=ok)
                return

    def set_badge(self, text, kind='ready'):
        bg, fg = _BADGE.get(kind, _BADGE['wait'])
        prefix = '\u2713  ' if kind == 'ready' else ''
        try:
            self._badge.config(text=(prefix + text) if text else '',
                               bg=bg, fg=fg)
        except Exception:
            pass

    def actions_row(self):
        """Return the actions row frame, creating it on first call."""
        if self._actions is None:
            self._actions = tk.Frame(self._right, bg=COLORS['bg_2'])
            self._actions.pack(fill='x', pady=(14, 0))
        return self._actions

    # Convenience alias so call sites can do `hero.actions` after the
    # row exists (mirrors Card.actions naming).
    @property
    def actions(self):
        return self.actions_row()


def scroll_empty_state(parent, icon, title, description, height=280):
    """Build a shared EmptyState that behaves inside pack/canvas
    scroll frames.

    EmptyState centers its content with place(), so in a pack context
    its requested height is 0 and it vanishes. Give it an explicit
    height and stop geometry propagation so it renders as a proper
    centered block. Returns the EmptyState (caller packs it, and can
    fill `.actions`).
    """
    es = EmptyState(parent, icon=icon, title=title,
                    description=description, bg='bg_2')
    es.configure(height=height)
    es.pack_propagate(False)
    return es
