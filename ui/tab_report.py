"""
ui/tab_report.py — "Report Issue" support page.

    PageHeader "Report an issue"
    ┌ Diagnostic report ───────────────┐ ┌ Recent log preview (inset well) ─┐
    │ what it contains · status        │ │ last 200 lines of the output log │
    │ [Generate] [Open folder] [Copy]  │ │                                  │
    └──────────────────────────────────┘ └──────────────────────────────────┘
    Where to report
    [GitHub Issues] [Telegram Group]
    [Telegram Direct] [Discord]                 (channel cards)

Reuses the app's existing diagnostic generator (app.create_bug_report_file)
so report content and redaction are unchanged. No auto-upload: it writes a
local .txt, shows its path + a recent-log preview, and offers the four
support destinations plus "open report folder" / "copy path".

Entry point: build_report_tab(parent, app)
"""

import os
import sys
import webbrowser

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import (
    PageHeader, Card, Badge, Button, IconLabel, Rounded, SectionTitle,
    autowrap,
)

from tkinter_theme import COLORS, FONTS, px
from ui.shared.scroll import attach_scroll

GITHUB_ISSUES   = 'https://github.com/kerrdec97/ps5-exfat-builder/issues'
TELEGRAM_GROUP  = 'https://t.me/ps5exfatbuilder'
TELEGRAM_DIRECT = '@deckerr97'
DISCORD_HANDLE  = 'scottish_deckerr'

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']


def _btn(parent, text, primary=False, command=None, accent=None, icon=None):
    """Design-system button (primary or secondary)."""
    return Button(parent, text=text, command=command, icon=icon,
                  variant='primary' if primary else 'secondary')


def _open(url):
    try:
        webbrowser.open(url)
    except Exception:
        pass


def build_report_tab(parent, app):
    """Build the Report Issue page into `parent`."""
    parent.configure(bg=BG1)
    # Scrollable host (consistent with other tabs).
    canvas = tk.Canvas(parent, bg=BG1, bd=0, highlightthickness=0)
    sb = XScrollbar(parent, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(canvas, bg=BG1)
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>',
               lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>',
                lambda e: canvas.itemconfig(inner_id, width=e.width))
    attach_scroll(canvas)

    # ── Header ──
    head = PageHeader(
        inner, 'Report an issue',
        'Generate a diagnostic report and send it through any of the '
        'channels below. Passwords and host details are redacted, and '
        'nothing is uploaded automatically — you attach the report '
        'yourself.')
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    # ── State ──
    state = {'path': None}
    status_var = tk.StringVar(value='No report generated yet.')

    # ── Row 1: diagnostic report + log preview ──
    row1 = tk.Frame(inner, bg=BG1)
    row1.pack(fill='x', padx=px(24))
    row1.grid_columnconfigure(0, weight=1, uniform='rep')
    row1.grid_columnconfigure(1, weight=1, uniform='rep')

    gen_card = Card(row1, title='Diagnostic report',
                    subtitle='A plain-text file you attach to your report.')
    gen_card.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))
    gi = gen_card.body

    tk.Label(gi, text='INCLUDES', font=FONTS['overline'], bg=BG2,
             fg=C['fg_4'], anchor='w').pack(fill='x')
    inc = tk.Frame(gi, bg=BG2)
    inc.pack(fill='x', pady=(px(6), 0))
    inc.grid_columnconfigure(0, weight=1, uniform='inc')
    inc.grid_columnconfigure(1, weight=1, uniform='inc')
    items = ('App version and timestamp', 'Windows version and system info',
             'Free disk space', 'Recent build summary',
             'Last error or traceback', 'Redacted settings',
             'Full output log')
    for i, it in enumerate(items):
        cell = tk.Frame(inc, bg=BG2)
        cell.grid(row=i // 2, column=i % 2, sticky='ew', pady=px(3))
        IconLabel(cell, 'check', 12, C['fg_4'], BG2).pack(
            side='left', padx=(0, px(8)))
        tk.Label(cell, text=it, font=FONTS['body'], bg=BG2, fg=C['fg_2'],
                 anchor='w').pack(side='left')

    # Status well
    well = Rounded(gi, bg=C['bg_0'], border=C['border_2'], radius=4,
                   padding=(px(12), px(10)), surface=BG2)
    well.pack(fill='x', pady=(px(16), px(16)))
    well_top = tk.Frame(well, bg=C['bg_0'])
    well_top.pack(fill='x')
    status_badge = Badge(well_top, 'Not generated', 'neutral',
                         surface=C['bg_0'])
    status_badge.pack(side='left')
    status_lbl = tk.Label(well, textvariable=status_var, font=FONTS['caption'],
                          bg=C['bg_0'], fg=C['fg_3'], anchor='w',
                          justify='left')
    status_lbl.pack(fill='x', pady=(px(8), 0))
    autowrap(status_lbl)

    copy_path_btn = Button(well_top, text='Copy path', icon='copy',
                           size='sm', surface=C['bg_0'],
                           command=lambda: _copy(app, state['path'],
                                                 copy_path_btn))
    copy_path_btn.pack(side='right')
    open_folder_btn = Button(well_top, text='Open report folder',
                             icon='folder_open', size='sm', surface=C['bg_0'],
                             command=lambda: _open_report_folder(state['path']))
    open_folder_btn.pack(side='right', padx=(0, px(6)))

    gbtns = tk.Frame(gi, bg=BG2)
    gbtns.pack(fill='x', side='bottom')

    # Recent log preview (read-only), in an inset well.
    prev_card = Card(row1, title='Recent log preview',
                     subtitle='The last 200 lines of the output log, '
                              'refreshed when you generate a report.')
    prev_card.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))
    prev_frame = Rounded(prev_card.body, bg=C['bg_0'], border=C['border_2'],
                         radius=4, padding=px(2), surface=BG2)
    prev_frame.pack(fill='both', expand=True)
    log_text = tk.Text(prev_frame, height=14, font=FONTS['code'],
                       bg=C['bg_0'], fg=C['fg_2'], relief='flat', bd=0,
                       padx=px(10), pady=px(8), wrap='none',
                       highlightthickness=0, state='disabled',
                       insertbackground=C['fg_2'],
                       selectbackground=C['accent_lo'])
    log_sb = XScrollbar(prev_frame, orient='vertical', command=log_text.yview,
                        surface=C['bg_0'])
    log_text.configure(yscrollcommand=log_sb.set)
    log_sb.pack(side='right', fill='y')
    log_text.pack(side='left', fill='both', expand=True)

    def _refresh_preview():
        try:
            box = getattr(app, 'log_box', None)
            txt = box.get('1.0', 'end-1c') if box else ''
            tail = '\n'.join(txt.splitlines()[-200:]) or '(output log is empty)'
        except Exception:
            tail = '(output log unavailable)'
        try:
            log_text.config(state='normal')
            log_text.delete('1.0', 'end')
            log_text.insert('1.0', tail)
            log_text.see('end')
            log_text.config(state='disabled')
        except Exception:
            pass

    def _generate():
        try:
            info = app.create_bug_report_file()
            path = info.get('path')
        except Exception as e:
            status_var.set('Could not create report: %s' % e)
            status_badge.set('Failed', 'danger')
            return
        ok = False
        try:
            ok = bool(path) and os.path.isfile(path) \
                and os.path.getsize(path) > 0
        except Exception:
            ok = False
        if ok:
            state['path'] = path
            status_var.set(path)
            status_lbl.config(font=FONTS['code'], fg=C['fg_1'])
            status_badge.set('Saved', 'success')
            open_folder_btn.config(state='normal')
            copy_path_btn.config(state='normal')
        else:
            status_var.set('Could not write the report file. You can still '
                           'copy the output log manually from the log panel.')
            status_badge.set('Failed', 'danger')
        _refresh_preview()

    # Generate button (primary) first, then the secondary actions.
    _btn(gbtns, 'Generate diagnostic report', primary=True, icon='zap',
         command=_generate).pack(side='left')
    open_folder_btn.config(state='disabled')
    copy_path_btn.config(state='disabled')

    # ── Destination cards (2x2 grid) ──
    sect = SectionTitle(inner, 'Where to report',
                        caption='Pick a channel, then attach the report file.')
    sect.pack(fill='x', padx=px(24), pady=(px(24), px(12)))

    grid = tk.Frame(inner, bg=BG1)
    grid.pack(fill='x', padx=px(24), pady=(0, px(24)))
    grid.columnconfigure(0, weight=1, uniform='rc')
    grid.columnconfigure(1, weight=1, uniform='rc')

    def _card(col, row, icon, title, desc, detail, action_label, action,
              btn_icon):
        card = Card(grid, padding=16)
        card.grid(row=row, column=col, sticky='nsew',
                  padx=(0 if col == 0 else px(8), px(8) if col == 0 else 0),
                  pady=(0 if row == 0 else px(8), px(8) if row == 0 else 0))
        top = tk.Frame(card, bg=BG2)
        top.pack(fill='x')
        tile = Rounded(top, bg=BG3, radius=8, surface=BG2)
        tile.pack(side='left', anchor='n', padx=(0, px(14)))
        IconLabel(tile, icon, 16, C['fg_2'], BG3).pack(padx=px(10),
                                                       pady=px(10))
        col_f = tk.Frame(top, bg=BG2)
        col_f.pack(side='left', fill='x', expand=True)
        tk.Label(col_f, text=title, font=FONTS['subtitle'], bg=BG2,
                 fg=C['fg_0'], anchor='w').pack(fill='x')
        d = tk.Label(col_f, text=desc, font=FONTS['body'], bg=BG2,
                     fg=C['fg_2'], anchor='w', justify='left')
        d.pack(fill='x', pady=(px(2), 0))
        autowrap(d)
        tk.Label(col_f, text=detail, font=FONTS['code'], bg=BG2,
                 fg=C['fg_3'], anchor='w').pack(fill='x', pady=(px(6), 0))
        b = Button(col_f, text=action_label, icon=btn_icon, surface=BG2)
        b.configure(command=lambda: action(b))
        b.pack(anchor='w', pady=(px(12), 0))

    _card(0, 0, 'github', 'GitHub Issues',
          'Best for bugs, crashes and feature requests. Tracked + '
          'searchable history.',
          GITHUB_ISSUES, 'Open GitHub Issues',
          lambda b: _open(GITHUB_ISSUES), 'external')
    _card(1, 0, 'users', 'Telegram Group',
          'Best for community help and testing feedback.',
          't.me/ps5exfatbuilder', 'Open Telegram Group',
          lambda b: _open(TELEGRAM_GROUP), 'external')
    _card(0, 1, 'send', 'Telegram Direct',
          'Best for direct contact with Deckerr97.',
          'Username: ' + TELEGRAM_DIRECT, 'Copy Telegram Username',
          lambda b: _copy(app, TELEGRAM_DIRECT, b), 'copy')
    _card(1, 1, 'message', 'Discord',
          'Best for quick support and troubleshooting.',
          'Username: ' + DISCORD_HANDLE, 'Copy Discord Username',
          lambda b: _copy(app, DISCORD_HANDLE, b), 'copy')

    # Initial preview fill.
    _refresh_preview()


def _copy(app, text, button=None):
    if not text:
        return
    try:
        app.clipboard_clear()
        app.clipboard_append(text)
        app.update()
    except Exception:
        pass
    # Brief confirmation on the button that was pressed.
    if button is not None:
        try:
            old = button.cget('text')
            if old == 'Copied':
                return
            button.config(text='Copied', icon='check')
            button.after(1500, lambda: button.winfo_exists() and
                         button.config(text=old, icon='copy'))
        except Exception:
            pass


def _open_report_folder(path):
    try:
        if not path:
            return
        if sys.platform.startswith('win'):
            import subprocess
            subprocess.Popen(['explorer', '/select,', path])
        else:
            folder = os.path.dirname(path)
            import subprocess
            opener = 'open' if sys.platform == 'darwin' else 'xdg-open'
            subprocess.Popen([opener, folder])
    except Exception:
        pass
