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
from ui.shared.scroll import attach_scroll


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

    # v4.0 Fix 1: page-level scroll. Previously `body` was a plain
    # non-scrolling Frame and only the inner game list scrolled — after the
    # Phase 1-3 cards were added, the page overflowed the viewport and the
    # lower content (Tool Cards / Game Manager) was clipped. We now wrap the
    # ENTIRE page in one scroll canvas (same shared attach_scroll pattern as
    # other pages). The game list below is converted to a plain frame (see
    # _build_table) so there is exactly ONE scrollbar — no nested-canvas
    # weirdness; the game list simply extends the page.
    page_canvas = tk.Canvas(parent, bg=COLORS['bg_1'], highlightthickness=0)
    page_sb = tk.Scrollbar(parent, orient='vertical',
                           command=page_canvas.yview,
                           bg=COLORS['bg_3'], troughcolor=COLORS['bg_1'])
    page_canvas.configure(yscrollcommand=page_sb.set)
    page_sb.pack(side='right', fill='y')
    page_canvas.pack(side='left', fill='both', expand=True)
    app._ps5mgr_page_canvas = page_canvas

    body = tk.Frame(page_canvas, bg=COLORS['bg_1'])
    _page_win = page_canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>', lambda e:
              page_canvas.configure(scrollregion=page_canvas.bbox('all')))
    page_canvas.bind('<Configure>', lambda e:
                     page_canvas.itemconfig(_page_win, width=e.width))
    attach_scroll(page_canvas)

    _build_hero(body, app)
    _build_connection_card(body, app)
    _build_setup_checklist(body, app)
    _build_tool_cards(body, app)
    _build_recent_activity(body, app)
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
    # v4.0 Phase 1: the bare quick-launch buttons that used to live here were
    # replaced by the richer Tool Cards section below the hero
    # (see _build_tool_cards). Only "Refresh All" remains in the hero.
    actions = hero.actions_row()
    make_themed_button(actions, '\u21bb  ' + _('Refresh All'),
                       command=app._ps5mgr_refresh, kind='primary',
                       font_size=9, padx=12, pady=6
                       ).pack(side='left')

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
# Connection Card (v4.0 Phase 2) — surfaces connection info that already
# exists, in one clear place: online/offline status, IP, and the three
# service ports (FTP / Kernel Log / Payload). Test Connection reuses the
# existing _ps5mgr_refresh (the Manager's existing connection test/refresh).
#
# Presentation only. No new backend/connection/network logic, no new state.
# Ports are read from SETTINGS (always available) rather than the lazily-
# built _klog_port_var / _pl_port_var sub-tab vars (which don't exist until
# Kernel Log / Payloads are opened) — same values, same 2121/3232/9090
# defaults, robust regardless of lazy-build order.
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_card(body, app):
    section = tk.Frame(body, bg=COLORS['bg_1'])
    section.pack(fill='x', padx=24, pady=(4, 6))

    card = tk.Frame(section, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x')
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=16, pady=12)

    # ── Header row: title + status pill + Test Connection button ──
    head = tk.Frame(inner, bg=COLORS['bg_2'])
    head.pack(fill='x')
    tk.Label(head, text=_('PS5 Connection'),
             font=(FONTS['h3'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w'
             ).pack(side='left')

    # Test Connection — reuse the existing Manager refresh/test verbatim.
    make_themed_button(head, '\u21bb  ' + _('Test Connection'),
                       command=app._ps5mgr_refresh, kind='ghost',
                       font_size=9, padx=12, pady=5
                       ).pack(side='right')

    # Status pill (driven by the existing _ps5mgr_conn_var).
    status_lbl = tk.Label(head, text='', font=(FONTS['mono_sm'][0], 10, 'bold'),
                          bg=COLORS['bg_2'], fg=COLORS['fg_5'])
    status_lbl.pack(side='right', padx=(0, 14))

    # ── Detail row: IP + three service ports ──
    detail = tk.Frame(inner, bg=COLORS['bg_2'])
    detail.pack(fill='x', pady=(10, 0))

    def _field(parent, label, value, value_fg=None):
        col = tk.Frame(parent, bg=COLORS['bg_2'])
        col.pack(side='left', padx=(0, 32))
        tk.Label(col, text=label.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(anchor='w')
        v = tk.Label(col, text=value, font=(FONTS['mono_sm'][0], 11, 'bold'),
                     bg=COLORS['bg_2'],
                     fg=value_fg or COLORS['fg_1'], anchor='w')
        v.pack(anchor='w', pady=(2, 0))
        return v

    # Ports come from settings (always present, with the documented defaults).
    s = getattr(app, '_settings', {}) or {}
    ftp_port  = str(s.get('ftp_port', 2121))
    klog_port = str(s.get('klog_port', 3232))
    pl_port   = str(s.get('pl_port', 9090))
    ip_now    = app._ftp_ip_var.get().strip()

    ip_lbl = _field(detail, _('IP Address'),
                    ip_now or '\u2014',
                    value_fg=COLORS['teal'] if ip_now else COLORS['fg_5'])
    _field(detail, _('FTP'), ftp_port, value_fg=COLORS['fg_1'])
    _field(detail, _('Kernel Log'), klog_port, value_fg=COLORS['fg_1'])
    _field(detail, _('Payload'), pl_port, value_fg=COLORS['fg_1'])

    # ── Live status/IP wiring (reuse the existing connection var) ──
    def _sync(*_a):
        try:
            state = app._ps5mgr_conn_var.get()
            ip = app._ftp_ip_var.get().strip()
            ip_lbl.config(text=ip or '\u2014',
                          fg=COLORS['teal'] if ip else COLORS['fg_5'])
            if state == _('CONNECTED'):
                status_lbl.config(text='\u25cf ' + _('Online'),
                                  fg=COLORS['success'])
            elif state == _('OFFLINE'):
                status_lbl.config(text='\u25cf ' + _('Offline'),
                                  fg=COLORS['warn'])
            else:
                status_lbl.config(text='\u25cf ' + _('Not tested'),
                                  fg=COLORS['fg_5'])
        except Exception:
            pass
    try:
        app._ps5mgr_conn_var.trace_add('write', _sync)
        _sync()
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Setup Checklist (v4.0 Phase 3) — at-a-glance "what needs setup?" derived
# entirely from EXISTING state. Each item is a check/pending dot + label.
#
# Presentation only. No new backend logic, no new state. All values read
# from SETTINGS (always available, never lazy) EXCEPT the live "FTP
# reachable" row, which reads the Manager connection var (_ps5mgr_conn_var)
# — the corrected signal from the v4.0 status fix.
#
# Lazy-var note: the per-service config (al_dir, pl_ip, klog_ip) lives in
# settings rather than the sub-tab StringVars (_al_dir_var/_pl_ip_var/
# _klog_ip_var), because those are created inside their sub-tab builders and
# don't exist until those tabs are opened. Settings always have the values.
# ─────────────────────────────────────────────────────────────────────────
def _build_setup_checklist(body, app):
    section = tk.Frame(body, bg=COLORS['bg_1'])
    section.pack(fill='x', padx=24, pady=(0, 6))

    card = tk.Frame(section, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x')
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=16, pady=12)

    # Header row: title + "N / total complete" progress (v4.0 polish).
    hrow = tk.Frame(inner, bg=COLORS['bg_2'])
    hrow.pack(fill='x', pady=(0, 8))
    tk.Label(hrow, text=_('Setup Checklist'),
             font=(FONTS['h3'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w'
             ).pack(side='left')
    app._ps5ov_check_progress = tk.Label(
        hrow, text='', font=(FONTS['mono_sm'][0], 10, 'bold'),
        bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    app._ps5ov_check_progress.pack(side='right')

    rows = tk.Frame(inner, bg=COLORS['bg_2'])
    rows.pack(fill='x')

    # Track row widgets + their done/pending labels so the rows can show
    # explicit wording per state (not just a dot colour change).
    app._ps5ov_check_rows = {}

    def _row(key, done_label, pending_label):
        r = tk.Frame(rows, bg=COLORS['bg_2'])
        r.pack(fill='x', pady=2)
        dot = tk.Label(r, text='\u25cb', font=(FONTS['mono_sm'][0], 11),
                       bg=COLORS['bg_2'], fg=COLORS['fg_5'], width=2)
        dot.pack(side='left')
        txt = tk.Label(r, text=pending_label, font=FONTS['meta'],
                       bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w')
        txt.pack(side='left')
        app._ps5ov_check_rows[key] = {
            'dot': dot, 'txt': txt,
            'done': done_label, 'pending': pending_label, 'state': False}
        return dot, txt

    def _set(key, done):
        row = app._ps5ov_check_rows.get(key)
        if not row:
            return
        row['state'] = bool(done)
        try:
            if done:
                row['dot'].config(text='\u2713', fg=COLORS['success'])
                row['txt'].config(text=row['done'], fg=COLORS['fg_2'])
            else:
                # Explicit "not configured / not reachable" wording + a warn
                # glyph, so missing items are obvious at a glance.
                row['dot'].config(text='\u26a0', fg=COLORS['warn'])
                row['txt'].config(text=row['pending'], fg=COLORS['fg_4'])
        except Exception:
            pass

    # Build the rows — each with explicit done vs pending wording.
    _row('ip',      _('PS5 IP configured'),
                    _('PS5 IP not configured'))
    _row('ftp',     _('FTP reachable'),
                    _('FTP not reachable'))
    _row('klog',    _('Kernel Log configured'),
                    _('Kernel Log not configured'))
    _row('payload', _('Payload configured'),
                    _('Payload not configured'))
    _row('al_dir',  _('Payload folder configured'),
                    _('Payload folder not configured'))

    def _update_progress():
        try:
            rowmap = app._ps5ov_check_rows
            total = len(rowmap)
            done = sum(1 for r in rowmap.values() if r.get('state'))
            lbl = app._ps5ov_check_progress
            lbl.config(text='%d / %d %s' % (done, total, _('complete')),
                       fg=COLORS['success'] if done == total
                       else COLORS['fg_4'])
        except Exception:
            pass

    # Derive states from settings (always-available, lazy-safe).
    def _refresh(*_a):
        s = getattr(app, '_settings', {}) or {}
        _set('ip',      bool(str(s.get('ftp_ip', '')).strip()))
        _set('klog',    bool(str(s.get('klog_ip', '')).strip()))
        _set('payload', bool(str(s.get('pl_ip', '')).strip()))
        _set('al_dir',  bool(str(s.get('al_dir', '')).strip()))
        # FTP reachable ← the corrected Manager connection signal.
        try:
            _set('ftp', app._ps5mgr_conn_var.get() == _('CONNECTED'))
        except Exception:
            _set('ftp', False)
        _update_progress()   # refresh the "N / total complete" header

    # Live-update the FTP-reachable row when the connection state changes;
    # the rest refresh on each Manager build / refresh (settings are stable
    # within a session unless the user edits them in a sub-tab, which a
    # Refresh All / re-open will pick up).
    try:
        app._ps5mgr_conn_var.trace_add('write', _refresh)
    except Exception:
        pass
    _refresh()


# ─────────────────────────────────────────────────────────────────────────
# Tool Cards (v4.0 Phase 1) — richer replacement for the old bare
# quick-launch buttons. Each card: icon + title + plain-language
# description + (optional) live status + Open button. Open routes through
# the EXISTING PS5 sub-tab router (_ps5_subtab_activate) — no new
# navigation, no backend change. Pure presentation over already-wired
# routing and status vars.
# ─────────────────────────────────────────────────────────────────────────
def _build_tool_cards(body, app):
    def _go(key):
        # Reuse the existing PS5 sub-tab activation router. Guarded so a
        # missing activator (e.g. very early build order) never raises.
        try:
            getattr(app, '_ps5_subtab_activate', lambda k: None)(key)
        except Exception:
            pass

    section = tk.Frame(body, bg=COLORS['bg_1'])
    section.pack(fill='x', padx=24, pady=(4, 10))

    tk.Label(section, text=_('PS5 Tools'),
             font=(FONTS['h3'][0], 11, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_2'], anchor='w'
             ).pack(fill='x', pady=(0, 8))

    grid = tk.Frame(section, bg=COLORS['bg_1'])
    grid.pack(fill='x')

    # 3-column responsive grid (cards wrap to a second row).
    _COLS = 3
    for c in range(_COLS):
        grid.grid_columnconfigure(c, weight=1, uniform='toolcard')

    # (key, icon, title, description). Wording reuses the v3.6.x audit
    # refinements (Kernel Log, Y2JB spelled out, SMP/MicroMount phrasing).
    cards = [
        ('ftp',        '\U0001f4e1', _('File Transfer'),
         _('Browse and transfer files to and from your PS5.')),
        ('payloads',   '\U0001f680', _('Send Payloads'),
         _('Send .elf / .bin payloads to your PS5.')),
        ('klog',       '\U0001f4dd', _('Kernel Log'),
         _('Stream the PS5 kernel log live.')),
        ('y2jb',       '\U0001f3ac', 'Y2JB',
         _('YouTube to Jailbreak \u2014 install enabling PKGs.')),
        ('smp',        '\u26a1',     'ShadowMount+',
         _('Configure and deploy the ShadowMount+ automounter.')),
        ('micromount', '\U0001f5c2', 'MicroMount',
         _('Configure and deploy the MicroMount automounter.')),
    ]

    # Status-var labels per card, populated live where a source exists.
    # Stored on app so the live-sync hooks (below) can update them.
    app._ps5ov_status_lbls = {}

    def _make_card(col_idx, key, icon, title, desc):
        r, c = divmod(col_idx, _COLS)
        card = tk.Frame(grid, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        card.grid(row=r, column=c, sticky='nsew',
                  padx=(0 if c == 0 else 10, 0), pady=(0, 10))

        inner = tk.Frame(card, bg=COLORS['bg_3'])
        inner.pack(fill='both', expand=True, padx=14, pady=12)

        # Title row: icon + name (+ status pill on the right).
        trow = tk.Frame(inner, bg=COLORS['bg_3'])
        trow.pack(fill='x')
        tk.Label(trow, text=icon, font=(FONTS['h3'][0], 14),
                 bg=COLORS['bg_3'], fg=COLORS['teal']
                 ).pack(side='left', padx=(0, 8))
        tk.Label(trow, text=title, font=(FONTS['h3'][0], 11, 'bold'),
                 bg=COLORS['bg_3'], fg=COLORS['fg_1'], anchor='w'
                 ).pack(side='left')

        status_lbl = tk.Label(trow, text='', font=FONTS['meta'],
                              bg=COLORS['bg_3'], fg=COLORS['fg_5'])
        status_lbl.pack(side='right')
        app._ps5ov_status_lbls[key] = status_lbl

        # Description.
        tk.Label(inner, text=desc, font=FONTS['meta'],
                 bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='w',
                 justify='left', wraplength=240
                 ).pack(fill='x', pady=(6, 10))

        # Open button (routes through the existing sub-tab activator).
        make_themed_button(inner, _('Open'),
                           command=lambda k=key: _go(k), kind='ghost',
                           font_size=9, padx=12, pady=5
                           ).pack(anchor='w')

    for i, (key, icon, title, desc) in enumerate(cards):
        _make_card(i, key, icon, title, desc)

    # ── Live status wiring (guarded; sub-tab vars build lazily) ──
    # Connection status drives the FTP card; payload status drives the
    # Payloads card. Both read EXISTING vars — no new state. Guards mirror
    # the hero's _hook_payload_status retry pattern so a not-yet-built
    # sub-tab var never errors.
    def _set_status(key, text, kind='muted'):
        lbl = app._ps5ov_status_lbls.get(key)
        if lbl is None:
            return
        fg = {'ok': COLORS['success'], 'warn': COLORS['warn'],
              'muted': COLORS['fg_5']}.get(kind, COLORS['fg_5'])
        try:
            lbl.config(text=text, fg=fg)
        except Exception:
            pass

    # FTP card ← connection pill state.
    def _sync_ftp(*_a):
        try:
            state = app._ps5mgr_conn_var.get()
            if state == _('CONNECTED'):
                _set_status('ftp', _('Online'), 'ok')
            elif state == _('OFFLINE'):
                _set_status('ftp', _('Offline'), 'warn')
            else:
                _set_status('ftp', '', 'muted')
        except Exception:
            pass
    try:
        app._ps5mgr_conn_var.trace_add('write', _sync_ftp)
        _sync_ftp()
    except Exception:
        pass

    # Payloads card ← payload status var (built lazily; retry-hook).
    def _hook_payload(tries=0):
        var = getattr(app, '_pl_status_var', None)
        if var is None:
            if tries < 10:
                body.after(600, lambda: _hook_payload(tries + 1))
            return

        def _sync_pl(*_a):
            try:
                t = var.get()
                if not t:
                    return
                if t.startswith('\u2713'):
                    _set_status('payloads', _('Sent'), 'ok')
                elif t.startswith('\u2717'):
                    _set_status('payloads', _('Failed'), 'warn')
                elif 'Sending' in t or '%' in t:
                    _set_status('payloads', _('Sending\u2026'), 'muted')
            except Exception:
                pass
        try:
            var.trace_add('write', _sync_pl)
        except Exception:
            pass
    body.after(600, _hook_payload)


# ─────────────────────────────────────────────────────────────────────────
# Page head — title + count + bulk action buttons
# ─────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────
# Recent Activity (v4.0 Phase 5) — shows the last few PS5 events (payload
# sends, connection results, errors) from the app's lightweight activity
# log. Read-only presentation of _ps5_activity_recent(); registers a refresh
# callback (_ps5ov_activity_refresh) so newly-logged events appear live.
# The event tracking itself lives in the main file (the only new state the
# PS5 Overview adds). No backend/sync/FTP change here.
# ─────────────────────────────────────────────────────────────────────────
def _build_recent_activity(body, app):
    section = tk.Frame(body, bg=COLORS['bg_1'])
    section.pack(fill='x', padx=24, pady=(0, 6))

    card = tk.Frame(section, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x')
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=16, pady=12)

    tk.Label(inner, text=_('Recent PS5 Activity'),
             font=(FONTS['h3'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w'
             ).pack(fill='x', pady=(0, 8))

    rows_wrap = tk.Frame(inner, bg=COLORS['bg_2'])
    rows_wrap.pack(fill='x')

    # Icon per event kind (token-coloured in _render).
    _ICONS = {'payload': '\U0001f680', 'connection': '\U0001f4e1',
              'error': '\u26a0'}

    def _render():
        # Rebuild the rows from the current activity log.
        try:
            for w in rows_wrap.winfo_children():
                w.destroy()
        except Exception:
            return
        events = []
        try:
            events = app._ps5_activity_recent(5)
        except Exception:
            events = []

        if not events:
            tk.Label(rows_wrap,
                     text=_('No recent activity yet.'),
                     font=FONTS['meta'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_5'], anchor='w').pack(fill='x', pady=2)
            return

        for ev in events:
            kind = ev.get('kind', '')
            r = tk.Frame(rows_wrap, bg=COLORS['bg_2'])
            r.pack(fill='x', pady=2)
            icon = _ICONS.get(kind, '\u2022')
            icon_fg = (COLORS['warn'] if kind == 'error'
                       else COLORS['teal'])
            tk.Label(r, text=icon, font=(FONTS['mono_sm'][0], 10),
                     bg=COLORS['bg_2'], fg=icon_fg, width=2
                     ).pack(side='left')
            tk.Label(r, text=ev.get('detail', ''), font=FONTS['meta'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w'
                     ).pack(side='left')
            tk.Label(r, text=ev.get('time', ''),
                     font=(FONTS['mono_sm'][0], 8),
                     bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='e'
                     ).pack(side='right')

    # Register the live-refresh callback the activity logger nudges.
    app._ps5ov_activity_refresh = _render
    _render()


# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    # v4.0 Phase 4: visual separation between the Overview dashboard (hero /
    # connection / checklist / tools) and the Game Sync section below. A
    # divider marks the end of the dashboard; the header is strengthened with
    # a subtitle so the page reads as two distinct zones. Presentation only —
    # the legend, toolbar, table, filters, render, sync, and FTP are unchanged.
    divider = tk.Frame(body, bg=COLORS['bg_1'])
    divider.pack(fill='x', padx=24, pady=(8, 0))
    tk.Frame(divider, bg=COLORS['border_2'], height=1).pack(fill='x')

    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(16, 12))

    # Title + subtitle stacked in a column so the section reads as its own zone.
    title_col = tk.Frame(head, bg=COLORS['bg_1'])
    title_col.pack(side='left')
    tk.Label(title_col, text=_('PS5 Game Manager'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Compare local images with games on your PS5.'),
             font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w').pack(anchor='w', pady=(1, 0))

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

    # Game list body.
    # v4.0 Fix 1: this was a self-scrolling canvas (_ps5mgr_canvas). With the
    # new page-level scroll, a second canvas here would nest two scroll
    # regions (the classic tkinter scroll-fight). So the list is now a PLAIN
    # frame that grows naturally inside the page scroll — one scrollbar total.
    # `_ps5mgr_rows` (the frame _ps5mgr_render populates) is preserved exactly,
    # so the render logic in the main file is untouched.
    outer = tk.Frame(wrap, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
    outer.pack(fill='both', expand=True)

    app._ps5mgr_rows = tk.Frame(outer, bg=COLORS['bg_2'])
    app._ps5mgr_rows.pack(fill='both', expand=True)

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
