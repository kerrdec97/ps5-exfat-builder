"""
ui/tab_backports.py — Backports tab (v3: sub-tabs Games / Auto / Results).

Structure:

    ┌─ Sub-tab strip ───────────────────────────────────────┐
    │ [🎮 Games]  [⚙ Auto Backport]  [📋 Results]            │
    ├───────────────────────────────────────────────────────┤
    │                                                       │
    │   Active sub-tab content here                         │
    │                                                       │
    └───────────────────────────────────────────────────────┘

Sub-tab 1 — Games (DEFAULT)
    Library-style cover-art grid. Multi-folder support, search box,
    FW filter pills. Click a cover to toggle the game in/out of the
    backport queue. The "Process queue (N)" button auto-switches to
    the Auto Backport tab and runs every queued game sequentially.

Sub-tab 2 — Auto Backport
    The full options form (output folder, fakelib path, target SDK,
    advanced toggles, etc.) — built by `_build_bp_auto_panel`.
    Live build details show in the global OUTPUT LOG drawer at the
    bottom of the window (click it to expand).

Sub-tab 3 — Results
    ✅ worked / ❌ didn't work tracking — built by `_build_bpr_panel`.

Backwards compat: every `_bp_*` attribute the rest of exfat_builder.py
expects is constructed (form pane, results pane, switch helpers, scan
vars, etc.). Stub `_bp_rt_form_btn` / `_bp_rt_results_btn` labels
absorb config() calls from the legacy `_bp_switch_right` helper.
"""

import os
import threading
import tkinter as tk

from tkinter_theme import COLORS, FONTS
from ui.shared.cards import GameCard, build_subtabs

# Star-import for legacy theme constants used by inner panel builders
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, _load_cover_image
from ui.shared.scroll import attach_scroll


# ─────────────────────────────────────────────────────────────────────────
# Filter pills — keys map to predicates over a game's `system_ver` field
# ─────────────────────────────────────────────────────────────────────────
_FILTER_PILLS = [
    ('all',   _('All'),     lambda sdk: True),
    ('le4',   _('FW 4'),    lambda sdk: sdk and sdk <= 4),
    ('le7',   _('FW 5-7'),  lambda sdk: sdk and 5 <= sdk <= 7),
    ('9plus', _('FW 9+'),   lambda sdk: sdk and sdk >= 9),
]
_FILTER_PRED = {key: pred for key, _label, pred in _FILTER_PILLS}


# ─────────────────────────────────────────────────────────────────────────
# Public entry point
# ─────────────────────────────────────────────────────────────────────────
# Natural width of one game card with a fixed 2:3 cover and fixed info
# column: 120px cover + 252px info + paddings ~= 411px, plus grid pad.
_CARD_MIN_W = 425


def build_backports_tab(parent, app):
    """Build the v3 Backports tab body (Games / Auto / Results sub-tabs)."""
    parent.configure(bg=COLORS['bg_1'])

    # ── Persistent state on `app` ──
    if not hasattr(app, '_bp_mode_var'):
        app._bp_mode_var = tk.StringVar(value='auto')
    if not hasattr(app, '_bp_filter_var'):
        app._bp_filter_var = tk.StringVar(value='all')
    if not hasattr(app, '_bp_active_band_var'):
        app._bp_active_band_var = tk.StringVar(value='4.xx')
    if not hasattr(app, '_bp_search_var'):
        app._bp_search_var = tk.StringVar()
    if not hasattr(app, '_bp_queue'):
        app._bp_queue = []
    if not hasattr(app, '_bp_folders'):
        app._bp_folders = list(app._settings.get('bp_folders', []) or [])
        legacy = (app._settings.get('bp_scan_folder', '') or '').strip()
        if legacy and legacy not in app._bp_folders:
            app._bp_folders.insert(0, legacy)

    # Re-render the games grid when filter or search changes
    if not getattr(app, '_bp_traces_wired', False):
        app._bp_filter_var.trace_add('write',
            lambda *a: _refresh_grid(app))
        app._bp_search_var.trace_add('write',
            lambda *a: _refresh_grid(app))
        app._bp_traces_wired = True

    # ── Sub-tabs ──
    items = [
        ('games',   '\U0001f3ae  ' + _('Games'),
            lambda f: _build_games_subtab(f, app)),
        ('auto',    '\u2699  ' + _('Auto Backport'),
            lambda f: _build_auto_subtab(f, app)),
        ('results', '\U0001f4cb  ' + _('Results'),
            lambda f: _build_results_subtab(f, app)),
    ]
    app._bp_subtab_activate = build_subtabs(parent, items, default='games')

    # Stub the legacy sub-tab button refs that `_bp_switch_right` paints.
    if not hasattr(app, '_bp_rt_form_btn') or \
            not _widget_alive(app._bp_rt_form_btn):
        app._bp_rt_form_btn = tk.Label(parent)
    if not hasattr(app, '_bp_rt_results_btn') or \
            not _widget_alive(app._bp_rt_results_btn):
        app._bp_rt_results_btn = tk.Label(parent)
    if not hasattr(app, '_bp_right_tab_var'):
        app._bp_right_tab_var = tk.StringVar(value='form')

    # Redirect the legacy `_bp_switch_right(key)` helper so calls from
    # `_abp_run` etc. drive the new sub-tabs instead of the removed pane.
    def _redirect_switch_right(key):
        try:
            app._bp_right_tab_var.set(key)
        except Exception:
            pass
        if key == 'results':
            try:
                app._bp_subtab_activate('results')
                if app._bpr_list_frame is not None:
                    app._bpr_render()
            except Exception:
                pass
        else:
            try:
                app._bp_subtab_activate('auto')
            except Exception:
                pass
    app._bp_switch_right = _redirect_switch_right

    # If folders are saved, auto-scan on tab construction
    if app._bp_folders:
        app.after(300, lambda: _scan_all_folders(app))


def _widget_alive(w):
    try:
        return bool(w.winfo_exists())
    except Exception:
        return False


# ─────────────────────────────────────────────────────────────────────────
# SUB-TAB 1: GAMES — grid + queue
# ─────────────────────────────────────────────────────────────────────────
def _build_games_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    _build_games_head(body, app)
    _build_games_toolbar(body, app)
    _build_games_folder_strip(body, app)
    _build_games_scan_bar(body, app)
    _build_games_grid(body, app)
    _build_games_stats(body, app)


# ── Page head: title + count + action buttons ──
def _build_games_head(body, app):
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 12))

    tk.Label(head, text='\u26a1  ' + _('Backports'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    app._bp_count_var = tk.StringVar(value=_('0 games'))
    count_pill = tk.Label(head, textvariable=app._bp_count_var,
                          font=FONTS['mono_sm'],
                          bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                          padx=8, pady=2,
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    count_pill.pack(side='left', padx=(12, 0))

    # Right-aligned actions
    app._bp_process_btn = _btn(head, _process_label(app),
                                command=lambda: _process_queue(app),
                                kind='purple')
    app._bp_process_btn.pack(side='right')
    _update_process_btn(app)

    _btn(head, '\u21bb  ' + _('Scan'),
         command=lambda: _scan_all_folders(app),
         kind='primary'
         ).pack(side='right', padx=(0, 6))

    _btn(head, '+  ' + _('Add folder'),
         command=lambda: _add_folder(app),
         kind='ghost'
         ).pack(side='right', padx=(0, 6))


def _process_label(app):
    n = len(app._bp_queue)
    if n == 0:
        return '\u26a1  ' + _('Process queue')
    return '\u26a1  ' + _('Process queue (%d)') % n


# ── Toolbar: search + filter pills + status ──
def _build_games_toolbar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_0'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    bar.pack(fill='x')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=10)

    # Search
    sw = tk.Frame(inner, bg=COLORS['bg_0'],
                  highlightbackground=COLORS['border_3'],
                  highlightthickness=1)
    sw.pack(side='left')
    tk.Label(sw, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(sw, textvariable=app._bp_search_var,
             font=FONTS['body'],
             bg=COLORS['bg_0'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0, width=32
             ).pack(side='left', ipady=5, padx=(0, 8))

    # Filter pills
    pills = tk.Frame(inner, bg=COLORS['bg_0'])
    pills.pack(side='left', padx=(10, 0))

    app._bp_pill_widgets = {}
    for key, label, _pred in _FILTER_PILLS:
        pill = tk.Label(pills, text=label,
                        font=(FONTS['mono_sm'][0], 9, 'bold'),
                        bg=COLORS['bg_3'], fg=COLORS['fg_3'],
                        padx=12, pady=4,
                        cursor='hand2',
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        pill.pack(side='left', padx=2)
        pill.bind('<Button-1>',
                  lambda e, k=key: app._bp_filter_var.set(k))
        app._bp_pill_widgets[key] = pill

    _paint_filter_pills(app)

    # Right: status
    app._bp_status_var = tk.StringVar(
        value=_('No games scanned yet — add a folder and click Scan'))
    tk.Label(inner, textvariable=app._bp_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='right')


def _paint_filter_pills(app):
    active = app._bp_filter_var.get()
    for key, pill in app._bp_pill_widgets.items():
        if not _widget_alive(pill):
            continue
        if key == active:
            pill.configure(bg=COLORS.get('accent_lo', '#1a1024'),
                           fg=COLORS.get('purple_hi', '#c891e0'),
                           highlightbackground=COLORS.get('purple',
                                                          '#9b59b6'))
        else:
            pill.configure(bg=COLORS['bg_3'], fg=COLORS['fg_3'],
                           highlightbackground=COLORS['border_3'])


# ── Folder chip strip ──
def _build_games_folder_strip(body, app):
    outer = tk.Frame(body, bg=COLORS['bg_0'])
    outer.pack(fill='x')
    app._bp_folder_frame = tk.Frame(outer, bg=COLORS['bg_0'])
    app._bp_folder_frame.pack(fill='x', padx=24, pady=(0, 8))
    _refresh_folder_chips(app)


def _refresh_folder_chips(app):
    ff = getattr(app, '_bp_folder_frame', None)
    if not _widget_alive(ff):
        return
    for w in ff.winfo_children():
        w.destroy()
    if not app._bp_folders:
        tk.Label(ff,
                 text=_('No folders added yet.'),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_0'], fg=COLORS['fg_5']
                 ).pack(side='left', pady=4)
        return
    for folder in app._bp_folders:
        chip = tk.Frame(ff,
                        bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        chip.pack(side='left', padx=(0, 6), pady=4)
        tk.Label(chip, text='\U0001f4c1  ' + folder,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_2'],
                 padx=8, pady=4
                 ).pack(side='left')
        rm = tk.Label(chip, text=' \u2715 ',
                      font=(FONTS['body'][0], 8),
                      bg=COLORS['bg_2'], fg=COLORS['fg_5'],
                      cursor='hand2', padx=6)
        rm.pack(side='left')
        rm.bind('<Button-1>',
                lambda e, f=folder: _remove_folder(app, f))


# ── Scan progress strip ──
def _build_games_scan_bar(body, app):
    app._bp_progress_frame = tk.Frame(body, bg=COLORS['bg_0'])
    app._bp_progress_frame.pack(fill='x')
    inner = tk.Frame(app._bp_progress_frame, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=(0, 8))
    tk.Label(inner, text='\u25cf',
             font=(FONTS['body'][0], 8),
             bg=COLORS['bg_0'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 6))
    tk.Label(inner, text=_('Scanning\u2026'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left')
    app._bp_progress_frame.pack_forget()


# ── Grid area ──
def _build_games_grid(body, app):
    grid_outer = tk.Frame(body, bg=COLORS['bg_1'])
    grid_outer.pack(fill='both', expand=True, padx=24, pady=(0, 12))

    canvas = tk.Canvas(grid_outer, bg=COLORS['bg_1'],
                        highlightthickness=0)
    sb = tk.Scrollbar(grid_outer, orient='vertical',
                      command=canvas.yview,
                      bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._bp_canvas = canvas
    app._bp_games_frame = tk.Frame(canvas, bg=COLORS['bg_1'])
    canvas.create_window((0, 0), window=app._bp_games_frame,
                         anchor='nw', tags='bpg')
    app._bp_games_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))

    def _on_canvas_resize(e):
        # v3.6.2: only rebuild the grid when the column count actually
        # changes — the old unconditional rebuild fired on every resize
        # pixel and re-created every card (and its async cover load).
        canvas.itemconfig('bpg', width=e.width)
        new_cols = max(1, e.width // _CARD_MIN_W)
        if new_cols != getattr(app, '_bp_last_cols', None):
            _refresh_grid(app)
    canvas.bind('<Configure>', _on_canvas_resize)
    attach_scroll(canvas)

    if getattr(app, '_bp_games_list', None):
        app.after(50, lambda: _refresh_grid(app))


# ─────────────────────────────────────────────────────────────────────────
# SUB-TAB 2: AUTO BACKPORT — options form only.
# The global OUTPUT LOG at the bottom of the window shows the details
# of what's happening during a backport; we don't duplicate it here.
# ─────────────────────────────────────────────────────────────────────────
def _build_auto_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    app._bp_form_frame = tk.Frame(parent, bg=COLORS['bg_1'])
    app._bp_form_frame.pack(fill='both', expand=True)
    try:
        app._build_bp_auto_panel(app._bp_form_frame)
    except Exception as e:
        tk.Label(app._bp_form_frame,
                 text=_('Could not load Auto Backport form:') +
                      '\n\n' + str(e),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_1'], fg=COLORS.get('err', '#e0584b'),
                 wraplength=720, justify='left'
                 ).pack(padx=24, pady=24, anchor='nw')

    # Hide the panel's built-in OUTPUT LOG widgets. The single global
    # OUTPUT LOG at the bottom of the window is the one and only log
    # surface — backport details are funnelled there via the redirected
    # `_abp_log_line` method.
    #
    # `_build_output_log` in ui/panel_bp_auto.py is the LAST builder
    # called in the panel and adds exactly two widgets to the form
    # parent: a header Frame containing "OUTPUT LOG" + Clear, then a
    # ConsoleView (stored as `app._abp_log_console`). We hide both by
    # pack_forget-ing the ConsoleView and its preceding sibling.
    try:
        cv = getattr(app, '_abp_log_console', None)
        if cv is not None and cv.winfo_exists():
            # Find the head Frame: it's the sibling packed just before
            # the ConsoleView in the same parent.
            siblings = list(app._bp_form_frame.winfo_children())
            try:
                idx = siblings.index(cv)
                if idx > 0:
                    head = siblings[idx - 1]
                    head.pack_forget()
            except ValueError:
                pass
            cv.pack_forget()
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# SUB-TAB 3: RESULTS — ✅/❌ tracking pane (existing builder)
# ─────────────────────────────────────────────────────────────────────────
def _build_results_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])
    app._bp_results_frame = tk.Frame(parent, bg=COLORS['bg_1'])
    app._bp_results_frame.pack(fill='both', expand=True)
    try:
        app._build_bpr_panel(app._bp_results_frame)
    except Exception as e:
        tk.Label(app._bp_results_frame,
                 text=_('Could not load Results pane:') +
                      '\n\n' + str(e),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_1'], fg=COLORS.get('err', '#e0584b'),
                 wraplength=720, justify='left'
                 ).pack(padx=24, pady=24, anchor='nw')


# ─────────────────────────────────────────────────────────────────────────
# Folder management
# ─────────────────────────────────────────────────────────────────────────
def _add_folder(app):
    from tkinter import filedialog
    folder = filedialog.askdirectory(
        title=_('Add folder containing PS5 game dumps'))
    if not folder:
        return
    folder = folder.replace('/', os.sep)
    if folder not in app._bp_folders:
        app._bp_folders.append(folder)
        app._settings['bp_folders'] = app._bp_folders
        app._settings['bp_scan_folder'] = folder
        try:
            save_settings(app._settings)
        except Exception:
            pass
        _refresh_folder_chips(app)
    _scan_all_folders(app)


def _remove_folder(app, folder):
    if folder in app._bp_folders:
        app._bp_folders.remove(folder)
        app._settings['bp_folders'] = app._bp_folders
        try:
            save_settings(app._settings)
        except Exception:
            pass
        _refresh_folder_chips(app)
    app._bp_queue = [g for g in app._bp_queue
                     if not (g.get('folder', '') or '').startswith(folder)]
    _refresh_grid(app)
    _update_process_btn(app)


# ─────────────────────────────────────────────────────────────────────────
# Scanning — multi-folder aggregator
# ─────────────────────────────────────────────────────────────────────────
def _scan_all_folders(app):
    if not app._bp_folders:
        try:
            app._bp_status_var.set(
                _('Add a folder first to start scanning.'))
        except Exception:
            pass
        return
    try:
        app._bp_progress_frame.pack(fill='x')
        app._bp_status_var.set(_('Scanning\u2026'))
    except Exception:
        pass

    def _worker():
        all_games = []
        for folder in list(app._bp_folders):
            try:
                if not os.path.isdir(folder):
                    continue
                for entry in sorted(os.scandir(folder),
                                    key=lambda e: e.name):
                    if not entry.is_dir():
                        continue
                    eboot = os.path.join(entry.path, 'eboot.bin')
                    if not os.path.isfile(eboot):
                        try:
                            for sub in os.scandir(entry.path):
                                if not sub.is_dir():
                                    continue
                                sub_eboot = os.path.join(
                                    sub.path, 'eboot.bin')
                                if os.path.isfile(sub_eboot):
                                    info = app._read_sfo_info(sub.path)
                                    cover = (app._find_cover(sub.path)
                                             or app._find_cover(
                                                 entry.path))
                                    all_games.append({
                                        'folder':   sub.path,
                                        'title':    info.get('TITLE')
                                                    or entry.name,
                                        'ppsa':     info.get(
                                            'TITLE_ID', ''),
                                        'title_id': info.get(
                                            'TITLE_ID', ''),
                                        'version':  info.get(
                                            'APP_VER', ''),
                                        'cover':    cover,
                                    })
                        except Exception:
                            pass
                        continue
                    info  = app._read_sfo_info(entry.path)
                    cover = app._find_cover(entry.path)
                    all_games.append({
                        'folder':   entry.path,
                        'title':    info.get('TITLE') or entry.name,
                        'ppsa':     info.get('TITLE_ID', ''),
                        'title_id': info.get('TITLE_ID', ''),
                        'version':  info.get('APP_VER', ''),
                        'cover':    cover,
                    })
            except Exception:
                continue
        app.after(0, lambda: _scan_done(app, all_games))

    threading.Thread(target=_worker, daemon=True).start()


def _scan_done(app, games):
    app._bp_games_list = games
    try:
        app._bp_progress_frame.pack_forget()
    except Exception:
        pass
    n = len(games)
    try:
        app._bp_count_var.set(
            '%d %s' % (n,
                        _('game') if n == 1 else _('games')))
        app._bp_status_var.set(
            _('Scan complete — %d game(s) found') % n)
    except Exception:
        pass

    valid_paths = {g['folder'] for g in games}
    app._bp_queue = [g for g in app._bp_queue
                     if g.get('folder') in valid_paths]

    _refresh_grid(app)
    _update_process_btn(app)
    _start_sdk_detect(app)


# ─────────────────────────────────────────────────────────────────────────
# Async SDK detection — uses App._read_sdk_from_folder
# ─────────────────────────────────────────────────────────────────────────
def _start_sdk_detect(app):
    detect = getattr(app, '_read_sdk_from_folder', None)
    games = list(getattr(app, '_bp_games_list', []))
    if not games:
        return

    def _worker():
        for g in games:
            if g.get('system_ver') or not callable(detect):
                pass
            else:
                try:
                    sv = detect(g['folder'])
                    if sv:
                        g['system_ver'] = sv
                except Exception:
                    pass
            # v3.6.0 pass: fill display-only metadata (dump size on
            # disk + folder date) used by the enriched game cards.
            if 'size_bytes' not in g:
                try:
                    total = 0
                    for root, _dirs, files in os.walk(g['folder']):
                        for fn in files:
                            try:
                                total += os.path.getsize(
                                    os.path.join(root, fn))
                            except OSError:
                                pass
                    g['size_bytes'] = total
                except Exception:
                    g['size_bytes'] = 0
            if 'mtime' not in g:
                try:
                    g['mtime'] = os.path.getmtime(g['folder'])
                except Exception:
                    g['mtime'] = 0
        app.after(0, lambda: _refresh_grid(app))

    threading.Thread(target=_worker, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────────
# Grid rendering — applies search + filter to games list
# ─────────────────────────────────────────────────────────────────────────
def _refresh_grid(app):
    _paint_filter_pills(app)
    grid = getattr(app, '_bp_games_frame', None)
    if not _widget_alive(grid):
        return

    for w in grid.winfo_children():
        w.destroy()

    games = list(getattr(app, '_bp_games_list', []))
    if not games:
        tk.Label(grid,
                 text=_('No games scanned yet.\nAdd a folder and '
                        'click Scan to find PS5 game dumps.'),
                 font=FONTS['body'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                 justify='center', pady=40
                 ).pack(expand=True)
        _refresh_games_stats(app)
        return

    pred = _FILTER_PRED.get(app._bp_filter_var.get(), lambda s: True)
    search = (app._bp_search_var.get() or '').strip().lower()

    def _matches(g):
        if not pred(g.get('system_ver', 0)):
            return False
        if search:
            hay = (g.get('title', '') + ' ' +
                   g.get('ppsa', '') + ' ' +
                   g.get('title_id', '')).lower()
            if search not in hay:
                return False
        return True

    visible = [g for g in games if _matches(g)]
    if not visible:
        tk.Label(grid,
                 text=_('No games match the current filter.'),
                 font=FONTS['body'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                 pady=40
                 ).pack(expand=True)
        _refresh_games_stats(app)
        return

    try:
        width = app._bp_canvas.winfo_width()
    except Exception:
        width = 800
    # v3.6.2: a card is a 180px cover + text column + padding (~400px
    # natural width). The old `width // 200` packed up to twice as many
    # columns as fit, crushing every card into slivers. Size columns
    # from the real card width and keep them uniform.
    cols = max(1, width // _CARD_MIN_W)
    app._bp_last_cols = cols

    # Reset stale column weights from a previous, wider layout — empty
    # columns that keep weight=1 would still claim a share of the width.
    try:
        old_cols = grid.grid_size()[0]
    except Exception:
        old_cols = 0
    for c in range(max(old_cols, cols)):
        if c < cols:
            grid.grid_columnconfigure(c, weight=1, uniform='bpcards')
        else:
            grid.grid_columnconfigure(c, weight=0, uniform='')

    queued_paths = {g['folder'] for g in app._bp_queue}

    for i, game in enumerate(visible):
        r, c = i // cols, i % cols
        _make_card(app, grid, game, r, c,
                   queued=(game['folder'] in queued_paths))

    total = len(games)
    qn = len(app._bp_queue)
    try:
        if qn:
            app._bp_count_var.set(
                '%d %s \u00b7 %d %s' %
                (total,
                 _('games') if total != 1 else _('game'),
                 qn, _('queued')))
        else:
            app._bp_count_var.set(
                '%d %s' % (total,
                            _('games') if total != 1 else _('game')))
    except Exception:
        pass
    _refresh_games_stats(app)


def _make_card(app, parent, game, row, col, queued):
    sdk_val  = game.get('system_ver', 0)
    title    = game.get('title') or os.path.basename(
        game.get('folder', ''))
    title_id = game.get('ppsa') or game.get('title_id', '')
    version  = None
    v_raw = game.get('version', '')
    if v_raw:
        try:
            parts = v_raw.split('.')
            major = int(parts[0])
            minor = int(parts[1]) if len(parts) > 1 else 0
            version = 'v%d.%02d' % (major, minor)
        except (ValueError, IndexError):
            version = 'v' + v_raw[:8]

    # v3.6.0 pass: SDK badge rides on the cover; size / date / folder
    # meta rows come from the detect worker's display-only fields.
    cover_badge = None
    if sdk_val:
        cover_badge = ('SDK %d' % sdk_val,
                       'warn' if sdk_val >= 16 else 'purple')

    size_text = None
    sz = game.get('size_bytes')
    if sz:
        size_text = ('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % max(1, sz // 1024**2))
    date_text = None
    if game.get('mtime'):
        try:
            import datetime as _dt
            date_text = _dt.datetime.fromtimestamp(
                game['mtime']).strftime('%d/%m/%Y %H:%M')
        except Exception:
            pass
    folder_text = os.path.dirname(game.get('folder', '')) or None

    def _menu(e, g=game):
        _card_menu(app, e, g)

    card = GameCard(
        parent,
        title=title,
        game_id=title_id,
        version=version,
        size_text=size_text,
        date_text=date_text,
        folder_text=folder_text,
        kind_text=_('Base Game'),
        cover_badge=cover_badge,
        badges=[],
        cover_size=180,
        on_click=lambda e, g=game: _toggle_queue(app, g),
        on_double=lambda e, g=game: _toggle_queue(app, g),
        on_right=_menu,
        on_menu=_menu,
    )
    card.grid(row=row, column=col, padx=6, pady=6, sticky='nsew')

    # ── Per-card action row (Queue / Process / handled by ⋮ too) ──
    arow = tk.Frame(card, bg=COLORS['bg_2'])
    arow.pack(fill='x', padx=12, pady=(0, 12))
    if queued:
        qbtn = tk.Button(arow, text='\u2713  ' + _('Queued'),
                         font=(FONTS['button'][0], 9, 'bold'),
                         bg=COLORS.get('purple', '#9b59b6'),
                         fg=COLORS['fg_0'],
                         activebackground=COLORS.get('purple_hi',
                                                     '#c891e0'),
                         activeforeground=COLORS['fg_0'],
                         relief='flat', bd=0, padx=12, pady=5,
                         cursor='hand2',
                         command=lambda g=game: _toggle_queue(app, g))
    else:
        qbtn = tk.Button(arow, text='+  ' + _('Queue'),
                         font=(FONTS['button'][0], 9, 'bold'),
                         bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                         activebackground=COLORS['bg_4'],
                         activeforeground=COLORS['fg_0'],
                         relief='flat', bd=0, padx=12, pady=5,
                         cursor='hand2',
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1,
                         command=lambda g=game: _toggle_queue(app, g))
    qbtn.pack(side='left')
    tk.Button(arow, text='\u26a1  ' + _('Process'),
              font=(FONTS['button'][0], 9, 'bold'),
              bg=COLORS['bg_3'],
              fg=COLORS.get('purple_hi', '#c891e0'),
              activebackground=COLORS['bg_4'],
              activeforeground=COLORS.get('purple_hi', '#c891e0'),
              relief='flat', bd=0, padx=12, pady=5,
              cursor='hand2',
              highlightbackground=COLORS.get('purple', '#9b59b6'),
              highlightthickness=1,
              command=lambda g=game: _process_single(app, g)
              ).pack(side='left', padx=(6, 0))

    if queued:
        try:
            purple = COLORS.get('purple', '#9b59b6')
            purple_hi = COLORS.get('purple_hi', '#c891e0')
            card.configure(highlightbackground=purple,
                           highlightthickness=2)
            card._idle_border = purple
            card._hover_border = purple_hi
        except Exception:
            pass
        try:
            cover_frame = card.cover_label.master
            qbadge = tk.Label(
                cover_frame,
                text=' \u2713 ' + _('Queued') + ' ',
                font=(FONTS['mono_sm'][0], 8, 'bold'),
                bg=COLORS.get('purple', '#9b59b6'),
                fg=COLORS['fg_0'],
                padx=4, pady=1)
            qbadge.place(relx=0.5, rely=1.0, anchor='s', y=-8)
        except Exception:
            pass

    if game.get('cover'):
        def _load(path=game['cover'], lbl=card.cover_label):
            try:
                img = _load_cover_image(path, target=180)  # 2:3 -> 120x180
                if img is None:
                    return
                from PIL import ImageTk
                photo = ImageTk.PhotoImage(img)

                def _apply():
                    try:
                        if lbl.winfo_exists():
                            lbl.config(image=photo, text='')
                            lbl.image = photo
                    except Exception:
                        pass
                app.after(0, _apply)
            except Exception:
                pass
        threading.Thread(target=_load, daemon=True).start()


def _card_menu(app, event, game):
    """⋮ / right-click context menu for a game card (v3.6.0 pass)."""
    try:
        menu = tk.Menu(app, tearoff=0,
                       bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                       activebackground=COLORS['bg_3'],
                       activeforeground=COLORS['fg_0'],
                       relief='flat', bd=1)
        in_q = any(g.get('folder') == game.get('folder')
                   for g in app._bp_queue)
        menu.add_command(
            label=(_('Remove from queue') if in_q else _('Add to queue')),
            command=lambda g=game: _toggle_queue(app, g))
        menu.add_command(label='\u26a1  ' + _('Process this game'),
                         command=lambda g=game: _process_single(app, g))
        menu.add_separator()

        def _open_folder(g=game):
            try:
                os.startfile(g.get('folder', ''))
            except Exception:
                pass
        menu.add_command(label='\U0001f4c2  ' + _('Open folder'),
                         command=_open_folder)
        menu.tk_popup(event.x_root, event.y_root)
    except Exception:
        pass
    finally:
        try:
            menu.grab_release()
        except Exception:
            pass


def _process_single(app, game):
    """Run the auto-backport flow for a single game card. Same
    mechanism as the queue path (temporary _lib_games swap around the
    existing _lib_backport_all) — just a one-game list."""
    import tkinter.messagebox as _mb
    if not _mb.askyesno(_('Process backport'),
                        _('Run auto-backport on "%s"?')
                        % (game.get('title') or
                           os.path.basename(game.get('folder', '')))):
        return
    try:
        app._bp_subtab_activate('auto')
    except Exception:
        pass
    saved_lib_games = getattr(app, '_lib_games', None)
    app._lib_games = [dict(game)]
    try:
        app._lib_backport_all()
    finally:
        if saved_lib_games is not None:
            app._lib_games = saved_lib_games
    _refresh_grid(app)
    _update_process_btn(app)


# ─────────────────────────────────────────────────────────────────────────
# Bottom stats strip — Games / Queued / Completed / Est. time
# ─────────────────────────────────────────────────────────────────────────
def _build_games_stats(body, app):
    outer = tk.Frame(body, bg=COLORS['bg_1'])
    outer.pack(fill='x', padx=24, pady=(0, 14))
    strip = tk.Frame(outer, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    strip.pack(fill='x')
    row = tk.Frame(strip, bg=COLORS['bg_2'])
    row.pack(fill='x', padx=10, pady=12)
    for i in range(4):
        row.grid_columnconfigure(i, weight=1, uniform='bpstat')

    cells = {}

    def _cell(col, icon, icon_fg, caption, sub):
        cell = tk.Frame(row, bg=COLORS['bg_2'])
        cell.grid(row=0, column=col, sticky='ew')
        ic = tk.Label(cell, text=icon, font=(FONTS['body'][0], 16),
                      bg=COLORS['bg_2'], fg=icon_fg)
        ic.pack(side='left', padx=(14, 12))
        colf = tk.Frame(cell, bg=COLORS['bg_2'])
        colf.pack(side='left')
        tk.Label(colf, text=caption.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(anchor='w')
        v = tk.Label(colf, text='0',
                     font=(FONTS['h2'][0], 16, 'bold'),
                     bg=COLORS['bg_2'], fg=COLORS['fg_0'])
        v.pack(anchor='w')
        tk.Label(colf, text=sub,
                 font=(FONTS['mono_sm'][0], 8),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(anchor='w')
        return v

    cells['games'] = _cell(0, '\U0001f3ae',
                           COLORS.get('purple_hi', '#c891e0'),
                           _('Games'), _('Total games'))
    cells['queued'] = _cell(1, '\U0001f4c1', COLORS['warn_hi'],
                            _('Queued'), _('In the queue'))
    cells['done'] = _cell(2, '\u2713', COLORS['success_hi'],
                          _('Completed'), _('Backports completed'))
    cells['eta'] = _cell(3, '\u23f1', COLORS['accent_hi'],
                         _('Est. Time'), _('Estimated queue time'))
    app._bp_stat_cells = cells
    _refresh_games_stats(app)


def _refresh_games_stats(app):
    cells = getattr(app, '_bp_stat_cells', None)
    if not cells:
        return
    try:
        if not cells['games'].winfo_exists():
            return
        total = len(getattr(app, '_bp_games_list', []) or [])
        qn = len(app._bp_queue)
        done = 0
        try:
            log = (getattr(app, '_bpr_log', None)
                   or app._settings.get('bpr_log', []) or [])
            done = sum(1 for e in log if e.get('status') == 'worked')
        except Exception:
            pass
        cells['games'].config(text=str(total))
        cells['queued'].config(text=str(qn))
        cells['done'].config(text=str(done))
        cells['eta'].config(
            text=('~ %d min' % max(1, qn)) if qn else '\u2014')
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Queue management
# ─────────────────────────────────────────────────────────────────────────
def _toggle_queue(app, game):
    folder = game.get('folder', '')
    idx = None
    for i, g in enumerate(app._bp_queue):
        if g.get('folder') == folder:
            idx = i
            break
    if idx is not None:
        app._bp_queue.pop(idx)
    else:
        app._bp_queue.append(dict(game))
    _refresh_grid(app)
    _update_process_btn(app)


def _update_process_btn(app):
    btn = getattr(app, '_bp_process_btn', None)
    if not _widget_alive(btn):
        return
    try:
        n = len(app._bp_queue)
        btn.config(text=_process_label(app))
        btn.config(state='normal' if n else 'disabled')
    except Exception:
        pass


def _process_queue(app):
    """Run every game in the queue through the auto-backport flow.

    Switches to the Auto Backport sub-tab first so the user sees the
    live log + status fields update. Reuses `_lib_backport_all` by
    temporarily swapping `app._lib_games` with our queued list.
    """
    if not app._bp_queue:
        return
    import tkinter.messagebox as _mb
    if not _mb.askyesno(_('Process backport queue'),
                        _('Run auto-backport on %d game(s)?\n\n'
                          'You\'ll be asked for the backup folder and '
                          'target SDK next. The games run sequentially '
                          'so this may take a while.')
                        % len(app._bp_queue)):
        return

    # Switch to Auto Backport so the user can watch the log
    try:
        app._bp_subtab_activate('auto')
    except Exception:
        pass

    saved_lib_games = getattr(app, '_lib_games', None)
    app._lib_games = list(app._bp_queue)
    try:
        app._lib_backport_all()
    finally:
        if saved_lib_games is not None:
            app._lib_games = saved_lib_games

    app._bp_queue = []
    _refresh_grid(app)
    _update_process_btn(app)


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _btn(parent, text, command, kind='ghost'):
    schemes = {
        'primary': (COLORS['accent'],  COLORS['fg_0'],
                    COLORS.get('accent_hi', COLORS['accent']),
                    COLORS['fg_0']),
        'success': (COLORS.get('success', '#4caf50'), COLORS['fg_0'],
                    COLORS.get('success_hi', '#5dbf60'),
                    COLORS['fg_0']),
        'purple':  (COLORS.get('purple', '#9b59b6'), COLORS['fg_0'],
                    '#a567c1', COLORS['fg_0']),
        'ghost':   (COLORS['bg_2'],    COLORS['fg_2'],
                    COLORS['bg_3'],     COLORS['fg_0']),
    }
    bg, fg, abg, afg = schemes.get(kind, schemes['ghost'])
    btn = tk.Button(parent, text=text,
                    font=(FONTS['button'][0], 9, 'bold'),
                    bg=bg, fg=fg,
                    activebackground=abg, activeforeground=afg,
                    relief='flat', bd=0,
                    padx=12, pady=6,
                    cursor='hand2',
                    command=command)
    if kind == 'ghost':
        btn.configure(highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
    return btn
