"""
ui/tab_home.py — Home dashboard (v4.0 Entry 2 Phase 1).

The landing surface for the app: goal-first cards that route the user to the
right workflow. Every card routes through the EXISTING _switch_tab router —
no new navigation, no backend logic. Additive: the top bar and sidebar are
untouched; this is simply a new destination.

Presentation only. All colours/fonts via COLORS/FONTS tokens (standing rule).
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import _
from ui.shared.page_head import make_themed_button
from ui.shared.scroll import attach_scroll


def build_home_tab(parent, app):
    """Build the Home dashboard into `parent`.

    Cards call app._switch_tab(<registry key>) — the same router the top bar
    and sidebar use. No new state, no backend change.
    """
    parent.configure(bg=COLORS['bg_1'])

    # ── Page-level scroll (consistent with the PS5 Overview page) ──
    page_canvas = tk.Canvas(parent, bg=COLORS['bg_1'], highlightthickness=0)
    page_sb = tk.Scrollbar(parent, orient='vertical',
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
             font=(FONTS['h1'][0], 22, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(hero, text=_('What would you like to do?'),
             font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w').pack(anchor='w', pady=(2, 0))

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
                  padx=(0 if c == 0 else 12, 0), pady=(0, 12))

        inner = tk.Frame(card, bg=COLORS['bg_3'])
        inner.pack(fill='both', expand=True, padx=16, pady=14)

        trow = tk.Frame(inner, bg=COLORS['bg_3'])
        trow.pack(fill='x')
        tk.Label(trow, text=icon, font=(FONTS['h2'][0], 18),
                 bg=COLORS['bg_3'], fg=COLORS['teal']
                 ).pack(side='left', padx=(0, 10))
        tk.Label(trow, text=title, font=(FONTS['h3'][0], 13, 'bold'),
                 bg=COLORS['bg_3'], fg=COLORS['fg_0'], anchor='w'
                 ).pack(side='left')

        tk.Label(inner, text=desc, font=FONTS['meta'],
                 bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='w',
                 justify='left', wraplength=240
                 ).pack(fill='x', pady=(8, 12))

        make_themed_button(inner, _('Open'),
                           command=lambda k=key: _go(k), kind='primary',
                           font_size=9, padx=14, pady=6
                           ).pack(anchor='w')

    for i, (key, icon, title, desc) in enumerate(cards):
        _make_card(i, key, icon, title, desc)
