"""
ui/tab_klog.py — Kernel log sub-tab of PS5 (live PS5 kernel log viewer).

Farewell Edition redesign on the design system:

    PageHeader  Kernel Log Monitor
    ┌ Connection ─ IP [....]  Port [3232]  [Connect] [Disconnect] [Pause]
    │                                            [status badge] hint     ┘
    Stat cards: total lines · lines/sec · warnings · errors · connection
    ┌ Log ─ [Clear] [Export log]  Auto-scroll · Word wrap · Timestamps  N lines
    │ [filter …………………………………………………………] [Clear]
    │ ┌ inset console (monospace, design-system log colours) ┐
    └─┴──────────────────────────────────────────────────────┘

Backwards compat: every `app._klog_*` attribute the callbacks rely on is
preserved with the same name and a compatible widget API:
    _klog_box, _klog_lines, _klog_count_var, _klog_filter_var,
    _klog_autoscroll, _klog_word_wrap, _klog_timestamps, _klog_paused,
    _klog_status_var, _klog_status_lbl (a VarBadge: .config(fg=…) maps to
    a tone), _klog_status_dot, _klog_connect_btn, _klog_stop_btn,
    _klog_pause_btn, _klog_stat_*.
"""

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.tab_ps5_mgr import (
    ps5_page, page_header, section, StatTile, VarBadge, NullWidget, console,
    search_field,
)

C = COLORS


def build_klog_tab(parent, app):
    """Build the Kernel log sub-tab into `parent`."""
    app._klog_ip_var      = tk.StringVar(
        value=app._settings.get('klog_ip', app._settings.get('ftp_ip', '')))
    app._klog_port_var    = tk.StringVar(
        value=str(app._settings.get('klog_port', 3232)))
    app._klog_running     = False
    app._klog_socket      = [None]
    app._klog_lines       = []
    app._klog_filter_var  = tk.StringVar()
    app._klog_autoscroll  = tk.BooleanVar(value=True)
    app._klog_word_wrap   = tk.BooleanVar(value=True)
    app._klog_timestamps  = tk.BooleanVar(value=True)
    app._klog_paused      = tk.BooleanVar(value=False)

    # No page scroll: the console takes the remaining height.
    body = ps5_page(parent, scroll=False)
    page_header(body, _('Kernel Log Monitor'),
                _('Stream the PS5 kernel log live — filter, color, export.'))
    _build_page_head(body, app)
    _build_stats_strip(body, app)
    _build_log_card(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Connection card — IP / Port · Connect / Disconnect / Pause · status
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    from ui.shared.widgets import Card, Button, field_label
    sec = section(body)
    card = Card(sec, padding=16)
    card.pack(fill='x')
    BG2 = C['bg_2']

    def _field(label, var, width):
        f = tk.Frame(card, bg=BG2)
        f.pack(side='left', padx=(0, px(12)))
        field_label(f, label, bg=BG2).pack(fill='x')
        XEntry(f, textvariable=var, mono=True, width=width).pack(
            anchor='w', pady=(px(4), 0))
        return f

    _field(_('PS5 IP'), app._klog_ip_var, 18)
    _field(_('Port'), app._klog_port_var, 7)

    btns = tk.Frame(card, bg=BG2)
    btns.pack(side='left', anchor='s')
    app._klog_connect_btn = Button(btns, text=_('Connect'), icon='play',
                                   variant='primary',
                                   command=app._klog_connect)
    app._klog_connect_btn.pack(side='left')
    app._klog_stop_btn = Button(btns, text=_('Disconnect'), icon='stop',
                                command=app._klog_disconnect,
                                state='disabled')
    app._klog_stop_btn.pack(side='left', padx=(px(6), 0))
    app._klog_pause_btn = Button(btns, text='Pause', icon='pause',
                                 command=app._klog_toggle_pause,
                                 state='disabled')
    app._klog_pause_btn.pack(side='left', padx=(px(6), 0))

    right = tk.Frame(card, bg=BG2)
    right.pack(side='right', anchor='s')
    app._klog_status_var = tk.StringVar(value=_('Disconnected'))
    app._klog_status_lbl = VarBadge(right, textvariable=app._klog_status_var)
    app._klog_status_lbl.pack(anchor='e')
    app._klog_status_dot = NullWidget()
    tk.Label(right, text=_('Default port: 3232  ·  TCP socket'),
             font=FONTS['caption'], bg=BG2, fg=C['fg_3']
             ).pack(anchor='e', pady=(px(6), px(6)))


# ─────────────────────────────────────────────────────────────────────────
# Stat cards — live counters derived on the presentation side: a 1 s
# after() poll walks NEW entries in `app._klog_lines` and classifies them
# with the existing `app._klog_get_tag`. Socket / parse code untouched.
# ─────────────────────────────────────────────────────────────────────────
def _build_stats_strip(body, app):
    strip = section(body)
    for c in range(5):
        strip.columnconfigure(c, weight=1, uniform='klogstat')

    def _cell(col, caption, value='0', icon=None, tone=None, compact=False):
        t = StatTile(strip, caption, value, icon=icon, tone=tone,
                     compact=compact)
        t.grid(row=0, column=col, sticky='nsew',
               padx=(0 if col == 0 else px(8), 0 if col == 4 else px(8)))
        return t

    app._klog_stat_total = _cell(0, _('Total Lines'), icon='list')
    app._klog_stat_rate  = _cell(1, _('Lines / sec'), icon='timer')
    app._klog_stat_warn  = _cell(2, _('Warnings'), icon='warning', tone='warn')
    app._klog_stat_err   = _cell(3, _('Errors'), icon='error', tone='danger')
    conn = _cell(4, _('Connection'), _('OFFLINE'), icon='plug', compact=True)
    app._klog_stat_conn = conn
    app._klog_stat_info = app._klog_stat_total

    def _sync_conn(*_a):
        try:
            conn.set_value(app._klog_status_var.get())
        except Exception:
            pass
    app._klog_status_var.trace_add('write', _sync_conn)
    _sync_conn()

    state = {'idx': 0, 'warn': 0, 'err': 0, 'last_total': 0}

    def _tick():
        try:
            if not strip.winfo_exists():
                return
        except Exception:
            return
        try:
            lines = app._klog_lines
            total = len(lines)
            if total < state['idx']:
                # Buffer was cleared (Clear button) — reset counters
                state['idx'] = 0
                state['warn'] = 0
                state['err'] = 0
                state['last_total'] = 0
            for i in range(state['idx'], total):
                try:
                    tag = app._klog_get_tag(lines[i])
                except Exception:
                    tag = 'info'
                if tag == 'error':
                    state['err'] += 1
                elif tag == 'warning':
                    state['warn'] += 1
            state['idx'] = total
            rate = max(0, total - state['last_total'])
            state['last_total'] = total
            app._klog_stat_total.set_value('{:,}'.format(total))
            app._klog_stat_rate.set_value(str(rate))
            app._klog_stat_warn.set_value('{:,}'.format(state['warn']))
            app._klog_stat_err.set_value('{:,}'.format(state['err']))
        except Exception:
            pass
        try:
            strip.after(1000, _tick)
        except Exception:
            pass
    strip.after(1000, _tick)


# ─────────────────────────────────────────────────────────────────────────
# Log card — toolbar, filter and the inset console
# ─────────────────────────────────────────────────────────────────────────
def _build_log_card(body, app):
    from ui.shared.widgets import Card, Button, Switch, Divider
    sec = tk.Frame(body, bg=C['bg_1'])
    sec.pack(fill='both', expand=True, padx=px(24), pady=(0, px(24)))
    card = Card(sec, padding=16)
    card.pack(fill='both', expand=True)
    BG2 = C['bg_2']

    bar = tk.Frame(card, bg=BG2)
    bar.pack(fill='x')
    Button(bar, text=_('Clear'), icon='trash', size='sm',
           command=app._klog_clear).pack(side='left')
    Button(bar, text='Export log', icon='download', size='sm',
           command=app._klog_export).pack(side='left', padx=(px(6), 0))
    Divider(bar, color=C['border_2'], vertical=True).pack(
        side='left', fill='y', padx=px(12), pady=px(4))
    Switch(bar, variable=app._klog_autoscroll, text=_('Auto-scroll'),
           show_state=False).pack(side='left')
    Switch(bar, variable=app._klog_word_wrap, text=_('Word wrap'),
           show_state=False, command=app._klog_toggle_wrap
           ).pack(side='left', padx=(px(16), 0))
    Switch(bar, variable=app._klog_timestamps, text=_('Timestamps'),
           show_state=False).pack(side='left', padx=(px(16), 0))
    app._klog_count_var = tk.StringVar(value='0 lines')
    tk.Label(bar, textvariable=app._klog_count_var, font=FONTS['code'],
             bg=BG2, fg=C['fg_3']).pack(side='right')

    frow = tk.Frame(card, bg=BG2)
    frow.pack(fill='x', pady=(px(12), px(12)))
    Button(frow, text=_('Clear'), size='sm', variant='subtle',
           command=lambda: app._klog_filter_var.set('')).pack(
               side='right', padx=(px(6), 0))
    sf = search_field(frow, app._klog_filter_var,
                      placeholder='Filter — type to highlight')
    sf.pack(side='left', fill='x', expand=True)
    app._klog_filter_var.trace_add('write',
                                   lambda *a: app._klog_apply_filter())

    well, txt = console(card, height=12)
    well.pack(fill='both', expand=True)
    app._klog_box = txt
    app._klog_console = well
