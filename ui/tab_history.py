"""
ui/tab_history.py — Build History tab.

Step 10 (v2.1.2): refactored against preview/history-tab-redesign.html.

Layout:

    ┌─ page-head ─────────────────────────────────────────────────────┐
    │  Build History  [since 2025-08-11]  [↻ Refresh][⤓CSV][🗑Clear]   │
    ├─ stats grid (4 cards) ──────────────────────────────────────────┤
    │ [Total: 142]  [Success: 94%]  [Failed: 9]  [Total built: 8.4 TB]│
    ├─ filters row ───────────────────────────────────────────────────┤
    │ 🔍 Search…  [All][✓ Success][✗ Failed]    [Type ▼] [Range ▼]    │
    ├─ table (sticky header) ─────────────────────────────────────────┤
    │ Game  | Type  | Output           | Size    | Dur  | When | Status│
    │ ────────────────────────────────────────────────────────────────│
    │ Returnal  EXFAT  D:/...  56.8GB  04:12  12m  ●SUCCESS  📂↻✕    │
    │ ...                                                              │
    └─────────────────────────────────────────────────────────────────┘

Backwards compat: every `app._hist_*` and history widget the existing
callbacks read is preserved. Worker logic (read JSON, filter, save)
untouched. The `_history_refresh(list_frame)` callback is rewritten to
build rows in the new design — UI-only rewrite, same scope-bend pattern
as `_lib_make_card`.
"""

import os
import json
import time
import datetime
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _
from ui.shared.scroll import attach_scroll


def build_history_tab(parent, app):
    """Build the redesigned History tab body into `parent`."""
    parent.configure(bg=COLORS['bg_1'])

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    # ── Page head ──
    _build_page_head(body, app)

    # ── Stats grid ──
    _build_stats(body, app)

    # ── Filters row ──
    _build_filters(body, app)

    # ── Table (header + scrollable body) ──
    list_frame = _build_table(body, app)

    # Initial render — defer slightly so the canvas has a real width
    app.after(100, lambda: app._history_refresh(list_frame))


# ─────────────────────────────────────────────────────────────────────────
# Page head — title + since pill + Refresh / Export / Clear buttons
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 12))

    tk.Label(head, text=_('Build History'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    # "since YYYY-MM-DD" pill — derived from oldest entry, if any
    since_text = _compute_since_text(app)
    tk.Label(head, text=since_text,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             padx=8, pady=2,
             highlightbackground=COLORS['border_2'],
             highlightthickness=1
             ).pack(side='left', padx=(12, 0))

    # Right-aligned action buttons
    _danger_btn(head, '\U0001f5d1  ' + _('Clear history'),
                command=app._history_clear).pack(side='right')
    _ghost_btn(head, '\u2913  ' + _('Export CSV'),
               command=lambda: _export_csv(app)
               ).pack(side='right', padx=(0, 6))
    _ghost_btn(head, '\u21bb  ' + _('Refresh'),
               command=lambda: app._history_refresh(app._hist_list_frame)
               ).pack(side='right', padx=(0, 6))


def _compute_since_text(app):
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
        if not history:
            return _('no entries yet')
        oldest = history[-1].get('time', '')
        # Pull just the date portion if format is YYYY-MM-DD HH:MM:SS
        date_part = oldest.split(' ')[0] if oldest else ''
        return _('since %s') % date_part if date_part else _('no date')
    except Exception:
        return _('no entries yet')


# ─────────────────────────────────────────────────────────────────────────
# Stats grid — 4 cards
# ─────────────────────────────────────────────────────────────────────────
def _build_stats(body, app):
    """4-column stat grid: Total / Success rate / Failures / Total built."""
    stats = _compute_stats(app)

    grid = tk.Frame(body, bg=COLORS['bg_1'])
    grid.pack(fill='x', padx=24, pady=(0, 14))
    for i in range(4):
        grid.grid_columnconfigure(i, weight=1, uniform='stat')

    _stat_card(grid, 0, _('Total builds'), str(stats['total']),
               sub=_('last: %s') % stats['last_when'],
               value_color=COLORS['fg_0'])
    _stat_card(grid, 1, _('Success rate'),
               '%d%%' % stats['success_pct'] if stats['total'] else '—',
               sub='%d / %d %s' % (stats['success_count'], stats['total'],
                                    _('succeeded')) if stats['total'] else '',
               value_color=COLORS['success_hi'])
    _stat_card(grid, 2, _('Failures'),
               str(stats['failed_count']),
               sub=_('out of %d builds') % stats['total']
                   if stats['total'] else _('no builds yet'),
               value_color=COLORS['danger_hi'] if stats['failed_count']
                                              else COLORS['fg_4'])
    _stat_card(grid, 3, _('Total built'),
               stats['total_size_str'],
               sub='%d %s' % (stats['outputs_on_disk'],
                              _('outputs on disk')),
               value_color=COLORS['accent_hi'])


def _stat_card(grid, col, label, value, sub='', value_color=None):
    """Single stat card — label + big value + sub text."""
    card = tk.Frame(grid, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.grid(row=0, column=col, sticky='nsew', padx=(0, 8) if col < 3 else 0)
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='both', expand=True, padx=14, pady=12)

    tk.Label(inner, text=label.upper(),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x')
    tk.Label(inner, text=value,
             font=(FONTS['mono'][0], 22, 'bold'),
             bg=COLORS['bg_2'],
             fg=value_color or COLORS['fg_0'],
             anchor='w'
             ).pack(fill='x', pady=(4, 2))
    if sub:
        tk.Label(inner, text=sub,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'],
                 anchor='w'
                 ).pack(fill='x')


def _compute_stats(app):
    """Aggregate stats from the history JSON."""
    out = {'total': 0, 'success_count': 0, 'failed_count': 0,
           'success_pct': 0, 'last_when': '—', 'total_size_str': '—',
           'outputs_on_disk': 0}
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
    except Exception:
        history = []

    out['total'] = len(history)
    if not history:
        return out

    out['success_count'] = sum(1 for h in history if h.get('success', True))
    out['failed_count']  = out['total'] - out['success_count']
    out['success_pct']   = int(out['success_count'] / out['total'] * 100)
    out['last_when']     = _humanize_when(history[0].get('time', ''))

    total_bytes = 0
    on_disk = 0
    for h in history:
        op = h.get('output', '')
        if op and os.path.isfile(op):
            try:
                total_bytes += os.path.getsize(op)
                on_disk += 1
            except Exception:
                pass
    out['outputs_on_disk'] = on_disk

    if total_bytes >= 1024**4:
        out['total_size_str'] = '%.1f TB' % (total_bytes / 1024**4)
    elif total_bytes >= 1024**3:
        out['total_size_str'] = '%.0f GB' % (total_bytes / 1024**3)
    elif total_bytes > 0:
        out['total_size_str'] = '%d MB' % (total_bytes // 1024**2)
    return out


# ─────────────────────────────────────────────────────────────────────────
# Filters row — search + chip filters + dropdowns
# ─────────────────────────────────────────────────────────────────────────
def _build_filters(body, app):
    app._hist_search_var = tk.StringVar()
    app._hist_show_success = tk.BooleanVar(value=True)
    app._hist_show_failed  = tk.BooleanVar(value=True)

    row = tk.Frame(body, bg=COLORS['bg_1'])
    row.pack(fill='x', padx=24, pady=(0, 12))

    # Search input
    search_wrap = tk.Frame(row, bg=COLORS['bg_0'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(side='left', fill='x', expand=False)

    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(search_wrap, textvariable=app._hist_search_var,
             font=FONTS['body'],
             bg=COLORS['bg_0'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0,
             width=36
             ).pack(side='left', ipady=5, padx=(0, 8))

    # Trigger filter on every keystroke
    app._hist_search_var.trace_add('write',
        lambda *a: app._history_refresh(app._hist_list_frame))

    # Chip filters — All / Successful / Failed
    chips_frame = tk.Frame(row, bg=COLORS['bg_1'])
    chips_frame.pack(side='left', padx=(10, 0))
    _chip(chips_frame, _('All'), kind='all',
          on_click=lambda: (app._hist_show_success.set(True),
                            app._hist_show_failed.set(True),
                            app._history_refresh(app._hist_list_frame))
          ).pack(side='left', padx=2)
    _chip(chips_frame, '\u25cf  ' + _('Successful'), kind='ok',
          on_click=lambda: (
              app._hist_show_success.set(not app._hist_show_success.get()),
              app._history_refresh(app._hist_list_frame))
          ).pack(side='left', padx=2)
    _chip(chips_frame, '\u25cf  ' + _('Failed'), kind='fail',
          on_click=lambda: (
              app._hist_show_failed.set(not app._hist_show_failed.get()),
              app._history_refresh(app._hist_list_frame))
          ).pack(side='left', padx=2)


def _chip(parent, text, kind='all', on_click=None):
    """A small clickable chip pill with an optional colored dot."""
    bg = COLORS['bg_3']
    if kind == 'ok':
        fg = COLORS['success_hi']
    elif kind == 'fail':
        fg = COLORS['danger_hi']
    else:
        fg = COLORS['fg_2']
    chip = tk.Label(parent, text=text,
                    font=FONTS['meta'],
                    bg=bg, fg=fg,
                    padx=10, pady=4,
                    cursor='hand2',
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    if on_click:
        chip.bind('<Button-1>', lambda e: on_click())
    return chip


# ─────────────────────────────────────────────────────────────────────────
# Table — sticky header + scrollable body
# ─────────────────────────────────────────────────────────────────────────
# Column widths (proportional). Order matters — must match _history_refresh.
_COLS = [
    ('game',     _('Game'),     None),     # flex
    ('type',     _('Type'),     90),
    ('output',   _('Output'),   None),     # flex
    ('size',     _('Size'),     90),       # right-aligned
    ('duration', _('Duration'), 80),       # right-aligned
    ('when',     _('When'),     130),
    ('status',   _('Status'),   120),
    ('actions',  '',            90),
]


def _build_table(body, app):
    """Build the table header + scrollable list, return the list_frame
    that `_history_refresh` populates."""
    # Wrapper with hairline border
    wrap = tk.Frame(body, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=24, pady=(0, 14))

    # Header row (sticky-style — pinned above the scroll area)
    head = tk.Frame(wrap, bg=COLORS['bg_3'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    head.pack(fill='x')

    head_inner = tk.Frame(head, bg=COLORS['bg_3'])
    head_inner.pack(fill='x', padx=14, pady=10)

    # Configure grid columns to match the rows below
    for i, (key, label, width) in enumerate(_COLS):
        if width is None:
            head_inner.grid_columnconfigure(i, weight=1, uniform='col')
        else:
            head_inner.grid_columnconfigure(i, weight=0, minsize=width)

    for i, (key, label, width) in enumerate(_COLS):
        anchor = 'e' if key in ('size', 'duration') else 'w'
        tk.Label(head_inner, text=label.upper(),
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

    canvas = tk.Canvas(outer, bg=COLORS['bg_2'], highlightthickness=0)
    sb = tk.Scrollbar(outer, orient='vertical', command=canvas.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    list_frame = tk.Frame(canvas, bg=COLORS['bg_2'])
    win = canvas.create_window((0, 0), window=list_frame, anchor='nw')
    list_frame.bind('<Configure>',
        lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>',
        lambda e: canvas.itemconfig(win, width=e.width))
    attach_scroll(canvas)

    # Stash on app so refresh callback can find it
    app._hist_list_frame = list_frame
    return list_frame


# ─────────────────────────────────────────────────────────────────────────
# Helpers for the row builder (called by main file's _history_refresh)
# These are exposed so the rewritten callback can use them directly.
# ─────────────────────────────────────────────────────────────────────────
def _humanize_when(time_str):
    """Convert 'YYYY-MM-DD HH:MM:SS' to '12m ago' / '2h ago' / 'Yesterday 22:14' / '3d ago'."""
    if not time_str:
        return '—'
    try:
        dt = datetime.datetime.strptime(time_str, '%Y-%m-%d %H:%M:%S')
    except Exception:
        return time_str
    now = datetime.datetime.now()
    delta = now - dt
    secs = delta.total_seconds()
    if secs < 60:
        return _('just now')
    if secs < 3600:
        return '%dm %s' % (int(secs // 60), _('ago'))
    if secs < 86400 and dt.date() == now.date():
        return '%dh %s' % (int(secs // 3600), _('ago'))
    yesterday = now.date() - datetime.timedelta(days=1)
    if dt.date() == yesterday:
        return _('Yesterday %s') % dt.strftime('%H:%M')
    days = int(secs // 86400)
    if days < 7:
        return '%dd %s' % (days, _('ago'))
    return dt.strftime('%Y-%m-%d')


def _detect_kind(entry):
    """Determine build type from output filename / folder.
    Returns one of: 'exfat', 'ffpkg', 'backport', 'unknown'."""
    out = (entry.get('output', '') or '').lower()
    folder = (entry.get('folder', '') or '').lower()
    if out.endswith('.ffpkg'):
        return 'ffpkg'
    if out.endswith('.exfat'):
        # Distinguish backport from regular exFAT by folder hint
        if 'backport' in out or 'backport' in folder:
            return 'backport'
        return 'exfat'
    return 'unknown'


def _export_csv(app):
    """Export history to CSV via file dialog."""
    from tkinter import filedialog, messagebox
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
    except Exception:
        history = []
    if not history:
        messagebox.showinfo(_('Export CSV'),
                            _('No history to export.'))
        return
    path = filedialog.asksaveasfilename(
        defaultextension='.csv',
        filetypes=[('CSV files', '*.csv')],
        initialfile='build_history.csv')
    if not path:
        return
    try:
        import csv
        with open(path, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['Time', 'Title', 'Title ID', 'Version',
                        'Type', 'Success', 'Output', 'Folder'])
            for h in history:
                w.writerow([h.get('time', ''),
                            h.get('title', ''),
                            h.get('title_id', ''),
                            h.get('version', ''),
                            _detect_kind(h),
                            'yes' if h.get('success', True) else 'no',
                            h.get('output', ''),
                            h.get('folder', '')])
        messagebox.showinfo(_('Export CSV'),
                            _('Saved %d entries to %s') %
                            (len(history), os.path.basename(path)))
    except Exception as e:
        messagebox.showerror(_('Export CSV'), str(e))


# ─────────────────────────────────────────────────────────────────────────
# Helpers for buttons
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


def _icon_btn(parent, glyph, tooltip='', command=None, accent=False, danger=False):
    """Small square icon button — used in the table row Actions column."""
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
