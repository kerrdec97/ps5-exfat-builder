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

from ui.shared.ps5_kit import ControlHero, scroll_empty_state
from ui.shared.page_head import make_themed_button


def build_ps5_mgr_tab(parent, app):
    """Build the PS5 Control Center tab body into `parent`.

    v3.6.0 PS5 pass: the slim connection bar + plain page head became a
    Build-style hero card (status badge, console stats strip, quick
    actions into the other PS5 sub-tabs). The game table below is
    unchanged; refresh logic in the main file is untouched.
    """
    parent.configure(bg=COLORS['bg_1'])
    app._ps5mgr_local  = []
    app._ps5mgr_remote = []
    app._ps5mgr_merged = []
    app._ps5mgr_filter_var = tk.StringVar()

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    _build_hero(body, app)
    _build_page_head(body, app)
    _build_legend(body, app)
    _build_toolbar(body, app)
    _build_table(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Control Center hero — replaces the old slim connection bar.
#
# Backwards compat: `_ps5mgr_render` in the main file recolors the
# connection pill (`_ps5mgr_conn_pill` / `_conn_pill_inner` / `_conn_dot`
# / `_conn_lbl` / `_conn_var`) and writes `_ps5mgr_storage_var`. All of
# those attributes still exist — the pill now lives in the hero's title
# row, and the storage var stays as a silent state holder (the hero's
# STORAGE / FREE cells carry the visible numbers).
# ─────────────────────────────────────────────────────────────────────────
def _build_hero(body, app):
    wrap = tk.Frame(body, bg=COLORS['bg_1'])
    wrap.pack(fill='x', padx=24, pady=(14, 6))

    ip_now = app._ftp_ip_var.get().strip()
    hero = ControlHero(
        wrap,
        title=_('PS5 Control Center'),
        subtitle=(ip_now or _('No PS5 configured — set the IP in the '
                              'FTP tab or Settings')),
        stats=[(_('IP Address'), 'ip'),
               (_('Firmware'), 'fw'),
               ('etaHEN', 'etahen'),
               ('FTP', 'ftp'),
               (_('Payload'), 'payload')],
        icon='\U0001f3ae', icon_size=72)
    hero.pack(fill='x')
    hero.add_strip([(_('Storage Used'), 'used'),
                    (_('Free Space'), 'free'),
                    (_('Temperature'), 'temp'),
                    (_('Games on PS5'), 'games'),
                    (_('Local Images'), 'local')])
    app._ps5mgr_hero = hero

    hero.set_stat('ip', ip_now or '\u2014')
    hero.set_badge(_('NOT TESTED'), 'wait')

    # ── Legacy connection pill, re-homed into the hero title row ──
    # Same widget structure as before so the existing recolor code in
    # _ps5mgr_render keeps working unchanged.
    app._ps5mgr_conn_var = tk.StringVar(value=_('NOT TESTED'))
    pill = tk.Frame(hero.title_row, bg=COLORS['bg_3'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    pill.pack(side='right', anchor='n', padx=(0, 8))
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
    app._ps5mgr_conn_pill = pill
    app._ps5mgr_conn_pill_inner = pill_inner

    # Hide the hero's own badge — the legacy pill IS the status badge
    # here (it's the widget the render callback drives).
    hero.set_badge('', 'wait')

    # Mirror pill state onto the hero badge-adjacent visuals: keep the
    # subtitle in sync with the configured IP on connection changes.
    def _on_conn(*_a):
        try:
            state = app._ps5mgr_conn_var.get()
            ip = app._ftp_ip_var.get().strip()
            hero.set_stat('ip', ip or '\u2014')
            hero.set_subtitle(ip or _('No PS5 configured'))
            if state == _('CONNECTED'):
                hero.set_stat('ftp', _('ONLINE') + ' \u00b7 :' +
                              (app._ftp_port_var.get().strip() or '2121'),
                              ok=True)
            elif state == _('OFFLINE'):
                hero.set_stat('ftp', _('OFFLINE'), warn=True)
        except Exception:
            pass
    app._ps5mgr_conn_var.trace_add('write', _on_conn)

    # Storage var must keep existing (render writes it); the visible
    # numbers come from the hero cells fed directly by the render.
    app._ps5mgr_storage_var = tk.StringVar(value='')

    # ── Quick actions ──
    actions = hero.actions_row()
    make_themed_button(actions, '\u21bb  ' + _('Refresh All'),
                       command=app._ps5mgr_refresh, kind='primary',
                       font_size=9, padx=12, pady=6
                       ).pack(side='left')

    def _go(key):
        try:
            getattr(app, '_ps5_subtab_activate', lambda k: None)(key)
        except Exception:
            pass

    for label, icon, key in [
            ('FTP',           '\U0001f4e1', 'ftp'),
            (_('Payloads'),   '\U0001f680', 'payloads'),
            ('Klog',          '\U0001f4dd', 'klog'),
            ('Y2JB',          '\U0001f3ac', 'y2jb'),
            ('ShadowMount+',  '\u26a1',     'smp'),
            ('MicroMount',    '\U0001f5c2', 'micromount')]:
        make_themed_button(actions, icon + '  ' + label,
                           command=lambda k=key: _go(k), kind='ghost',
                           font_size=9, padx=10, pady=6
                           ).pack(side='left', padx=(6, 0))

    # ── Payload status cell — mirrors the Payloads sub-tab's status
    # var once that sub-tab has been built (it builds after this one
    # in the same build_subtabs pass, so hook up with a short retry).
    def _hook_payload_status(tries=0):
        var = getattr(app, '_pl_status_var', None)
        if var is None:
            if tries < 10:
                body.after(600, lambda: _hook_payload_status(tries + 1))
            return

        def _sync(*_a):
            try:
                t = var.get()
                if not t:
                    return
                if t.startswith('\u2713'):
                    hero.set_stat('payload', _('SENT') + ' \u2713', ok=True)
                elif t.startswith('\u2717'):
                    hero.set_stat('payload', _('FAILED'), warn=True)
                elif 'Sending' in t or '%' in t:
                    hero.set_stat('payload', _('SENDING\u2026'))
            except Exception:
                pass
        var.trace_add('write', _sync)
    body.after(600, _hook_payload_status)


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

    # Empty state — shown until first refresh (shared component)
    _show_empty_state(app._ps5mgr_rows, app)


def _show_empty_state(parent, app=None):
    """Shared EmptyState inside the rows frame, with a Refresh action."""
    es = scroll_empty_state(
        parent, icon='\U0001f3ae',
        title=_('No data yet'),
        description=_('Refresh to scan local images and connect to '
                      'your PS5 over FTP.'),
        height=300)
    es.pack(fill='x')
    if app is not None:
        make_themed_button(es.actions, '\u21bb  ' + _('Refresh All'),
                           command=app._ps5mgr_refresh, kind='primary',
                           font_size=9, padx=12, pady=6).pack()


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
