"""
ui/tab_payloads.py — Payload Manager tab.

Step 14 (v2.1.6): refactored against preview/payloads-tab-redesign.html.

Layout:

    ┌─ connection bar ────────────────────────────────────────────────┐
    │ ●LISTENING  192.168.1.42:9020  …  [+ Add custom] [↻ Refresh]    │
    ├─ workspace ─────────────────────────────────────────────────────┤
    │ ┌─ list-wrap (left, expands) ──┐ ┌─ detail aside (right, 280px)┐│
    │ │ Payloads  N saved             │ │ etaHEN                       ││
    │ │ 🔍 search…                    │ │ v2.1b · 2.4 MB              ││
    │ │ ┌─ payload card ─────────────┐│ │                              ││
    │ │ │⚡ etaHEN                    ││ │ Description text…            ││
    │ │ │ HEN payload, FTP+klog       ││ │ ──────────────────           ││
    │ │ │ FW 5.50 · v2.1b · ↗ Send   ││ │ file: etahen.bin             ││
    │ │ └────────────────────────────┘│ │ target: ...                  ││
    │ │ ┌─ payload card (selected) ──┐│ │ requires: ...                ││
    │ │ │⚡ kex umtx                  ││ │ ──────────────────           ││
    │ │ └────────────────────────────┘│ │ Target consoles              ││
    │ │ ...                           │ │ ●192.168.1.42 · :9020        ││
    │ │                               │ │ [↗ Send to ...]              ││
    │ └───────────────────────────────┘ └─────────────────────────────┘│
    └─────────────────────────────────────────────────────────────────┘

The mock's "categories sidebar" (Source / Category / Firmware) is dropped
this iteration — the existing payload schema has no category field, so
populating those bins would require a data-model change. Search input +
filter on name/notes covers the practical use case.

Backwards compat: every `_pl_*` attribute the existing 6+ callbacks read
is preserved with the same name. The `_pl_render` callback is rewritten
in place to build cards in the new design — UI-only rewrite, same
scope-bend pattern.
"""

import os
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings


def build_payloads_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State
    app._pl_payloads     = app._settings.get('payloads', [])
    app._pl_ip_var       = tk.StringVar(
        value=app._settings.get('pl_ip', app._settings.get('ftp_ip', '')))
    app._pl_port_var     = tk.StringVar(
        value=str(app._settings.get('pl_port', 9090)))
    app._pl_sending      = False
    app._pl_search_var   = tk.StringVar()
    app._pl_selected_idx = tk.IntVar(value=-1)
    app._pl_status_var   = tk.StringVar(value='')

    _build_connection_bar(parent, app)
    _build_workspace(parent, app)


# ─────────────────────────────────────────────────────────────────────────
# Connection bar at top
# ─────────────────────────────────────────────────────────────────────────
def _build_connection_bar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_0'])
    bar.pack(fill='x')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=8)

    # Status pill — visual: shows the IP from settings (no live probe)
    pill = tk.Frame(inner, bg=COLORS['accent_08'],
                    highlightbackground=COLORS['accent_lo'],
                    highlightthickness=1)
    pill.pack(side='left')
    pill_inner = tk.Frame(pill, bg=COLORS['accent_08'])
    pill_inner.pack(padx=10, pady=4)
    tk.Frame(pill_inner, bg=COLORS['accent'],
             width=8, height=8
             ).pack(side='left', padx=(0, 6))
    tk.Label(pill_inner, text=_('PAYLOAD BUS'),
             font=(FONTS['mono_sm'][0], 10, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent_hi']
             ).pack(side='left')

    # Editable IP + port (legacy attrs)
    tk.Label(inner, text=_('IP'),
             font=FONTS['label'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(16, 4))
    ip_wrap = tk.Frame(inner, bg=COLORS['bg_3'],
                       highlightbackground=COLORS['border_3'],
                       highlightthickness=1)
    ip_wrap.pack(side='left')
    tk.Entry(ip_wrap, textvariable=app._pl_ip_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=4, width=16
             ).pack()

    tk.Label(inner, text=_('Port'),
             font=FONTS['label'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    port_wrap = tk.Frame(inner, bg=COLORS['bg_3'],
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
    port_wrap.pack(side='left')
    tk.Entry(port_wrap, textvariable=app._pl_port_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=4, width=7
             ).pack()

    _ghost_btn(inner, _('Save'),
               command=app._pl_save_conn
               ).pack(side='left', padx=(6, 0))
    tk.Label(inner, text=_('Default ports: 9090 / 9021'),
             font=FONTS['meta'],
             bg=COLORS['bg_0'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(12, 0))

    # Right-aligned actions
    _accent_btn(inner, '+  ' + _('Add payload'),
                command=app._pl_add
                ).pack(side='right')

    tk.Frame(parent, bg=COLORS['border_2'], height=1).pack(fill='x')


# ─────────────────────────────────────────────────────────────────────────
# Workspace — list + detail
# ─────────────────────────────────────────────────────────────────────────
def _build_workspace(parent, app):
    ws = tk.Frame(parent, bg=COLORS['bg_1'])
    ws.pack(fill='both', expand=True)
    ws.grid_columnconfigure(0, weight=1)
    ws.grid_columnconfigure(1, weight=0, minsize=300)
    ws.grid_rowconfigure(0, weight=1)

    _build_list_pane(ws, app).grid(row=0, column=0, sticky='nsew')
    tk.Frame(ws, bg=COLORS['border_2'], width=1
             ).grid(row=0, column=0, sticky='nse')
    _build_detail_pane(ws, app).grid(row=0, column=1, sticky='nsew')


def _build_list_pane(parent, app):
    pane = tk.Frame(parent, bg=COLORS['bg_1'])

    # Head
    head = tk.Frame(pane, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=22, pady=(14, 8))
    tk.Label(head, text=_('Payloads'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    app._pl_count_var = tk.StringVar(value='')
    tk.Label(head, textvariable=app._pl_count_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             padx=8, pady=2,
             highlightbackground=COLORS['border_2'],
             highlightthickness=1
             ).pack(side='left', padx=(12, 0))

    # Search input
    search_wrap = tk.Frame(pane, bg=COLORS['bg_0'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(fill='x', padx=22, pady=(0, 10))
    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(search_wrap, textvariable=app._pl_search_var,
             font=FONTS['body'],
             bg=COLORS['bg_0'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0
             ).pack(fill='x', ipady=5, padx=(0, 8))

    # Live filter
    app._pl_search_var.trace_add('write', lambda *a: app._pl_render())

    # Scrollable list area
    list_outer = tk.Frame(pane, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    list_outer.pack(fill='both', expand=True, padx=22, pady=(0, 12))

    canvas = tk.Canvas(list_outer, bg=COLORS['bg_2'], highlightthickness=0)
    sb = tk.Scrollbar(list_outer, command=canvas.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._pl_list_frame = tk.Frame(canvas, bg=COLORS['bg_2'])
    win = canvas.create_window((0, 0), window=app._pl_list_frame,
                                anchor='nw')
    app._pl_list_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(win, width=e.width))
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    # Status text — kept for legacy compat (worker writes here)
    tk.Label(pane, textvariable=app._pl_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x', padx=22, pady=(0, 8))

    # Initial render
    app._pl_render()
    return pane


def _build_detail_pane(parent, app):
    pane = tk.Frame(parent, bg=COLORS['bg_1'])

    head = tk.Frame(pane, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=18, pady=(14, 8))
    tk.Label(head, text=_('Detail'),
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='left')
    tk.Frame(pane, bg=COLORS['border_2'], height=1).pack(fill='x', padx=18)

    # Detail content area — re-rendered when selection changes
    app._pl_detail_frame = tk.Frame(pane, bg=COLORS['bg_1'])
    app._pl_detail_frame.pack(fill='both', expand=True, padx=18, pady=12)

    _render_detail_empty(app)
    return pane


def _render_detail_empty(app):
    """Empty state for the detail pane — shown when nothing selected."""
    for w in app._pl_detail_frame.winfo_children():
        w.destroy()
    inner = tk.Frame(app._pl_detail_frame, bg=COLORS['bg_1'])
    inner.pack(fill='both', expand=True, pady=40)
    tk.Label(inner, text='\U0001f4e5',
             font=(FONTS['body'][0], 32),
             bg=COLORS['bg_1'], fg=COLORS['fg_6']
             ).pack()
    tk.Label(inner, text=_('Select a payload to see details'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5'],
             wraplength=240, justify='center'
             ).pack(pady=(8, 0))


def _render_detail_for(app, idx):
    """Render the detail pane for the payload at index `idx` in the list."""
    if idx < 0 or idx >= len(app._pl_payloads):
        _render_detail_empty(app)
        return
    p = app._pl_payloads[idx]

    for w in app._pl_detail_frame.winfo_children():
        w.destroy()

    parent = app._pl_detail_frame

    # Title
    tk.Label(parent, text=p.get('name', '(unnamed)'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')

    # Subtitle — file size
    path = p.get('path', '')
    size_str = ''
    if path and os.path.isfile(path):
        try:
            sz = os.path.getsize(path)
            size_str = ('%.1f KB' % (sz / 1024) if sz < 1024**2
                        else '%.2f MB' % (sz / 1024**2))
        except Exception:
            pass
    tk.Label(parent,
             text=os.path.basename(path) + ((' \u00b7 ' + size_str)
                                             if size_str else ''),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(2, 12))

    # Notes / description
    notes = p.get('notes', '').strip()
    if notes:
        tk.Label(parent, text=notes,
                 font=FONTS['body'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_3'],
                 anchor='w', justify='left', wraplength=240
                 ).pack(fill='x', pady=(0, 12))

    # KV section
    tk.Frame(parent, bg=COLORS['border_2'], height=1
             ).pack(fill='x', pady=(4, 8))

    def _kv(key, value):
        row = tk.Frame(parent, bg=COLORS['bg_1'])
        row.pack(fill='x', pady=2)
        tk.Label(row, text=key.upper(),
                 font=(FONTS['eyebrow'][0], 8, 'bold'),
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(fill='x')
        tk.Label(row, text=value,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_2'],
                 anchor='w', justify='left', wraplength=240
                 ).pack(fill='x', pady=(2, 0))

    _kv(_('File'), path or '—')
    _kv(_('Per-payload IP'),
        p.get('ip', '') or _('uses connection bar IP'))
    _kv(_('Per-payload port'),
        str(p.get('port', '')) or _('uses connection bar port'))

    # Target consoles section
    tk.Frame(parent, bg=COLORS['border_2'], height=1
             ).pack(fill='x', pady=(8, 8))
    tk.Label(parent, text=_('TARGET'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(0, 4))

    target_ip = p.get('ip') or app._pl_ip_var.get()
    target_port = p.get('port') or app._pl_port_var.get()
    target_row = tk.Frame(parent, bg=COLORS['bg_1'])
    target_row.pack(fill='x', pady=(0, 12))
    dot = tk.Frame(target_row,
                   bg=COLORS['success'] if target_ip else COLORS['fg_5'],
                   width=7, height=7)
    dot.pack(side='left', padx=(0, 8))
    dot.pack_propagate(False)
    tk.Label(target_row, text=target_ip or _('(no IP set)'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_2']
             ).pack(side='left')
    tk.Label(target_row, text=':' + str(target_port),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(2, 0))

    # Send / Edit / Delete actions — fall through to legacy callbacks
    actions = tk.Frame(parent, bg=COLORS['bg_1'])
    actions.pack(fill='x')

    _accent_btn(actions, '\u2197  ' + _('Send to %s') % (target_ip or '...'),
                command=lambda: app._pl_send(p)
                ).pack(fill='x', pady=(0, 6))
    _ghost_btn(actions, '\u270e  ' + _('Edit'),
               command=lambda: app._pl_edit(idx)
               ).pack(side='left', fill='x', expand=True, padx=(0, 4))
    _danger_btn(actions, '\U0001f5d1  ' + _('Remove'),
                command=lambda: _pl_remove_then_refresh(app, idx)
                ).pack(side='left', fill='x', expand=True)

    # Warning banner (matches mock)
    tk.Frame(parent, bg=COLORS['border_2'], height=1
             ).pack(fill='x', pady=(12, 8))
    warn = tk.Frame(parent, bg=COLORS['warn_bg'],
                    highlightbackground=COLORS['warn'],
                    highlightthickness=1)
    warn.pack(fill='x')
    wi = tk.Frame(warn, bg=COLORS['warn_bg'])
    wi.pack(fill='x', padx=10, pady=6)
    tk.Label(wi, text='\u26a0',
             font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['warn_bg'], fg=COLORS['warn']
             ).pack(side='left', padx=(0, 6))
    tk.Label(wi,
             text=_('Sending a payload while another is resident can crash '
                    'the kernel. Reboot between unrelated payloads.'),
             font=FONTS['meta'],
             bg=COLORS['warn_bg'], fg=COLORS['warn_hi'],
             anchor='w', justify='left', wraplength=220
             ).pack(side='left', fill='x', expand=True)


def _pl_remove_then_refresh(app, idx):
    """Remove confirmation + refresh both panes."""
    from tkinter import messagebox
    if idx < 0 or idx >= len(app._pl_payloads):
        return
    name = app._pl_payloads[idx].get('name', '')
    if not messagebox.askyesno(_('Remove payload'),
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
                     padx=12, pady=8,
                     cursor='hand2',
                     command=command)


def _danger_btn(parent, text, command):
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9),
                     bg=COLORS['bg_2'], fg=COLORS['danger_hi'],
                     activebackground=COLORS['bg_3'],
                     activeforeground=COLORS['danger_hi'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     highlightbackground=COLORS['danger'],
                     highlightthickness=1,
                     command=command)
