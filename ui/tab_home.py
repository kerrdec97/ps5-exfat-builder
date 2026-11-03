"""
ui/tab_home.py — Home dashboard (v4.0 Entry 2 Phase 1).

The landing surface for the app: goal-first cards that route the user to the
right workflow. Every card routes through the EXISTING _switch_tab router —
no new navigation, no backend logic. Additive: the top bar and sidebar are
untouched; this is simply a new destination.

Presentation only. All colours/fonts via COLORS/FONTS tokens (standing rule).
"""

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)

from tkinter_theme import COLORS, FONTS

from exfat_builder import _
from ui.shared.scroll import attach_scroll


def build_home_tab(parent, app):
    """Build the Home dashboard into `parent`.

    Cards call app._switch_tab(<registry key>) — the same router the top bar
    and sidebar use. No new state, no backend change.
    """
    parent.configure(bg=COLORS['bg_1'])

    # ── Page-level scroll (consistent with the PS5 Overview page) ──
    page_canvas = tk.Canvas(parent, bg=COLORS['bg_1'], highlightthickness=0)
    page_sb = XScrollbar(parent, orient='vertical',
                           command=page_canvas.yview,
                           bg=COLORS['bg_3'], troughcolor=COLORS['bg_1'])
    page_canvas.configure(yscrollcommand=page_sb.set)
    page_sb.pack(side='right', fill='y')
    page_canvas.pack(side='left', fill='both', expand=True)

    body = tk.Frame(page_canvas, bg=COLORS['bg_1'])
    _win = page_canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>', lambda e:
              page_canvas.configure(scrollregion=page_canvas.bbox('all')))
    page_canvas.bind('<Configure>', lambda e:
                     page_canvas.itemconfig(_win, width=e.width))
    attach_scroll(page_canvas)

    def _go(key):
        # Route through the existing router. Guarded so a missing router
        # (very early build order) never raises.
        try:
            getattr(app, '_switch_tab', lambda k: None)(key)
        except Exception:
            pass

    # ── Hero / welcome header ──
    hero = tk.Frame(body, bg=COLORS['bg_1'])
    hero.pack(fill='x', padx=24, pady=(20, 6))
    tk.Label(hero, text=_('exFAT Image Builder'),
             font=(FONTS['body_strong'][0], 22),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(hero, text=_('What would you like to do?'),
             font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w').pack(anchor='w', pady=(2, 0))

    # ── Status strip: PS5 / FTP / Queue (reads existing StringVars when the
    # monolith exposes them; falls back to a static placeholder otherwise) ──
    strip = tk.Frame(body, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    strip.pack(fill='x', padx=24, pady=(12, 2))

    def _chip(var_name, fallback):
        var = getattr(app, var_name, None)
        cell = tk.Frame(strip, bg=COLORS['bg_2'])
        cell.pack(side='left', padx=14, pady=8)
        if var is not None:
            tk.Label(cell, textvariable=var, font=FONTS['meta'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_2']).pack(side='left')
        else:
            tk.Label(cell, text=fallback, font=FONTS['meta'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(side='left')

    def _sep():
        tk.Frame(strip, bg=COLORS['border_2'], width=1).pack(
            side='left', fill='y', pady=8)

    _chip('_ps5_status_var', _('PS5  \u25cf  \u2014'))
    _sep()
    _chip('_ftp_status_var', _('FTP  \u25cf  \u2014'))
    _sep()
    _chip('_queue_status_var', _('Queue  \u2014'))

    # ── Primary goal-first cards ──
    section = tk.Frame(body, bg=COLORS['bg_1'])
    section.pack(fill='x', padx=24, pady=(14, 10))

    grid = tk.Frame(section, bg=COLORS['bg_1'])
    grid.pack(fill='x')
    _COLS = 3
    for c in range(_COLS):
        grid.grid_columnconfigure(c, weight=1, uniform='homecard')

    # (registry key, icon, title, description). Routes are existing keys.
    cards = [
        ('unibuild', '\U0001f6e0', _('Build Image'),
         _('Create a new image from a game folder.')),
        ('extract',  '\U0001f4e4', _('Extract Image'),
         _('Unpack an image back into files and folders.')),
        ('convert',  '\U0001f504', _('Convert Image'),
         _('Change an existing image between exFAT and ffpkg.')),
        ('ps5',      '\U0001f3ae', _('PS5 Tools'),
         _('Connect to your PS5: transfer, payloads, kernel log.')),
        ('library',  '\U0001f4da', _('Library'),
         _('Browse and manage your local game images.')),
        ('report',   '\U0001f41e', _('Report Issue'),
         _('Report a problem or get help.')),
    ]

    def _make_card(col_idx, key, icon, title, desc):
        r, c = divmod(col_idx, _COLS)
        card = tk.Frame(grid, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        card.grid(row=r, column=c, sticky='nsew',
                  padx=(0 if c == 0 else 10, 0), pady=(0, 10))

        inner = tk.Frame(card, bg=COLORS['bg_3'])
        inner.pack(fill='both', expand=True, padx=14, pady=11)

        trow = tk.Frame(inner, bg=COLORS['bg_3'])
        trow.pack(fill='x')
        icon_tile = tk.Label(trow, text=icon, font=(FONTS['body'][0], 14),
                             bg=COLORS['accent_08'], fg=COLORS['teal'],
                             padx=6, pady=3)
        icon_tile.pack(side='left', padx=(0, 9))
        tk.Label(trow, text=title, font=(FONTS['body_strong'][0], 11),
                 bg=COLORS['bg_3'], fg=COLORS['fg_0'], anchor='w'
                 ).pack(side='left')
        # "Open →" affordance on the right of the title row — the whole
        # card is clickable, this just signals it.
        open_lbl = tk.Label(trow, text=_('Open \u2192'), font=FONTS['meta'],
                            bg=COLORS['bg_3'], fg=COLORS['accent_hi'])
        open_lbl.pack(side='right')

        desc_lbl = tk.Label(inner, text=desc, font=FONTS['meta'],
                            bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='w',
                            justify='left', wraplength=240)
        desc_lbl.pack(fill='x', pady=(7, 0))

        # Whole card is the click target; hover lifts the surface.
        _kids = (card, inner, trow, icon_tile, open_lbl, desc_lbl)
        def _enter(_e=None):
            for w in (card, inner, trow):
                w.configure(bg=COLORS['bg_4'])
        def _leave(_e=None):
            for w in (card, inner, trow):
                w.configure(bg=COLORS['bg_3'])
        for w in _kids:
            w.bind('<Button-1>', lambda e, k=key: _go(k))
            w.bind('<Enter>', _enter)
            w.bind('<Leave>', _leave)
            w.configure(cursor='hand2')

    for i, (key, icon, title, desc) in enumerate(cards):
        _make_card(i, key, icon, title, desc)
