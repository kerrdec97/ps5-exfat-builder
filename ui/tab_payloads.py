"""
ui/tab_payloads.py — Payloads sub-tab of PS5 (payload manager).

Farewell Edition redesign on the design system:

    PageHeader  Payload Manager                          [+ Add payload]
    ┌ Payload target ─ IP [....] Port [9090] [Save]  default ports ────┐
    │ ── payloads · console IP · default port · last sent ──            │
    └──────────────────────────────────────────────────────────────────┘
    ┌ Payloads (N saved) ──────────────────┐ ┌ Detail ────────────────┐
    │ [search…]                            │ │ name  [Ready]          │
    │ row: icon · name · notes · meta [Send]│ │ file · port · ip · last│
    │ row …                                │ │ [Send to …]            │
    └──────────────────────────────────────┘ │ [Edit] [Remove]  ⚠ bar │
                                             └────────────────────────┘

Backwards compat: every `_pl_*` attribute the callbacks read is preserved.
Rows are rendered by app._pl_render (main file) into `_pl_list_frame`.
"""

import os
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from ui.shared.app_dialog import app_confirm
from exfat_builder import _, save_settings

from ui.tab_ps5_mgr import (
    ps5_page, page_header, section, two_columns, StatGrid, search_field,
)

C = COLORS


def build_payloads_tab(parent, app):
    app._pl_payloads     = app._settings.get('payloads', [])
    app._pl_ip_var       = tk.StringVar(
        value=app._settings.get('pl_ip', app._settings.get('ftp_ip', '')))
    app._pl_port_var     = tk.StringVar(
        value=str(app._settings.get('pl_port', 9090)))
    app._pl_sending      = False
    app._pl_search_var   = tk.StringVar()
    app._pl_selected_idx = tk.IntVar(value=-1)
    app._pl_status_var   = tk.StringVar(value='')

    body = ps5_page(parent)
    hdr = page_header(body, _('Payload Manager'),
                      _('Send .elf / .bin payloads to your PS5.'))
    from ui.shared.widgets import Button
    Button(hdr.actions, text=_('Add payload'), icon='plus',
           command=app._pl_add).pack(side='right')

    _build_connection_bar(body, app)
    _build_workspace(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Payload target card — IP / port + metrics
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_bar(parent, app):
    from ui.shared.widgets import Card, Button, Divider, field_label, Badge
    sec = section(parent)
    card = Card(sec, title='Payload target',
                subtitle=_('Default ports: 9090 / 9021'))
    card.pack(fill='x')
    BG2 = C['bg_2']

    row = tk.Frame(card.body, bg=BG2)
    row.pack(fill='x')

    def _field(label, var, width):
        f = tk.Frame(row, bg=BG2)
        f.pack(side='left', padx=(0, px(12)))
        field_label(f, label, bg=BG2).pack(fill='x')
        XEntry(f, textvariable=var, mono=True, width=width).pack(
            anchor='w', pady=(px(4), 0))

    _field(_('IP'), app._pl_ip_var, 18)
    _field(_('Port'), app._pl_port_var, 7)
    Button(row, text=_('Save'), icon='save', command=app._pl_save_conn
           ).pack(side='left', anchor='s')

    Divider(card.body, color=C['border_1']).pack(fill='x', pady=px(16))
    _build_metrics_strip(card.body, app)


def _build_metrics_strip(parent, app):
    """Payloads / Console IP / Default Port / Last Sent. Every value
    mirrors an existing var; Last Sent is a session timestamp recorded
    when the status line reports a successful send."""
    strip = StatGrid(parent, [('Payloads', 'count'),
                              ('Console IP', 'ip'),
                              ('Default Port', 'port'),
                              ('Last Sent', 'last')])
    strip.pack(fill='x')
    app._pl_metrics_strip = strip

    def _mirror(*_a):
        try:
            strip.set('count', str(len(app._pl_payloads)))
        except Exception:
            pass
        try:
            strip.set('ip', app._pl_ip_var.get().strip() or '—')
            strip.set('port', ':' + (app._pl_port_var.get().strip()
                                     or '9090'))
        except Exception:
            pass

    def _on_status(*_a):
        try:
            txt = app._pl_status_var.get()
            if '✓' in txt and ('sent' in txt.lower() or
                                     'Sent' in txt):
                import time as _t
                strip.set('last', _t.strftime('%H:%M:%S'), ok=True)
            _mirror()
        except Exception:
            pass

    _mirror()
    try:
        app._pl_ip_var.trace_add('write', lambda *a: _mirror())
        app._pl_port_var.trace_add('write', lambda *a: _mirror())
        app._pl_status_var.trace_add('write', _on_status)
    except Exception:
        pass

    def _wire_count(tries=0):
        v = getattr(app, '_pl_count_var', None)
        if v is not None:
            try:
                v.trace_add('write', lambda *a: _mirror())
            except Exception:
                pass
            _mirror()
            return
        if tries < 10:
            strip.after(300, lambda: _wire_count(tries + 1))
    strip.after(300, _wire_count)


# ─────────────────────────────────────────────────────────────────────────
# Workspace — list card + detail card
# ─────────────────────────────────────────────────────────────────────────
def _build_workspace(parent, app):
    left, right = two_columns(parent, 7, 5, pady=(0, 24))
    _build_list_pane(left, app).pack(fill='both', expand=True)
    _build_detail_pane(right, app).pack(fill='both', expand=True)


def _build_list_pane(parent, app):
    from ui.shared.widgets import Card, Divider
    card = Card(parent, title=_('Payloads'), padding=0)
    try:
        card.title_label.master.master.pack_configure(
            padx=px(16), pady=(px(16), px(12)))
    except Exception:
        pass
    app._pl_count_var = tk.StringVar(value='')
    tk.Label(card.head_actions, textvariable=app._pl_count_var,
             font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3']
             ).pack(side='right', pady=(px(3), 0))

    sf = search_field(card.body, app._pl_search_var,
                      placeholder='Search payloads')
    sf.pack(fill='x', padx=px(16), pady=(0, px(12)))
    app._pl_search_var.trace_add('write', lambda *a: app._pl_render())

    # Status text — kept for legacy compat (worker writes here)
    foot = tk.Frame(card.body, bg=C['bg_2'])
    foot.pack(fill='x', side='bottom')
    tk.Label(foot, textvariable=app._pl_status_var, font=FONTS['caption'],
             bg=C['bg_2'], fg=C['fg_3'], anchor='w'
             ).pack(fill='x', padx=px(16), pady=px(10))
    Divider(card.body, color=C['border_1']).pack(fill='x', side='bottom')

    Divider(card.body, color=C['border_2']).pack(fill='x')
    # The list grows inside the page scroll (one scrollbar per page).
    app._pl_list_frame = tk.Frame(card.body, bg=C['bg_2'])
    app._pl_list_frame.pack(fill='both', expand=True)
    app._pl_render()
    return card


def _build_detail_pane(parent, app):
    from ui.shared.widgets import Card
    card = Card(parent, title=_('Detail'))
    app._pl_detail_frame = card.body
    _render_detail_empty(app)
    return card


def _render_detail_empty(app):
    """Empty state for the detail card — shown when nothing is selected."""
    from ui.shared.widgets import EmptyState
    for w in app._pl_detail_frame.winfo_children():
        w.destroy()
    es = EmptyState(app._pl_detail_frame, icon='rocket',
                    title=_('Select a payload to see details'),
                    bg=C['bg_2'])
    es.configure(height=px(200))
    es.pack(fill='both', expand=True)


def _render_detail_for(app, idx):
    """Render the detail card for the payload at index `idx`."""
    from ui.shared.widgets import (Badge, Button, Divider, InfoBar,
                                   autowrap)
    if idx < 0 or idx >= len(app._pl_payloads):
        _render_detail_empty(app)
        return
    p = app._pl_payloads[idx]
    for w in app._pl_detail_frame.winfo_children():
        w.destroy()
    parent = app._pl_detail_frame
    BG2 = C['bg_2']

    path = p.get('path', '')
    exists = bool(path and os.path.isfile(path))
    title_row = tk.Frame(parent, bg=BG2)
    title_row.pack(fill='x')
    Badge(title_row, _('READY').capitalize() if exists else
          _('MISSING').capitalize(),
          'success' if exists else 'danger').pack(side='right', anchor='n')
    t = tk.Label(title_row, text=p.get('name', '(unnamed)'),
                 font=FONTS['subtitle'], bg=BG2, fg=C['fg_0'], anchor='w',
                 justify='left')
    t.pack(side='left', fill='x', expand=True)
    autowrap(t)

    size_str = ''
    if exists:
        try:
            sz = os.path.getsize(path)
            size_str = ('%.1f KB' % (sz / 1024) if sz < 1024**2
                        else '%.2f MB' % (sz / 1024**2))
        except Exception:
            pass
    tk.Label(parent, text=os.path.basename(path) + ((' · ' + size_str)
                                                    if size_str else ''),
             font=FONTS['code'], bg=BG2, fg=C['fg_3'], anchor='w'
             ).pack(fill='x', pady=(px(2), px(12)))

    notes = p.get('notes', '').strip()
    if notes:
        n = tk.Label(parent, text=notes, font=FONTS['body'], bg=BG2,
                     fg=C['fg_2'], anchor='w', justify='left')
        n.pack(fill='x', pady=(0, px(12)))
        autowrap(n)

    Divider(parent, color=C['border_1']).pack(fill='x', pady=(0, px(4)))

    def _kv(key, value, mono=True):
        row = tk.Frame(parent, bg=BG2)
        row.pack(fill='x', pady=px(6))
        tk.Label(row, text=key, font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
                 anchor='w').pack(fill='x')
        v = tk.Label(row, text=value, font=FONTS['code'] if mono else
                     FONTS['body'], bg=BG2, fg=C['fg_1'], anchor='w',
                     justify='left')
        v.pack(fill='x', pady=(px(2), 0))
        autowrap(v)

    _kv(_('File'), path or '—')
    eff_port = str(p.get('port', '') or app._pl_port_var.get().strip()
                   or '9090')
    _kv(_('Port'), eff_port +
        ('' if p.get('port') else '  (' + _('from connection bar') + ')'))
    _kv(_('Per-payload IP'),
        p.get('ip', '') or _('uses connection bar IP'))
    last = p.get('last_sent', '')
    _kv(_('Last sent'), str(last) if last else '—')

    target_ip = p.get('ip') or app._pl_ip_var.get()
    target_port = p.get('port') or app._pl_port_var.get()
    _kv(_('TARGET').capitalize(),
        (target_ip or _('(no IP set)')) + ':' + str(target_port))

    actions = tk.Frame(parent, bg=BG2)
    actions.pack(fill='x', pady=(px(12), 0))
    Button(actions, text=_('Send to %s') % (target_ip or '...'), icon='send',
           variant='primary', command=lambda: app._pl_send(p)
           ).pack(fill='x', pady=(0, px(8)))
    row2 = tk.Frame(actions, bg=BG2)
    row2.pack(fill='x')
    row2.columnconfigure(0, weight=1, uniform='pla')
    row2.columnconfigure(1, weight=1, uniform='pla')
    Button(row2, text=_('Edit'), icon='pencil',
           command=lambda: app._pl_edit(idx)
           ).grid(row=0, column=0, sticky='ew', padx=(0, px(4)))
    Button(row2, text=_('Remove'), icon='trash', variant='danger',
           command=lambda: _pl_remove_then_refresh(app, idx)
           ).grid(row=0, column=1, sticky='ew', padx=(px(4), 0))

    InfoBar(parent, kind='warn',
            text=_('Sending a payload while another is resident can crash '
                   'the kernel. Reboot between unrelated payloads.')
            ).pack(fill='x', pady=(px(16), 0))


def _pl_remove_then_refresh(app, idx):
    """Remove confirmation + refresh both panes."""
    if idx < 0 or idx >= len(app._pl_payloads):
        return
    name = app._pl_payloads[idx].get('name', '')
    if not app_confirm(app, _('Remove payload'),
        _('Remove "%s" from the list?') % name):
        return
    app._pl_payloads.pop(idx)
    app._settings['payloads'] = app._pl_payloads
    save_settings(app._settings)
    app._pl_render()
    app._pl_selected_idx.set(-1)
    _render_detail_empty(app)


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
    return XButton(parent, text=text, command=command)


def _accent_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='primary')


def _danger_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='danger')
