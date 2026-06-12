"""
ui/tab_report.py — dedicated "Report Issue" support-center page.

Replaces the old Report Issue modal with a full page. Reuses the app's
existing diagnostic generator (app.create_bug_report_file) so report content
and redaction are unchanged. No auto-upload: it writes a local .txt, shows a
contents summary + recent-log preview, and offers the four support
destinations plus an "open report folder" action.

Entry point: build_report_tab(parent, app)
"""

import os
import sys
import webbrowser

import tkinter as tk

from tkinter_theme import COLORS, FONTS
from ui.shared.scroll import attach_scroll

GITHUB_ISSUES   = 'https://github.com/kerrdec97/ps5-exfat-builder/issues'
TELEGRAM_GROUP  = 'https://t.me/ps5exfatbuilder'
TELEGRAM_DIRECT = '@deckerr97'
DISCORD_HANDLE  = 'scottish_deckerr'


def _btn(parent, text, primary=False, command=None, accent=None):
    if primary:
        bg = accent or COLORS['accent']
        fg = '#ffffff'
        abg = COLORS.get('accent_pressed', bg)
    else:
        bg = COLORS['bg_4']
        fg = COLORS['fg_2']
        abg = COLORS['bg_5']
    return tk.Button(parent, text=text, command=command,
                     font=FONTS['button'], bg=bg, fg=fg,
                     activebackground=abg, activeforeground='#ffffff',
                     relief='flat', bd=0, padx=14, pady=8, cursor='hand2')


def _open(url):
    try:
        webbrowser.open(url)
    except Exception:
        pass


def build_report_tab(parent, app):
    """Build the Report Issue page into `parent`."""
    # Scrollable host (consistent with other tabs).
    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0, highlightthickness=0)
    sb = tk.Scrollbar(parent, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(canvas, bg=COLORS['bg_1'])
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>',
               lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>',
                lambda e: canvas.itemconfig(inner_id, width=e.width))
    attach_scroll(canvas)

    body = tk.Frame(inner, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=28, pady=22)

    # ── Header ──
    tk.Label(body, text='SUPPORT CENTER', font=FONTS['eyebrow'],
             bg=COLORS['bg_1'], fg=COLORS['accent_hi'], anchor='w').pack(
                 fill='x')
    tk.Label(body, text='Report an Issue', font=FONTS['h1'],
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w').pack(
                 fill='x', pady=(2, 4))
    tk.Label(body,
             text=('Generate a diagnostic report and send it through any of '
                   'the channels below. Passwords and host details are '
                   'redacted, and nothing is uploaded automatically \u2014 '
                   'you attach the report yourself.'),
             font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             anchor='w', justify='left', wraplength=760).pack(fill='x')

    # ── State ──
    state = {'path': None}
    status_var = tk.StringVar(value='No report generated yet.')

    tk.Frame(body, bg=COLORS['border_2'], height=1).pack(fill='x', pady=18)

    # ── Generate card ──
    gen_card = tk.Frame(body, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
    gen_card.pack(fill='x')
    gi = tk.Frame(gen_card, bg=COLORS['bg_2'])
    gi.pack(fill='x', padx=18, pady=16)
    tk.Label(gi, text='\U0001f4c4  Diagnostic report', font=FONTS['h3'],
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(fill='x')
    tk.Label(gi,
             text=('Includes: app version, timestamp, Windows version, '
                   'system info, free disk space, recent build summary, last '
                   'error/traceback, redacted settings, and the full output '
                   'log.'),
             font=FONTS['label'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=720).pack(
                 fill='x', pady=(4, 4))
    tk.Label(gi, textvariable=status_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w',
             justify='left', wraplength=720).pack(fill='x', pady=(4, 12))

    gbtns = tk.Frame(gi, bg=COLORS['bg_2'])
    gbtns.pack(fill='x')

    open_folder_btn = _btn(gbtns, 'Open report folder',
                           command=lambda: _open_report_folder(state['path']))
    copy_path_btn = _btn(gbtns, 'Copy path',
                         command=lambda: _copy(app, state['path']))

    # Recent log preview (read-only).
    prev_frame = tk.Frame(body, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    tk.Label(body, text='Recent log preview', font=FONTS['h3'],
             bg=COLORS['bg_1'], fg=COLORS['fg_1'], anchor='w').pack(
                 fill='x', pady=(20, 6))
    prev_frame.pack(fill='x')
    log_text = tk.Text(prev_frame, height=10, font=('Consolas', 9),
                       bg='#0a0a0f', fg='#a0c8a0', relief='flat', bd=8,
                       wrap='none', state='disabled')
    log_text.pack(fill='both', expand=True)

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
            return
        ok = False
        try:
            ok = bool(path) and os.path.isfile(path) \
                and os.path.getsize(path) > 0
        except Exception:
            ok = False
        if ok:
            state['path'] = path
            status_var.set('Report saved:  %s' % path)
            open_folder_btn.config(state='normal')
            copy_path_btn.config(state='normal')
        else:
            status_var.set('Could not write the report file. You can still '
                           'copy the output log manually from the log panel.')
        _refresh_preview()

    # Generate button (primary) — placed first, before the secondary actions.
    _btn(gi, '\u26a1  Generate Diagnostic Report', primary=True,
         command=_generate).pack(anchor='w', pady=(0, 10), before=gbtns)
    open_folder_btn.pack(side='left')
    copy_path_btn.pack(side='left', padx=8)
    open_folder_btn.config(state='disabled')
    copy_path_btn.config(state='disabled')

    # ── Destination cards (2x2 grid) ──
    tk.Label(body, text='Where to report', font=FONTS['h3'],
             bg=COLORS['bg_1'], fg=COLORS['fg_1'], anchor='w').pack(
                 fill='x', pady=(22, 2))
    tk.Label(body, text='Pick a channel, then attach the report file.',
             font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w').pack(fill='x', pady=(0, 10))

    grid = tk.Frame(body, bg=COLORS['bg_1'])
    grid.pack(fill='x')
    grid.columnconfigure(0, weight=1, uniform='rc')
    grid.columnconfigure(1, weight=1, uniform='rc')

    def _card(col, row, icon, title, desc, detail, action_label, action,
              accent_key='accent'):
        card = tk.Frame(grid, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        card.grid(row=row, column=col, sticky='ew', padx=6, pady=6)
        ci = tk.Frame(card, bg=COLORS['bg_2'])
        ci.pack(fill='both', expand=True, padx=16, pady=14)
        tk.Label(ci, text='%s  %s' % (icon, title), font=FONTS['body_b'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(
                     fill='x')
        tk.Label(ci, text=desc, font=FONTS['meta'], bg=COLORS['bg_2'],
                 fg=COLORS['fg_4'], anchor='w', justify='left',
                 wraplength=320).pack(fill='x', pady=(3, 2))
        tk.Label(ci, text=detail, font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                 fg=COLORS['fg_3'], anchor='w').pack(fill='x', pady=(0, 10))
        _btn(ci, action_label, command=action,
             accent=COLORS.get(accent_key)).pack(anchor='w')

    _card(0, 0, '\U0001f41e', 'GitHub Issues',
          'Best for bugs, crashes and feature requests. Tracked + '
          'searchable history.',
          GITHUB_ISSUES, 'Open GitHub Issues',
          lambda: _open(GITHUB_ISSUES))
    _card(1, 0, '\U0001f465', 'Telegram Group',
          'Best for community help and testing feedback.',
          't.me/ps5exfatbuilder', 'Open Telegram Group',
          lambda: _open(TELEGRAM_GROUP), accent_key='teal')
    _card(0, 1, '\U0001f4e2', 'Telegram Direct',
          'Best for direct contact with Deckerr97.',
          'Username: ' + TELEGRAM_DIRECT, 'Copy Telegram Username',
          lambda: _copy(app, TELEGRAM_DIRECT), accent_key='teal')
    _card(1, 1, '\U0001f4ac', 'Discord',
          'Best for quick support and troubleshooting.',
          'Username: ' + DISCORD_HANDLE, 'Copy Discord Username',
          lambda: _copy(app, DISCORD_HANDLE))

    # Initial preview fill.
    _refresh_preview()


def _copy(app, text):
    if not text:
        return
    try:
        app.clipboard_clear()
        app.clipboard_append(text)
        app.update()
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
