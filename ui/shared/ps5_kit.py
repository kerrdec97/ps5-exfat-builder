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
    """A row of facts: caption label above a semibold value.

    stats: ordered list of (label, key) pairs. `.set(key, value,
    warn=False, ok=False)` updates a cell (warn -> warn_hi, ok ->
    success_hi). Values default to an em-dash.
    """

    def __init__(self, parent, stats, bg=None, gap=28):
        from tkinter_theme import px
        bg = bg or COLORS['bg_2']
        super().__init__(parent, bg=bg)
        self._bg = bg
        self._cells = {}
        for i, (label, key) in enumerate(stats):
            cell = tk.Frame(self, bg=bg)
            cell.pack(side='left', padx=(0 if i == 0 else px(gap), 0))
            lab = label.capitalize() if label.isupper() else label
            tk.Label(cell, text=lab, font=FONTS['caption'], bg=bg,
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
            v = tk.StringVar(value='\u2014')
            val = tk.Label(cell, textvariable=v, font=FONTS['body_strong'],
                           bg=bg, fg=COLORS['fg_0'], anchor='w')
            val.pack(anchor='w', pady=(px(2), 0))
            self._cells[key] = (v, val)

    def set(self, key, value, warn=False, ok=False):
        cell = self._cells.get(key)
        if not cell:
            return
        v, val = cell
        v.set(value if value not in (None, '') else '\u2014')
        try:
            val.config(fg=COLORS['warn_hi'] if warn else (
                COLORS['success_hi'] if ok else COLORS['fg_0']))
        except Exception:
            pass

    def get(self, key):
        cell = self._cells.get(key)
        return cell[0].get() if cell else ''


_HERO_TONE = {'ready': 'success', 'wait': 'neutral', 'busy': 'info',
              'warn': 'warn', 'fail': 'danger', 'error': 'danger',
              'ok': 'success', 'off': 'neutral'}


class ControlHero(tk.Frame):
    """Hero card for PS5 tool pages (design system › Card): icon tile,
    title + subtitle, status badge, facts strip(s) and an actions row.

    API: set_title, set_subtitle, set_badge, set_stat, add_strip,
    actions_row() / .actions — unchanged.
    """

    def __init__(self, parent, title='', subtitle='', stats=(),
                 icon='\U0001f3ae', icon_size=64):
        from tkinter_theme import px
        from ui.shared.widgets import Rounded, IconLabel, Badge, autowrap
        from ui.icons import leading_icon, has_icon
        super().__init__(parent, bg=COLORS['bg_1'], highlightthickness=0)
        card = Rounded(self, bg=COLORS['bg_2'], border=COLORS['border_2'],
                       radius=8, padding=px(16), surface=COLORS['bg_1'])
        card.pack(fill='both', expand=True)
        name = icon if has_icon(str(icon)) else (leading_icon(str(icon)) or 'ps5')
        tile = Rounded(card, bg=COLORS['accent_lo'], radius=8,
                       surface=COLORS['bg_2'])
        tile.pack(side='left', anchor='n')
        self._icon_lbl = IconLabel(tile, name, 24, COLORS['accent_hi'],
                                   COLORS['accent_lo'])
        self._icon_lbl.pack(padx=px(12), pady=px(12))
        right = tk.Frame(card, bg=COLORS['bg_2'])
        right.pack(side='left', fill='both', expand=True, padx=(px(16), 0))
        title_row = tk.Frame(right, bg=COLORS['bg_2'])
        title_row.pack(fill='x')
        tcol = tk.Frame(title_row, bg=COLORS['bg_2'])
        tcol.pack(side='left', fill='x', expand=True)
        self.title_var = tk.StringVar(value=title or '')
        tk.Label(tcol, textvariable=self.title_var, font=FONTS['subtitle'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(
                     anchor='w', fill='x')
        self.subtitle_var = tk.StringVar(value=subtitle or '')
        sub = tk.Label(tcol, textvariable=self.subtitle_var,
                       font=FONTS['caption'], bg=COLORS['bg_2'],
                       fg=COLORS['fg_3'], anchor='w', justify='left')
        sub.pack(anchor='w', fill='x', pady=(px(2), 0))
        autowrap(sub)
        self._badge = Badge(title_row, '', 'neutral', surface=COLORS['bg_2'])
        self._badge_shown = False
        self.strip = StatStrip(right, stats) if stats else None
        if self.strip is not None:
            self.strip.pack(fill='x', pady=(px(14), 0))
        self.title_row = title_row
        self._right = right
        self._actions = None
        self._extra_strips = []

    def add_strip(self, stats):
        from tkinter_theme import px
        s = StatStrip(self._right, stats)
        s.pack(fill='x', pady=(px(10), 0))
        self._extra_strips.append(s)
        return s

    def set_title(self, title):
        self.title_var.set((title or '') or '\u2014')

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
        try:
            if not text:
                if self._badge_shown:
                    self._badge.pack_forget()
                    self._badge_shown = False
                return
            self._badge.set(text, _HERO_TONE.get(kind, 'neutral'))
            if not self._badge_shown:
                self._badge.pack(side='right', anchor='n')
                self._badge_shown = True
        except Exception:
            pass

    def actions_row(self):
        from tkinter_theme import px
        if self._actions is None:
            self._actions = tk.Frame(self._right, bg=COLORS['bg_2'])
            self._actions.pack(fill='x', pady=(px(14), 0))
        return self._actions

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
