"""
ui/tab_ftp.py — FTP tab.

Step 12 (v2.1.4): refactored against preview/ftp-tab-redesign.html.

Layout:

    ┌─ subtab bar ────────────────────────────────────────────────────┐
    │ [↑ Quick Upload] [📂 PS5 Browser ←active]                       │
    ├─ connection bar ────────────────────────────────────────────────┤
    │ ●CONNECTED  192.168.1.42:2121  FW 5.50  latency 4ms             │
    │                              [Disconnect] [↻ Refresh]           │
    ├─ workspace ─────────────────────────────────────────────────────┤
    │ ┌─ Local pane ───────┐ ┌─ → ←┐ ┌─ PS5 pane ────────────────┐    │
    │ │ Local  [path][⌂]   │ │     │ │ PS5   [path][⌂]            │   │
    │ │ ↑ ↻ 📁+ 🗑 ✎       │ │     │ │ ↑ ↻ 📁+ 🗑 ✎               │   │
    │ │ 📁 ..              │ │     │ │ 📁 ..                       │   │
    │ │ 💾 file1.exfat     │ │     │ │ 💾 file1.exfat              │   │
    │ │ ...                │ │     │ │ ...                          │   │
    │ │ 7 items, 2 sel     │ │     │ │ 7 items, 0 sel               │   │
    │ └────────────────────┘ └─────┘ └─────────────────────────────┘   │
    │ ┌─ Transfers ─────────────────────────────────────────────────┐  │
    │ │ Transfers  1 active · 0 queued · 0 completed                │  │
    │ │ ↑ CUSA-30188.exfat  D:/ → /data/  38%  412 MB/s  UPLOADING  │  │
    │ └────────────────────────────────────────────────────────────┘  │
    └─────────────────────────────────────────────────────────────────┘

The Quick Upload sub-tab keeps its form layout, restyled with design
tokens.

Backwards compat: every `_ftptab_*`, `_br_*`, and `_ftp_sub*` attribute
the existing 30+ callbacks read is preserved with the same name. Existing
single-pane PS5 browser callbacks continue to work because the right
pane keeps using `_br_listbox` etc.

NEW attributes for the local pane (no existing callbacks read these —
they're driven entirely by helpers in this module):
  _lp_path     — StringVar, current local directory
  _lp_listbox  — Listbox showing local entries
  _lp_entries  — list of (name, is_dir, size_bytes) tuples
  _lp_status   — StringVar showing "N items, M selected"

NEW attributes for transfer panel (visual only this iteration):
  _xfers_list_frame — container for transfer rows
"""

import os
import tkinter as tk
from tkinter import filedialog, messagebox

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.cards import Card
from ui.shared.ps5_kit import StatStrip


# ─────────────────────────────────────────────────────────────────────────
# Public entry
# ─────────────────────────────────────────────────────────────────────────
def build_ftp_tab(parent, app):
    """Build the redesigned FTP tab into `parent`."""
    parent.configure(bg=COLORS['bg_1'])

    # Sub-tab state
    app._ftp_subtab_var = tk.StringVar(value='browser')

    # ── Initialise all browser + local-pane state up front ──
    # The connection bar references _br_status / _br_free, so these must
    # exist BEFORE _build_connection_bar runs. (Earlier versions created
    # them inside _build_browser_subtab and crashed on startup.)
    app._br_path      = tk.StringVar(
        value=app._settings.get('ftp_path', '/data/etaHEN/games/'))
    app._br_status    = tk.StringVar(value=_('Not connected'))
    app._br_free      = tk.StringVar(value='')
    app._br_sel_info  = tk.StringVar(value='')
    app._br_ftp       = [None]
    app._br_entries   = []
    app._br_clipboard = [None]

    initial_local = app._settings.get('ftp_local_path',
                                       os.path.expanduser('~'))
    if not os.path.isdir(initial_local):
        initial_local = os.path.expanduser('~')
    app._lp_path      = tk.StringVar(value=initial_local)
    app._lp_entries   = []
    app._lp_status    = tk.StringVar(value='')
    app._lp_clipboard = []

    # v3.6.0 PS5 pass: dashboard stat cells that mirror connection
    # state. _conn_set() walks this list.
    app._ftp_conn_cells = []

    # ── Sub-tab bar ──
    _build_subtab_bar(parent, app)

    # ── Connection bar ──
    _build_connection_bar(parent, app)

    # ── Content frames ──
    app._ftp_sub_upload_frame  = tk.Frame(parent, bg=COLORS['bg_1'])
    app._ftp_sub_browser_frame = tk.Frame(parent, bg=COLORS['bg_1'])

    # Build sub-tab content
    _build_upload_subtab(app._ftp_sub_upload_frame, app)
    _build_browser_subtab(app._ftp_sub_browser_frame, app)

    # Show the browser sub-tab by default (per mock)
    app._ftp_switch_sub('browser')


# ─────────────────────────────────────────────────────────────────────────
# Sub-tab bar
# ─────────────────────────────────────────────────────────────────────────
def _build_subtab_bar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_0'])
    bar.pack(fill='x')
    tk.Frame(bar, bg=COLORS['border_2'], height=1).pack(side='bottom', fill='x')

    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=16)

    def _make_subtab(text, key):
        wrap = tk.Frame(inner, bg=COLORS['bg_0'])
        wrap.pack(side='left')
        btn = tk.Button(wrap, text=text,
                        font=(FONTS['button'][0], 10, 'bold'),
                        bg=COLORS['bg_0'], fg=COLORS['fg_4'],
                        activebackground=COLORS['bg_0'],
                        activeforeground=COLORS['fg_1'],
                        relief='flat', bd=0,
                        padx=18, pady=10,
                        cursor='hand2',
                        command=lambda k=key: app._ftp_switch_sub(k))
        btn.pack()
        underline = tk.Frame(wrap, bg=COLORS['bg_0'], height=2)
        underline.pack(fill='x')
        btn._underline = underline
        return btn

    app._ftp_sub_upload_btn  = _make_subtab('\u2191  ' + _('Quick Upload'),
                                             'upload')
    app._ftp_sub_browser_btn = _make_subtab('\U0001f4c2  ' + _('PS5 Browser'),
                                             'browser')


# ─────────────────────────────────────────────────────────────────────────
# Connection bar (slim, top)
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_bar(parent, app):
    """Slim connection bar. Reuses `app._br_status` and `app._br_free`
    StringVars that the existing _br_* callbacks update."""
    bar = tk.Frame(parent, bg=COLORS['bg_0'])
    bar.pack(fill='x')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=16, pady=8)

    # Connection pill — visual reflection of _br_status content
    app._ftp_conn_pill_var = tk.StringVar(value=_('NOT CONNECTED'))

    pill = tk.Frame(inner, bg=COLORS['bg_3'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    pill.pack(side='left')
    pill_inner = tk.Frame(pill, bg=COLORS['bg_3'])
    pill_inner.pack(padx=10, pady=4)
    app._ftp_conn_dot = tk.Frame(pill_inner, bg=COLORS['fg_5'],
                                  width=8, height=8)
    app._ftp_conn_dot.pack(side='left', padx=(0, 6))
    app._ftp_conn_dot.pack_propagate(False)
    app._ftp_conn_lbl = tk.Label(pill_inner,
                                  textvariable=app._ftp_conn_pill_var,
                                  font=(FONTS['mono_sm'][0], 10, 'bold'),
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'])
    app._ftp_conn_lbl.pack(side='left')
    app._ftp_conn_pill = pill
    app._ftp_conn_pill_inner = pill_inner

    # IP info
    ip = app._ftp_ip_var.get().strip() or _('No PS5 IP set')
    port = app._ftp_port_var.get().strip() or '2121'
    tk.Label(inner, text='%s:%s' % (ip, port),
             font=(FONTS['mono_sm'][0], 11, 'bold'),
             bg=COLORS['bg_0'], fg=COLORS['fg_1']
             ).pack(side='left', padx=(12, 12))

    # Status text — reused from _br_status
    tk.Label(inner, textvariable=app._br_status,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(0, 12))

    # Free space text — reused from _br_free
    tk.Label(inner, textvariable=app._br_free,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left')

    # Right-aligned action buttons
    _accent_btn(inner, '\u21bb  ' + _('Connect / Refresh'),
                command=app._br_connect
                ).pack(side='right')
    _ghost_btn(inner, _('Disconnect'),
               command=app._br_disconnect
               ).pack(side='right', padx=(0, 6))

    tk.Frame(parent, bg=COLORS['border_2'], height=1).pack(fill='x')

    # When _br_status changes, also update the connection pill colors.
    # Heuristic: if status starts with "Connected" or contains "files in",
    # treat as connected.
    def _sync_pill(*_a):
        try:
            s = app._br_status.get().lower()
            if 'connect' in s and 'not' not in s and 'fail' not in s:
                _conn_set(app, True)
            elif 'files in' in s or 'items in' in s:
                _conn_set(app, True)
            elif 'disconnect' in s or 'not connect' in s or 'error' in s:
                _conn_set(app, False)
        except Exception:
            pass
    app._br_status.trace_add('write', _sync_pill)


def _conn_set(app, connected):
    """Update connection pill colors (and any dashboard cells)."""
    # v3.6.0 PS5 pass: dashboard stat cells registered by the upload
    # dashboard / browser strip mirror the same state.
    for strip, key in getattr(app, '_ftp_conn_cells', []):
        try:
            if connected:
                strip.set(key, _('CONNECTED') + ' \u2713', ok=True)
            else:
                strip.set(key, _('OFFLINE'), warn=True)
        except Exception:
            pass
    try:
        if connected:
            app._ftp_conn_pill_var.set(_('CONNECTED'))
            app._ftp_conn_dot.configure(bg=COLORS['success'])
            app._ftp_conn_pill.configure(highlightbackground=COLORS['success'])
            app._ftp_conn_pill_inner.configure(bg=COLORS['success_bg'])
            app._ftp_conn_lbl.configure(bg=COLORS['success_bg'],
                                         fg=COLORS['success_hi'])
        else:
            app._ftp_conn_pill_var.set(_('OFFLINE'))
            app._ftp_conn_dot.configure(bg=COLORS['fg_5'])
            app._ftp_conn_pill.configure(highlightbackground=COLORS['border_2'])
            app._ftp_conn_pill_inner.configure(bg=COLORS['bg_3'])
            app._ftp_conn_lbl.configure(bg=COLORS['bg_3'], fg=COLORS['fg_4'])
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# QUICK UPLOAD sub-tab — restyled form
# ─────────────────────────────────────────────────────────────────────────
def _build_upload_subtab(parent, app):
    """Quick Upload sub-tab — single-file/folder upload form.

    Same logic as the legacy implementation, restyled with design tokens.
    Every `_ftptab_*` attribute is preserved.
    """
    app._ftptab_local_var  = tk.StringVar()
    app._ftptab_remote_var = tk.StringVar(
        value=app._settings.get('ftp_path', '/data/etaHEN/games/'))
    app._ftptab_is_folder  = tk.BooleanVar(value=False)
    app._ftptab_uploading  = False
    app._ftptab_pct        = 0
    app._ftptab_cancel_flag = False

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=24, pady=14)

    # Header
    tk.Label(body, text=_('File Transfer Dashboard'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(body,
             text=_('Upload a single file or folder directly. For multi-'
                    'file work, use the PS5 Browser sub-tab\u2019s dual '
                    'pane.'),
             font=FONTS['body'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(anchor='w', pady=(2, 12))

    # ── Stats strip card (v3.6.0 PS5 pass) ──
    # CONNECTION mirrors the connection bar; TARGET PATH follows the
    # remote-path field; UPLOAD reflects the live single-shot upload;
    # BROWSER PATH / FILES mirror the PS5 Browser's current listing.
    stats_card = tk.Frame(body, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    stats_card.pack(fill='x', pady=(0, 12))
    strip = StatStrip(stats_card,
                      [(_('Connection'), 'conn'),
                       (_('Target Path'), 'target'),
                       (_('Upload'), 'xfer'),
                       (_('Browser Path'), 'brpath'),
                       (_('Files Listed'), 'files')])
    strip.pack(fill='x', padx=18, pady=12)
    app._ftp_dash_strip = strip
    app._ftp_conn_cells.append((strip, 'conn'))
    strip.set('conn', _('OFFLINE'), warn=True)
    strip.set('xfer', _('IDLE'))

    def _trim(p, n=34):
        p = p or ''
        return ('\u2026' + p[-(n - 1):]) if len(p) > n else (p or '\u2014')

    def _sync_target(*_a):
        try:
            strip.set('target', _trim(app._ftptab_remote_var.get().strip()))
        except Exception:
            pass
    app._ftptab_remote_var.trace_add('write', _sync_target)
    _sync_target()

    def _sync_xfer_cell(*_a):
        try:
            t = app._ftptab_status_var.get().strip()
            if not t:
                strip.set('xfer', _('IDLE'))
            elif t.startswith('\u2713') or 'complete' in t.lower():
                strip.set('xfer', _('DONE') + ' \u2713', ok=True)
            elif t.startswith('\u2717') or 'fail' in t.lower() \
                    or 'cancel' in t.lower():
                strip.set('xfer', t.split('\u2014')[0].strip()[:18],
                          warn=True)
            else:
                strip.set('xfer', _('ACTIVE') + ' \u00b7 ' + t[:14])
        except Exception:
            pass

    def _poll_browser_cells():
        try:
            if not stats_card.winfo_exists():
                return
        except Exception:
            return
        try:
            strip.set('brpath', _trim(app._br_path.get().strip()))
            strip.set('files', str(len(getattr(app, '_br_entries', []) or [])))
        except Exception:
            pass
        try:
            stats_card.after(1000, _poll_browser_cells)
        except Exception:
            pass
    stats_card.after(1000, _poll_browser_cells)

    # ── Connection information card ──
    info_card = Card(body, title=_('Connection information'),
                     icon='\U0001f50c',
                     subtitle=_('Set the PS5 IP in Settings or the '
                                'connection bar above. Default FTP ports: '
                                '2121 (ftpsrv) / 1337 (etaHEN).'))
    info_card.pack(fill='x', pady=(0, 12))
    ci = tk.Frame(info_card.body, bg=COLORS['bg_2'])
    ci.pack(fill='x')

    def _info_cell(label, var_getter):
        cell = tk.Frame(ci, bg=COLORS['bg_2'])
        cell.pack(side='left', padx=(0, 26))
        tk.Label(cell, text=label.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(anchor='w')
        v = tk.StringVar(value=var_getter())
        tk.Label(cell, textvariable=v,
                 font=(FONTS['mono_sm'][0], 11, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w'
                 ).pack(anchor='w', pady=(2, 0))
        return v

    ip_cell   = _info_cell(_('PS5 IP'),
                           lambda: app._ftp_ip_var.get().strip() or '\u2014')
    port_cell = _info_cell(_('FTP Port'),
                           lambda: app._ftp_port_var.get().strip() or '2121')
    _info_cell(_('Default Path'), lambda: '/data/etaHEN/games/')
    app._ftp_ip_var.trace_add('write', lambda *a: ip_cell.set(
        app._ftp_ip_var.get().strip() or '\u2014'))
    app._ftp_port_var.trace_add('write', lambda *a: port_cell.set(
        app._ftp_port_var.get().strip() or '2121'))

    # ── Upload form card ──
    form_card = Card(body, title=_('Quick upload'), icon='\u2191')
    form_card.pack(fill='x', pady=(0, 12))
    form = form_card.body

    # Type selector — pill toggle row
    type_row = tk.Frame(form, bg=COLORS['bg_2'])
    type_row.pack(fill='x', pady=(0, 12))
    tk.Label(type_row, text=_('Upload type:'),
             font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(side='left', padx=(0, 12))
    for val, lbl in [(False, _('File')), (True, _('Folder'))]:
        tk.Radiobutton(type_row, text=lbl,
                       variable=app._ftptab_is_folder, value=val,
                       font=FONTS['body'],
                       bg=COLORS['bg_2'], fg=COLORS['fg_2'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['fg_0'],
                       selectcolor=COLORS['bg_3'],
                       cursor='hand2',
                       command=app._ftptab_update_browse
                       ).pack(side='left', padx=(0, 16))

    # Local path
    lbl_row = tk.Frame(form, bg=COLORS['bg_2'])
    lbl_row.pack(fill='x')
    app._ftptab_local_lbl = tk.Label(lbl_row, text=_('Local file'),
                                      font=FONTS['label'],
                                      bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                                      anchor='w')
    app._ftptab_local_lbl.pack(side='left')

    local_inner = tk.Frame(form, bg=COLORS['bg_2'])
    local_inner.pack(fill='x', pady=(4, 12))
    _dark_entry(local_inner, app._ftptab_local_var, readonly=True
                ).pack(side='left', fill='x', expand=True)
    app._ftptab_browse_btn = _ghost_btn(local_inner, _('Browse'),
                                         command=app._ftptab_browse)
    app._ftptab_browse_btn.pack(side='left', padx=(6, 0))

    # Remote path
    tk.Label(form, text=_('PS5 remote path'),
             font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w'
             ).pack(fill='x')
    remote_inner = tk.Frame(form, bg=COLORS['bg_2'])
    remote_inner.pack(fill='x', pady=(4, 12))
    _dark_entry(remote_inner, app._ftptab_remote_var, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(remote_inner, _('Default'),
               command=lambda: app._ftptab_remote_var.set('/data/etaHEN/games/')
               ).pack(side='left', padx=(6, 0))

    # Action row — Upload button + Cancel
    btn_row = tk.Frame(form, bg=COLORS['bg_2'])
    btn_row.pack(fill='x', pady=(8, 0))
    app._ftptab_upload_btn = _accent_btn(btn_row,
        '\U0001f4e1  ' + _('Upload to PS5'),
        command=app._ftptab_start_upload)
    app._ftptab_upload_btn.pack(side='left')
    app._ftptab_cancel_btn = tk.Button(btn_row,
        text='\u2715  ' + _('Cancel'),
        font=(FONTS['button'][0], 9),
        bg=COLORS['bg_2'], fg=COLORS['danger_hi'],
        activebackground=COLORS['bg_3'],
        activeforeground=COLORS['danger_hi'],
        relief='flat', bd=0,
        padx=12, pady=8,
        cursor='hand2',
        highlightbackground=COLORS['danger'],
        highlightthickness=1,
        command=app._ftptab_cancel)
    # Not packed by default — legacy code packs/unpacks during upload

    # Status + progress (inside the form card — the "upload queue"
    # surface for this single-shot uploader)
    app._ftptab_status_var = tk.StringVar(value='')
    tk.Label(form, textvariable=app._ftptab_status_var,
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['success_hi'],
             anchor='w'
             ).pack(fill='x', pady=(14, 4))

    # Mirror the upload state into the dashboard strip
    app._ftptab_status_var.trace_add('write', lambda *a: _sync_xfer_cell())

    bar_bg = tk.Frame(form, bg=COLORS['bg_4'], height=8)
    bar_bg.pack(fill='x', pady=(0, 4))
    bar_bg.pack_propagate(False)
    app._ftptab_canvas = tk.Canvas(bar_bg, height=8, bg=COLORS['bg_4'],
                                    highlightthickness=0)
    app._ftptab_canvas.pack(fill='both', expand=True)
    app._ftptab_bar = app._ftptab_canvas.create_rectangle(
        0, 0, 0, 8, fill=COLORS['accent'], outline='')
    app._ftptab_canvas.bind('<Configure>',
        lambda e: app._ftptab_set_bar(app._ftptab_pct))

    app._ftptab_eta_var = tk.StringVar(value='')
    tk.Label(form, textvariable=app._ftptab_eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x')

    # Output log — its own card so the dashboard reads as discrete
    # surfaces (strip / connection / upload / log)
    log_card = Card(body, title=_('Output log'), icon='\U0001f4dd')
    log_card.pack(fill='both', expand=True)
    log_frame = tk.Frame(log_card.body, bg=COLORS['bg_3'],
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
    log_frame.pack(fill='both', expand=True)
    app._ftptab_log = tk.Text(log_frame,
                              font=FONTS['mono_sm'],
                              bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                              insertbackground=COLORS['accent'],
                              relief='flat', bd=6,
                              state='disabled', wrap='word',
                              height=8)
    sb = tk.Scrollbar(log_frame, command=app._ftptab_log.yview,
                      bg=COLORS['bg_4'], troughcolor=COLORS['bg_2'])
    app._ftptab_log.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    app._ftptab_log.pack(fill='both', expand=True)


# ─────────────────────────────────────────────────────────────────────────
# PS5 BROWSER sub-tab — DUAL PANE (Local | PS5) + transfers panel
# ─────────────────────────────────────────────────────────────────────────
def _build_browser_subtab(parent, app):
    """Dual-pane file browser. Left = local FS, Right = PS5 FTP.
    Bottom = transfers panel (visual scaffold; queue wiring deferred).

    State (`_br_*` and `_lp_*` StringVars) is initialised in build_ftp_tab
    before this runs — needed because the connection bar already references
    `_br_status`.
    """

    # ── Workspace: top = panes, bottom = transfers ──
    # We use grid so the transfers panel has a fixed height and the panes
    # row expands.
    ws = tk.Frame(parent, bg=COLORS['bg_1'])
    ws.pack(fill='both', expand=True)
    ws.grid_rowconfigure(0, weight=0)   # stats strip
    ws.grid_rowconfigure(1, weight=1)   # panes
    ws.grid_rowconfigure(2, weight=0)   # transfers
    ws.grid_columnconfigure(0, weight=1)

    # ── Stats strip (v3.6.0 PS5 pass) ──
    # Connection summary · current PS5 path · listed files · selection ·
    # upload queue. All values mirror state the existing callbacks
    # already maintain — no new FTP traffic.
    _build_browser_strip(ws, app).grid(row=0, column=0, sticky='ew',
                                       padx=16, pady=(12, 8))

    # ── Top — panes container ──
    panes_row = tk.Frame(ws, bg=COLORS['bg_1'])
    panes_row.grid(row=1, column=0, sticky='nsew',
                   padx=16, pady=(0, 8))
    panes_row.grid_columnconfigure(0, weight=1, uniform='pane')
    panes_row.grid_columnconfigure(1, weight=0)
    panes_row.grid_columnconfigure(2, weight=1, uniform='pane')
    panes_row.grid_rowconfigure(0, weight=1)

    _build_local_pane(panes_row, app).grid(row=0, column=0, sticky='nsew')
    _build_divider(panes_row, app).grid(row=0, column=1, sticky='ns', padx=10)
    _build_remote_pane(panes_row, app).grid(row=0, column=2, sticky='nsew')

    # ── Bottom — transfers panel ──
    _build_xfers_panel(ws, app).grid(row=2, column=0, sticky='ew',
                                      padx=16, pady=(0, 12))


def _build_browser_strip(parent, app):
    """Compact stats strip above the dual panes (v3.6.0 PS5 pass)."""
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    strip = StatStrip(card,
                      [(_('Connection'), 'conn'),
                       (_('Current Path'), 'path'),
                       (_('Files'), 'files'),
                       (_('Selected'), 'sel'),
                       (_('Upload Queue'), 'queue')])
    strip.pack(fill='x', padx=18, pady=10)
    app._ftp_browser_strip = strip
    app._ftp_conn_cells.append((strip, 'conn'))
    strip.set('conn', _('OFFLINE'), warn=True)
    strip.set('queue', '0')

    def _trim(p, n=30):
        p = p or ''
        return ('\u2026' + p[-(n - 1):]) if len(p) > n else (p or '\u2014')

    def _sync_path(*_a):
        try:
            strip.set('path', _trim(app._br_path.get().strip()))
        except Exception:
            pass
    app._br_path.trace_add('write', _sync_path)
    _sync_path()

    def _sync_sel(*_a):
        try:
            txt = app._br_sel_info.get().strip()
            strip.set('sel', txt[:24] if txt else '\u2014')
        except Exception:
            pass
    app._br_sel_info.trace_add('write', _sync_sel)

    def _poll():
        try:
            if not card.winfo_exists():
                return
        except Exception:
            return
        try:
            strip.set('files', str(len(getattr(app, '_br_entries', []) or [])))
            active = 1 if getattr(app, '_xfer_active_row', None) else 0
            strip.set('queue',
                      ('1 ' + _('ACTIVE')) if active else '0',
                      ok=bool(active))
        except Exception:
            pass
        try:
            card.after(1000, _poll)
        except Exception:
            pass
    card.after(1000, _poll)
    return card


def _build_local_pane(parent, app):
    """Build the local-side (left) pane. Returns the pane Frame."""
    pane = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)

    # Head — label + path input + home button
    head = tk.Frame(pane, bg=COLORS['bg_3'])
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=COLORS['bg_3'])
    head_inner.pack(fill='x', padx=10, pady=8)
    tk.Label(head_inner, text=_('Local'),
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['fg_2']
             ).pack(side='left', padx=(0, 8))
    _dark_entry(head_inner, app._lp_path, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _icon_btn(head_inner, '\u2302', tooltip='Home',
              command=lambda: _lp_navigate(app, os.path.expanduser('~'))
              ).pack(side='left', padx=(4, 0))

    # Bind Enter on the path entry to navigate
    head_inner.winfo_children()[1].winfo_children()[0].bind(
        '<Return>', lambda e: _lp_navigate(app, app._lp_path.get()))

    # Toolbar
    tb = tk.Frame(pane, bg=COLORS['bg_2'])
    tb.pack(fill='x', padx=10, pady=(4, 4))
    _icon_btn(tb, '\u2191', tooltip='Up one dir',
              command=lambda: _lp_navigate(app, os.path.dirname(app._lp_path.get()))
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\u21bb', tooltip='Refresh',
              command=lambda: _lp_load(app)
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\U0001f4c1+', tooltip='New folder',
              command=lambda: _lp_new_folder(app)
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\U0001f5d1', tooltip='Delete', danger=True,
              command=lambda: _lp_delete(app)
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\u270e', tooltip='Rename',
              command=lambda: _lp_rename(app)
              ).pack(side='left', padx=2)

    # File list
    list_outer = tk.Frame(pane, bg=COLORS['bg_2'])
    list_outer.pack(fill='both', expand=True, padx=10, pady=(0, 4))

    app._lp_listbox = tk.Listbox(list_outer,
                                  font=FONTS['mono_sm'],
                                  bg=COLORS['bg_2'], fg=COLORS['fg_2'],
                                  selectbackground=COLORS['accent'],
                                  selectforeground=COLORS['fg_0'],
                                  activestyle='none',
                                  relief='flat', bd=4,
                                  selectmode='extended',
                                  highlightthickness=0)
    sb = tk.Scrollbar(list_outer, command=app._lp_listbox.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    app._lp_listbox.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    app._lp_listbox.pack(fill='both', expand=True)
    app._lp_listbox.bind('<Double-Button-1>', lambda e: _lp_open_selected(app))
    app._lp_listbox.bind('<<ListboxSelect>>', lambda e: _lp_update_status(app))

    # Foot — selection info
    foot = tk.Frame(pane, bg=COLORS['bg_3'])
    foot.pack(fill='x', side='bottom')
    tk.Label(foot, textvariable=app._lp_status,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(side='left', padx=10, pady=4)

    # Initial load — defer slightly so widgets exist
    parent.after(50, lambda: _lp_load(app))

    return pane


def _build_divider(parent, app):
    """Vertical divider between panes with → / ← transfer arrows."""
    div = tk.Frame(parent, bg=COLORS['bg_1'])

    # Center the arrows vertically
    spacer_top = tk.Frame(div, bg=COLORS['bg_1'])
    spacer_top.pack(fill='both', expand=True)

    arrows = tk.Frame(div, bg=COLORS['bg_1'])
    arrows.pack()

    _accent_btn(arrows, '\u2192',
                command=lambda: _xfer_to_remote(app)
                ).pack(pady=4)
    _ghost_btn(arrows, '\u2190',
               command=lambda: _xfer_to_local(app)
               ).pack(pady=4)

    spacer_bot = tk.Frame(div, bg=COLORS['bg_1'])
    spacer_bot.pack(fill='both', expand=True)

    return div


def _build_remote_pane(parent, app):
    """Build the PS5-side (right) pane. Returns the pane Frame.

    Uses the existing `_br_*` attributes and callbacks. The Listbox is
    `_br_listbox` so all existing callbacks keep working.
    """
    pane = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)

    # Head — label + path input + home button
    head = tk.Frame(pane, bg=COLORS['bg_3'])
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=COLORS['bg_3'])
    head_inner.pack(fill='x', padx=10, pady=8)
    tk.Label(head_inner, text=_('PS5'),
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['fg_2']
             ).pack(side='left', padx=(0, 8))
    _dark_entry(head_inner, app._br_path, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _icon_btn(head_inner, '\u2302', tooltip='Home',
              command=lambda: app._br_load('/data/etaHEN/games/')
              ).pack(side='left', padx=(4, 0))

    # Bind Enter on path entry
    head_inner.winfo_children()[1].winfo_children()[0].bind(
        '<Return>', lambda e: app._br_load(app._br_path.get()))

    # Toolbar — uses existing _br_* callbacks
    tb = tk.Frame(pane, bg=COLORS['bg_2'])
    tb.pack(fill='x', padx=10, pady=(4, 4))
    _icon_btn(tb, '\u2191', tooltip='Up one dir',
              command=app._br_go_up
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\u21bb', tooltip='Refresh',
              command=lambda: app._br_load(app._br_path.get())
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\U0001f4c1+', tooltip='New folder',
              command=app._br_new_folder
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\U0001f5d1', tooltip='Delete', danger=True,
              command=app._br_delete
              ).pack(side='left', padx=2)
    _icon_btn(tb, '\u270e', tooltip='Rename',
              command=app._br_rename
              ).pack(side='left', padx=2)

    # File list — uses _br_listbox (legacy attribute)
    list_outer = tk.Frame(pane, bg=COLORS['bg_2'])
    list_outer.pack(fill='both', expand=True, padx=10, pady=(0, 4))

    app._br_listbox = tk.Listbox(list_outer,
                                  font=FONTS['mono_sm'],
                                  bg=COLORS['bg_2'], fg=COLORS['fg_2'],
                                  selectbackground=COLORS['accent'],
                                  selectforeground=COLORS['fg_0'],
                                  activestyle='none',
                                  relief='flat', bd=4,
                                  selectmode='extended',
                                  highlightthickness=0)
    sb = tk.Scrollbar(list_outer, command=app._br_listbox.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    app._br_listbox.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    app._br_listbox.pack(fill='both', expand=True)
    app._br_listbox.bind('<Double-Button-1>', lambda e: app._br_nav())
    app._br_listbox.bind('<Button-3>',        lambda e: app._br_ctx(e))
    app._br_listbox.bind('<<ListboxSelect>>', lambda e: app._br_update_sel())

    # Foot — selection info
    foot = tk.Frame(pane, bg=COLORS['bg_3'])
    foot.pack(fill='x', side='bottom')
    tk.Label(foot, textvariable=app._br_sel_info,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(side='left', padx=10, pady=4)

    return pane


def _build_xfers_panel(parent, app):
    """Transfers panel at the bottom of the workspace.

    Visual scaffold this iteration. Wiring an actual transfer queue
    would require modifying the FTP worker logic, which is out of scope
    for a UI refactor turn. The active single upload (driven by Quick
    Upload) IS reflected here via traces on _ftptab_status_var.
    """
    panel = tk.Frame(parent, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)

    # Head
    head = tk.Frame(panel, bg=COLORS['bg_2'])
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=COLORS['bg_2'])
    head_inner.pack(fill='x', padx=14, pady=10)

    tk.Label(head_inner, text=_('Transfers'),
             font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(side='left')

    app._xfers_stats_var = tk.StringVar(
        value=_('No transfers yet'))
    tk.Label(head_inner, textvariable=app._xfers_stats_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(12, 0))

    # Right action buttons (decorative this iteration)
    _ghost_btn(head_inner, _('Pause all'),
               command=lambda: None  # no callback wiring this iteration
               ).pack(side='right', padx=(4, 0))
    _ghost_btn(head_inner, _('Clear completed'),
               command=lambda: _xfers_clear(app)
               ).pack(side='right')

    tk.Frame(panel, bg=COLORS['border_2'], height=1).pack(fill='x')

    # List area — capped height
    app._xfers_list_frame = tk.Frame(panel, bg=COLORS['bg_2'])
    app._xfers_list_frame.pack(fill='x', padx=14, pady=(8, 12))

    # Empty state
    app._xfers_empty_lbl = tk.Label(app._xfers_list_frame,
        text=_('Use the \u2192 arrow above to start an upload, or use the '
               'Quick Upload sub-tab.'),
        font=FONTS['mono_sm'],
        bg=COLORS['bg_2'], fg=COLORS['fg_5'],
        pady=12)
    app._xfers_empty_lbl.pack()

    # Mirror the active Quick-Upload transfer into the panel via
    # _ftptab_status_var trace
    def _sync_xfer(*_a):
        try:
            txt = app._ftptab_status_var.get().strip()
            if not txt:
                return
            _xfers_show_active(app, txt)
        except Exception:
            pass
    app._ftptab_status_var.trace_add('write', _sync_xfer)

    return panel


def _xfers_show_active(app, status_text):
    """Render the active Quick-Upload transfer as a row in the xfers panel."""
    # Clear empty state
    if app._xfers_empty_lbl is not None:
        try:
            app._xfers_empty_lbl.destroy()
        except Exception:
            pass
        app._xfers_empty_lbl = None

    # Reuse the same row if it already exists; otherwise create
    if hasattr(app, '_xfer_active_row') and app._xfer_active_row is not None:
        try:
            app._xfer_active_status_var.set(status_text)
            return
        except Exception:
            pass

    try:
        app._xfers_stats_var.set('1 ' + _('active transfer'))
    except Exception:
        pass

    row = tk.Frame(app._xfers_list_frame, bg=COLORS['bg_3'])
    row.pack(fill='x', pady=2)
    inner = tk.Frame(row, bg=COLORS['bg_3'])
    inner.pack(fill='x', padx=10, pady=8)

    tk.Label(inner, text='\u2191',
             font=(FONTS['body'][0], 14, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 10))

    name_col = tk.Frame(inner, bg=COLORS['bg_3'])
    name_col.pack(side='left', fill='x', expand=True)
    local = app._ftptab_local_var.get() if hasattr(app, '_ftptab_local_var') else ''
    remote = app._ftptab_remote_var.get() if hasattr(app, '_ftptab_remote_var') else ''
    tk.Label(name_col, text=os.path.basename(local) or _('(unknown file)'),
             font=(FONTS['body'][0], 10, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['fg_0'],
             anchor='w'
             ).pack(fill='x')
    tk.Label(name_col, text=os.path.dirname(local) + '  \u2192  ' + remote,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x', pady=(2, 0))

    app._xfer_active_status_var = tk.StringVar(value=status_text)
    tk.Label(inner, textvariable=app._xfer_active_status_var,
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['bg_3'], fg=COLORS['accent_hi']
             ).pack(side='right', padx=(8, 0))

    # Cancel button — drives main file's _cancel_ftp_upload, which sets
    # _ftp_cancel = True; the upload worker checks it on each chunk.
    tk.Button(inner, text=_('Cancel'),
              font=(FONTS['button'][0], 9, 'bold'),
              bg=COLORS['danger'], fg=COLORS['fg_0'],
              activebackground=COLORS['danger_hi'],
              activeforeground=COLORS['fg_0'],
              relief='flat', bd=0,
              padx=12, pady=4,
              cursor='hand2',
              command=app._cancel_ftp_upload
              ).pack(side='right', padx=(8, 0))

    app._xfer_active_row = row


def _xfers_clear(app):
    """Remove all rows from the transfers panel."""
    if app._xfers_list_frame is None:
        return
    for w in app._xfers_list_frame.winfo_children():
        w.destroy()
    app._xfer_active_row = None
    try:
        app._xfers_stats_var.set(_('No transfers yet'))
    except Exception:
        pass
    app._xfers_empty_lbl = tk.Label(app._xfers_list_frame,
        text=_('No transfers yet.'),
        font=FONTS['mono_sm'],
        bg=COLORS['bg_2'], fg=COLORS['fg_5'],
        pady=12)
    app._xfers_empty_lbl.pack()


# ─────────────────────────────────────────────────────────────────────────
# LOCAL PANE — file-list operations (driven by os module)
# ─────────────────────────────────────────────────────────────────────────
def _lp_load(app):
    """Refresh the local pane file list from app._lp_path."""
    path = app._lp_path.get()
    if not os.path.isdir(path):
        try:
            app._lp_listbox.delete(0, 'end')
            app._lp_listbox.insert('end', '  ' + _('(invalid directory)'))
        except Exception:
            pass
        return

    entries = []
    try:
        for name in sorted(os.listdir(path), key=str.lower):
            full = os.path.join(path, name)
            try:
                is_dir = os.path.isdir(full)
                sz = 0 if is_dir else os.path.getsize(full)
                entries.append((name, is_dir, sz))
            except (OSError, PermissionError):
                continue
    except (OSError, PermissionError):
        pass

    # Save & redraw
    app._lp_entries = entries
    try:
        app._lp_listbox.delete(0, 'end')
    except Exception:
        return

    # Show ".." up-one-level except at the drive root
    parent_dir = os.path.dirname(path.rstrip(os.sep))
    if parent_dir and parent_dir != path:
        app._lp_listbox.insert('end', '\U0001f4c1  ..')

    for name, is_dir, sz in entries:
        icon = '\U0001f4c1' if is_dir else _file_icon(name)
        size_str = '' if is_dir else _humanize(sz)
        # Pad name to consistent column for the size on the right
        line = '%s  %s' % (icon, name)
        if size_str:
            # Fixed-width tail: pad with spaces
            line = line.ljust(50) + size_str.rjust(10)
        app._lp_listbox.insert('end', line)

    # Color directories
    offset = 1 if (parent_dir and parent_dir != path) else 0
    for i, (_n, is_dir, _s) in enumerate(entries):
        if is_dir:
            app._lp_listbox.itemconfig(offset + i, fg=COLORS['accent_hi'])

    _lp_update_status(app)

    # Save current path
    try:
        app._settings['ftp_local_path'] = path
        save_settings(app._settings)
    except Exception:
        pass


def _lp_navigate(app, new_path):
    """Navigate the local pane to a new path."""
    new_path = os.path.normpath(new_path)
    if os.path.isdir(new_path):
        app._lp_path.set(new_path)
        _lp_load(app)


def _lp_open_selected(app):
    """Double-click handler — enter directory if a dir, else no-op."""
    sel = app._lp_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    parent_dir = os.path.dirname(app._lp_path.get().rstrip(os.sep))
    has_up = (parent_dir and parent_dir != app._lp_path.get())
    if has_up and idx == 0:
        _lp_navigate(app, parent_dir)
        return
    real_idx = idx - (1 if has_up else 0)
    if real_idx < 0 or real_idx >= len(app._lp_entries):
        return
    name, is_dir, _sz = app._lp_entries[real_idx]
    if is_dir:
        _lp_navigate(app, os.path.join(app._lp_path.get(), name))


def _lp_selected_paths(app):
    """Return list of full paths for currently selected items.
    Excludes the synthetic '..' row."""
    sel = app._lp_listbox.curselection()
    if not sel:
        return []
    parent_dir = os.path.dirname(app._lp_path.get().rstrip(os.sep))
    has_up = (parent_dir and parent_dir != app._lp_path.get())
    paths = []
    for idx in sel:
        if has_up and idx == 0:
            continue
        real_idx = idx - (1 if has_up else 0)
        if 0 <= real_idx < len(app._lp_entries):
            name, is_dir, sz = app._lp_entries[real_idx]
            paths.append(
                (os.path.join(app._lp_path.get(), name), is_dir, sz))
    return paths


def _lp_update_status(app):
    """Update the foot status text — count + selection size."""
    n_total = len(app._lp_entries)
    sel_paths = _lp_selected_paths(app)
    n_sel = len(sel_paths)
    sel_size = sum(sz for _p, is_dir, sz in sel_paths if not is_dir)
    if n_sel:
        app._lp_status.set('%d %s \u00b7 %d %s \u00b7 %s' % (
            n_total, _('items'),
            n_sel, _('selected'),
            _humanize(sel_size) + ' ' + _('selected')))
    else:
        app._lp_status.set('%d %s' % (n_total, _('items')))


def _lp_new_folder(app):
    """Create a new folder in the current local path."""
    from tkinter import simpledialog
    name = simpledialog.askstring(_('New folder'),
                                   _('Folder name:'),
                                   parent=app)
    if not name:
        return
    try:
        os.makedirs(os.path.join(app._lp_path.get(), name), exist_ok=False)
        _lp_load(app)
    except Exception as e:
        messagebox.showerror(_('Could not create folder'), str(e))


def _lp_delete(app):
    """Delete selected local files/folders (with confirmation)."""
    sel = _lp_selected_paths(app)
    if not sel:
        return
    if not messagebox.askyesno(_('Delete'),
        _('Delete %d item(s)? This cannot be undone.') % len(sel)):
        return
    import shutil
    errors = []
    for path, is_dir, _sz in sel:
        try:
            if is_dir:
                shutil.rmtree(path)
            else:
                os.remove(path)
        except Exception as e:
            errors.append('%s: %s' % (os.path.basename(path), str(e)))
    if errors:
        messagebox.showerror(_('Delete errors'), '\n'.join(errors))
    _lp_load(app)


def _lp_rename(app):
    """Rename the first selected item."""
    from tkinter import simpledialog
    sel = _lp_selected_paths(app)
    if not sel:
        return
    path, is_dir, _sz = sel[0]
    old_name = os.path.basename(path)
    new_name = simpledialog.askstring(_('Rename'),
                                       _('New name:'),
                                       initialvalue=old_name,
                                       parent=app)
    if not new_name or new_name == old_name:
        return
    try:
        os.rename(path, os.path.join(os.path.dirname(path), new_name))
        _lp_load(app)
    except Exception as e:
        messagebox.showerror(_('Rename failed'), str(e))


# ─────────────────────────────────────────────────────────────────────────
# TRANSFER ARROWS — between panes
# ─────────────────────────────────────────────────────────────────────────
def _xfer_to_remote(app):
    """Upload local-pane selection to the remote-pane current path.

    Reuses the legacy `_ftptab_*` upload form: pre-fills the local + remote
    fields, then calls `_ftptab_start_upload`. Single-file at a time
    (matches the existing capability).
    """
    sel = _lp_selected_paths(app)
    if not sel:
        messagebox.showinfo(_('No selection'),
                            _('Select one or more files in the Local '
                              'pane first.'))
        return
    # If multiple selected, prompt that only the first will be uploaded
    if len(sel) > 1:
        if not messagebox.askyesno(_('Multiple files'),
            _('Quick Upload supports one file at a time. Upload only "%s" '
              'now?') % os.path.basename(sel[0][0])):
            return

    local_path, is_dir, _sz = sel[0]
    remote_path = app._br_path.get()

    app._ftptab_local_var.set(local_path.replace('/', os.sep))
    app._ftptab_remote_var.set(remote_path)
    app._ftptab_is_folder.set(is_dir)
    app._ftptab_start_upload()


def _xfer_to_local(app):
    """Download remote-pane selection to the local-pane current path.

    Reuses the legacy `_br_download` callback but pre-stages the local
    destination so the user doesn't get a filedialog. We do this by
    monkey-patching `filedialog.askdirectory` for the scope of this call.
    """
    if not app._br_listbox.curselection():
        messagebox.showinfo(_('No selection'),
                            _('Select one or more files in the PS5 pane '
                              'first.'))
        return
    dest = app._lp_path.get()
    if not os.path.isdir(dest):
        messagebox.showerror(_('Invalid local directory'),
                             _('Local pane path is invalid.'))
        return

    # Pre-stage the dest by patching filedialog briefly
    import tkinter.filedialog as _fd
    orig = _fd.askdirectory
    _fd.askdirectory = lambda *a, **kw: dest
    try:
        app._br_download()
    finally:
        # Restore on next event-loop tick (so worker thread can still see it)
        app.after(100, lambda: setattr(_fd, 'askdirectory', orig))

    # Refresh local pane after a delay so downloads land
    app.after(2000, lambda: _lp_load(app))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _file_icon(name):
    """Pick a small emoji icon for a filename."""
    lower = name.lower()
    if lower.endswith('.exfat'):
        return '\U0001f4be'  # 💾
    if lower.endswith('.ffpkg'):
        return '\U0001f4e6'  # 📦
    if lower.endswith(('.txt', '.log', '.csv', '.json')):
        return '\U0001f4cb'  # 📋
    if lower.endswith(('.zip', '.7z', '.rar', '.tar')):
        return '\U0001f5dc'  # 🗜
    if lower.endswith(('.png', '.jpg', '.jpeg', '.bmp', '.gif')):
        return '\U0001f5bc'  # 🖼
    if lower.endswith(('.exe', '.bat', '.ps1')):
        return '\u2699'      # ⚙
    return '\U0001f4c4'      # 📄


def _humanize(sz):
    if sz is None:
        return ''
    if sz >= 1024**3:
        return '%.2f GB' % (sz / 1024**3)
    if sz >= 1024**2:
        return '%.0f MB' % (sz / 1024**2)
    if sz >= 1024:
        return '%d KB' % (sz // 1024)
    return '%d B' % sz


def _dark_entry(parent, var, readonly=False, width=None):
    wrap = tk.Frame(parent, bg=COLORS['bg_0'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    state = 'readonly' if readonly else 'normal'
    kw = dict(textvariable=var,
              font=FONTS['mono_sm'],
              bg=COLORS['bg_0'], fg=COLORS['fg_1'],
              # readonly Entries display via disabledforeground/
              # readonlybackground, not fg/bg. Set both so the path
              # text remains visible.
              disabledforeground=COLORS['fg_1'],
              readonlybackground=COLORS['bg_0'],
              insertbackground=COLORS['accent'],
              selectbackground=COLORS['accent'],
              selectforeground=COLORS['fg_0'],
              relief='flat', bd=4,
              state=state)
    if width is not None:
        kw['width'] = width
    tk.Entry(wrap, **kw).pack(fill='x')
    return wrap


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
    """Square icon button for the pane toolbars."""
    fg = (COLORS['accent'] if accent
          else COLORS['danger_hi'] if danger
          else COLORS['fg_3'])
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
