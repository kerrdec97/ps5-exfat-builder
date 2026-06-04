"""
ui/tab_ps5_mgr.py — PS5 Game Manager tab.

Step 11 (v2.1.3): refactored against preview/ps5mgr-tab-redesign.html.

Layout:

    ┌─ connection bar (slim, top) ────────────────────────────────────┐
    │ ●CONNECTED  192.168.1.42  FW 5.50  Storage: 1.4/2.0 TB used    │
    │                              [Disconnect] [↻ Refresh All]       │
    ├─ page head ─────────────────────────────────────────────────────┤
    │  PS5 Game Manager  [14 on PS5 · 8 local · 6 unbuilt]            │
    │                          [⤓ Pull all] [↑ Upload all local]     │
    ├─ legend ────────────────────────────────────────────────────────┤
    │ ●On PS5  ●Built locally, not uploaded  ●Not built yet           │
    ├─ filter toolbar ────────────────────────────────────────────────┤
    │ 🔍 Search  [All][●On PS5][●Local only][●Size mismatch]          │
    ├─ table (sticky header) ─────────────────────────────────────────┤
    │   Game             Local    PS5     Status    Path     Actions  │
    │ 🎮 Astros Playroom 4.2GB    4.2GB   ●ON PS5   /data/   ▶ ✓ 🗑   │
    │ 🎮 Returnal        56.8GB   56.8GB  ●ON PS5   /data/   ▶ ✓ 🗑   │
    │ 🎮 Demon's Souls   62.4GB   23.7GB  ●UPLOAD   /tmp     ⏸ ✕      │
    │ ...                                                              │
    └─────────────────────────────────────────────────────────────────┘

Backwards compat: every `app._ps5mgr_*` attribute the existing 2 callbacks
read is preserved. The `_ps5mgr_render` callback is rewritten in place to
match the new table layout.
"""

import os
import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _


def build_ps5_mgr_tab(parent, app):
    """Build the redesigned PS5 Mgr tab body into `parent`."""
    parent.configure(bg=COLORS['bg_1'])
    app._ps5mgr_local  = []
    app._ps5mgr_remote = []
    app._ps5mgr_merged = []
    app._ps5mgr_filter_var = tk.StringVar()

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    _build_connection_bar(body, app)
    _build_page_head(body, app)
    _build_legend(body, app)
    _build_toolbar(body, app)
    _build_table(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Slim connection bar at the top
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_bar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_0'])
    bar.pack(fill='x')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=8)

    # Connection pill — shows static "Use Refresh" until populated by render.
    # The pill background/text update via _ps5mgr_conn_var trace.
    app._ps5mgr_conn_var = tk.StringVar(value=_('NOT TESTED'))
    pill = tk.Frame(inner, bg=COLORS['bg_3'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    pill.pack(side='left')
    pill_inner = tk.Frame(pill, bg=COLORS['bg_3'])
    pill_inner.pack(padx=10, pady=4)
    app._ps5mgr_conn_dot = tk.Frame(pill_inner, bg=COLORS['fg_5'],
                                     width=8, height=8)
    app._ps5mgr_conn_dot.pack(side='left', padx=(0, 6))
    app._ps5mgr_conn_dot.pack_propagate(False)
    app._ps5mgr_conn_lbl = tk.Label(pill_inner, textvariable=app._ps5mgr_conn_var,
             font=(FONTS['mono_sm'][0], 10, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['fg_4'])
    app._ps5mgr_conn_lbl.pack(side='left')
    # Stash the wrapper for color updates from _ps5mgr_render
    app._ps5mgr_conn_pill = pill
    app._ps5mgr_conn_pill_inner = pill_inner

    # IP info
    ip_text = app._ftp_ip_var.get().strip() or _('No PS5 configured')
    tk.Label(inner, text=ip_text,
             font=(FONTS['mono_sm'][0], 11, 'bold'),
             bg=COLORS['bg_0'], fg=COLORS['fg_1']
             ).pack(side='left', padx=(12, 12))

    # Storage info — derived from PS5 storage scan
    app._ps5mgr_storage_var = tk.StringVar(value='')
    tk.Label(inner, textvariable=app._ps5mgr_storage_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left')

    # Right-aligned action buttons
    _accent_btn(inner, '\u21bb  ' + _('Refresh All'),
                command=app._ps5mgr_refresh
                ).pack(side='right')

    tk.Frame(body, bg=COLORS['border_2'], height=1).pack(fill='x')


# ─────────────────────────────────────────────────────────────────────────
# Page head — title + count + bulk action buttons
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 12))

    tk.Label(head, text=_('PS5 Game Manager'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    # Count pill — fed by _ps5mgr_status_var (legacy compat)
    app._ps5mgr_status_var = tk.StringVar(
        value=_('Click Refresh All to load PS5 game list'))
    tk.Label(head, textvariable=app._ps5mgr_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             padx=8, pady=2,
             highlightbackground=COLORS['border_2'],
             highlightthickness=1
             ).pack(side='left', padx=(12, 0))


# ─────────────────────────────────────────────────────────────────────────
# Legend — color-coded dots explaining the row pills
# ─────────────────────────────────────────────────────────────────────────
def _build_legend(body, app):
    leg = tk.Frame(body, bg=COLORS['bg_1'])
    leg.pack(fill='x', padx=24, pady=(0, 10))

    items = [
        (COLORS['success'], _('On PS5')),
        (COLORS['warn'],    _('Built locally, not uploaded')),
        (COLORS['fg_5'],    _('Not built yet')),
    ]
    for color, label in items:
        item = tk.Frame(leg, bg=COLORS['bg_1'])
        item.pack(side='left', padx=(0, 18))
        dot = tk.Frame(item, bg=color, width=8, height=8)
        dot.pack(side='left', padx=(0, 6))
        dot.pack_propagate(False)
        tk.Label(item, text=label,
                 font=FONTS['meta'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_4']
                 ).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# Toolbar — search + chip filters
# ─────────────────────────────────────────────────────────────────────────
def _build_toolbar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_1'])
    bar.pack(fill='x', padx=24, pady=(0, 10))

    # Search input
    search_wrap = tk.Frame(bar, bg=COLORS['bg_0'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(side='left')
    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(search_wrap, textvariable=app._ps5mgr_filter_var,
             font=FONTS['body'],
             bg=COLORS['bg_0'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0,
             width=32
             ).pack(side='left', ipady=5, padx=(0, 8))

    # Live filter trigger — re-render with current data
    def _on_search(*_a):
        if app._ps5mgr_merged:
            try:
                app._ps5mgr_render(app._ps5mgr_merged,
                                   len(app._ps5mgr_local),
                                   len(app._ps5mgr_remote))
            except Exception:
                pass
    app._ps5mgr_filter_var.trace_add('write', _on_search)

    # Chip filters — visual only this iteration (would need callback work
    # to wire actual filter state)
    chips_frame = tk.Frame(bar, bg=COLORS['bg_1'])
    chips_frame.pack(side='left', padx=(10, 0))
    for label, color in [
        (_('All'),           None),
        (_('On PS5'),        COLORS['success']),
        (_('Local only'),    COLORS['warn']),
        (_('Size mismatch'), COLORS['accent']),
    ]:
        chip = tk.Frame(chips_frame, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        chip.pack(side='left', padx=2)
        chip_inner = tk.Frame(chip, bg=COLORS['bg_3'])
        chip_inner.pack(padx=10, pady=4)
        if color:
            dot = tk.Frame(chip_inner, bg=color, width=7, height=7)
            dot.pack(side='left', padx=(0, 6))
            dot.pack_propagate(False)
        tk.Label(chip_inner, text=label,
                 font=FONTS['meta'],
                 bg=COLORS['bg_3'], fg=COLORS['fg_3']
                 ).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# Table (sticky header + scrollable body)
# ─────────────────────────────────────────────────────────────────────────
# Column specs — (key, label, width or None=flex, anchor)
_COLS = [
    ('cover',   '',           48,   'w'),
    ('game',    _('Game'),    None, 'w'),
    ('local',   _('Local'),   90,   'e'),
    ('ps5',     _('PS5'),     90,   'e'),
    ('status',  _('Status'),  140,  'w'),
    ('path',    _('Path on PS5'), None, 'w'),
    ('actions', '',           110,  'e'),
]


def _build_table(body, app):
    """Sticky header + scrollable list area."""
    wrap = tk.Frame(body, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=24, pady=(0, 14))

    # Sticky header
    head = tk.Frame(wrap, bg=COLORS['bg_3'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=COLORS['bg_3'])
    head_inner.pack(fill='x', padx=14, pady=10)

    for i, (key, label, width, anchor) in enumerate(_COLS):
        if width is None:
            head_inner.grid_columnconfigure(i, weight=1, uniform='col')
        else:
            head_inner.grid_columnconfigure(i, weight=0, minsize=width)

    for i, (key, label, width, anchor) in enumerate(_COLS):
        tk.Label(head_inner, text=(label or '').upper(),
                 font=(FONTS['eyebrow'][0], 8, 'bold'),
                 bg=COLORS['bg_3'], fg=COLORS['fg_4'],
                 anchor=anchor
                 ).grid(row=0, column=i, sticky='ew',
                        padx=(0 if i == 0 else 8, 0))

    # Scrollable body
    outer = tk.Frame(wrap, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
    outer.pack(fill='both', expand=True)

    # Reuse legacy attribute name `_ps5mgr_canvas` and `_ps5mgr_rows` since
    # the existing _ps5mgr_render writes to them.
    app._ps5mgr_canvas = tk.Canvas(outer, bg=COLORS['bg_2'],
                                   highlightthickness=0)
    sb = tk.Scrollbar(outer, orient='vertical',
                      command=app._ps5mgr_canvas.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    app._ps5mgr_canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    app._ps5mgr_canvas.pack(side='left', fill='both', expand=True)

    app._ps5mgr_rows = tk.Frame(app._ps5mgr_canvas, bg=COLORS['bg_2'])
    win = app._ps5mgr_canvas.create_window((0, 0), window=app._ps5mgr_rows,
                                           anchor='nw')
    app._ps5mgr_rows.bind('<Configure>', lambda e:
        app._ps5mgr_canvas.configure(
            scrollregion=app._ps5mgr_canvas.bbox('all')))
    app._ps5mgr_canvas.bind('<Configure>', lambda e:
        app._ps5mgr_canvas.itemconfig(win, width=e.width))
    app._ps5mgr_canvas.bind('<MouseWheel>', lambda e:
        app._ps5mgr_canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    # Empty state — shown until first refresh
    _show_empty_state(app._ps5mgr_rows)


def _show_empty_state(parent):
    """Centered empty-state message inside the rows frame."""
    inner = tk.Frame(parent, bg=COLORS['bg_2'])
    inner.pack(fill='both', expand=True, pady=60)
    tk.Label(inner, text='\U0001f3ae',
             font=(FONTS['body'][0], 36),
             bg=COLORS['bg_2'], fg=COLORS['fg_6']
             ).pack()
    tk.Label(inner, text=_('No data yet'),
             font=(FONTS['body'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(pady=(8, 4))
    tk.Label(inner,
             text=_('Click "Refresh All" above to scan local images and '
                    'connect to your PS5.'),
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             wraplength=420, justify='center'
             ).pack()


# ─────────────────────────────────────────────────────────────────────────
# Helpers exposed for the rewritten _ps5mgr_render in main file
# ─────────────────────────────────────────────────────────────────────────
def parse_meta_from_filename(name):
    """Parse a CUSA/PPSA id and version out of an .exfat filename.

    Returns (game_id, version, display_title). Examples:
      'CUSA-12345-Astros.exfat'     -> ('CUSA-12345', None,    'Astros')
      'CUSA-22871-Returnal-v2.10.exfat' -> ('CUSA-22871', '2.10', 'Returnal')
      'PPSA-01325 Bloodborne.exfat' -> ('PPSA-01325', None,    'Bloodborne')
    """
    base = name.rsplit('.', 1)[0]
    gid = None
    m = re.search(r'((?:CUSA|PPSA|PPLH)[-_ ]?\d{5})', base, re.IGNORECASE)
    if m:
        gid = m.group(1).upper().replace('_', '-').replace(' ', '-')
        # Normalize CUSA12345 → CUSA-12345
        if '-' not in gid:
            gid = gid[:4] + '-' + gid[4:]
    ver = None
    m = re.search(r'v(\d+\.\d+)', base, re.IGNORECASE)
    if m:
        ver = m.group(1)
    # Strip the gid + 'v...' from base to get a title-ish display name
    cleaned = base
    if gid:
        cleaned = re.sub(r'(?:CUSA|PPSA|PPLH)[-_ ]?\d{5}', '', cleaned,
                         flags=re.IGNORECASE)
    if ver:
        cleaned = re.sub(r'v\d+\.\d+', '', cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r'^[\s\-_.]+|[\s\-_.]+$', '', cleaned).strip()
    cleaned = re.sub(r'[\s_]+', ' ', cleaned).strip()
    return gid, ver, cleaned or base


def humanize_size(sz):
    if sz is None:
        return '—'
    if sz >= 1024**3:
        return '%.1f GB' % (sz / 1024**3)
    if sz >= 1024**2:
        return '%d MB' % (sz // 1024**2)
    return '%d KB' % (sz // 1024) if sz else '0 KB'


def _ghost_btn(parent, text, command):
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9),
                     bg=COLORS['bg_2'], fg=COLORS['fg_2'],
                     activebackground=COLORS['bg_3'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1,
                     command=command)


def _accent_btn(parent, text, command):
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9, 'bold'),
                     bg=COLORS['accent'], fg=COLORS['fg_0'],
                     activebackground=COLORS['accent_hi'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     command=command)


def _icon_btn(parent, glyph, tooltip='', command=None,
              accent=False, danger=False):
    """Square icon button for the row Actions column."""
    fg = (COLORS['accent'] if accent
          else COLORS['danger_hi'] if danger
          else COLORS['fg_4'])
    btn = tk.Button(parent, text=glyph,
                    font=(FONTS['body'][0], 11),
                    bg=COLORS['bg_2'], fg=fg,
                    activebackground=COLORS['bg_3'],
                    activeforeground=fg,
                    relief='flat', bd=0,
                    padx=4, pady=2,
                    width=2,
                    cursor='hand2',
                    command=command)
    return btn
