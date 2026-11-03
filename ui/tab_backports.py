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
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)

from tkinter_theme import COLORS, FONTS
from ui.shared.cards import GameCard, build_subtabs

# Star-import for legacy theme constants used by inner panel builders
from exfat_builder import *  # noqa: F401,F403
from ui.shared.app_dialog import app_confirm, app_error, app_info, app_warning
from exfat_builder import _, _load_cover_image
from ui.shared.scroll import attach_scroll
from ui.shared.empty_state import EmptyState


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
        ('sdkfixer', '\U0001f527  ' + _('SDK Fixer'),
            lambda f: _build_sdkfixer_subtab(f, app)),
        ('batchsdk', '\U0001f4e6  ' + _('Batch SDK Retag'),
            lambda f: _build_batchsdk_subtab(f, app)),
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
# SUB-TAB 3: RESULTS — ✅/❌ tracking pane (existing builder)
# ─────────────────────────────────────────────────────────────────────────
def _build_results_subtab(parent, app):
    """Build the Backports Results sub-tab.

    Restored wrapper (v3.6.5 fix): this function's body had been orphaned
    as unreachable code after `return b` inside the nested `_sdkfix_btn`
    helper during the sub-tab refactor, so the `('results', …,
    lambda f: _build_results_subtab(f, app))` registration raised a
    NameError that `build_subtabs` swallowed to stderr — leaving the
    Results tab blank with no in-app log. The real Results panel
    (`app._build_bpr_panel` / `app._bpr_render`, including its empty-state)
    was always intact; it just never got built. This wrapper restores the
    wiring; nothing in the panel or its data path changed.
    """
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
             font=(FONTS['body_strong'][0], 14),
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
    XEntry(sw, textvariable=app._bp_search_var,
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
                        font=(FONTS['body_strong'][0], 9),
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
                      font=(FONTS['body'][0], 9),
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
             font=(FONTS['body'][0], 9),
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
    sb = XScrollbar(grid_outer, orient='vertical',
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


def _build_advfixes_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])
    from ui import audio_fix_scan as afs
    if not hasattr(app, '_afx_folder_var'):
        app._afx_folder_var = tk.StringVar(value='')
    if not hasattr(app, '_afx_whole_var'):
        app._afx_whole_var = tk.BooleanVar(value=False)
    app._afx_last_result = None
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=16, pady=12)
    tk.Label(wrap, text='🔍  ' + _('Advanced Fixes — read-only diagnostics'), font=(FONTS['body_strong'][0], 14), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w')
    tk.Label(wrap, text=_('Read-only. Evidence hierarchy (strongest first):\n  1. CONFIRMED PROFILE (DBS, SM2) — console-backed, patch-eligible (Channel-Config scan)\n  2. TWIN-BRANCH / API-context — strong lead, report-only (Channel-Config scan)\n  3. LEGACY structural fields (0C F1 63 2D) — NOT a confirmed fix, report-only (Scan Audio Fix Candidates)\n  4. Loose clusters — noise, report-only\nOnly a CONFIRMED PROFILE is ever patch-eligible. Use “Scan Channel-Config” as the primary scanner.'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left', wraplength=760).pack(anchor='w', pady=(2, 12))
    fr = tk.Frame(wrap, bg=COLORS['bg_1'])
    fr.pack(fill='x', pady=(0, 6))
    tk.Label(fr, text=_('Dump folder:'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_3']).pack(side='left', padx=(0, 8))
    ent = XEntry(fr, textvariable=app._afx_folder_var, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
    ent.pack(side='left', fill='x', expand=True, padx=(0, 8))
    def _browse():
        d = filedialog.askdirectory(title=_('Select the PS5 dump folder'))
        if d:
            app._afx_folder_var.set(d)
    _btn(fr, _('Browse…'), _browse, kind='ghost').pack(side='left')
    scope = tk.Frame(wrap, bg=COLORS['bg_1'])
    scope.pack(fill='x', pady=(0, 8))
    XCheck(scope, text=_('Also scan all .bin/.prx/.sprx in the whole folder'), variable=app._afx_whole_var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_3'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_1'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0).pack(side='left')
    tk.Label(scope, text=_('  (default: eboot.bin + prx/ + sce_module/ + fakelib/)'), font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_5']).pack(side='left')
    summary_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    summary_card.pack(fill='x', pady=(6, 8))
    summary_var = tk.StringVar(value=_('No scan run yet.'))
    tk.Label(summary_card, text=_('Summary'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    tk.Label(summary_card, textvariable=summary_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], justify='left', anchor='w', wraplength=760).pack(anchor='w', fill='x', padx=12, pady=(0, 10))
    prv_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    tk.Label(prv_card, text=_('Profile-relative validation'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    prv_verdict_var = tk.StringVar(value='')
    prv_verdict_lbl = tk.Label(prv_card, textvariable=prv_verdict_var, font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w')
    prv_verdict_lbl.pack(anchor='w', padx=12, pady=(0, 2))
    prv_body_var = tk.StringVar(value='')
    tk.Label(prv_card, textvariable=prv_body_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], justify='left', anchor='w', wraplength=760).pack(anchor='w', fill='x', padx=12, pady=(0, 6))
    prv_patch_lbl = tk.Label(prv_card, text=_('Patch recommendation: NO — requires real console test'), font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w')
    prv_patch_lbl.pack(anchor='w', padx=12, pady=(0, 8))
    app._afx_prv_card = prv_card
    app._afx_prv_verdict_var = prv_verdict_var
    app._afx_prv_verdict_lbl = prv_verdict_lbl
    app._afx_prv_body_var = prv_body_var
    table_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    tk.Label(table_card, text=_('Scan report'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    table_holder = tk.Frame(table_card, bg=COLORS['bg_3'])
    table_holder.pack(fill='both', expand=True, padx=10, pady=(0, 10))
    table_scroll = XScrollbar(table_holder, orient='vertical')
    table_scroll.pack(side='right', fill='y')
    table_txt = tk.Text(table_holder, height=8, bg=COLORS['bg_3'], fg=COLORS['fg_2'], font=FONTS['mono_sm'], relief='flat', wrap='none', highlightthickness=0, yscrollcommand=table_scroll.set)
    table_txt.pack(side='left', fill='both', expand=True)
    table_scroll.configure(command=table_txt.yview)
    table_txt.configure(state='disabled')
    def _set_table(text):
        table_txt.configure(state='normal')
        table_txt.delete('1.0', 'end')
        table_txt.insert('1.0', text)
        table_txt.configure(state='disabled')
    footer_note = tk.Label(wrap, text=_('Candidate audio-fix data fields. Read-only scan — no files are modified and no audio fix is guaranteed.'), font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_5'], justify='left', wraplength=760)
    footer_note.pack(side='bottom', anchor='w', pady=(8, 0))
    btns = tk.Frame(wrap, bg=COLORS['bg_1'])
    btns.pack(side='bottom', fill='x', pady=(2, 0))
    def _run_scan():
        folder = app._afx_folder_var.get().strip()
        if not folder or not os.path.isdir(folder):
            summary_var.set(_('Select a valid dump folder first.'))
        else:
            summary_var.set(_('Scanning (read-only)…'))
            _set_table('')
            def _work():
                try:
                    res = afs.scan_dump(folder, whole_folder=bool(app._afx_whole_var.get()))
                except Exception as e:
                    _msg = str(e)
                    app.after(0, lambda m=_msg: summary_var.set(_('Scan error: ') + m))
                    return
                def _done():
                    app._afx_last_result = res
                    app._afx_last_scan_kind = 'field'
                    s = res['summary']
                    warn = '  ⚠ ' + '; '.join(s['warnings']) if s['warnings'] else ''
                    bc = s['by_confidence']
                    prv = s.get('profile_relative_validation')
                    if prv:
                        pr_matched = prv['exact'] + prv['near']
                        pr_total = pr_matched + prv['missing']
                        prof_lines = _('Profile-relative matches: %d/%d\nValidation verdict: %s (delta %s)\n') % (pr_matched, pr_total, prv['verdict'], prv['delta'])
                    else:
                        prof_lines = _('Profile-relative matches: — (no profile relocation found)\n')
                    summary_var.set(_('LEGACY field scan (0C F1 63 2D) — report-only\nTier: LEGACY_F1_FIELD   (NOT a confirmed audio fix)\nFiles raw-scanned: %d   Skipped (unreadable): %d\nLegacy fields matched (absolute): %d/%d\n%sUnpatched: %d   Already patched: %d\nConfidence: HIGH=%d STRUCTURAL=%d MEDIUM=%d LOW=%d SKIP=%d\nPatch recommendation: NO — legacy marker. Run the channel-config scan for confirmed-profile detection.%s') % (s['files_raw_scanned'], s['files_skipped'], s['known_offsets_matched'], s['known_offsets_total'], prof_lines, s['unpatched'], s['already_patched'], bc.get('HIGH', 0), bc.get('STRUCTURAL', 0), bc.get('MEDIUM', 0), bc.get('LOW', 0), bc.get('SKIP', 0), warn))
                    prv = s.get('profile_relative_validation')
                    if prv:
                        verdict = prv['verdict']
                        vcolor = COLORS['teal']
                        if verdict in ('NO_RELOCATION',):
                            vcolor = COLORS.get('fg_5', COLORS['fg_4'])
                        elif verdict in ('PARTIAL_STRUCTURAL',):
                            vcolor = COLORS.get('amber', COLORS['teal'])
                        try:
                            app._afx_prv_verdict_lbl.configure(fg=vcolor)
                        except Exception:
                            pass
                        app._afx_prv_verdict_var.set(verdict)
                        pred_lines = []
                        for p in prv['predictions']:
                            if p['kind'] == 'exact':
                                pred_lines.append(f"  {p['profile_offset']!s} + delta = {p['predicted']!s}   exact")
                            elif p['kind'] == 'near':
                                pred_lines.append('  %s + delta = %s   near %s (%s%d)' % (p['profile_offset'], p['predicted'], p['matched'], '+' if (p['delta_off'] or 0) >= 0 else '', p['delta_off']))
                            else:
                                pred_lines.append(f"  {p['profile_offset']!s} + delta = {p['predicted']!s}   MISSING")
                        app._afx_prv_body_var.set(_('Profile: %s\nAnchor:  %s → %s\nDelta:   %s\nMatches: exact %d   near %d   missing %d\n%s') % (prv.get('profile_name') or '?', prv['anchor_profile'], prv['anchor_found'], prv['delta'], prv['exact'], prv['near'], prv['missing'], '\n'.join(pred_lines)))
                        try:
                            app._afx_prv_card.pack(fill='x', pady=(0, 8), after=summary_card)
                        except Exception:
                            app._afx_prv_card.pack(fill='x', pady=(0, 8))
                    else:
                        try:
                            app._afx_prv_card.pack_forget()
                        except Exception:
                            pass
                    try:
                        _set_table(afs.export_txt(res))
                    except Exception:
                        _set_table(_('(could not render table)'))
                    try:
                        has_candidates = s['candidates_found'] > 0
                        if has_candidates:
                            app._afx_testpatch_btn.pack(side='left', padx=(8, 0))
                        else:
                            app._afx_testpatch_btn.pack_forget()
                    except Exception:
                        pass
                    try:
                        app._afx_selector_btn.pack_forget()
                    except Exception:
                        pass
                app.after(0, _done)
            threading.Thread(target=_work, daemon=True).start()
    def _export(kind):
        ext = '.json' if kind == 'json' else '.txt'
        chan = getattr(app, '_afx_last_channel_result', None)
        field = getattr(app, '_afx_last_result', None)
        which = getattr(app, '_afx_last_scan_kind', None)
        if which == 'channel' and chan:
            from ui import audio_channel_scan as _mod
            res = chan
        elif which == 'field' and field:
            from ui import audio_fix_scan as _mod
            res = field
        elif chan:
            from ui import audio_channel_scan as _mod
            res = chan
        elif field:
            from ui import audio_fix_scan as _mod
            res = field
        else:
            summary_var.set(_('Run a scan first, then export.'))
            return
        path = filedialog.asksaveasfilename(title=_('Export scan report'), defaultextension=ext, filetypes=[('JSON', '*.json')] if kind == 'json' else [('Text', '*.txt')])
        if not path:
            return
        try:
            content = _mod.export_json(res) if kind == 'json' else _mod.export_txt(res)
            with open(path, 'w', encoding='utf-8') as f:
                f.write(content)
            summary_var.set(_('Report exported: ') + path)
        except Exception as e:
            summary_var.set(_('Export failed: ') + str(e))
    def _run_channel_scan():
        folder = app._afx_folder_var.get().strip()
        if not folder or not os.path.isdir(folder):
            summary_var.set(_('Select a valid dump folder first.'))
        else:
            summary_var.set(_('Scanning channel-config (read-only)…'))
            _set_table('')
            def _work():
                try:
                    from ui import audio_channel_scan as acs
                    res = acs.scan_dump(folder, whole_folder=bool(app._afx_whole_var.get()))
                except Exception as e:
                    _msg = str(e)
                    app.after(0, lambda m=_msg: summary_var.set(_('Channel scan error: ') + m))
                    return
                def _done():
                    from ui import audio_channel_scan as acs
                    app._afx_last_channel_result = res
                    app._afx_last_scan_kind = 'channel'
                    s = res['summary']
                    tier = s.get('tier', 'NO_MATCH')
                    cpm = s.get('confirmed_profile_matches', [])
                    if cpm:
                        m = cpm[0]
                        state = _('UNPATCHED — patch-eligible') if m['any_unpatched'] else _('already patched')
                        head = _('Channel-config scan — Tier: %s\nCONFIRMED PROFILE: %s (%s)\nStatus: %s\nPatch eligible: %s   Twin-branch matches: %d   Loose leads: %d') % (tier, m['profile_id'], m['app_ver'], state, _('YES') if s['patch_eligible'] else _('NO'), s.get('twin_branches_found', 0), s['loose_leads_found'])
                    elif tier == 'TWIN_BRANCH_MATCH':
                        region = ''
                        vals = ''
                        for fr in res['files']:
                            if fr.get('twin_branches'):
                                tb0 = fr['twin_branches'][0]
                                inv0 = tb0.get('investigation') or {}
                                region = f"{inv0.get('region_start', '?')!s}..{inv0.get('region_end', '?')!s}"
                                pairs = inv0.get('channel_value_pairs') or []
                                if pairs:
                                    vals = ' / '.join('%s(%dch)' % (p['imm'], p['channels']) for p in pairs)
                                break
                        head = _('Channel-config scan — PRIMARY INVESTIGATION TARGET\nRegion: %s   Encoding: IMM_SHIFTED_8\nValues: %s\nTier: TWIN_BRANCH_MATCH   Patch eligible: NO (no confirmed profile)\nLoose leads: %d (see JSON export — not patch targets)') % (region or '?', vals or '(install capstone for values)', s['loose_leads_found'])
                    else:
                        head = _('Channel-config scan — Tier: %s\nNo confirmed profile, no twin-branch.\nLoose leads: %d — hidden by default, NOT patch targets. Use Export JSON.\nPatch eligible: NO') % (tier, s['loose_leads_found'])
                    summary_var.set(head)
                    try:
                        app._afx_prv_card.pack_forget()
                    except Exception:
                        pass
                    try:
                        _set_table(acs.export_txt(res))
                    except Exception:
                        _set_table(_('(could not render channel report)'))
                    try:
                        app._afx_testpatch_btn.pack_forget()
                    except Exception:
                        pass
                    try:
                        sel = _find_selector_for_patch(res)
                        if sel is not None:
                            app._afx_selector = sel
                            app._afx_selector_btn.pack_forget()
                        else:
                            app._afx_selector_btn.pack_forget()
                    except Exception:
                        try:
                            app._afx_selector_btn.pack_forget()
                            return
                        except Exception:
                            return
                app.after(0, _done)
            threading.Thread(target=_work, daemon=True).start()
    _btn(btns, '🎵  ' + _('Scan Channel-Config'), _run_channel_scan, kind='primary').pack(side='left', padx=(0, 8))
    _btn(btns, '📜  ' + _('Legacy Field Scan'), _run_scan, kind='ghost').pack(side='left', padx=(0, 8))
    _btn(btns, _('Export .txt'), lambda: _export('txt'), kind='ghost').pack(side='left', padx=(0, 6))
    _btn(btns, _('Export .json'), lambda: _export('json'), kind='ghost').pack(side='left', padx=(0, 6))
    testpatch_btn = _btn(btns, _('Create Test Patch…'), lambda: _open_testpatch_dialog(app, afs), kind='ghost')
    app._afx_testpatch_btn = testpatch_btn
    selector_btn = _btn(btns, '⚠  ' + _('Create Experimental Selector Test Patch…'), lambda: _open_selector_patch_dialog(app), kind='ghost')
    app._afx_selector_btn = selector_btn
    table_card.pack(fill='both', expand=True, pady=(0, 8))


def _find_selector_for_patch(channel_result):
    """Return {file_path, value_offset, region, ...} for a twin-branch selector
    that spans BOTH 8 and 12 channels (firmware-compat shape), or None. The
    value_offset is the 12-branch value byte (0xC00's 0x0C). Gates the
    EXPERIMENTAL selector test-patch button."""
    for fr in channel_result.get('files', []):
        for tb in fr.get('twin_branches', []):
            inv = tb.get('investigation') or {}
            pairs = inv.get('channel_value_pairs') or []
            chans = set(p['channels'] for p in pairs) or set(inv.get('channel_values') or [])
            if 8 in chans and (12 in chans and tb.get('encoding') == 'IMM_SHIFTED_8'):
                tv = tb.get('twelve_value_offset')
                voff = int(tv, 16) if tv else int(tb['twelve_offset'], 16) + 2
                return {'file_path': fr['file_path'], 'file_name': fr['file_name'], 'value_offset': voff, 'region': f"{inv.get('region_start', '?')!s}..{inv.get('region_end', '?')!s}", 'pairs': pairs}


def _open_selector_patch_dialog(app):
    """Gated EXPERIMENTAL selector test-patch dialog. Patches ONLY the detected
    selector's 12-value byte (0xC00->0x800) on a COPY. Original never written.
    Testing-eligible only; never creates a confirmed profile."""
    sel = getattr(app, '_afx_selector', None)
    if not sel:
        return
    dump_folder = app._afx_folder_var.get().strip()
    dlg = tk.Toplevel(app)
    dlg.title(_('Experimental Selector Test Patch'))
    dlg.configure(bg=COLORS['bg_1'])
    dlg.transient(app)
    dlg.grab_set()
    dlg.geometry('640x560')
    pad = tk.Frame(dlg, bg=COLORS['bg_1'])
    pad.pack(fill='both', expand=True, padx=16, pady=14)
    tk.Label(pad, text=_('Experimental Selector Test Patch'), font=(FONTS['body_strong'][0], 14), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w')
    warn = tk.Frame(pad, bg=COLORS.get('teal_bg', COLORS['bg_2']), highlightbackground=COLORS.get('amber', COLORS['border_2']), highlightthickness=1)
    warn.pack(fill='x', pady=(8, 12))
    tk.Label(warn, text=_('⚠  EXPERIMENTAL\nThis is NOT a confirmed fix.\nThis creates a patched COPY for console testing only.\nAudio may remain broken. Game may behave identically.\nThe original file is never modified.\nThis selector resembles the DBS/SM2 channel selector but is NOT a confirmed profile for this title. A successful test does NOT by itself create a confirmed profile.'), font=FONTS['mono_sm'], bg=COLORS.get('teal_bg', COLORS['bg_2']), fg=COLORS['fg_1'], justify='left', wraplength=580).pack(anchor='w', padx=10, pady=8)
    tk.Label(pad, text=_('Target (detected channel selector):\n  File:   %s\n  Region: %s\n  Change: 0xC00 → 0x800  (12ch → 8ch) at value byte 0x%X\n  Other branches (1ch/2ch/8ch) are left untouched.') % (sel['file_name'], sel['region'], sel['value_offset']), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_2'], justify='left').pack(anchor='w', pady=(0, 10))
    ack1 = tk.BooleanVar(value=False)
    ack2 = tk.BooleanVar(value=False)
    ack3 = tk.BooleanVar(value=False)
    ack4 = tk.BooleanVar(value=False)
    holder = {}
    def _gate(*_a):
        ok = ack1.get() and (ack2.get() and (ack3.get() and ack4.get()))
        b = holder.get('btn')
        if b is not None:
            try:
                b.configure(state='normal' if ok else 'disabled')
            except Exception:
                pass
    for var, label in (ack1, _('I understand this is EXPERIMENTAL and NOT a confirmed fix')), (ack2, _('I understand audio may remain broken / behavior may be unchanged')), (ack3, _('Patch a COPY, not the original')), (ack4, _('This test result does not by itself create a confirmed profile')):
        XCheck(pad, text=label, variable=var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_1'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0, command=_gate).pack(anchor='w', pady=1)
    status = tk.StringVar(value='')
    tk.Label(pad, textvariable=status, font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left', wraplength=580).pack(anchor='w', pady=(6, 0))
    row = tk.Frame(pad, bg=COLORS['bg_1'])
    row.pack(fill='x', pady=(10, 0))
    def _do():
        from ui import audio_fix_patch as PP
        if not dump_folder or not os.path.isdir(dump_folder):
            status.set(_('Dump folder is not valid.'))
            return
        r = PP.create_selector_test_patch(dump_folder, sel['file_path'],
                                          sel['value_offset'])
        app._afx_selector_result = r
        if r['ok']:
            status.set(_('Experimental patch created.\n'
                         'Patched COPY: %s\n'
                         'Original untouched: %s\n'
                         'Change: %s\n'
                         'Test this copy on a lower-firmware console.')
                       % (r['copy'],
                          _('YES') if r['original_untouched'] else _('NO (!)'),
                          r['change']))
            if app_confirm(app,
                           _('Export patch report?'),
                           _('Save an EXPERIMENTAL selector patch report for the console test?')):
                path = filedialog.asksaveasfilename(
                    title=_('Export selector patch report'),
                    defaultextension='.txt',
                    filetypes=[('Text', '*.txt'), ('JSON', '*.json')])
                if path:
                    try:
                        content = (PP.export_selector_report_json(r)
                                   if path.lower().endswith('.json')
                                   else PP.export_selector_report_txt(r))
                        with open(path, 'w', encoding='utf-8') as f:
                            f.write(content)
                        status.set(status.get() + '\n'
                                   + _('Report exported: ') + path)
                    except Exception as e:
                        status.set(_('Report export failed: ') + str(e))
        else:
            status.set(_('Patch refused / failed: %s')
                       % (r.get('error') or 'unknown'))
    btn = _btn(row, _('Create Patched Copy'), _do, kind='primary')
    btn.configure(state='disabled')
    btn.pack(side='left', padx=(0, 8))
    holder['btn'] = btn
    _btn(row, _('Cancel'), dlg.destroy, kind='ghost').pack(side='left')
    _gate()


def _open_testpatch_dialog(app, afs):
    """Gated 'Unconfirmed Test Patch' dialog. Lets the user select candidates,
    requires three acknowledgements, and creates a PATCHED COPY (never touches
    the original). STRUCTURAL is offered as 'test patch available' — never
    called a confirmed fix.
    """
    res = getattr(app, '_afx_last_result', None)
    if not res:
        return
    cands = []
    for fr in res['files']:
        for c in fr['candidates']:
            if c['state'] == 'unpatched' and c['confidence'] in ('HIGH', 'STRUCTURAL', 'MEDIUM'):
                cands.append({'file_path': c['file_path'], 'file_name': c['file_name'], 'offset': c['offset'], 'offset_hex': c['offset_hex'], 'confidence': c['confidence'], 'leading_byte': c['leading_byte']})
    if not cands:
        app_info(app, _('No patchable candidates'), _('There are no unpatched candidate fields to create a test patch from.'))
        return
    dump_folder = app._afx_folder_var.get().strip()
    dlg = tk.Toplevel(app)
    dlg.title(_('Unconfirmed Test Patch'))
    dlg.configure(bg=COLORS['bg_1'])
    dlg.transient(app)
    dlg.grab_set()
    dlg.geometry('640x620')
    pad = tk.Frame(dlg, bg=COLORS['bg_1'])
    pad.pack(fill='both', expand=True, padx=16, pady=14)
    tk.Label(pad, text=_('Unconfirmed Test Patch'), font=(FONTS['body_strong'][0], 14), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w')
    warn = tk.Frame(pad, bg=COLORS.get('teal_bg', COLORS['bg_2']), highlightbackground=COLORS.get('amber', COLORS['border_2']), highlightthickness=1)
    warn.pack(fill='x', pady=(8, 12))
    tk.Label(warn, text=_('⚠  LEGACY TEST PATCH — UNCONFIRMED. These are the historical 0C F1 63 2D fields. Patching exactly this pattern did NOT fix audio on console (DBS), and these fields were ABSENT from the DBS real working diff. This is a LEGACY MARKER, not a confirmed fix. Use the channel-config scan for confirmed-profile detection. The original file is never modified — a patched COPY is created. Only proceed to gather your own console evidence.'), font=FONTS['mono_sm'], bg=COLORS.get('teal_bg', COLORS['bg_2']), fg=COLORS['fg_1'], justify='left', wraplength=580).pack(anchor='w', padx=10, pady=8)
    tk.Label(pad, text=_('Available test patches:'), font=(FONTS['body_strong'][0], 10), bg=COLORS['bg_1'], fg=COLORS['fg_3']).pack(anchor='w')
    tk.Label(pad, text=_('STRUCTURAL = relocated profile match (selected). MEDIUM = heuristic only (off by default).'), font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_5']).pack(anchor='w', pady=(0, 4))
    list_card = tk.Frame(pad, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    list_card.pack(fill='both', expand=True, pady=(0, 10))
    cand_vars = []
    for c in cands:
        default_on = c['confidence'] in ('HIGH', 'STRUCTURAL')
        v = tk.BooleanVar(value=default_on)
        cand_vars.append((v, c))
        row = tk.Frame(list_card, bg=COLORS['bg_2'])
        row.pack(fill='x', padx=8, pady=1)
        XCheck(row, text=f"{c['confidence']!s:11} {c['offset_hex']!s}   {c['leading_byte'][2:]!s} → 08   ({c['file_name']!s})", variable=v, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_2'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0, anchor='w').pack(anchor='w', fill='x')
    tk.Label(pad, text=_('Required acknowledgements:'), font=(FONTS['body_strong'][0], 10), bg=COLORS['bg_1'], fg=COLORS['fg_3']).pack(anchor='w')
    ack1 = tk.BooleanVar(value=False)
    ack2 = tk.BooleanVar(value=False)
    ack3 = tk.BooleanVar(value=False)
    create_btn_holder = {}
    def _update_gate(*_a):
        ok = ack1.get() and (ack2.get() and (ack3.get() and any(v.get() for v, _c in cand_vars)))
        b = create_btn_holder.get('btn')
        if b is not None:
            try:
                b.configure(state='normal' if ok else 'disabled')
            except Exception:
                pass
    for var, label in (ack1, _('I understand this is unconfirmed and may not fix audio')), (ack2, _('Create a .bak backup first')), (ack3, _('Patch a COPY, not the original')):
        XCheck(pad, text=label, variable=var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_1'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0, command=_update_gate).pack(anchor='w', pady=1)
    status_var = tk.StringVar(value='')
    tk.Label(pad, textvariable=status_var, font=FONTS['meta'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left', wraplength=580).pack(anchor='w', pady=(6, 0))
    btn_row = tk.Frame(pad, bg=COLORS['bg_1'])
    btn_row.pack(fill='x', pady=(10, 0))
    def _do_create():
        from ui import audio_fix_patch as PP
        selected = [c for v, c in cand_vars if v.get()]
        if not selected:
            return
        by_file = {}
        for c in selected:
            by_file.setdefault(c['file_path'], []).append(int(c['offset']))
        if not dump_folder or not os.path.isdir(dump_folder):
            status_var.set(_('Dump folder is not valid.'))
        else:
            results = []
            for fpath, offs in by_file.items():
                r = PP.create_test_patch(dump_folder, fpath, sorted(offs))
                results.append(r)
            app._afx_testpatch_results = results
            total_patched = sum(r['patched_count'] for r in results)
            total_refused = sum(r['refused_count'] for r in results)
            untouched = all(r['original_untouched'] for r in results)
            copies = [r['copy'] for r in results if r.get('copy')]
            msg = _('Test patch complete.\nPatched sites: %d   Refused: %d\nOriginal(s) untouched: %s\nPatched copy/copies:\n%s') % (total_patched, total_refused, _('YES') if untouched else _('NO (!)'), '\n'.join('  ' + c for c in copies))
            status_var.set(msg)
            if app_confirm(app, _('Export patch report?'), _('Save a TEST PATCH report (unconfirmed) for the console test?')):
                path = filedialog.asksaveasfilename(title=_('Export patch report'), defaultextension='.txt', filetypes=[('Text', '*.txt'), ('JSON', '*.json')])
                if path:
                    try:
                        if path.lower().endswith('.json'):
                            content = '[\n' + ',\n'.join(PP.export_patch_report_json(r) for r in results) + '\n]'
                        else:
                            content = '\n\n============================================================\n\n'.join(PP.export_patch_report_txt(r) for r in results)
                        with open(path, 'w', encoding='utf-8') as f:
                            f.write(content)
                        status_var.set(status_var.get() + '\n' + _('Report exported: ') + path)
                    except Exception as e:
                        status_var.set(_('Report export failed: ') + str(e))
    create_btn = _btn(btn_row, _('Create Patched Copy'), _do_create, kind='primary')
    create_btn.configure(state='disabled')
    create_btn.pack(side='left', padx=(0, 8))
    create_btn_holder['btn'] = create_btn
    _btn(btn_row, _('Cancel'), dlg.destroy, kind='ghost').pack(side='left')
    for v, _c in cand_vars:
        v.trace_add('write', _update_gate)
    _update_gate()


def _build_switcher_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])
    from ui import backport_profiles as bpm
    if not hasattr(app, '_bsw_mode_var'):
        app._bsw_mode_var = tk.StringVar(value='build')
    if not hasattr(app, '_bsw_folder_var'):
        app._bsw_folder_var = tk.StringVar(value='')
    if not hasattr(app, '_bsw_target_var'):
        app._bsw_target_var = tk.StringVar(value='4')
    if not hasattr(app, '_bsw_state_var'):
        app._bsw_state_var = tk.StringVar(value='Select a game folder.')
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=16, pady=12)
    tk.Label(wrap, text='🔄  ' + _('Backport Target Switcher'), font=(FONTS['body_strong'][0], 14), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w')
    tk.Label(wrap, text=_('Safely build, swap, or remove a backport. Swap and Remove always restore verified original files first — no patching over patched files.'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left', wraplength=720).pack(anchor='w', pady=(2, 12))
    mode_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    mode_card.pack(fill='x', pady=(0, 10))
    tk.Label(mode_card, text=_('Mode'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    for val, lab in ('build', _('Build new backport from clean / original files')), ('swap', _('Swap existing backport to another target')), ('remove', _('Remove backport / restore defaults')):
        XRadio(mode_card, text=lab, value=val, variable=app._bsw_mode_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_2'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0, command=(lambda: _bsw_refresh_state(app, bpm))).pack(anchor='w', padx=14, pady=1)
    tk.Frame(mode_card, bg=COLORS['bg_2'], height=6).pack()
    fr = tk.Frame(wrap, bg=COLORS['bg_1'])
    fr.pack(fill='x', pady=(0, 8))
    tk.Label(fr, text=_('Game folder / image:'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_3']).pack(side='left', padx=(0, 8))
    ent = XEntry(fr, textvariable=app._bsw_folder_var, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
    ent.pack(side='left', fill='x', expand=True, padx=(0, 8))
    def _browse():
        d = filedialog.askdirectory(title=_('Select the PS5 game root folder'))
        if d:
            app._bsw_folder_var.set(d)
            _bsw_refresh_state(app, bpm)
    _btn(fr, _('Browse…'), _browse, kind='ghost').pack(side='left')
    state_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    state_card.pack(fill='x', pady=(2, 10))
    tk.Label(state_card, text=_('Current detected state'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    tk.Label(state_card, textvariable=app._bsw_state_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], justify='left', anchor='w', wraplength=720).pack(anchor='w', fill='x', padx=12, pady=(0, 10))
    tr = tk.Frame(wrap, bg=COLORS['bg_1'])
    tr.pack(fill='x', pady=(0, 8))
    tk.Label(tr, text=_('New target firmware:'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_3']).pack(side='left', padx=(0, 8))
    for val, lab in ('4', '4.xx'), ('5', '5.xx'), ('7', '7.xx'), ('10', '10.xx'):
        XRadio(tr, text=lab, value=val, variable=app._bsw_target_var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'], activebackground=COLORS['bg_1'], activeforeground=COLORS['teal'], highlightthickness=0, bd=0).pack(side='left', padx=(0, 10))
    prev_card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    prev_card.pack(fill='both', expand=True, pady=(2, 10))
    tk.Label(prev_card, text=_('Preview (dry-run — nothing is changed)'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=10, pady=(8, 2))
    prev_txt = tk.Text(prev_card, height=9, bg=COLORS['bg_3'], fg=COLORS['fg_2'], font=FONTS['mono_sm'], relief='flat', wrap='word', highlightthickness=0)
    prev_txt.pack(fill='both', expand=True, padx=10, pady=(0, 10))
    prev_txt.configure(state='disabled')
    app._bsw_prev_txt = prev_txt
    btns = tk.Frame(wrap, bg=COLORS['bg_1'])
    btns.pack(fill='x', pady=(2, 0))
    def _set_preview(text):
        try:
            prev_txt.configure(state='normal')
            prev_txt.delete('1.0', 'end')
            prev_txt.insert('1.0', text)
            prev_txt.configure(state='disabled')
        except Exception:
            pass
    app._bsw_set_preview = _set_preview
    def _do_preview():
        folder = app._bsw_folder_var.get().strip()
        if not folder or not os.path.isdir(folder):
            _set_preview(_('Select a valid game folder first.'))
        else:
            mode = app._bsw_mode_var.get()
            target = app._bsw_target_var.get() if mode in ('build', 'swap') else None
            try:
                tid = _bsw_title_id(folder)
                base_dir = _bsw_base_dir(app, folder)
                plan = bpm.preview_changes(mode, folder, tid, target, base_dir=base_dir)
            except Exception as e:
                _set_preview(_('Preview error: ') + str(e))
                return
            lines = []
            lines.append('Mode: %s' % mode)
            if plan.get('new_target'):
                lines.append('New target: %s' % plan['new_target'])
            st = plan.get('state', {})
            lines.append('Managed: %s' % ('yes' if st.get('managed') else 'no (unmanaged)'))
            if st.get('current_target'):
                lines.append('Current target: %s' % st['current_target'])
            lines.append('Backup verified: %s' % ('yes' if st.get('backup_verified') else 'no'))
            lines.append('')
            lines.append('Planned steps:')
            for i, s in enumerate(plan.get('steps', []), 1):
                lines.append('  %d. %s' % (i, s))
            if plan.get('blockers'):
                lines.append('')
                lines.append('⚠ BLOCKERS (will refuse):')
                for b in plan['blockers']:
                    lines.append('  • ' + b)
            lines.append('')
            lines.append('Result: %s' % ('SAFE to run' if plan.get('safe') else 'BLOCKED — resolve blockers first'))
            _set_preview('\n'.join(lines))
    _btn(btns, '👁  ' + _('Preview Changes'), _do_preview, kind='ghost').pack(side='left', padx=(0, 8))
    _btn(btns, '▶  ' + _('Start'), lambda: _bsw_start(app, bpm), kind='primary').pack(side='left')
    _bsw_refresh_state(app, bpm)


def _bsw_title_id(folder):
    try:
        _t, tid, _v = get_game_info(folder)
        return tid or ''
    except Exception:
        return ''


def _bsw_base_dir(app, folder):
    try:
        od = (app._settings.get('abp_zip_orig_dest', '') or '').strip()
        if od and os.path.isdir(od):
            return os.path.join(od, 'BackportBackup')
    except Exception:
        pass
    return os.path.join(os.path.dirname(os.path.normpath(folder)), 'BackportBackup')


def _bsw_refresh_state(app, bpm):
    folder = app._bsw_folder_var.get().strip()
    if not folder or not os.path.isdir(folder):
        app._bsw_state_var.set(_('Select a valid game folder.'))
    else:
        try:
            tid = _bsw_title_id(folder)
            base_dir = _bsw_base_dir(app, folder)
            st = bpm.detect_state(folder, tid, base_dir=base_dir)
        except Exception as e:
            app._bsw_state_var.set(_('State error: ') + str(e))
            return
        if not st['managed']:
            msg = _('Unmanaged backport (no manifest).') + '\n' + _('Target: unknown · Manifest: not found · ') + _('Target switching requires a clean source + Build. ') + _('Use Legacy Restore (Auto Backport tab) for manual restore.')
        else:
            msg = _('Target: %s') % (st.get('current_target') or _('clean/none')) + '\n' + _('Manifest: found · Originals backup: %s · Restore safety: %s') % (_('present') if st.get('originals_present') else _('missing'), _('OK') if st.get('backup_verified') else _('FAILED'))
            if not st.get('backup_verified'):
                msg += '\n⚠ ' + st.get('verify_reason', '')
        app._bsw_state_var.set(msg)


def _bsw_start(app, bpm):
    """Run the selected mode. Swap/Remove enforce restore->verify first.

    Heavy lifting (patching) is delegated to the existing engine
    (_abp_run via the Auto Backport tab); here we enforce the safe ordering
    and the manifest bookkeeping. To keep this change self-contained and
    safe, Start performs the NON-patching safe operations directly (restore,
    verify, manifest update) and, for steps that require patching, routes the
    user to the Auto Backport tab with the folder/target pre-filled.
    """
    folder = app._bsw_folder_var.get().strip()
    if not folder or not os.path.isdir(folder):
        app_warning(app, _('No folder'), _('Select a valid game folder first.'))
        return
    mode = app._bsw_mode_var.get()
    target = app._bsw_target_var.get()
    tid = _bsw_title_id(folder)
    base_dir = _bsw_base_dir(app, folder)
    plan = bpm.preview_changes(mode, folder, tid, target if mode in ('build', 'swap') else None, base_dir=base_dir)
    if not plan.get('safe'):
        app_error(app, _('Cannot proceed'), _('This operation is blocked:\n\n') + '\n'.join('• ' + b for b in plan.get('blockers', [])))
        return
    if not app_confirm(app, _('Confirm %s') % mode, _('About to run "%s" on:\n%s\n\nProceed?') % (mode, folder)):
        return
    mpath = bpm.manifest_path_for(folder, tid, base_dir=base_dir)
    manifest = bpm.read_manifest(mpath)
    def _log(m):
        try:
            app._abp_log_line(m.rstrip('\n'), 'info')
        except Exception:
            pass
    if mode == 'remove':
        ok, reason, _n = bpm.restore_originals(manifest, folder, base_dir=base_dir, log=_log)
        if not ok:
            app_error(app, _('Restore refused'), reason)
            return
        cok, creason, _mm = bpm.verify_clean(manifest, folder)
        manifest['current_target'] = None
        bpm.write_manifest(mpath, manifest)
        _bsw_refresh_state(app, bpm)
        app_info(app, _('Backport removed'), _('Originals restored and managed patched state cleared.\n%s') % (creason if cok else _('Note: ') + creason))
        return
    if mode == 'swap':
        ok, reason, _n = bpm.restore_originals(manifest, folder, base_dir=base_dir, log=_log)
        if not ok:
            app_error(app, _('Restore refused'), reason)
            return
        cok, creason, _mm = bpm.verify_clean(manifest, folder)
        if not cok and not app_confirm(app, _('Clean check warning'), creason + '\n\n' + _('Continue patching anyway?')):
            return
        bpm.write_manifest(mpath, manifest)
    try:
        app._abp_game_var.set(folder)
    except Exception:
        pass
    try:
        app._abp_sdk_var.set(str(target))
    except Exception:
        pass
    try:
        app._bp_subtab_activate('auto')
    except Exception:
        pass
    app_info(app, _('Ready to patch'), _('Originals are verified/clean and the Auto Backport tab is pre-filled with this folder and target %s.\n\nClick "Run" there to apply the target. A managed manifest will be written automatically on completion.') % bpm.normalize_fw(target))


def _build_sdkfixer_subtab(parent, app):
    parent.configure(bg=COLORS['bg_1'])
    if not hasattr(app, '_sdkfix_src_var'):
        app._sdkfix_src_var = tk.StringVar()
    if not hasattr(app, '_sdkfix_tgt_var'):
        app._sdkfix_tgt_var = tk.StringVar(value='5')
    if not hasattr(app, '_sdkfix_out_mode_var'):
        app._sdkfix_out_mode_var = tk.StringVar(value='copy')
    if not hasattr(app, '_sdkfix_status_var'):
        app._sdkfix_status_var = tk.StringVar(value='')
    app._sdkfix_running = getattr(app, '_sdkfix_running', False)
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)
    pad = 24
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=pad, pady=(16, 4))
    tk.Label(head, text='🔧  ' + _('SDK Fixer'), font=(FONTS['body_strong'][0], 15), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(side='left')
    tk.Label(head, text=_('Change only the SDK marker in a folder’s executables.'), font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_4']).pack(side='left', padx=(12, 0))
    warn = tk.Frame(body, bg=COLORS.get('bg_2', '#1a1426'), highlightbackground=COLORS.get('warn', '#d6a13a'), highlightthickness=1)
    warn.pack(fill='x', padx=pad, pady=(6, 12))
    tk.Label(warn, text=_('⚠ SDK Fixer only changes SDK markers. It does not apply normal backport patches, fakelib, or libc patches. Use this only when the game already works and only the SDK marker needs changing.'), font=FONTS['body'], bg=COLORS.get('bg_2', '#1a1426'), fg=COLORS.get('warn', '#d6a13a'), wraplength=860, justify='left').pack(anchor='w', padx=12, pady=8)
    _sdkfix_label(body, _('Source folder'), _('Backport files — eboot.bin / .prx / .sprx / sce_module / fakelib. param.json is not required.'), pad)
    srow = tk.Frame(body, bg=COLORS['bg_1'])
    srow.pack(fill='x', padx=pad, pady=(0, 10))
    ent = XEntry(srow, textvariable=app._sdkfix_src_var, font=FONTS['body'], bg=COLORS['bg_0'], fg=COLORS['fg_1'], insertbackground=COLORS.get('accent', '#b07ad6'), relief='flat', highlightthickness=1, highlightbackground=COLORS.get('border_3', '#2c2438'))
    ent.pack(side='left', fill='x', expand=True, ipady=6, padx=(0, 8))
    ent.bind('<FocusOut>', lambda e: app._sdkfix_scan_folder())
    ent.bind('<Return>', lambda e: app._sdkfix_scan_folder())
    _sdkfix_btn(srow, _('Browse'), app._sdkfix_browse_src, primary=False).pack(side='left')
    strip = tk.Frame(body, bg=COLORS['bg_0'], highlightbackground=COLORS.get('border_2', '#241d33'), highlightthickness=1)
    strip.pack(fill='x', padx=pad, pady=(0, 12))
    inner = tk.Frame(strip, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=14, pady=10)
    def _cell(col_parent, caption):
        c = tk.Frame(col_parent, bg=COLORS['bg_0'])
        c.pack(side='left', expand=True, fill='x')
        tk.Label(c, text=caption, font=(FONTS['body'][0], 9), bg=COLORS['bg_0'], fg=COLORS['fg_4']).pack(anchor='w')
        v = tk.Label(c, text='—', font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_0'], fg=COLORS['fg_1'])
        v.pack(anchor='w')
        return v
    app._sdkfix_det_type = _cell(inner, _('FOLDER TYPE'))
    app._sdkfix_det_sdk = _cell(inner, _('DETECTED SDK'))
    app._sdkfix_det_files = _cell(inner, _('FILES TO PATCH'))
    _sdkfix_label(body, _('Target SDK'), _('4 = firmware 4.xx · 5 = firmware 5.xx · 7 = firmware 7.xx, etc.'), pad)
    trow = tk.Frame(body, bg=COLORS['bg_1'])
    trow.pack(fill='x', padx=pad, pady=(0, 12))
    om = tk.OptionMenu(trow, app._sdkfix_tgt_var, *[str(n) for n in range(1, 11)])
    try:
        om.config(font=FONTS['body'], bg=COLORS['bg_0'], fg=COLORS['fg_1'], activebackground=COLORS.get('bg_2', '#1a1426'), activeforeground=COLORS['fg_0'], relief='flat', highlightthickness=1, highlightbackground=COLORS.get('border_3', '#2c2438'), width=6)
        om['menu'].config(bg=COLORS['bg_0'], fg=COLORS['fg_1'])
    except Exception:
        pass
    om.pack(side='left')
    _sdkfix_label(body, _('Output mode'), '', pad)
    orow = tk.Frame(body, bg=COLORS['bg_1'])
    orow.pack(fill='x', padx=pad, pady=(0, 12))
    for val, label in ('copy', _('Save as new copy (safe, default)')), ('overwrite', _('Overwrite selected folder')):
        rb = XRadio(orow, text=label, value=val, variable=app._sdkfix_out_mode_var, font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_1'], selectcolor=COLORS['bg_0'], activebackground=COLORS['bg_1'], activeforeground=COLORS['fg_0'], highlightthickness=0, anchor='w')
        rb.pack(side='left', padx=(0, 18))
    arow = tk.Frame(body, bg=COLORS['bg_1'])
    arow.pack(fill='x', padx=pad, pady=(4, 8))
    _sdkfix_btn(arow, '🔍  ' + _('Preview (dry-run)'), app._sdkfix_preview, primary=False).pack(side='left')
    app._sdkfix_run_btn = _sdkfix_btn(arow, '⚡  ' + _('Run SDK Fix'), app._sdkfix_run, primary=True)
    app._sdkfix_run_btn.pack(side='left', padx=(10, 0))
    tk.Label(body, textvariable=app._sdkfix_status_var, font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS.get('accent', '#b07ad6')).pack(anchor='w', padx=pad, pady=(8, 2))
    prog = tk.Canvas(body, height=12, bg=COLORS['bg_0'], highlightthickness=0)
    prog.pack(fill='x', padx=pad, pady=(0, 6))
    app._sdkfix_prog_cv = prog
    app._sdkfix_prog_rect = prog.create_rectangle(0, 0, 0, 12, fill=COLORS.get('accent', '#b07ad6'), width=0)
    tk.Label(body, text=_('Detailed output appears in the OUTPUT LOG at the bottom of the window.'), font=(FONTS['body'][0], 9), bg=COLORS['bg_1'], fg=COLORS['fg_4']).pack(anchor='w', padx=pad, pady=(0, 18))


def _build_batchsdk_subtab(parent, app):
    """Batch SDK Retag — patch every backport ZIP in a source folder to a
    chosen target SDK, writing retagged copies to an output folder. Reuses
    the same SDK-only pipeline as SDK Fixer (fakelib OFF, libc patch OFF).
    Source folder is never modified. Only the 'FW Nxx' filename tag changes.
    """
    parent.configure(bg=COLORS['bg_1'])
    if not hasattr(app, '_batchsdk_src_var'):
        app._batchsdk_src_var = tk.StringVar()
    if not hasattr(app, '_batchsdk_out_var'):
        app._batchsdk_out_var = tk.StringVar()
    if not hasattr(app, '_batchsdk_tgt_var'):
        app._batchsdk_tgt_var = tk.StringVar(value='6')
    if not hasattr(app, '_batchsdk_status_var'):
        app._batchsdk_status_var = tk.StringVar(value='')
    app._batchsdk_running = getattr(app, '_batchsdk_running', False)
    pad = 24
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=pad, pady=(16, 4))
    tk.Label(head, text='📦  ' + _('Batch SDK Retag'), font=(FONTS['body_strong'][0], 15), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(side='left')
    tk.Label(head, text=_('Change the SDK marker across a whole folder of backport zips.'), font=FONTS['body'], bg=COLORS['bg_1'], fg=COLORS['fg_4']).pack(side='left', padx=(12, 0))
    warn = tk.Frame(body, bg=COLORS.get('bg_2', '#1a1426'), highlightbackground=COLORS.get('warn', '#d6a13a'), highlightthickness=1)
    warn.pack(fill='x', padx=pad, pady=(6, 12))
    tk.Label(warn, text=_('⚠ Batch SDK Retag only changes SDK markers inside each zip’s eboot. It does not apply normal backport patches, fakelib, or libc patches. The source folder is never modified — retagged copies are written to the output folder.'), font=FONTS['body'], bg=COLORS.get('bg_2', '#1a1426'), fg=COLORS.get('warn', '#d6a13a'), wraplength=860, justify='left').pack(anchor='w', padx=12, pady=8)
    _sdkfix_label(body, _('Source folder (backport zips)'), _('A folder containing backport zips, e.g. “FW 7xx PPSA21382 Tomba! (01.003.000) backport files.zip”.'), pad)
    srow = tk.Frame(body, bg=COLORS['bg_1'])
    srow.pack(fill='x', padx=pad, pady=(0, 10))
    sent = XEntry(srow, textvariable=app._batchsdk_src_var, font=FONTS['body'], bg=COLORS['bg_0'], fg=COLORS['fg_1'], insertbackground=COLORS.get('accent', '#b07ad6'), relief='flat', highlightthickness=1, highlightbackground=COLORS.get('border_3', '#2c2438'))
    sent.pack(side='left', fill='x', expand=True, ipady=6, padx=(0, 8))
    sent.bind('<Return>', lambda e: app._batchsdk_scan())
    _sdkfix_btn(srow, _('Browse'), app._batchsdk_browse_src, primary=False).pack(side='left')
    _sdkfix_label(body, _('Output folder'), _('Retagged zips are written here. Must be different from the source folder.'), pad)
    orow = tk.Frame(body, bg=COLORS['bg_1'])
    orow.pack(fill='x', padx=pad, pady=(0, 10))
    oent = XEntry(orow, textvariable=app._batchsdk_out_var, font=FONTS['body'], bg=COLORS['bg_0'], fg=COLORS['fg_1'], insertbackground=COLORS.get('accent', '#b07ad6'), relief='flat', highlightthickness=1, highlightbackground=COLORS.get('border_3', '#2c2438'))
    oent.pack(side='left', fill='x', expand=True, ipady=6, padx=(0, 8))
    _sdkfix_btn(orow, _('Browse'), app._batchsdk_browse_out, primary=False).pack(side='left')
    _sdkfix_label(body, _('Target SDK'), _('Any direction. 6 = firmware 6.xx, 7 = firmware 7.xx, etc. Each zip’s filename “FW Nxx” tag is updated to match.'), pad)
    trow = tk.Frame(body, bg=COLORS['bg_1'])
    trow.pack(fill='x', padx=pad, pady=(0, 8))
    om = tk.OptionMenu(trow, app._batchsdk_tgt_var, *[str(n) for n in range(1, 11)])
    try:
        om.config(font=FONTS['body'], bg=COLORS['bg_0'], fg=COLORS['fg_1'], activebackground=COLORS.get('bg_2', '#1a1426'), activeforeground=COLORS['fg_0'], relief='flat', highlightthickness=1, highlightbackground=COLORS.get('border_3', '#2c2438'), width=6)
        om['menu'].config(bg=COLORS['bg_0'], fg=COLORS['fg_1'])
    except Exception:
        pass
    om.pack(side='left')
    arow = tk.Frame(body, bg=COLORS['bg_1'])
    arow.pack(fill='x', padx=pad, pady=(4, 8))
    _sdkfix_btn(arow, '🔍  ' + _('Scan'), app._batchsdk_scan, primary=False).pack(side='left')
    app._batchsdk_run_btn = _sdkfix_btn(arow, '⚡  ' + _('Run Batch Retag'), app._batchsdk_run, primary=True)
    app._batchsdk_run_btn.pack(side='left', padx=(10, 0))
    _sdkfix_label(body, _('Preview'), _('Detected current SDK and the planned action per zip. Scan first to populate.'), pad)
    tblf = tk.Frame(body, bg=COLORS['bg_0'], highlightbackground=COLORS.get('border_2', '#241d33'), highlightthickness=1)
    tblf.pack(fill='both', expand=True, padx=pad, pady=(0, 10))
    cols = 'zip', 'cur', 'action'
    tree = ttk.Treeview(tblf, columns=cols, show='headings', height=8)
    tree.heading('zip', text=_('Zip'))
    tree.heading('cur', text=_('Current SDK'))
    tree.heading('action', text=_('Status'))
    tree.column('zip', width=420, anchor='w')
    tree.column('cur', width=110, anchor='center')
    tree.column('action', width=230, anchor='w')
    vsb = ttk.Scrollbar(tblf, orient='vertical', command=tree.yview)
    tree.configure(yscrollcommand=vsb.set)
    tree.pack(side='left', fill='both', expand=True)
    vsb.pack(side='right', fill='y')
    app._batchsdk_tree = tree
    tk.Label(body, textvariable=app._batchsdk_status_var, font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_1'], fg=COLORS.get('accent', '#b07ad6')).pack(anchor='w', padx=pad, pady=(4, 2))
    prog = tk.Canvas(body, height=12, bg=COLORS['bg_0'], highlightthickness=0)
    prog.pack(fill='x', padx=pad, pady=(0, 6))
    app._batchsdk_prog_cv = prog
    app._batchsdk_prog_rect = prog.create_rectangle(0, 0, 0, 12, fill=COLORS.get('accent', '#b07ad6'), width=0)
    logf = tk.Frame(body, bg=COLORS['bg_0'], highlightbackground=COLORS.get('border_2', '#241d33'), highlightthickness=1)
    logf.pack(fill='both', expand=True, padx=pad, pady=(0, 18))
    logw = tk.Text(logf, height=7, bg=COLORS['bg_0'], fg=COLORS['fg_1'], relief='flat', wrap='word', font=(FONTS['body'][0], 9), state='disabled', insertbackground=COLORS.get('accent', '#b07ad6'))
    lvsb = ttk.Scrollbar(logf, orient='vertical', command=logw.yview)
    logw.configure(yscrollcommand=lvsb.set)
    logw.pack(side='left', fill='both', expand=True)
    lvsb.pack(side='right', fill='y')
    app._batchsdk_log_widget = logw


def _sdkfix_label(parent, title, sub, pad):
    box = tk.Frame(parent, bg=COLORS['bg_1'])
    box.pack(fill='x', padx=pad, pady=(2, 2))
    tk.Label(box, text=title, font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_1'], fg=COLORS['fg_1']).pack(anchor='w')
    if sub:
        tk.Label(box, text=sub, font=(FONTS['body'][0], 9), bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left').pack(anchor='w')


def _sdkfix_btn(parent, text, cmd, primary=False):
    bg = COLORS.get('accent', '#b07ad6') if primary else COLORS.get('bg_2', '#1a1426')
    fg = '#ffffff' if primary else COLORS['fg_1']
    b = XButton(parent, text=text, command=cmd, font=FONTS['body'], bg=bg, fg=fg, activebackground=bg, activeforeground=fg, relief='flat', bd=0, padx=16, pady=8, cursor='hand2', highlightthickness=0)
    return b


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
        holder = tk.Frame(grid, bg=COLORS['bg_1'], height=300)
        holder.pack(fill='x', pady=20)
        holder.pack_propagate(False)
        EmptyState(holder, icon='\U0001f4e6',
                   title=_('No games scanned yet'),
                   description=_('Add a folder and click Scan to find '
                                 'PS5 game dumps.')
                   ).pack(fill='both', expand=True)
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
        holder = tk.Frame(grid, bg=COLORS['bg_1'], height=300)
        holder.pack(fill='x', pady=20)
        holder.pack_propagate(False)
        EmptyState(holder, icon='\U0001f50d',
                   title=_('No matches'),
                   description=_('No games match the current filter.')
                   ).pack(fill='both', expand=True)
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
        qbtn = XButton(arow, text='\u2713  ' + _('Queued'),
                         font=(FONTS['body_strong'][0], 9),
                         bg=COLORS.get('purple', '#9b59b6'),
                         fg=COLORS['fg_0'],
                         activebackground=COLORS.get('purple_hi',
                                                     '#c891e0'),
                         activeforeground=COLORS['fg_0'],
                         relief='flat', bd=0, padx=12, pady=5,
                         cursor='hand2',
                         command=lambda g=game: _toggle_queue(app, g))
    else:
        qbtn = XButton(arow, text='+  ' + _('Queue'),
                         font=(FONTS['body_strong'][0], 9),
                         bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                         activebackground=COLORS['bg_4'],
                         activeforeground=COLORS['fg_0'],
                         relief='flat', bd=0, padx=12, pady=5,
                         cursor='hand2',
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1,
                         command=lambda g=game: _toggle_queue(app, g))
    qbtn.pack(side='left')
    XButton(arow, text='\u26a1  ' + _('Process'),
              font=(FONTS['body_strong'][0], 9),
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
                font=(FONTS['body_strong'][0], 9),
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
    if not app_confirm(app, _('Process backport'),
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
                 font=(FONTS['body_strong'][0], 9),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(anchor='w')
        v = tk.Label(colf, text='0',
                     font=(FONTS['body_strong'][0], 16),
                     bg=COLORS['bg_2'], fg=COLORS['fg_0'])
        v.pack(anchor='w')
        tk.Label(colf, text=sub,
                 font=(FONTS['body'][0], 9),
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
    if not app_confirm(app, _('Process backport queue'),
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
    btn = XButton(parent, text=text,
                    font=(FONTS['body_strong'][0], 9),
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
