"""
ui/tab_home.py — Home (landing page).

Design-system layout:

    exFAT Image Builder
    What would you like to do?
    ┌ Status ──────────────────────────────────────────────────────────┐
    │ [ps5] PS5 console   [net] FTP transfer   [queue] Build queue      │
    │       ● badge             ● badge                ● badge          │
    └──────────────────────────────────────────────────────────────────┘
    QUICK ACTIONS
    [Build Image]   [Extract Image]   [Convert Image]
    [PS5 Tools]     [Library]         [Report Issue]

Every action card routes through the EXISTING app._switch_tab router — no
new navigation, no backend logic. The status cells follow the app's own
status variables (_ps5_status_var / _ftp_status_var / _queue_status_var);
they are read, never written.
"""

import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS, px
from ui.shared.widgets import (
    Scrollbar as XScrollbar, PageHeader, Card, Rounded, Badge, IconLabel,
    IconButton, SectionTitle, Divider, autowrap)
from ui.shared.status import tone_for, clean
from ui.icons import icon_image

from exfat_builder import _
from ui.shared.scroll import attach_scroll

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']


def build_home_tab(parent, app):
    """Build the Home page into `parent`.

    Cards call app._switch_tab(<registry key>) — the same router the
    sidebar uses. No new state, no backend change.
    """
    parent.configure(bg=BG1)

    # ── Page-level scroll ──
    page_canvas = tk.Canvas(parent, bg=BG1, highlightthickness=0, bd=0)
    page_sb = XScrollbar(parent, orient='vertical', command=page_canvas.yview)
    page_canvas.configure(yscrollcommand=page_sb.set)
    page_canvas.pack(side='left', fill='both', expand=True)

    body = tk.Frame(page_canvas, bg=BG1)
    _win = page_canvas.create_window((0, 0), window=body, anchor='nw')

    def _sync(_e=None):
        try:
            page_canvas.configure(scrollregion=page_canvas.bbox('all'))
            need = body.winfo_reqheight() > page_canvas.winfo_height() + 1
            if need and not page_sb.winfo_ismapped():
                page_sb.pack(side='right', fill='y', before=page_canvas)
            elif not need and page_sb.winfo_ismapped():
                page_sb.pack_forget()
                page_canvas.yview_moveto(0)
        except Exception:
            pass
    body.bind('<Configure>', _sync)
    page_canvas.bind('<Configure>', lambda e: (
        page_canvas.itemconfig(_win, width=e.width), _sync()))
    attach_scroll(page_canvas)

    def _go(key):
        # Route through the existing router. Guarded so a missing router
        # (very early build order) never raises.
        try:
            getattr(app, '_switch_tab', lambda k: None)(key)
        except Exception:
            pass

    # ── Page header ──
    hdr = PageHeader(body, _('exFAT Image Builder'),
                     _('What would you like to do?'))
    hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    # ── Status ──
    _build_status(body, app, _go)

    # ── Quick actions ──
    SectionTitle(body, 'Quick actions', overline=True).pack(
        fill='x', padx=px(24), pady=(px(8), px(8)))

    grid = tk.Frame(body, bg=BG1)
    grid.pack(fill='x', padx=px(24), pady=(0, px(24)))
    _COLS = 3
    for c in range(_COLS):
        grid.grid_columnconfigure(c, weight=1, uniform='homecard')

    # (registry key, icon, title, description). Routes are existing keys.
    cards = [
        ('unibuild', 'build', _('Build Image'),
         _('Create a new image from a game folder.')),
        ('extract',  'extract', _('Extract Image'),
         _('Unpack an image back into files and folders.')),
        ('convert',  'convert', _('Convert Image'),
         _('Change an existing image between exFAT and ffpkg.')),
        ('ps5',      'ps5', _('PS5 Tools'),
         _('Connect to your PS5: transfer, payloads, kernel log.')),
        ('library',  'library', _('Library'),
         _('Browse and manage your local game images.')),
        ('report',   'bug', _('Report Issue'),
         _('Report a problem or get help.')),
    ]
    # "Open →" affordance; the arrow becomes an icon.
    open_text = _('Open →').replace('→', '').strip()
    for i, (key, icon, title, desc) in enumerate(cards):
        r, c = divmod(i, _COLS)
        card = _action_card(grid, icon, title, desc, open_text,
                            lambda k=key: _go(k))
        # Split the 16px gutters so every card ends up the same width.
        gl = px(16) * c // _COLS
        gr = px(16) - px(16) * (c + 1) // _COLS
        card.grid(row=r, column=c, sticky='nsew', padx=(gl, gr),
                  pady=(0, px(16)))


# ─────────────────────────────────────────────────────────────────────────
# Status card — PS5 / FTP / queue, read from the app's status variables
# ─────────────────────────────────────────────────────────────────────────
_TONE = {'success': 'success', 'info': 'info', 'danger': 'danger',
         'idle': 'neutral'}


def _status_text(raw, prefix, empty):
    t = clean(raw or '')
    if prefix and t.upper().startswith(prefix.upper()):
        t = t[len(prefix):]
    t = re.sub(r'\s{2,}', ' ', t).strip(' —-')
    return t or empty


def _build_status(body, app, go):
    card = Card(body, title='Status',
                subtitle='Connection and queue at a glance. Open a page to '
                         'change it.')
    card.pack(fill='x', padx=px(24), pady=(0, px(16)))
    row = tk.Frame(card.body, bg=BG2)
    row.pack(fill='x')
    cells = [
        ('_ps5_status_var', _('PS5  ●  —'), 'PS5', 'ps5',
         'PS5 console', 'ps5', 'Not connected'),
        ('_ftp_status_var', _('FTP  ●  —'), 'FTP', 'network',
         'FTP transfer', 'ftp', 'Idle'),
        ('_queue_status_var', _('Queue  —'), 'Queue', 'queue',
         'Build queue', 'unibuild', 'No active jobs'),
    ]
    for i in range(len(cells)):
        row.grid_columnconfigure(i * 2, weight=1, uniform='hs')
    for i, (var_name, fallback, prefix, icon, label, route, empty) in \
            enumerate(cells):
        if i:
            Divider(row, vertical=True, color=C['border_1']).grid(
                row=0, column=i * 2 - 1, sticky='ns', padx=px(16))
        var = getattr(app, var_name, None)
        if var is None:
            var = tk.StringVar(value=fallback)
        cell = tk.Frame(row, bg=BG2)
        cell.grid(row=0, column=i * 2, sticky='ew')
        tile = tk.Label(cell, image=icon_image(icon, 16, C['fg_2'], BG3),
                        bg=BG3, width=px(32), height=px(32), bd=0)
        tile.pack(side='left', anchor='n')
        IconButton(cell, 'chevron_right', (lambda r=route: go(r)),
                   tooltip='Open', size='sm', surface=BG2).pack(
                       side='right', anchor='center')
        col = tk.Frame(cell, bg=BG2)
        col.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
        tk.Label(col, text=label, font=FONTS['body_strong'], bg=BG2,
                 fg=C['fg_1'], anchor='w').pack(fill='x')
        badge = Badge(col, '', 'neutral', surface=BG2)
        badge.pack(anchor='w', pady=(px(4), 0))

        def _refresh(*_a, v=var, b=badge, p=prefix, e=empty):
            try:
                raw = v.get()
            except Exception:
                return
            text = _status_text(raw, p, e)
            tone = _TONE.get(tone_for(raw), 'neutral') if raw else 'neutral'
            try:
                b.set(text, tone)
            except Exception:
                pass
        try:
            var.trace_add('write', _refresh)
        except Exception:
            pass
        _refresh()


# ─────────────────────────────────────────────────────────────────────────
# Quick-action card — icon tile, title, description, "Open" link
# ─────────────────────────────────────────────────────────────────────────
def _action_card(parent, icon, title, desc, open_text, command):
    card = Rounded(parent, bg=BG2, border=C['border_2'], radius=8,
                   padding=px(16), cursor='hand2')
    top = tk.Frame(card, bg=BG2)
    top.pack(fill='x')
    tile = tk.Label(top, image=icon_image(icon, 16, C['accent_hi'],
                                          C['accent_lo']),
                    bg=C['accent_lo'], width=px(32), height=px(32), bd=0)
    tile.pack(side='left')
    title_lbl = tk.Label(top, text=title, font=FONTS['subtitle'], bg=BG2,
                         fg=C['fg_0'], anchor='w')
    title_lbl.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
    desc_lbl = tk.Label(card, text=desc, font=FONTS['body'], bg=BG2,
                        fg=C['fg_3'], anchor='nw', justify='left')
    desc_lbl.pack(fill='both', expand=True, pady=(px(12), px(12)))
    autowrap(desc_lbl)
    link = tk.Frame(card, bg=BG2)
    link.pack(fill='x', side='bottom')
    link_lbl = tk.Label(link, text=open_text, font=FONTS['body_strong'],
                        bg=BG2, fg=C['accent_hi'])
    link_lbl.pack(side='left')
    arrow = IconLabel(link, 'arrow_right', 16, C['accent_hi'], BG2)
    arrow.pack(side='left', padx=(px(6), 0))

    plain = (top, title_lbl, desc_lbl, link, link_lbl)
    state = {'on': False}

    def paint(on):
        if state['on'] == on:
            return
        state['on'] = on
        bg = BG3 if on else BG2
        card.set_style(bg=bg, border=C['border_4'] if on else C['border_2'])
        for w in plain:
            w.configure(bg=bg)
        arrow.set(bg=bg)

    def leave(e):
        try:
            under = card.winfo_containing(e.x_root, e.y_root)
        except Exception:
            under = None
        while under is not None:
            if under is card:
                return
            under = getattr(under, 'master', None)
        paint(False)

    def bind(w):
        w.bind('<Button-1>', lambda e: command(), add='+')
        w.bind('<Enter>', lambda e: paint(True), add='+')
        w.bind('<Leave>', leave, add='+')
        try:
            w.configure(cursor='hand2')
        except Exception:
            pass
        for ch in w.winfo_children():
            bind(ch)
    bind(card)
    return card
