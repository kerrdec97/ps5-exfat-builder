"""
ui/tab_ftp.py — FTP sub-tab of PS5 (file transfer).

Farewell Edition redesign on the design system:

    PageHeader  File Transfer                [Send to PS5 | PS5 Browser]
    ┌ Connection ─ wifi  192.168.1.42:2121  status · free ── [badge]
    │                                       [Disconnect] [Connect / Refresh]
    └──────────────────────────────────────────────────────────────────┘
    PS5 Browser view:
        stat grid card (connection · path · files · selected · free · queue)
        ┌ Local ───────────┐  → ←  ┌ PS5 ─────────────────────┐
        │ path · tools     │       │ path · quick paths · tools│
        │ list             │       │ list                      │
        └──────────────────┘       └───────────────────────────┘
        Transfers — one row per queue item with progress + state badge
        Connection diagnostics — results grid + Advanced details expander
    Send to PS5 view:
        stat grid · Connection information · Quick upload · Output log

Backwards compat: every `_ftptab_*`, `_br_*`, `_lp_*`, `_ftp_sub*`,
`_xfers_*`, `_tq_*`, `_ftpdiag_*` and `_ftpspeed_*` attribute the main
file reads is preserved with the same name and a compatible widget API.
"""

import os
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import filedialog

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.app_dialog import app_info, app_error, app_danger
from ui.tab_ps5_mgr import (
    ps5_page, page_header, section, StatGrid, VarBadge, NullWidget,
    legacy_bar, LegacyBar, console, fact,
)

C = COLORS


def _conn_word(value):
    """Badge text + tone for the FTP connection pill var."""
    if value == _('CONNECTED'):
        return _('Online'), 'success'
    if value == _('OFFLINE'):
        return _('Offline'), 'warn'
    return _('Not connected'), 'neutral'


# ─────────────────────────────────────────────────────────────────────────
# Public entry
# ─────────────────────────────────────────────────────────────────────────
def build_ftp_tab(parent, app):
    """Build the FTP sub-tab into `parent`."""
    app._ftp_subtab_var = tk.StringVar(value='browser')

    # Browser + local-pane state up front (the connection card reads them).
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

    # Stat cells that mirror the connection state; _conn_set() walks it.
    app._ftp_conn_cells = []

    body = ps5_page(parent)
    hdr = page_header(body, _('File Transfer'),
                      _('Browse and transfer files to and from your PS5.'))
    _build_subtab_bar(hdr.actions, app)
    _build_connection_bar(body, app)

    app._ftp_sub_upload_frame  = tk.Frame(body, bg=C['bg_1'])
    app._ftp_sub_browser_frame = tk.Frame(body, bg=C['bg_1'])
    _build_upload_subtab(app._ftp_sub_upload_frame, app)
    _build_browser_subtab(app._ftp_sub_browser_frame, app)

    app._ftp_switch_sub('browser')

    # Each view starts at the top of the page.
    def _to_top(*_a):
        try:
            body._ps5_canvas.yview_moveto(0)
        except Exception:
            pass
    app._ftp_subtab_var.trace_add('write', _to_top)


# ─────────────────────────────────────────────────────────────────────────
# View switch (Send to PS5 | PS5 Browser)
#
# _ftp_switch_sub (main file) sets _ftp_subtab_var and recolours
# _ftp_sub_upload_btn / _ftp_sub_browser_btn. The Segmented control follows
# the variable; the two button attributes are inert stand-ins.
# ─────────────────────────────────────────────────────────────────────────
def _build_subtab_bar(parent, app):
    from ui.shared.widgets import Segmented
    seg = Segmented(parent, [('upload', _('Send to PS5')),
                             ('browser', _('PS5 Browser'))],
                    variable=app._ftp_subtab_var,
                    command=lambda v: app._ftp_switch_sub(v))
    seg.pack(side='right')
    app._ftp_sub_seg = seg
    app._ftp_sub_upload_btn = NullWidget()
    app._ftp_sub_browser_btn = NullWidget()


# ─────────────────────────────────────────────────────────────────────────
# Connection card — the anchor. Reuses `_br_status` / `_br_free`.
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_bar(parent, app):
    from ui.shared.widgets import Card, Button, IconLabel
    sec = section(parent)
    card = Card(sec, padding=(px(16), px(12)))
    card.pack(fill='x')
    bg = C['bg_2']

    tile = tk.Frame(card, bg=C['bg_3'], width=px(36), height=px(36))
    tile.pack(side='left')
    tile.pack_propagate(False)
    IconLabel(tile, 'wifi', 16, C['fg_2'], C['bg_3']).place(
        relx=0.5, rely=0.5, anchor='center')

    col = tk.Frame(card, bg=bg)
    col.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
    top = tk.Frame(col, bg=bg)
    top.pack(fill='x')
    ip = app._ftp_ip_var.get().strip() or _('No PS5 IP set')
    port = app._ftp_port_var.get().strip() or '2121'
    tk.Label(top, text='%s:%s' % (ip, port), font=FONTS['body_strong'],
             bg=bg, fg=C['fg_0']).pack(side='left')

    app._ftp_conn_pill_var = tk.StringVar(value=_('NOT CONNECTED'))
    app._ftp_conn_lbl = VarBadge(top, textvariable=app._ftp_conn_pill_var,
                                 mapper=_conn_word)
    app._ftp_conn_lbl.pack(side='left', padx=(px(10), 0))
    app._ftp_conn_dot = NullWidget()
    app._ftp_conn_pill = NullWidget()
    app._ftp_conn_pill_inner = NullWidget()

    meta = tk.Frame(col, bg=bg)
    meta.pack(fill='x', pady=(px(2), 0))
    tk.Label(meta, textvariable=app._br_status, font=FONTS['caption'],
             bg=bg, fg=C['fg_3']).pack(side='left')
    tk.Label(meta, textvariable=app._br_free, font=FONTS['caption'],
             bg=bg, fg=C['fg_3']).pack(side='left', padx=(px(12), 0))

    Button(card, text=_('Connect / Refresh'), icon='refresh',
           command=app._br_connect).pack(side='right')
    Button(card, text=_('Disconnect'), icon='unplug', variant='subtle',
           command=app._br_disconnect).pack(side='right', padx=(0, px(6)))

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
    """Mirror the connection state onto the badge and the stat cells."""
    for strip, key in getattr(app, '_ftp_conn_cells', []):
        try:
            if connected:
                strip.set(key, _('CONNECTED'), ok=True)
            else:
                strip.set(key, _('OFFLINE'), warn=True)
        except Exception:
            pass
    try:
        app._ftp_conn_pill_var.set(_('CONNECTED') if connected
                                   else _('OFFLINE'))
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# SEND TO PS5 view — single file / folder upload
# ─────────────────────────────────────────────────────────────────────────
def _build_upload_subtab(parent, app):
    from ui.shared.widgets import (Card, Button, Divider, field_label,
                                   help_text, Radio)
    app._ftptab_local_var  = tk.StringVar()
    app._ftptab_remote_var = tk.StringVar(
        value=app._settings.get('ftp_path', '/data/etaHEN/games/'))
    app._ftptab_is_folder  = tk.BooleanVar(value=False)
    app._ftptab_uploading  = False
    app._ftptab_pct        = 0
    app._ftptab_cancel_flag = False
    BG2 = C['bg_2']

    # ── Stats ──
    sec = section(parent)
    stats_card = Card(sec, padding=16)
    stats_card.pack(fill='x')
    strip = StatGrid(stats_card, [(_('Connection'), 'conn'),
                                  (_('Target Path'), 'target'),
                                  (_('Upload'), 'xfer'),
                                  (_('Browser Path'), 'brpath'),
                                  (_('Files Listed'), 'files')])
    strip.pack(fill='x')
    app._ftp_dash_strip = strip
    app._ftp_conn_cells.append((strip, 'conn'))
    strip.set('conn', _('OFFLINE'), warn=True)
    strip.set('xfer', _('IDLE'))

    def _trim(p, n=34):
        p = p or ''
        return ('…' + p[-(n - 1):]) if len(p) > n else (p or '—')

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
            elif t.startswith('✓') or 'complete' in t.lower():
                strip.set('xfer', _('DONE'), ok=True)
            elif t.startswith('✗') or 'fail' in t.lower() \
                    or 'cancel' in t.lower():
                strip.set('xfer', t.split('—')[0].strip()[:18],
                          warn=True)
            else:
                strip.set('xfer', _('ACTIVE') + ' · ' + t[:14])
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

    # ── Two columns: upload form | connection information ──
    row = section(parent)
    row.columnconfigure(0, weight=7, uniform='ftpup')
    row.columnconfigure(1, weight=5, uniform='ftpup')
    left = tk.Frame(row, bg=C['bg_1'])
    left.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))
    right = tk.Frame(row, bg=C['bg_1'])
    right.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))

    form_card = Card(left, title=_('Quick upload'),
                     subtitle=_('Upload a single file or folder directly. '
                                'For multi-file work, use the PS5 Browser '
                                'sub-tab’s dual pane.'))
    form_card.pack(fill='both', expand=True)
    form = form_card.body

    type_row = tk.Frame(form, bg=BG2)
    type_row.pack(fill='x', pady=(0, px(12)))
    tk.Label(type_row, text=_('Upload type:'), font=FONTS['body'],
             bg=BG2, fg=C['fg_2']).pack(side='left', padx=(0, px(12)))
    for val, lbl in [(False, _('File')), (True, _('Folder'))]:
        Radio(type_row, text=lbl, variable=app._ftptab_is_folder, value=val,
              command=app._ftptab_update_browse).pack(side='left',
                                                      padx=(0, px(16)))

    # Local path
    app._ftptab_local_lbl = tk.Label(form, text=_('Local file'),
                                     font=FONTS['body'], bg=BG2,
                                     fg=C['fg_2'], anchor='w')
    app._ftptab_local_lbl.pack(fill='x')
    local_inner = tk.Frame(form, bg=BG2)
    local_inner.pack(fill='x', pady=(px(4), px(12)))
    XEntry(local_inner, textvariable=app._ftptab_local_var, mono=True,
           state='readonly').pack(side='left', fill='x', expand=True)
    app._ftptab_browse_btn = XButton(local_inner, text=_('Browse') + '…',
                                     icon='folder_open',
                                     command=app._ftptab_browse)
    app._ftptab_browse_btn.pack(side='left', padx=(px(6), 0))

    # Remote path
    field_label(form, _('PS5 remote path')).pack(fill='x')
    remote_inner = tk.Frame(form, bg=BG2)
    remote_inner.pack(fill='x', pady=(px(4), px(12)))
    XEntry(remote_inner, textvariable=app._ftptab_remote_var, mono=True
           ).pack(side='left', fill='x', expand=True)
    XButton(remote_inner, text=_('Default'),
            command=lambda: app._ftptab_remote_var.set('/data/etaHEN/games/')
            ).pack(side='left', padx=(px(6), 0))

    Divider(form, color=C['border_1']).pack(fill='x', pady=(px(4), px(12)))

    # Status + progress
    app._ftptab_status_var = tk.StringVar(value='')
    st_row = tk.Frame(form, bg=BG2)
    st_row.pack(fill='x')
    tk.Label(st_row, textvariable=app._ftptab_status_var,
             font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
             anchor='w').pack(side='left', fill='x', expand=True)
    app._ftptab_eta_var = tk.StringVar(value='')
    tk.Label(st_row, textvariable=app._ftptab_eta_var, font=FONTS['caption'],
             bg=BG2, fg=C['fg_3'], anchor='e').pack(side='right')
    app._ftptab_status_var.trace_add('write', lambda *a: _sync_xfer_cell())

    app._ftptab_canvas = legacy_bar(form)
    app._ftptab_canvas.pack(fill='x', pady=(px(8), px(16)))
    app._ftptab_bar = LegacyBar.ITEM
    app._ftptab_canvas.bind('<Configure>',
        lambda e: app._ftptab_set_bar(app._ftptab_pct), add='+')

    btn_row = tk.Frame(form, bg=BG2)
    btn_row.pack(fill='x')
    app._ftptab_upload_btn = XButton(btn_row, text=_('Upload to PS5'),
                                     icon='upload', variant='primary',
                                     command=app._ftptab_start_upload)
    app._ftptab_upload_btn.pack(side='left')
    # Not packed by default — legacy code packs/unpacks during upload.
    app._ftptab_cancel_btn = XButton(btn_row, text=_('Cancel'), icon='close',
                                     variant='danger',
                                     command=app._ftptab_cancel)

    # ── Connection information ──
    info_card = Card(right, title=_('Connection information'),
                     subtitle=_('Set the PS5 IP in Settings or the '
                                'connection bar above. Default FTP ports: '
                                '2121 (ftpsrv) / 1337 (etaHEN).'))
    info_card.pack(fill='both', expand=True)

    def _info_cell(label, getter, r):
        v = tk.StringVar(value=getter())
        box = fact(info_card.body, label, textvariable=v, mono=True)
        box.pack(fill='x', pady=(0 if r == 0 else px(12), 0))
        return v

    ip_cell = _info_cell(_('PS5 IP'),
                         lambda: app._ftp_ip_var.get().strip() or '—', 0)
    port_cell = _info_cell(_('FTP Port'),
                           lambda: app._ftp_port_var.get().strip() or '2121', 1)
    _info_cell(_('Default Path'), lambda: '/data/etaHEN/games/', 2)
    app._ftp_ip_var.trace_add('write', lambda *a: ip_cell.set(
        app._ftp_ip_var.get().strip() or '—'))
    app._ftp_port_var.trace_add('write', lambda *a: port_cell.set(
        app._ftp_port_var.get().strip() or '2121'))

    # ── Output log ──
    sec = section(parent, pady=(0, 24))
    log_card = Card(sec, title=_('Output log'))
    log_card.pack(fill='x')
    _well, app._ftptab_log = console(log_card.body, height=8)
    _well.pack(fill='x')


# ─────────────────────────────────────────────────────────────────────────
# PS5 BROWSER view — dual pane (Local | PS5) + transfers + diagnostics
# ─────────────────────────────────────────────────────────────────────────
def _build_browser_subtab(parent, app):
    """Dual-pane file browser. State (`_br_*`, `_lp_*`) is initialised in
    build_ftp_tab before this runs."""
    sec = section(parent)
    _build_browser_strip(sec, app).pack(fill='x')

    panes_row = section(parent)
    panes_row.grid_columnconfigure(0, weight=1, uniform='pane')
    panes_row.grid_columnconfigure(1, weight=0)
    panes_row.grid_columnconfigure(2, weight=1, uniform='pane')
    panes_row.grid_rowconfigure(0, weight=1)
    _build_local_pane(panes_row, app).grid(row=0, column=0, sticky='nsew')
    _build_divider(panes_row, app).grid(row=0, column=1, sticky='ns',
                                        padx=px(12))
    _build_remote_pane(panes_row, app).grid(row=0, column=2, sticky='nsew')

    sec = section(parent)
    _build_xfers_panel(sec, app).pack(fill='x')
    sec = section(parent, pady=(0, 24))
    _build_diagnostics_panel(sec, app).pack(fill='x')


def _build_browser_strip(parent, app):
    """Stat grid above the dual panes."""
    from ui.shared.widgets import Card
    card = Card(parent, padding=16)
    strip = StatGrid(card, [(_('Connection'), 'conn'),
                            (_('Current Path'), 'path'), (_('Files'), 'files'),
                            (_('Selected'), 'sel'), (_('Free'), 'free'),
                            (_('Upload Queue'), 'queue')])
    strip.pack(fill='x')
    app._ftp_browser_strip = strip
    app._ftp_conn_cells.append((strip, 'conn'))
    strip.set('conn', _('OFFLINE'), warn=True)
    strip.set('queue', '0')

    def _trim(p, n=30):
        p = p or ''
        return '…' + p[-(n - 1):] if len(p) > n else p or '—'

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
            strip.set('sel', txt[:24] if txt else '—')
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
            try:
                fr = (app._br_free.get() or '').strip()
                if fr.lower().startswith('free:'):
                    fr = fr.split(':', 1)[1].strip()
                strip.set('free', fr or '—')
            except Exception:
                pass
            q = getattr(app, '_tq_queue', None)
            if q is not None:
                c = q.counts()
                active = c.get('uploading', 0)
                queued = c.get('queued', 0)
                if active or queued:
                    strip.set('queue', '%d ' % (active + queued) + _('ACTIVE'), ok=True)
                else:
                    strip.set('queue', '0', ok=False)
            else:
                strip.set('queue', '0', ok=False)
        except Exception:
            pass
        try:
            card.after(1000, _poll)
        except Exception:
            pass
    card.after(1000, _poll)
    return card


def _pane(parent, title, path_var, on_home, on_enter):
    """A file pane card: head (title · path field · home), returns
    (card, head_frame, body_frame)."""
    from ui.shared.widgets import Card, IconButton, Divider
    card = Card(parent, padding=0)
    head = tk.Frame(card, bg=C['bg_2'])
    head.pack(fill='x', padx=px(12), pady=(px(12), px(8)))
    tk.Label(head, text=title, font=FONTS['subtitle'], bg=C['bg_2'],
             fg=C['fg_0']).pack(side='left', padx=(px(4), px(12)))
    e = XEntry(head, textvariable=path_var, mono=True)
    e.pack(side='left', fill='x', expand=True)
    e.bind('<Return>', lambda ev: on_enter())
    IconButton(head, 'home', on_home, tooltip='Home').pack(
        side='left', padx=(px(4), 0))
    return card, head


def _listbox(parent):
    """Listbox styled for a card body (bg_2, mono rows, accent selection)."""
    wrap = tk.Frame(parent, bg=C['bg_2'])
    lb = tk.Listbox(wrap, font=FONTS['code'], bg=C['bg_2'], fg=C['fg_1'],
                    selectbackground=C['accent_lo'],
                    selectforeground=C['fg_0'], activestyle='none',
                    relief='flat', bd=0, selectmode='extended',
                    highlightthickness=0, height=14)
    sb = XScrollbar(wrap, command=lb.yview)
    lb.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    lb.pack(side='left', fill='both', expand=True, padx=(px(12), 0))
    return wrap, lb


def _pane_foot(card, var):
    from ui.shared.widgets import Divider
    foot = tk.Frame(card, bg=C['bg_2'])
    foot.pack(fill='x', side='bottom')
    tk.Label(foot, textvariable=var, font=FONTS['caption'], bg=C['bg_2'],
             fg=C['fg_3'], anchor='w').pack(side='left', padx=px(16),
                                            pady=px(8))
    Divider(card, color=C['border_1']).pack(fill='x', side='bottom')


def _build_local_pane(parent, app):
    """Local-side (left) pane card."""
    from ui.shared.widgets import IconButton
    card, head = _pane(parent, _('Local'), app._lp_path,
                       lambda: _lp_navigate(app, os.path.expanduser('~')),
                       lambda: _lp_navigate(app, app._lp_path.get()))
    tb = tk.Frame(card, bg=C['bg_2'])
    tb.pack(fill='x', padx=px(12), pady=(0, px(8)))
    for icon, tip, cmd in (
            ('arrow_up', 'Up one dir',
             lambda: _lp_navigate(app, os.path.dirname(app._lp_path.get()))),
            ('refresh', 'Refresh', lambda: _lp_load(app)),
            ('folder_plus', 'New folder', lambda: _lp_new_folder(app)),
            ('pencil', 'Rename', lambda: _lp_rename(app)),
            ('trash', 'Delete', lambda: _lp_delete(app))):
        IconButton(tb, icon, cmd, tooltip=tip, size='sm').pack(
            side='left', padx=(0, px(2)))
    _pane_foot(card, app._lp_status)
    wrap, app._lp_listbox = _listbox(card)
    wrap.pack(fill='both', expand=True, pady=(0, px(4)))
    app._lp_listbox.bind('<Double-Button-1>', lambda e: _lp_open_selected(app))
    app._lp_listbox.bind('<<ListboxSelect>>', lambda e: _lp_update_status(app))
    parent.after(50, lambda: _lp_load(app))
    return card


def _build_divider(parent, app):
    """Transfer arrows between the panes. → shows a live count of the
    selected local items (Ctrl/Shift-select queues several at once)."""
    from ui.shared.widgets import Tooltip
    div = tk.Frame(parent, bg=C['bg_1'])
    tk.Frame(div, bg=C['bg_1']).pack(fill='both', expand=True)
    arrows = tk.Frame(div, bg=C['bg_1'])
    arrows.pack()
    app._xfer_to_remote_btn = XButton(arrows, text='', icon='arrow_right',
                                      variant='secondary',
                                      command=lambda: _xfer_to_remote(app))
    app._xfer_to_remote_btn.pack(pady=px(4), fill='x')
    Tooltip(app._xfer_to_remote_btn,
            'Queue selected local file(s) for upload. Tip: Ctrl/Shift-click '
            'to select several — each is queued as its own transfer.')
    back = XButton(arrows, text='', icon='arrow_left',
                   command=lambda: _xfer_to_local(app))
    back.pack(pady=px(4), fill='x')
    Tooltip(back, 'Download the selected PS5 item(s) into the local folder.')
    app._xfer_hint_var = tk.StringVar(value=_('Ctrl/Shift-select'))
    tk.Label(arrows, textvariable=app._xfer_hint_var, font=FONTS['caption'],
             bg=C['bg_1'], fg=C['fg_4'], wraplength=px(120),
             justify='center').pack(pady=(px(4), 0))
    tk.Frame(div, bg=C['bg_1']).pack(fill='both', expand=True)
    return div


def _xfer_update_count(app):
    """Refresh the → button label + caption with the local selection
    count. Called from _lp_update_status on every selection change."""
    try:
        n = len(_lp_selected_paths(app))
    except Exception:
        n = 0
    try:
        btn = getattr(app, '_xfer_to_remote_btn', None)
        if btn is not None:
            btn.configure(text=str(n) if n else '')
    except Exception:
        pass
    try:
        hint = getattr(app, '_xfer_hint_var', None)
        if hint is not None:
            if n > 1:
                hint.set(_('queue %d') % n)
                return
            if n == 1:
                hint.set(_('queue 1'))
                return
            hint.set(_('Ctrl/Shift-select'))
    except Exception:
        return


def _build_remote_pane(parent, app):
    """PS5-side (right) pane card. The Listbox is `_br_listbox`, so every
    existing _br_* callback keeps working."""
    from ui.shared.widgets import IconButton
    card, head = _pane(parent, _('PS5'), app._br_path,
                       lambda: app._br_load('/data/etaHEN/games/'),
                       lambda: app._br_load(app._br_path.get()))
    tb = tk.Frame(card, bg=C['bg_2'])
    tb.pack(fill='x', padx=px(12), pady=(0, px(8)))
    for icon, tip, cmd in (
            ('arrow_up', 'Up one dir', app._br_go_up),
            ('refresh', 'Refresh', lambda: app._br_load(app._br_path.get())),
            ('folder_plus', 'New folder', app._br_new_folder),
            ('pencil', 'Rename', app._br_rename),
            ('trash', 'Delete', app._br_delete)):
        IconButton(tb, icon, cmd, tooltip=tip, size='sm').pack(
            side='left', padx=(0, px(2)))
    # Quick paths (Root / Data / User / USB0 / Games) — a jump list.
    from ui.shared.widgets import Dropdown
    quick = [(_('Root'), '/'), (_('Data'), '/data'), (_('User'), '/user'),
             (_('USB0'), '/mnt/usb0'), (_('Games'), '/data/etaHEN/games/')]
    jump_var = tk.StringVar(value=_('Games'))
    jump = Dropdown(tb, textvariable=jump_var, width=10,
                    values=[q[0] for q in quick])
    jump.pack(side='right')

    def _jump(_e=None):
        for lbl, path in quick:
            if lbl == jump_var.get():
                app._br_load(path)
                break
    jump.bind('<<ComboboxSelected>>', _jump)
    tk.Label(tb, text=_('Path:'), font=FONTS['caption'], bg=C['bg_2'],
             fg=C['fg_3']).pack(side='right', padx=(0, px(6)))
    _pane_foot(card, app._br_sel_info)
    wrap, app._br_listbox = _listbox(card)
    wrap.pack(fill='both', expand=True, pady=(0, px(4)))
    app._br_listbox.bind('<Double-Button-1>', lambda e: app._br_nav(e))
    app._br_listbox.bind('<Button-3>', lambda e: app._br_ctx(e))
    app._br_listbox.bind('<<ListboxSelect>>', lambda e: app._br_update_sel())
    return card


# ─────────────────────────────────────────────────────────────────────────
# Transfers — model-driven upload queue (ui.tq_model via the backend
# dispatcher). One row per item: name, route, progress, state badge and a
# per-item Cancel. Pause all stays disabled (mid-stream suspend is a later
# phase) with an explanatory tooltip.
# ─────────────────────────────────────────────────────────────────────────
def _build_xfers_panel(parent, app):
    from ui.shared.widgets import Card, Button, Tooltip
    card = Card(parent, title=_('Transfers'), padding=0)
    try:
        card.title_label.master.master.pack_configure(
            padx=px(16), pady=(px(16), px(12)))
    except Exception:
        pass
    app._xfers_stats_var = tk.StringVar(value=_('No transfers yet'))
    tk.Label(card.title_label.master, textvariable=app._xfers_stats_var,
             font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3'],
             anchor='w').pack(fill='x', pady=(px(2), 0))
    pause_btn = Button(card.head_actions, text=_('Pause all'), icon='pause',
                       size='sm', state='disabled', command=lambda: None)
    pause_btn.pack(side='right', padx=(px(6), 0))
    Tooltip(pause_btn, 'Pausing an in-flight transfer is a later phase (it '
                       'needs mid-stream suspend/resume). Use per-item '
                       'Cancel for now.')
    Button(card.head_actions, text=_('Clear completed'), size='sm',
           command=lambda: app._tq_clear_completed()).pack(side='right')
    from ui.shared.widgets import Divider
    Divider(card.body, color=C['border_2']).pack(fill='x')
    app._xfers_list_frame = tk.Frame(card.body, bg=C['bg_2'])
    app._xfers_list_frame.pack(fill='x')
    app._xfers_empty_lbl = _xfers_empty(
        app._xfers_list_frame,
        _('Select one or more local files (Ctrl/Shift-click), then press → '
          'to queue them. Each uploads in turn.'))
    app._tq_rows = {}
    return card


def _xfers_empty(parent, text):
    """Centred empty state for the transfer list (icon + text)."""
    from ui.shared.widgets import IconLabel
    f = tk.Frame(parent, bg=C['bg_2'])
    f.pack(fill='x', pady=px(20))
    IconLabel(f, 'queue', 20, C['fg_4'], C['bg_2']).pack()
    tk.Label(f, text=text, font=FONTS['body'], bg=C['bg_2'], fg=C['fg_3'],
             justify='center', wraplength=px(520)).pack(pady=(px(8), 0))
    return f


# Badge text + tone per queue state (ui.tq_model states).
_TQ_STATE_STYLE = {'queued': ('Queued', 'neutral'),
                   'uploading': ('Uploading', 'info'),
                   'done': ('Done', 'success'),
                   'failed': ('Failed', 'danger'),
                   'cancelled': ('Cancelled', 'outline')}


def _tq_fmt_size(nbytes):
    try:
        n = float(nbytes)
    except Exception:
        return ''
    for unit in 'B', 'KB', 'MB', 'GB', 'TB':
        if n < 1024 or unit == 'TB':
            return '%.0f %s' % (n, unit) if unit == 'B' else '%.1f %s' % (n, unit)
        n /= 1024.0
    return ''


def _tq_render_panel(app, snapshot, counts):
    """Render the queue snapshot into the transfers panel. Called from the
    backend via self.after (so it runs on the Tk thread). Updates rows in
    place; creates/removes rows as the queue changes."""
    lst = getattr(app, '_xfers_list_frame', None)
    if lst is None:
        return
    rows = getattr(app, '_tq_rows', None)
    if rows is None:
        rows = app._tq_rows = {}
    live_q = getattr(app, '_tq_queue', None)
    live_empty = True
    if live_q is not None:
        try:
            live_empty = live_q.is_empty
        except Exception:
            live_empty = not snapshot
    if not snapshot and live_empty:
        for rid, r in list(rows.items()):
            try: r['row'].destroy()
            except Exception:
                pass
            rows.pop(rid, None)
        if getattr(app, '_xfers_empty_lbl', None) is None:
            app._xfers_empty_lbl = _xfers_empty(lst, _('No transfers yet.'))
        try:
            app._xfers_stats_var.set(_('No transfers yet'))
        except Exception:
            pass
        return
    if not snapshot and not live_empty:
        try:
            snapshot = live_q.snapshot()
            counts = live_q.counts()
        except Exception:
            snapshot = snapshot or []
    if getattr(app, '_xfers_empty_lbl', None) is not None:
        try: app._xfers_empty_lbl.destroy()
        except Exception:
            pass
        app._xfers_empty_lbl = None
    try:
        active = counts.get('uploading', 0)
        queued = counts.get('queued', 0)
        done = counts.get('done', 0)
        failed = counts.get('failed', 0)
        cancelled = counts.get('cancelled', 0)
        parts = []
        if active:
            parts.append('%d active' % active)
        if queued:
            parts.append('%d queued' % queued)
        if done:
            parts.append('%d done' % done)
        if failed:
            parts.append('%d failed' % failed)
        if cancelled:
            parts.append('%d cancelled' % cancelled)
        app._xfers_stats_var.set(' · '.join(parts) if parts else _('No transfers yet'))
    except Exception:
        pass
    seen = set()
    for snap in snapshot:
        rid = snap['id']
        seen.add(rid)
        if rid in rows:
            _tq_update_row(app, rows[rid], snap)
        else:
            rows[rid] = _tq_build_row(app, lst, snap)
    for rid in list(rows.keys()):
        if rid not in seen:
            try: rows[rid]['row'].destroy()
            except Exception:
                pass
            rows.pop(rid, None)


def _tq_build_row(app, parent, snap):
    """Create a row widget set for one queue item; returns its registry."""
    from ui.shared.widgets import (Badge, Button, IconLabel, RoundedBar,
                                   Divider)
    BG2 = C['bg_2']
    row = tk.Frame(parent, bg=BG2)
    row.pack(fill='x')
    if len(parent.winfo_children()) > 1:
        Divider(row, color=C['border_1']).pack(fill='x')
    inner = tk.Frame(row, bg=BG2)
    inner.pack(fill='x', padx=px(16), pady=px(10))
    tile = tk.Frame(inner, bg=C['bg_3'], width=px(32), height=px(32))
    tile.pack(side='left', anchor='n', padx=(0, px(12)))
    tile.pack_propagate(False)
    IconLabel(tile, 'folder' if snap.get('is_folder') else 'upload', 16,
              C['fg_2'], C['bg_3']).place(relx=0.5, rely=0.5, anchor='center')

    right = tk.Frame(inner, bg=BG2)
    right.pack(side='right', anchor='n', padx=(px(16), 0))
    badge = Badge(right, '', 'neutral')
    badge.pack(side='left', pady=(px(6), 0))
    cancel_btn = Button(right, text=_('Cancel'), size='sm', variant='subtle',
                        command=(lambda _id=snap['id']: app._tq_cancel_item(_id)))
    cancel_btn.pack(side='left', padx=(px(8), 0))

    col = tk.Frame(inner, bg=BG2)
    col.pack(side='left', fill='x', expand=True)
    top = tk.Frame(col, bg=BG2)
    top.pack(fill='x')
    tk.Label(top, text=snap['name'], font=FONTS['body_strong'], bg=BG2,
             fg=C['fg_0'], anchor='w').pack(side='left')
    if snap.get('is_folder'):
        Badge(top, _('Folder'), 'outline').pack(side='left', padx=(px(8), 0))
    tk.Label(top, text=_tq_fmt_size(snap.get('size', 0)), font=FONTS['code'],
             bg=BG2, fg=C['fg_3'], anchor='e').pack(side='right')
    tk.Label(col, text='→  ' + (snap.get('remote_display') or ''),
             font=FONTS['code'], bg=BG2, fg=C['fg_3'], anchor='w'
             ).pack(fill='x', pady=(px(2), px(6)))
    bar = RoundedBar(col)
    bar.pack(fill='x')
    meta_var = tk.StringVar(value='')
    tk.Label(col, textvariable=meta_var, font=FONTS['caption'], bg=BG2,
             fg=C['fg_3'], anchor='w').pack(fill='x', pady=(px(4), 0))
    reg = {'row': row, 'canvas': bar, 'bar': bar, 'meta_var': meta_var,
           'badge': badge, 'badge_var': NullWidget(), 'cancel_btn': cancel_btn,
           'cancel_parent': right}
    _tq_update_row(app, reg, snap)
    return reg


def _tq_update_row(app, reg, snap):
    """Update a row's progress bar, meta line, badge and Cancel button
    from a fresh snapshot."""
    state = snap['state']
    pct = snap.get('percent', 0)
    try:
        bar = reg['bar']
        bar.set_color(C['success'] if state == 'done' else
                      C['danger'] if state == 'failed' else
                      C['fg_5'] if state == 'cancelled' else C['accent'])
        bar.set(max(0.0, min(1.0, (pct or 0) / 100.0)))
    except Exception:
        pass
    try:
        if state == 'uploading':
            spd = snap.get('speed_mbs', 0.0)
            reg['meta_var'].set('%d%%  ·  %.1f MB/s' % (pct, spd))
        elif state == 'done':
            reg['meta_var'].set('100%%  ·  %s' % _tq_fmt_size(snap.get('size', 0)))
        elif state == 'failed':
            reg['meta_var'].set('Failed: ' + (snap.get('error') or 'unknown')[:60])
        elif state == 'cancelled':
            reg['meta_var'].set('Cancelled')
        else:
            reg['meta_var'].set('Waiting…')
    except Exception:
        pass
    try:
        label, tone = _TQ_STATE_STYLE.get(state, (state, 'neutral'))
        reg['badge'].set(label, tone)
    except Exception:
        pass
    try:
        if state in ('queued', 'uploading'):
            reg['cancel_btn'].configure(state='normal')
            if not reg['cancel_btn'].winfo_ismapped():
                reg['cancel_btn'].pack(side='left', padx=(px(8), 0))
        else:
            reg['cancel_btn'].pack_forget()
    except Exception:
        return


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
            app._lp_listbox.itemconfig(offset + i, fg=COLORS['fg_0'])

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
    # keep the → button label / caption in sync with the selection
    try:
        _xfer_update_count(app)
    except Exception:
        pass
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
        app_error(app, _('Could not create folder'), str(e))


def _lp_delete(app):
    """Delete selected local files/folders (with confirmation)."""
    sel = _lp_selected_paths(app)
    if not sel:
        return
    if not app_danger(app, _('Delete'),
        _('Delete %d item(s)? This cannot be undone.') % len(sel),
        ok_label=_('Delete'), cancel_label=_('Cancel')):
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
        app_error(app, _('Delete errors'), '\n'.join(errors))
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
        app_error(app, _('Rename failed'), str(e))


# ─────────────────────────────────────────────────────────────────────────
# TRANSFER ARROWS — between panes
# ─────────────────────────────────────────────────────────────────────────
def _xfer_to_remote(app):
    """Queue the local-pane selection for upload to the remote-pane path.

    Phase 1: enqueues EVERY selected item into the transfer queue (no more
    one-file-at-a-time prompt). Each becomes its own queue item; the serial
    dispatcher uploads them one at a time with per-item progress + cancel.
    """
    sel = _lp_selected_paths(app)
    if not sel:
        app_info(app, _('No selection'), _('Select one or more files in the Local pane first.'))
        return
    remote_path = app._br_path.get()
    specs = [(local_path, remote_path, bool(is_dir)) for local_path, is_dir, _sz in sel]
    app._tq_enqueue_many(specs)


def _xfer_to_local(app):
    """Download remote-pane selection to the local-pane current path.

    Reuses the legacy `_br_download` callback but pre-stages the local
    destination so the user doesn't get a filedialog. We do this by
    monkey-patching `filedialog.askdirectory` for the scope of this call.
    """
    if not app._br_listbox.curselection():
        app_info(app, _('No selection'),
                            _('Select one or more files in the PS5 pane '
                              'first.'))
        return
    dest = app._lp_path.get()
    if not os.path.isdir(dest):
        app_error(app, _('Invalid local directory'),
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
    """Rounded mono field in a wrapper frame (legacy helper shape)."""
    wrap = tk.Frame(parent, bg=parent.cget('bg'))
    kw = dict(textvariable=var, mono=True,
              state='readonly' if readonly else 'normal')
    if width is not None:
        kw['width'] = width
    XEntry(wrap, **kw).pack(fill='x')
    return wrap


def _ghost_btn(parent, text, command):
    return XButton(parent, text=text, command=command)


def _diag_btn(parent, text, command, primary=False):
    return XButton(parent, text=text, command=command, size='sm',
                   variant='primary' if primary else 'secondary')


def _accent_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='primary')


def _path_chip(parent, text, command):
    """Quick-jump chip for the remote pane (Root / Data / User / USB0 /
    Games). Navigation only — never writes."""
    return XButton(parent, text=text, command=command, size='sm',
                   variant='subtle')


_GLYPH_ICON = {'↑': 'arrow_up', '↻': 'refresh',
               '\U0001f4c1+': 'folder_plus', '\U0001f5d1': 'trash',
               '✎': 'pencil', '⌂': 'home'}


def _icon_btn(parent, glyph, tooltip='', command=None,
              accent=False, danger=False):
    from ui.shared.widgets import IconButton
    return IconButton(parent, _GLYPH_ICON.get(glyph, glyph), command,
                      tooltip=tooltip, size='sm')


try:
    from ui.shared.widgets import Badge as _BadgeBase
except Exception:   # pragma: no cover
    _BadgeBase = tk.Label


class _FeatChip(_BadgeBase):
    """FEAT capability chip. _ftpdiag_apply_feat recolours it with
    configure(bg=success_bg|bg_3, fg=…); that maps onto badge tones."""

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        bg = kw.pop('bg', None) or kw.pop('background', None)
        kw.pop('fg', None)
        kw.pop('foreground', None)
        if bg is not None:
            on = str(bg).lower() == C['success_bg'].lower()
            self.set(tone='success' if on else 'outline',
                     icon=None if on else False)
        if kw:
            return super().configure(**kw)

    config = configure


def _build_diagnostics_panel(parent, app):
    """Connection-health card: results grid, FEAT chips and an Advanced
    details expander (protocol flags, throughput sample, destination note,
    detector evidence). Every app._ftpdiag_* / app._ftpspeed_* attribute
    the backend writes is preserved."""
    from ui.shared.widgets import Card, Expander, IconLabel, Tooltip, Divider
    BG2 = C['bg_2']
    card = Card(parent, title=_('Connection Diagnostics'))
    app._ftpdiag_status = tk.StringVar(value=_('Not run yet'))
    tk.Label(card.title_label.master, textvariable=app._ftpdiag_status,
             font=FONTS['caption'], bg=BG2, fg=C['fg_3'], anchor='w'
             ).pack(fill='x', pady=(px(2), 0))

    btnbar = card.head_actions
    app._ftpdiag_btn = XButton(btnbar, text=_('Run Diagnostics'),
                               icon='microscope', command=app._ftpdiag_run)
    app._ftpdiag_btn.pack(side='left')
    app._ftpspeed_btn = XButton(btnbar, text=_('Run Speed Test'),
                                icon='timer', command=app._ftpspeed_run)
    app._ftpspeed_btn.pack(side='left', padx=(px(6), 0))
    # Packed by _ftpdiag_show_webui when a web UI is detected.
    app._ftpdiag_webui_btn = XButton(btnbar, text=_('Open Web UI'),
                                     icon='globe', variant='subtle',
                                     command=lambda: _ftpdiag_open_webui(app))

    def _field(parent_, caption, col=None, row=0):
        cell = tk.Frame(parent_, bg=BG2)
        if col is None:
            cell.pack(side='left', padx=(0, px(24)))
        else:
            cell.grid(row=row, column=col, sticky='nw',
                      pady=(0 if row == 0 else px(12), 0))
        tk.Label(cell, text=caption, font=FONTS['overline'], bg=BG2,
                 fg=C['fg_4'], anchor='w').pack(fill='x')
        var = tk.StringVar(value='—')
        line = tk.Frame(cell, bg=BG2)
        line.pack(fill='x', pady=(px(4), 0))
        lbl = tk.Label(line, textvariable=var, font=FONTS['body_strong'],
                       bg=BG2, fg=C['fg_0'], anchor='w')
        lbl.pack(side='left')
        cell.line = line
        return cell, var, lbl

    grid = tk.Frame(card.body, bg=BG2)
    grid.pack(fill='x')
    for c in range(5):
        grid.columnconfigure(c, weight=1, uniform='ftpdiag')
    _c, app._ftpdiag_reach_var, app._ftpdiag_reach_lbl = _field(
        grid, _('REACHABLE'), 0)
    _c, app._ftpdiag_rtt_var, _rtt_lbl = _field(grid, _('RTT'), 1)
    _c, app._ftpdiag_server_var, app._ftpdiag_server_lbl = _field(
        grid, _('SERVER'), 2)
    app._ftpdiag_server_lbl.bind(
        '<Button-1>', lambda e: _ftpdiag_open_webui(app)
        if getattr(app, '_ftpdiag_server_clickable', False) else None)
    dest_cell, app._ftpdiag_dest_var, app._ftpdiag_dest_lbl = _field(
        grid, _('DEST'), 3)
    app._ftpdiag_dest_dot = IconLabel(dest_cell.line, 'circle', 12,
                                      C['fg_5'], BG2)
    app._ftpdiag_dest_dot.pack(side='left', padx=(0, px(6)),
                               before=app._ftpdiag_dest_lbl)
    _c, app._ftpdiag_write_var, app._ftpdiag_write_lbl = _field(
        grid, _('WRITABLE'), 4)

    Divider(card.body, color=C['border_1']).pack(fill='x', pady=px(16))
    feat = tk.Frame(card.body, bg=BG2)
    feat.pack(fill='x')
    tk.Label(feat, text=_('FEAT'), font=FONTS['overline'], bg=BG2,
             fg=C['fg_4']).pack(side='left', padx=(0, px(12)))
    app._ftpdiag_feat_chips = {}
    _feat_help = {'mlsd': 'MLSD — modern machine-readable directory listings', 'rest': 'REST — transfer resume is supported', 'size': 'SIZE — server can report exact file sizes', 'mdtm': 'MDTM — server reports file modification times', 'utf8': 'UTF8 — Unicode filenames are supported', 'xcrypt': 'XCRYPT — optional encrypted transfers (zftpd; slower, off by default)'}
    for key, text in (('mlsd', 'MLSD'), ('rest', 'REST'), ('size', 'SIZE'),
                      ('mdtm', 'MDTM'), ('utf8', 'UTF8'),
                      ('xcrypt', 'XCRYPT')):
        chip = _FeatChip(feat, text, 'outline', icon=False)
        chip.pack(side='left', padx=(0, px(6)))
        Tooltip(chip, _feat_help[key])
        app._ftpdiag_feat_chips[key] = chip

    exp = Expander(card.body, title=_('Advanced Details'),
                   caption='Protocol · throughput · destination')
    exp.pack(fill='x', pady=(px(16), 0))
    app._ftpdiag_expander = exp
    app._ftpdiag_adv_open = False
    app._ftpdiag_adv_toggle = exp.head
    app._ftpdiag_adv_frame = exp.body
    exp.on_toggle = lambda is_open: setattr(app, '_ftpdiag_adv_open', is_open)

    adv_inner = Card(exp.body, padding=16, tone='canvas')
    adv_inner.pack(fill='x')
    agrid = tk.Frame(adv_inner, bg=C['bg_1'])
    agrid.pack(fill='x')
    for c in range(4):
        agrid.columnconfigure(c, weight=1, uniform='ftpadv')

    def _afield(caption, col, row):
        cell = tk.Frame(agrid, bg=C['bg_1'])
        cell.grid(row=row, column=col, sticky='nw',
                  pady=(0 if row == 0 else px(12), 0))
        tk.Label(cell, text=caption, font=FONTS['overline'], bg=C['bg_1'],
                 fg=C['fg_4'], anchor='w').pack(fill='x')
        var = tk.StringVar(value='—')
        lbl = tk.Label(cell, textvariable=var, font=FONTS['body_strong'],
                       bg=C['bg_1'], fg=C['fg_0'], anchor='w')
        lbl.pack(fill='x', pady=(px(4), 0))
        return cell, var, lbl

    tk.Label(agrid, text=_('PROTOCOL'), font=FONTS['caption_strong'],
             bg=C['bg_1'], fg=C['fg_2'], anchor='w'
             ).grid(row=0, column=0, columnspan=4, sticky='w',
                    pady=(0, px(8)))
    _c, app._ftpdiag_pasv_var, app._ftpdiag_pasv_lbl = _afield(
        _('PASSIVE'), 0, 1)
    _c, app._ftpdiag_bin_var, app._ftpdiag_bin_lbl = _afield(
        _('BINARY'), 1, 1)
    _c, app._ftpdiag_copy_var, app._ftpdiag_copy_lbl = _afield(
        _('SERVER-SIDE COPY'), 2, 1)
    tk.Label(agrid, text=_('THROUGHPUT'), font=FONTS['caption_strong'],
             bg=C['bg_1'], fg=C['fg_2'], anchor='w'
             ).grid(row=2, column=0, columnspan=4, sticky='w',
                    pady=(px(16), px(8)))
    _c, app._ftpspeed_up_var, _l = _afield(_('UPLOAD'), 0, 3)
    _c, app._ftpspeed_dn_var, _l = _afield(_('DOWNLOAD'), 1, 3)
    app._ftpspeed_status = tk.StringVar(value=_('Not run yet'))
    tk.Label(agrid, textvariable=app._ftpspeed_status, font=FONTS['caption'],
             bg=C['bg_1'], fg=C['fg_3'], anchor='w', justify='left'
             ).grid(row=3, column=2, columnspan=2, sticky='sw')

    from ui.shared.widgets import autowrap
    app._ftpdiag_dest_note = tk.StringVar(value='')
    app._ftpdiag_dest_note_lbl = tk.Label(
        adv_inner, textvariable=app._ftpdiag_dest_note, font=FONTS['caption'],
        bg=C['bg_1'], fg=C['fg_2'], anchor='w', justify='left')
    app._ftpdiag_dest_note_lbl.pack(fill='x', pady=(px(16), 0))
    autowrap(app._ftpdiag_dest_note_lbl)
    app._ftpdiag_evidence = tk.StringVar(value='')
    app._ftpdiag_evidence_lbl = tk.Label(
        adv_inner, textvariable=app._ftpdiag_evidence, font=FONTS['code'],
        bg=C['bg_1'], fg=C['fg_3'], anchor='w', justify='left')
    app._ftpdiag_evidence_lbl.pack(fill='x', pady=(px(4), 0))
    autowrap(app._ftpdiag_evidence_lbl)

    def _live_dest(*_a):
        try:
            key, label, note, tone = app._ps5_dest_class(app._br_path.get())
            app._ftpdiag_dest_var.set(label)
            app._ftpdiag_dest_note.set(note)
            _tone_color(app._ftpdiag_dest_lbl, tone)
            _dot_color(app._ftpdiag_dest_dot, tone)
        except Exception:
            pass
    try:
        app._br_path.trace_add('write', _live_dest)
        _live_dest()
    except Exception:
        pass
    return card


def _ftpdiag_toggle_adv(app):
    """Expand / collapse the Advanced details expander."""
    exp = getattr(app, '_ftpdiag_expander', None)
    if exp is None:
        return
    try:
        exp.toggle()
    except Exception:
        pass
    app._ftpdiag_adv_open = bool(getattr(exp, 'is_open', False))


def _tone_color(lbl, tone):
    """Colour a result value by tone: ok / warn / bad / muted."""
    try:
        if tone == 'ok':
            lbl.configure(fg=C['success_hi'])
        elif tone == 'warn':
            lbl.configure(fg=C['warn_hi'])
        elif tone == 'bad':
            lbl.configure(fg=C['danger_hi'])
        else:
            lbl.configure(fg=C['fg_3'])
    except Exception:
        pass


def _dot_color(dot, tone):
    """Status glyph beside a value: ok / warn / bad / muted. Accepts the
    IconLabel used now and plain Frames (legacy dots)."""
    icon, col = {'ok': ('success', C['success']),
                 'warn': ('warning', C['warn']),
                 'bad': ('error', C['danger'])}.get(tone, ('circle', C['fg_5']))
    try:
        if hasattr(dot, 'set'):
            dot.set(icon, col)
        else:
            dot.configure(bg=col)
    except Exception:
        pass


def _attach_tooltip(widget, text):
    """Hover tooltip (design-system Tooltip)."""
    try:
        from ui.shared.widgets import Tooltip
        Tooltip(widget, text)
    except Exception:
        return


def _ftpdiag_open_webui(app):
    """Open the detected ZHTTP web UI in the default browser (local only)."""
    import webbrowser
    url = getattr(app, '_ftpdiag_webui_url', None)
    if not url:
        ip = app._ftp_ip_var.get().strip()
        port = app._ftp_port_var.get().strip() or '2121'
        url = f"http://{ip!s}:{port!s}/"
    try:
        webbrowser.open(url)
    except Exception:
        return
