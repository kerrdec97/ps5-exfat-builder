"""
ui/panel_bp_auto.py — Auto Backport form panel.

Step 7 (v2.0.9): full refactor against backports-tab-redesign-standalone.html.

This is the right-pane "⚡ Auto Backport" content that lives inside the
Backports tab. The previous version (`App._build_bp_auto_panel`) was 271
lines of hex-literal-heavy widget code in the main file; this module
replaces it with a design-system-styled equivalent.

Layout (top to bottom):

    [Selected game banner]    ← purple-tinted card; hidden until selected
    [Description card]        ← purple-tinted "Auto Backport" intro
    [Game folder field]       ← path + Browse + Auto-detect SDK
    [Detected strip]          ← 3-cell: Detected SDK / Target SDK / Status
    [Originals zip field]     ← path + Browse + Clear
    [Backported zip field]    ← path + Browse + Clear
    [Target SDK + Fakelib]    ← ttk Combobox + radios
    [Action row]              ← purple Apply + warn Restore originals
    [Notes field]
    [Status + progress bar]
    [Output console]          ← terminal-styled ConsoleView

Backwards compat: every `app._abp_*` attribute the existing 18 callbacks
read or write is preserved with the same name. None of the worker logic
(`_abp_run`, `_abp_detect_sdk`, `_abp_browse_*`, etc.) is changed.

The panel exposes a public helper:
    update_selected_banner(app, game) — re-paints the top banner from a
    game dict. Called from `_bp_make_game_card._select` so picking a game
    in the left rail updates the banner.
"""

import os
import tkinter as tk
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

from ui.shared.hero import GameHero

# Star-import for `_` (i18n) and the legacy theme constants used by some
# inner widgets that we still touch via `app.*` attributes.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.log_view import ConsoleView


# ─────────────────────────────────────────────────────────────────────────
# Public entry — build the whole form into `parent`
# ─────────────────────────────────────────────────────────────────────────
def build_bp_auto_panel(parent, app):
    """Build the Auto Backport form into `parent` (a tk.Frame).

    `app` is the ExFATBuilder instance. All form state and widgets are
    attached to `app` under `_abp_*` names matching the legacy contract.
    """
    parent.configure(bg=COLORS['bg_1'])

    # ── State (StringVars + flags) ──
    app._abp_game_var      = tk.StringVar()
    app._abp_output_var    = tk.StringVar(
        value=app._settings.get('abp_output', ''))
    app._abp_sdk_var       = tk.StringVar(value='4')
    app._abp_status_var    = tk.StringVar(value='')
    app._abp_running       = False
    saved_fakelib          = app._settings.get('abp_fakelib', '')
    app._abp_fakelib_saved = saved_fakelib
    app._abp_zip_orig_var  = tk.StringVar(
        value=app._settings.get('abp_zip_orig_dest', ''))
    app._abp_zip_pat_var   = tk.StringVar(
        value=app._settings.get('abp_zip_pat_dest', ''))
    app._abp_notes_var     = tk.StringVar(
        value=app._settings.get('abp_notes', ''))
    app._abp_fakelib_var   = tk.StringVar(value='None')
    app._abp_fakelib_path  = tk.StringVar()
    # New StringVars introduced by the redesign — fed by detect_sdk.
    # Step 38 (v2.5.2): default to '—' instead of stale '4.51' so the
    # card doesn't lie about target FW before any detection has run.
    app._abp_detected_sdk_var = tk.StringVar(value='—')
    app._abp_target_fw_var    = tk.StringVar(value='—')
    app._abp_patches_var      = tk.StringVar(value='—')

    # Restore previously saved fakelib mode/path
    if saved_fakelib:
        app._abp_fakelib_path.set(saved_fakelib)
        if os.path.isfile(saved_fakelib):
            app._abp_fakelib_var.set('file')
        elif os.path.isdir(saved_fakelib):
            app._abp_fakelib_var.set('folder')

    # ── Build sections (top to bottom) ──
    _build_selected_banner(parent, app)
    _build_description_card(parent, app)
    _build_game_folder_field(parent, app)
    _build_detected_strip(parent, app)
    _build_zip_fields(parent, app)
    _build_sdk_and_fakelib(parent, app)
    _build_action_row(parent, app)
    _build_notes_field(parent, app)
    _build_status_and_progress(parent, app)
    _build_output_log(parent, app)

    # ── Settings persistence ──
    # Same as legacy: trace these vars so changes save automatically.
    app._abp_zip_orig_var.trace('w', lambda *a: (
        app._settings.update({'abp_zip_orig_dest': app._abp_zip_orig_var.get()}),
        save_settings(app._settings)))
    app._abp_zip_pat_var.trace('w', lambda *a: (
        app._settings.update({'abp_zip_pat_dest': app._abp_zip_pat_var.get()}),
        save_settings(app._settings)))
    app._abp_notes_var.trace('w', lambda *a: (
        app._settings.update({'abp_notes': app._abp_notes_var.get()}),
        save_settings(app._settings)))


# ─────────────────────────────────────────────────────────────────────────
# Selected game banner — top, purple-tinted, hidden until a game is picked
# ─────────────────────────────────────────────────────────────────────────
def _build_selected_banner(parent, app):
    """v3.6.0 pass: the slim purple banner became a Build-style
    GameHero — cover, title/PPSA, and a live stats strip (Detected SDK
    / Target Firmware / Patches / Status) fed by the existing detect
    vars. Hidden via pack_forget until a game is selected; the legacy
    `update_selected_banner(app, game)` entry point is unchanged.
    """
    hero = GameHero(parent,
                    stats=[('Detected SDK', 'sdk'),
                           ('Target Firmware', 'fw'),
                           ('Patches', 'patches'),
                           ('Status', 'status')],
                    cover_glyph='\U0001f3ae', cover_size=130)
    # Not packed here — update_selected_banner() shows it.
    hero.set_badge('NO GAME SELECTED', 'wait')
    hero.set_stat('status', 'Waiting')

    app._abp_banner = hero
    # Legacy write targets — kept as live StringVars that mirror into
    # the hero, so any old code that sets them still works.
    app._abp_banner_title = tk.StringVar(value='No game selected')
    app._abp_banner_meta  = tk.StringVar(value='')
    app._abp_banner_cover = tk.Label(hero)   # hidden legacy target
    app._abp_banner_fw_badge = tk.Label(hero)

    # ── live stat mirrors ──
    def _mirror(*_a):
        try:
            hero.set_stat('sdk', app._abp_detected_sdk_var.get())
            hero.set_stat('fw', app._abp_target_fw_var.get())
            hero.set_stat('patches', app._abp_patches_var.get())
            sdk = app._abp_detected_sdk_var.get().strip()
            ready = bool(app._abp_game_var.get().strip()) and \
                sdk not in ('', '\u2014', '—')
            if ready:
                hero.set_stat('status', 'Ready')
                hero.set_badge('READY TO PATCH', 'ready')
            elif app._abp_game_var.get().strip():
                hero.set_stat('status', 'Detecting\u2026')
                hero.set_badge('DETECTING', 'busy')
        except Exception:
            pass
    for v in (app._abp_detected_sdk_var, app._abp_target_fw_var,
              app._abp_patches_var, app._abp_game_var):
        v.trace_add('write', _mirror)


def update_selected_banner(app, game):
    """Refresh the selected-game hero from a game dict.

    Same entry point as before (called from the games grid / queue
    runner). If `game` is None the hero is hidden; otherwise it's
    packed at the top of the form with the game's metadata + cover.
    """
    hero = getattr(app, '_abp_banner', None)
    if hero is None:
        return
    if not game:
        try:
            hero.pack_forget()
        except Exception:
            pass
        return

    try:
        first = hero.master.winfo_children()[0] \
            if hero.master.winfo_children() else None
        hero.pack(fill='x', pady=(0, 12),
                  before=first if (first is not None and first is not hero)
                  else None)
    except Exception:
        hero.pack(fill='x', pady=(0, 12))

    title = game.get('title', 'Unknown')
    ppsa = game.get('ppsa') or game.get('title_id', '')
    parts = []
    if game.get('version'):
        parts.append('v' + game['version'])
    sys_ver = game.get('system_ver') or game.get('SYSTEM_VER', 0)
    if game.get('size', 0) > 0:
        sz = game['size']
        parts.append('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))
    elif game.get('size_bytes', 0) > 0:
        sz = game['size_bytes']
        parts.append('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))

    hero.set_title(title, ' \u00b7 '.join(
        [x for x in [ppsa] + parts if x]))
    hero.set_path(game.get('folder', ''))
    if sys_ver:
        hero.set_stat('sdk', str(sys_ver))
    # Keep legacy vars in sync for any old reader
    try:
        app._abp_banner_title.set(title)
        app._abp_banner_meta.set(' \u00b7 '.join(
            [x for x in [ppsa] + parts if x]))
    except Exception:
        pass

    # Cover via the shared async loader (uses app._load_cover_art)
    try:
        hero.set_cover_from_folder(app, game.get('folder', ''))
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Description card — purple-tinted intro
# ─────────────────────────────────────────────────────────────────────────
def _build_description_card(parent, app):
    bg = '#0a0816'  # very dark purple-tinted
    border = '#2a1a3e'
    desc = tk.Frame(parent, bg=bg,
                    highlightbackground=border, highlightthickness=1)
    desc.pack(fill='x', pady=(0, 12))

    inner = tk.Frame(desc, bg=bg)
    inner.pack(fill='x', padx=14, pady=10)

    tk.Label(inner, text='\u26a1  Auto Backport',
             font=(FONTS['h3'][0], 11, 'bold'),
             bg=bg, fg=COLORS['purple'],
             anchor='w'
             ).pack(fill='x')
    tk.Label(inner,
             text=_('Automatically patches PS5 game ELF executables to run on '
                    'older firmware. Decrypts and re-signs .self/.sprx/.prx '
                    'files with a lower SDK target. Original files are backed '
                    'up before patching.'),
             font=FONTS['meta'],
             bg=bg, fg=COLORS['fg_5'],
             justify='left', anchor='w', wraplength=700
             ).pack(fill='x', pady=(4, 0))


# ─────────────────────────────────────────────────────────────────────────
# Game folder field with Browse + Auto-detect SDK
# ─────────────────────────────────────────────────────────────────────────
def _build_game_folder_field(parent, app):
    row = _field_row(parent, _('Game folder'),
                     hint=_('eboot.bin + sce_sys must be present'))

    entry = _dark_entry(row, app._abp_game_var, readonly=True)
    entry.pack(side='left', fill='x', expand=True)

    # Browse button (light, attached to right of entry)
    _ghost_btn(row, _('Browse'),
               command=app._abp_browse_game
               ).pack(side='left', padx=(6, 0))

    _ghost_btn(row, '\U0001f50d  ' + _('Auto-detect SDK'),
               command=app._abp_detect_sdk
               ).pack(side='left', padx=(6, 0))


# ─────────────────────────────────────────────────────────────────────────
# Detected strip — 3 cells: Detected SDK / Target FW / Patches
# ─────────────────────────────────────────────────────────────────────────
def _build_detected_strip(parent, app):
    """3-cell strip showing detection results. Replaces the legacy
    single-cell amber `_abp_sdk_banner` while preserving its name + var."""
    bg = COLORS['bg_3']
    border = COLORS['border_2']

    strip = tk.Frame(parent, bg=bg,
                     highlightbackground=border, highlightthickness=1)
    strip.pack(fill='x', pady=(0, 10))

    inner = tk.Frame(strip, bg=bg)
    inner.pack(fill='x', padx=14, pady=8)

    # Configure 3 equal-weight columns via grid
    inner.grid_columnconfigure(0, weight=1, uniform='det')
    inner.grid_columnconfigure(1, weight=1, uniform='det')
    inner.grid_columnconfigure(2, weight=1, uniform='det')

    def _cell(col, label_text, value_var, value_kind='neutral'):
        cell = tk.Frame(inner, bg=bg)
        cell.grid(row=0, column=col, sticky='ew', padx=2)
        tk.Label(cell, text=label_text.upper(),
                 font=(FONTS['eyebrow'][0], 8, 'bold'),
                 bg=bg, fg=COLORS['fg_5'],
                 anchor='w'
                 ).pack(fill='x')
        fg = {
            'ok':   COLORS['success_hi'],
            'warn': COLORS['warn_hi'],
            'err':  COLORS['danger_hi'],
            'neutral': COLORS['fg_1'],
        }.get(value_kind, COLORS['fg_1'])
        lbl = tk.Label(cell, textvariable=value_var,
                       font=(FONTS['mono'][0], 11, 'bold'),
                       bg=bg, fg=fg,
                       anchor='w')
        lbl.pack(fill='x', pady=(1, 0))
        return lbl

    _cell(0, _('Detected SDK'), app._abp_detected_sdk_var, value_kind='warn')
    _cell(1, _('Target FW'),    app._abp_target_fw_var,    value_kind='ok')
    _cell(2, _('Patches'),      app._abp_patches_var,      value_kind='ok')

    # Backwards-compat aliases — the existing _abp_detect_sdk callback
    # writes to _abp_sdk_info_var and packs/unpacks _abp_sdk_banner. Those
    # references must remain valid. We give them benign no-op-ish shims:
    # the banner becomes a hidden Frame, the info_var feeds nowhere visible
    # but its trace also updates _abp_detected_sdk_var.
    app._abp_sdk_banner = tk.Frame(parent, bg=bg)  # never packed; harmless
    app._abp_sdk_info_var = tk.StringVar(value='')
    app._abp_sdk_banner_lbl = tk.Label(app._abp_sdk_banner,
                                       textvariable=app._abp_sdk_info_var)

    # When the legacy var changes, parse "SDK X" / "FW X.xx+" / "(N patches)"
    # out of the message and update the new card vars. Step 38 (v2.5.2):
    # rewrote regexes to match the ACTUAL message shape produced by
    # _abp_detect_sdk in exfat_builder.py:
    #   "🔍  Requires FW 9.xx+ — Recommended backport target: SDK 9 or lower"
    # The previous regexes (`SDK \d+\.\d+` and `FW \d+\.\d+`) expected
    # decimals like "SDK 9.00" / "FW 4.51" that are never produced — so
    # the card permanently displayed the initial "4.51" default.
    def _sync(*_args):
        msg = app._abp_sdk_info_var.get()
        import re
        # Detected SDK: "SDK 9" or "SDK 9.00" — accept either form
        m = re.search(r'SDK\s+(\d+(?:\.\d+)?)', msg)
        if m:
            app._abp_detected_sdk_var.set(m.group(1))
        # Required FW: "FW 9.xx+" or "FW 4.51" — accept "X.xx+" too
        m = re.search(r'FW\s+(\d+(?:\.\w+)?\+?)', msg)
        if m:
            app._abp_target_fw_var.set(m.group(1))
        # Patch count (only set if present in message)
        m = re.search(r'\((\d+)\s+patches?\)', msg)
        if m:
            app._abp_patches_var.set('\u2713 ' + m.group(1) + ' known')
        # Reset card to "—" on warning/error messages so stale values
        # from a previous successful detect don't persist after a
        # failed re-detect on a different game.
        if msg.startswith('\u26a0'):  # ⚠ warning prefix
            app._abp_detected_sdk_var.set('—')
            app._abp_target_fw_var.set('—')
            app._abp_patches_var.set('—')
    app._abp_sdk_info_var.trace('w', _sync)


# ─────────────────────────────────────────────────────────────────────────
# Originals zip + Backported zip fields
# ─────────────────────────────────────────────────────────────────────────
def _build_zip_fields(parent, app):
    # Originals zip
    row = _field_row(parent, _('Originals zip'),
                     hint=_('backup of pre-patch files'))
    entry = _dark_entry(row, app._abp_zip_orig_var, readonly=False)
    entry.pack(side='left', fill='x', expand=True)
    _ghost_btn(row, _('Browse'),
               command=lambda: app._abp_browse_zip_dest('orig')
               ).pack(side='left', padx=(6, 0))
    _ghost_btn(row, _('Clear'),
               command=lambda: (app._abp_zip_orig_var.set(''),
                                app._settings.update({'abp_zip_orig_dest': ''}),
                                save_settings(app._settings))
               ).pack(side='left', padx=(4, 0))

    # Backported zip
    row = _field_row(parent, _('Backported zip'),
                     hint=_('output zip after patching'))
    entry = _dark_entry(row, app._abp_zip_pat_var, readonly=False)
    entry.pack(side='left', fill='x', expand=True)
    _ghost_btn(row, _('Browse'),
               command=lambda: app._abp_browse_zip_dest('patched')
               ).pack(side='left', padx=(6, 0))
    _ghost_btn(row, _('Clear'),
               command=lambda: (app._abp_zip_pat_var.set(''),
                                app._settings.update({'abp_zip_pat_dest': ''}),
                                save_settings(app._settings))
               ).pack(side='left', padx=(4, 0))

    tk.Label(parent,
             text=_('Leave both blank to be asked each time'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5'],
             anchor='w'
             ).pack(fill='x', pady=(0, 10))


# ─────────────────────────────────────────────────────────────────────────
# Target SDK combobox + Fakelib radio + path
# ─────────────────────────────────────────────────────────────────────────
def _build_sdk_and_fakelib(parent, app):
    # Target SDK row
    sdk_row = tk.Frame(parent, bg=COLORS['bg_1'])
    sdk_row.pack(fill='x', pady=(0, 8))
    tk.Label(sdk_row, text=_('Target SDK'),
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             width=14, anchor='w'
             ).pack(side='left')

    # ttk.Combobox respects the apply_theme styling already.
    sdk_options = ['4', '5', '6', '7', '8', '9', '10', '11']
    sdk_combo = ttk.Combobox(sdk_row, textvariable=app._abp_sdk_var,
                             values=sdk_options, state='readonly',
                             width=6)
    sdk_combo.pack(side='left')
    tk.Label(sdk_row,
             text=_('4 = firmware 4.xx  ·  5 = firmware 5.xx  ·  '
                    '7 = firmware 7.xx  etc.'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(10, 0))

    # Fakelib row
    fl_row = tk.Frame(parent, bg=COLORS['bg_1'])
    fl_row.pack(fill='x', pady=(0, 4))
    tk.Label(fl_row, text=_('Fakelib'),
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             width=14, anchor='w'
             ).pack(side='left')

    for val, lbl in [('none',   _('None')),
                     ('folder', _('Select folder')),
                     ('file',   _('Select file'))]:
        tk.Radiobutton(fl_row, text=lbl,
                       variable=app._abp_fakelib_var, value=val,
                       font=FONTS['body'],
                       bg=COLORS['bg_1'], fg=COLORS['fg_3'],
                       activebackground=COLORS['bg_1'],
                       activeforeground=COLORS['fg_1'],
                       selectcolor=COLORS['bg_3'],
                       cursor='hand2',
                       command=app._abp_fakelib_mode_change
                       ).pack(side='left', padx=(0, 12))

    # Fakelib path row — pack/forget controlled by callback
    app._abp_fakelib_path_row = tk.Frame(parent, bg=COLORS['bg_1'])
    fl_entry = _dark_entry(app._abp_fakelib_path_row,
                            app._abp_fakelib_path, readonly=True)
    fl_entry.pack(side='left', fill='x', expand=True)
    _ghost_btn(app._abp_fakelib_path_row, _('Browse'),
               command=app._abp_browse_fakelib
               ).pack(side='left', padx=(6, 0))
    _ghost_btn(app._abp_fakelib_path_row, _('Clear'),
               command=lambda: (app._abp_fakelib_path.set(''),
                                app._abp_fakelib_var.set('none'),
                                app._abp_fakelib_path_row.pack_forget())
               ).pack(side='left', padx=(4, 0))

    # If a saved fakelib path was restored, pack the row immediately
    if app._abp_fakelib_var.get() in ('file', 'folder'):
        app._abp_fakelib_path_row.pack(fill='x', pady=(2, 0))

    tk.Label(parent,
             text=_('Optional — provides .sprx/.prx stub libraries for the '
                    'target firmware'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5'],
             anchor='w'
             ).pack(fill='x', pady=(0, 10))


# ─────────────────────────────────────────────────────────────────────────
# Action row — purple Apply + warn Restore
# ─────────────────────────────────────────────────────────────────────────
def _build_action_row(parent, app):
    row = tk.Frame(parent, bg=COLORS['bg_1'])
    row.pack(fill='x', pady=(4, 8))

    app._abp_btn = tk.Button(row,
                             text='\u26a1  ' + _('Apply Auto Backport'),
                             font=(FONTS['button'][0], 11, 'bold'),
                             bg=COLORS['purple'], fg=COLORS['fg_0'],
                             activebackground=COLORS['purple_hi'],
                             activeforeground=COLORS['fg_0'],
                             relief='flat', bd=0,
                             padx=18, pady=10,
                             cursor='hand2',
                             command=app._abp_run)
    app._abp_btn.pack(side='left')

    tk.Button(row, text='\u21a9  ' + _('Restore originals'),
              font=(FONTS['button'][0], 10),
              bg=COLORS['bg_2'], fg=COLORS['warn_hi'],
              activebackground=COLORS['bg_3'],
              activeforeground=COLORS['warn_hi'],
              relief='flat', bd=0,
              padx=14, pady=10,
              cursor='hand2',
              highlightbackground=COLORS['warn'],
              highlightthickness=1,
              command=app._abp_restore
              ).pack(side='left', padx=(8, 0))


# ─────────────────────────────────────────────────────────────────────────
# Notes field
# ─────────────────────────────────────────────────────────────────────────
def _build_notes_field(parent, app):
    row = tk.Frame(parent, bg=COLORS['bg_1'])
    row.pack(fill='x', pady=(0, 8))
    tk.Label(row, text=_('Notes'),
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             width=8, anchor='w'
             ).pack(side='left')
    entry = _dark_entry(row, app._abp_notes_var, readonly=False)
    entry.pack(side='left', fill='x', expand=True)
    tk.Label(row,
             text=_('e.g. Works on 4.51, tested OK'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(side='right', padx=(8, 0))


# ─────────────────────────────────────────────────────────────────────────
# Status text + thin progress bar
# ─────────────────────────────────────────────────────────────────────────
def _build_status_and_progress(parent, app):
    tk.Label(parent, textvariable=app._abp_status_var,
             font=FONTS['body'],
             bg=COLORS['bg_1'], fg=COLORS['success_hi'],
             anchor='w'
             ).pack(fill='x', pady=(0, 4))

    prog_bg = tk.Frame(parent, bg=COLORS['bg_4'], height=8)
    prog_bg.pack(fill='x', pady=(0, 8))
    prog_bg.pack_propagate(False)
    app._abp_prog_cv = tk.Canvas(prog_bg, height=8, bg=COLORS['bg_4'],
                                  highlightthickness=0)
    app._abp_prog_cv.pack(fill='both', expand=True)
    app._abp_prog_rect = app._abp_prog_cv.create_rectangle(
        0, 0, 0, 8, fill=COLORS['purple'], outline='')


# ─────────────────────────────────────────────────────────────────────────
# Output log — terminal-styled ConsoleView
# ─────────────────────────────────────────────────────────────────────────
def _build_output_log(parent, app):
    """Output log with terminal styling — reuses the shared ConsoleView."""
    # Header row with eyebrow label + Clear button
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x')
    tk.Label(head, text=_('OUTPUT LOG'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(side='left')
    tk.Button(head, text=_('Clear'),
              font=FONTS['meta'],
              bg=COLORS['bg_1'], fg=COLORS['fg_5'],
              activebackground=COLORS['bg_2'],
              activeforeground=COLORS['fg_2'],
              relief='flat', bd=0,
              cursor='hand2',
              command=app._abp_clear_log
              ).pack(side='right')

    # Console (terminal-styled tk.Text wrapped in our shared widget).
    # The legacy callbacks insert into `app._abp_log` directly with tag
    # names 'ok', 'err', 'warn', 'info'. Our ConsoleView uses 'info',
    # 'error', 'warning', 'debug'. We expose `app._abp_log` as the inner
    # text widget AND we add aliases for the legacy tag names.
    cv = ConsoleView(parent, height=180)
    cv.pack(fill='both', expand=True, pady=(2, 0))
    app._abp_log = cv.text
    app._abp_log_console = cv

    # Add legacy tag aliases — same colors as the existing _abp tags
    app._abp_log.tag_configure('ok',   foreground=COLORS['success_ok'])
    app._abp_log.tag_configure('err',  foreground=COLORS['danger_hi'])
    app._abp_log.tag_configure('warn', foreground=COLORS['warn_hi'])
    # Note: 'info' was 'foreground=#4a9eff' in legacy — accent blue.
    # Override the ConsoleView default (which used klog-green for 'info').
    app._abp_log.tag_configure('info', foreground=COLORS['accent_hi'])


# ─────────────────────────────────────────────────────────────────────────
# Helpers — small reusable form fragments
# ─────────────────────────────────────────────────────────────────────────
def _field_row(parent, label_text, hint=None):
    """Standard 'label above input row' container.

    Returns the input row (Frame) so the caller can pack widgets into it.
    The label sits above the row.
    """
    block = tk.Frame(parent, bg=COLORS['bg_1'])
    block.pack(fill='x', pady=(0, 8))

    lbl_row = tk.Frame(block, bg=COLORS['bg_1'])
    lbl_row.pack(fill='x')
    tk.Label(lbl_row, text=label_text,
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             anchor='w'
             ).pack(side='left')
    if hint:
        tk.Label(lbl_row, text='  \u2014  ' + hint,
                 font=FONTS['meta'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                 anchor='w'
                 ).pack(side='left')

    row = tk.Frame(block, bg=COLORS['bg_1'])
    row.pack(fill='x', pady=(4, 0))
    return row


def _dark_entry(parent, var, readonly=False, width=None):
    """Dark-themed entry box wrapped in a 1px-bordered Frame.

    Returns the wrapper Frame (which contains the actual tk.Entry) so the
    caller can pack the whole thing. The entry is sized to fill the wrap.

    Caller usage:
        wrap = _dark_entry(row, my_var, readonly=True)
        wrap.pack(side='left', fill='x', expand=True)
    """
    wrap = tk.Frame(parent, bg=COLORS['bg_4'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    state = 'readonly' if readonly else 'normal'
    entry_kwargs = dict(textvariable=var,
                        font=FONTS['mono_sm'],
                        bg=COLORS['bg_4'], fg=COLORS['fg_1'],
                        # readonly Entries display via disabledforeground/
                        # readonlybackground, NOT fg/bg. Set both so
                        # the text is visible without selection.
                        disabledforeground=COLORS['fg_1'],
                        readonlybackground=COLORS['bg_4'],
                        insertbackground=COLORS['accent'],
                        selectbackground=COLORS['accent'],
                        selectforeground=COLORS['fg_0'],
                        relief='flat', bd=4,
                        state=state)
    if width is not None:
        entry_kwargs['width'] = width
    entry = tk.Entry(wrap, **entry_kwargs)
    entry.pack(fill='x')
    return wrap


def _ghost_btn(parent, text, command):
    """Small ghost-style button matching mock's `.btn-ghost`."""
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9),
                     bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                     activebackground=COLORS['bg_4'],
                     activeforeground=COLORS['fg_1'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1,
                     command=command)
