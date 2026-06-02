"""ShadowMountPlus sub-tab for the PS5 tab.

Provides a GUI for:
  * Editing /data/shadowmount/config.ini fields (kstuff pause delays,
    scan paths, fakelib, mount options, per-image overrides, etc.)
  * Loading the current config from the PS5 over FTP
  * Pushing the edited config back to the PS5
  * Sending the shadowmountplus.elf payload to the PS5 (port 9021)
  * Fetching /data/shadowmount/debug.log for inspection

The form mirrors the keys documented in ShadowMountPlus's README and
the bundled `config.ini.example` template. Defaults match the README.

The tab is built lazily — only when the user clicks into it — to keep
PS5 tab construction time fast.
"""

import os
import io
import re
import socket
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# Theme is always loaded by the app on startup (see exfat_builder.py:636).
# Use the real token names from tkinter_theme.COLORS — no fallback layer.
from tkinter_theme import COLORS, FONTS

# Convenience aliases for the tokens we use most in this module.
# These map directly to keys in COLORS, no transformation.
_BG_APP       = COLORS['bg_1']         # global background
_BG_RAIL      = COLORS['bg_0']         # rail background (slightly darker)
_BG_TOOLBAR   = COLORS['bg_2']         # toolbar / chrome strips
_BG_CARD      = COLORS['bg_2']         # section card background
_BG_CARD_HEAD = COLORS['bg_3']         # card header strip
_BG_ROW_HOV   = COLORS['bg_3']         # row hover
_BG_SAVEBAR   = COLORS['bg_0']         # sticky save bar
_BG_LOG       = COLORS['bg_0']         # log strip
_BORDER       = COLORS['border_2']     # primary divider colour
_BORDER_STRG  = COLORS['border_3']     # field/button borders
_FG           = COLORS['fg_1']         # primary text
_FG_MUTED     = COLORS['fg_3']         # secondary text / sublabels
_FG_DIM       = COLORS['fg_5']         # tertiary / "default" badge
_ACCENT       = COLORS['accent']       # information / focus
_PURPLE       = COLORS['purple']       # ShadowMount+ brand colour
_PURPLE_HI    = COLORS['purple_hi']


def _blend_hex(fg_hex, bg_hex, alpha):
    """Compute the opaque equivalent of fg_hex@alpha over bg_hex.

    Tkinter doesn't support alpha channels on widget backgrounds, so
    the design-system 'soft' fills (e.g. purple at 10% over bg_1) have
    to be pre-blended into solid colours. Compose-over formula:
        out = fg*alpha + bg*(1-alpha)
    """
    fg_hex = fg_hex.lstrip('#')
    bg_hex = bg_hex.lstrip('#')
    fr, fg, fb = int(fg_hex[0:2], 16), int(fg_hex[2:4], 16), int(fg_hex[4:6], 16)
    br, bg_, bb = int(bg_hex[0:2], 16), int(bg_hex[2:4], 16), int(bg_hex[4:6], 16)
    r = round(fr * alpha + br * (1 - alpha))
    g = round(fg * alpha + bg_ * (1 - alpha))
    b = round(fb * alpha + bb * (1 - alpha))
    return '#%02x%02x%02x' % (r, g, b)


# purple-soft = purple @ 10% over bg_1 (theme module ships purple +
# purple_hi but no purple_soft yet). Pre-blended into a solid colour
# because Tk widget backgrounds don't take alpha.
_PURPLE_SOFT  = _blend_hex(COLORS['purple'], COLORS['bg_1'], 0.10)
_PURPLE_BORDER = _blend_hex(COLORS['purple'], COLORS['bg_1'], 0.30)
_SUCCESS      = COLORS['success']
_WARN         = COLORS['warn']
_WARN_BG      = COLORS['warn_bg']
_DANGER       = COLORS['danger']

_F_BODY    = FONTS['body']
_F_LABEL   = FONTS['label']
_F_META    = FONTS['meta']
_F_H2      = FONTS['h2']
_F_H3      = FONTS['h3']
_F_EYEBROW = FONTS['eyebrow']
_F_MONO    = FONTS['mono_sm']
_F_BUTTON  = FONTS['button']

# ── Legacy name aliases ─────────────────────────────────────────────
# Aliases for the original constant names. The old _section_* form
# builders that used these were removed in Prompt 6, but several
# remaining helpers (_set_status, _friendly_ftp_error, etc.) still
# reference them — notably as default values in function signatures
# like `def _set_status(app, text, color=MUTED)`, which forces these
# to resolve at module-import time.
BG       = _BG_APP
SURFACE  = _BG_TOOLBAR
SURFACE2 = _BG_CARD_HEAD
BORDER   = _BORDER
TEXT     = _FG
MUTED    = _FG_MUTED
ACCENT   = _PURPLE
SUCCESS  = _SUCCESS
WARNING  = _WARN
DANGER   = _DANGER


# Remote config path on the PS5 — fixed by ShadowMountPlus.
REMOTE_CONFIG_PATH = '/data/shadowmount/config.ini'
REMOTE_DEBUG_LOG   = '/data/shadowmount/debug.log'
REMOTE_AUTOTUNE    = '/data/shadowmount/autotune.ini'

# Default config values lifted from the README so the form is fully
# populated even before the user fetches anything from the PS5.
DEFAULTS = {
    'debug':                              '1',
    'quiet_mode':                         '0',
    'mount_read_only':                    '1',
    'force_mount':                        '0',
    'app_install_all':                    '0',
    'scan_depth':                         '1',
    'scan_interval_seconds':              '60',
    'stability_wait_seconds':             '10',
    'exfat_backend':                      'lvd',
    'ufs_backend':                        'lvd',
    'backport_fakelib':                   '1',
    'global_fakelib':                     '1',
    'global_fakelib_path':                '/data/shadowmount/fakelib',
    'global_fakelib_priority':            'game',
    'kstuff_game_auto_toggle':            '1',
    'kstuff_crash_detection':             '1',
    'kstuff_pause_delay_image_seconds':   '25',
    'kstuff_pause_delay_direct_seconds':  '15',
    'lvd_exfat_sector_size':              '512',
    'lvd_ufs_sector_size':                '4096',
    'lvd_pfs_sector_size':                '32768',
    'md_exfat_sector_size':               '512',
    'md_ufs_sector_size':                 '512',
}


# ════════════════════════════════════════════════════════════════════
# Public entry point
# ════════════════════════════════════════════════════════════════════
def build_shadowmount_tab(parent, app):
    """Construct the ShadowMountPlus sub-tab inside `parent`.

    `app` is the main `ExFATBuilder` instance — we reuse its FTP
    helpers (`_ftp_connect`, `_ftp_ip_var`, `_ftp_port_var`) and
    settings store (`_settings`, `save_settings`).

    Layout (Prompt 1 — shell only):
        ┌─ toolbar (icon, title+subtitle, dirty pill, action buttons) ─┐
        ├─────────────┬───────────────────────────────────────────────┤
        │   RAIL      │            CONTENT (scrolls)                  │
        │   220 px    │            one card per section               │
        ├─────────────┴───────────────────────────────────────────────┤
        │ STICKY save bar — diff summary · Save · Push                │
        ├─────────────────────────────────────────────────────────────┤
        │ Output log strip (single line)                              │
        └─────────────────────────────────────────────────────────────┘
    """
    # Wipe any prior contents (sub-tab rebuilds happen on activation)
    for w in parent.winfo_children():
        w.destroy()

    parent.configure(bg=_BG_APP)

    # Legacy form-var dicts (StringVar/BooleanVar by key) — kept around
    # for the not-yet-ported sections. Replaced by SettingRow instances
    # (app._smp_rows) as each section is ported in P3-P4.
    if not hasattr(app, '_smp_vars'):
        app._smp_vars = {}
        app._smp_check_vars = {}
    if not hasattr(app, '_smp_status_var'):
        app._smp_status_var = tk.StringVar(value='')

    # ── Dirty tracking state ───────────────────────────────────────
    # SettingRow instances keyed by config.ini key. Populated by each
    # section builder (only ported sections appear here).
    app._smp_rows = {}
    # Snapshot of the values at "clean baseline" — i.e. what's currently
    # saved locally OR most-recently loaded from PS5. Compared against
    # live row values to decide what's dirty.
    app._smp_initial_values = {}
    # Set of config-keys whose current value differs from the snapshot.
    app._smp_dirty_keys = set()

    # Track section frames + the currently-shown section
    app._smp_section_frames = {}    # key -> ttk.Frame (in content area)
    app._smp_rail_rows = {}         # key -> tk.Frame (in rail)
    app._smp_active_section = None

    # ── Toolbar (top) ──────────────────────────────────────────────
    _build_toolbar(parent, app)

    # ── Main: rail (left) + content (right) ────────────────────────
    main = tk.Frame(parent, bg=_BG_APP)
    main.pack(side='top', fill='both', expand=True)

    rail = tk.Frame(main, bg=_BG_RAIL, width=220)
    rail.pack(side='left', fill='y')
    rail.pack_propagate(False)   # honour fixed width
    _build_rail(rail, app)

    # 1px vertical divider between rail and content
    tk.Frame(main, bg=_BORDER, width=1).pack(side='left', fill='y')

    # ── Save bar (sticky bottom) — packed BEFORE content so it pins
    #    cleanly above the log strip. Per design system: use pack
    #    side='bottom' rather than place() to avoid fighting geometry.
    # The order matters: log strip first (bottom), then savebar (also
    # side='bottom' but packed after = sits above log), then content
    # fills the remaining space.
    log_strip = tk.Frame(main, bg=_BG_LOG, height=38)
    log_strip.pack(side='bottom', fill='x')
    log_strip.pack_propagate(False)
    _build_log_strip(log_strip, app)

    # Hairline border above the savebar — also pack/unpacked with it.
    savebar_border = tk.Frame(main, bg=_BORDER, height=1)
    savebar_border.pack(side='bottom', fill='x')

    savebar = tk.Frame(main, bg=_BG_SAVEBAR)
    savebar.pack(side='bottom', fill='x')
    _build_savebar(savebar, app)

    # Stash for show/hide. The savebar only appears when there are
    # pending changes (P5 polish). Starts hidden because the form is
    # clean on first render.
    app._smp_savebar_frame  = savebar
    app._smp_savebar_border = savebar_border
    savebar.pack_forget()
    savebar_border.pack_forget()

    # ── Content area (scrollable, fills remaining space) ───────────
    content_outer = tk.Frame(main, bg=_BG_APP)
    content_outer.pack(side='left', fill='both', expand=True)
    _build_content(content_outer, app)

    # Activate the first section by default
    _select_section(app, 'kstuff')

    # ── Keyboard shortcuts ─────────────────────────────────────────
    # Ctrl+S → save locally (only when there are pending edits)
    # Ctrl+Shift+S → push to PS5 (only when there are pending edits)
    # Bound on this tab's parent so they fire only when focus is
    # somewhere inside the ShadowMount+ subtree; the rest of the app's
    # tabs won't intercept these.
    def _kb_save(_=None):
        if app._smp_dirty_keys:
            _save_locally(app)
        return 'break'
    def _kb_push(_=None):
        if app._smp_dirty_keys:
            _push_to_ps5(app)
        return 'break'
    parent.bind('<Control-s>',       _kb_save)
    parent.bind('<Control-Shift-S>', _kb_push)


# ════════════════════════════════════════════════════════════════════
# Section catalogue — single source of truth for rail + content
# Tuple shape: (key, icon, label, sublabel, count, accent_token)
# `accent_token` keys into COLORS; cards use this colour for their
# header icon swatch.
# ════════════════════════════════════════════════════════════════════
_SECTIONS = [
    ('kstuff',    '\u23f8', 'Kstuff pause',        'delays around game exec/exit',           6, 'purple'),
    ('mount',     '\U0001f4be', 'Mount options',  'read-only, backends, install flow',       5, 'accent'),
    ('scan',      '\U0001f50d', 'Scanning',       'where ShadowMount+ looks, how often',     4, 'accent'),
    ('fakelib',   '\U0001f4e6', 'Fakelib overlays','sandbox library injection',              5, 'purple'),
    ('per_image', '\U0001f4d0', 'Per-image overrides','force RO/RW or sector size by file', 1, 'warn'),
    ('advanced',  '\u2699',  'Advanced',          'logging, sector sizes',                   7, 'fg_4'),
    ('payload',   '\U0001f4e1', 'Payload',        'send shadowmountplus.elf · TCP 9021',     2, 'purple'),
]


# config.ini key → section key. Used by dirty-state tracker to figure
# out which rail item should show its "this section has unsaved changes"
# dot. Updated in each Prompt as sections are ported.
_KEY_TO_SECTION = {
    # Kstuff (Prompt 3)
    'kstuff_game_auto_toggle':           'kstuff',
    'kstuff_crash_detection':            'kstuff',
    'kstuff_pause_delay_image_seconds':  'kstuff',
    'kstuff_pause_delay_direct_seconds': 'kstuff',
    'kstuff_no_pause':                   'kstuff',
    'kstuff_delay':                      'kstuff',
    # Mount (Prompt 4)
    'mount_read_only':                   'mount',
    'force_mount':                       'mount',
    'app_install_all':                   'mount',
    'exfat_backend':                     'mount',
    'ufs_backend':                       'mount',
    # Scanning (Prompt 4)
    'scan_depth':                        'scan',
    'scan_interval_seconds':             'scan',
    'stability_wait_seconds':            'scan',
    'scanpath':                          'scan',
    # Fakelib (Prompt 4)
    'backport_fakelib':                  'fakelib',
    'global_fakelib':                    'fakelib',
    'global_fakelib_path':               'fakelib',
    'global_fakelib_priority':           'fakelib',
    'global_fakelib_exclude':            'fakelib',
    # Per-image overrides (Prompt 4)
    'per_image_rules':                   'per_image',
    # Advanced (Prompt 4)
    'debug':                             'advanced',
    'quiet_mode':                        'advanced',
    'lvd_exfat_sector_size':             'advanced',
    'lvd_ufs_sector_size':               'advanced',
    'lvd_pfs_sector_size':               'advanced',
    'md_exfat_sector_size':              'advanced',
    'md_ufs_sector_size':                'advanced',
    # Payload (Prompt 4) — these aren't config.ini keys but app settings
    'smp_payload_path':                  'payload',
    'smp_payload_port':                  'payload',
}


# ════════════════════════════════════════════════════════════════════
# Toolbar
# ════════════════════════════════════════════════════════════════════
def _build_toolbar(parent, app):
    # Height 72px to fit both the H3 title row and the mono subtitle
    # row without clipping. The 58px in the mock assumed a single-line
    # title — we use two lines (brand + descriptor on row 1, path/meta
    # on row 2) so we need more vertical room.
    bar = tk.Frame(parent, bg=_BG_TOOLBAR, height=72)
    bar.pack(side='top', fill='x')
    bar.pack_propagate(False)

    # Bottom hairline border
    tk.Frame(parent, bg=_BORDER, height=1).pack(side='top', fill='x')

    # Inner row — 8px top/bottom padding leaves 56px for the content
    # stack (title row ~19px + sub row ~15px + ~2px gap = ~36px, with
    # ~20px breathing room split between top and bottom).
    inner = tk.Frame(bar, bg=_BG_TOOLBAR)
    inner.pack(fill='both', expand=True, padx=24, pady=8)

    # Icon swatch (purple-soft) — 40x40, centered vertically alongside
    # the two-line title stack
    icon_wrap = tk.Frame(inner, bg=_BG_TOOLBAR)
    icon_wrap.pack(side='left', fill='y')
    icon = tk.Frame(icon_wrap, bg=_PURPLE_SOFT, width=40, height=40,
                    highlightthickness=1, highlightbackground=_PURPLE_BORDER)
    icon.pack(expand=True)
    icon.pack_propagate(False)
    tk.Label(icon, text='\u26a1', bg=_PURPLE_SOFT, fg=_PURPLE,
             font=(_F_BODY[0], 16, 'bold')).pack(expand=True)

    # Title + subtitle stack
    title_col = tk.Frame(inner, bg=_BG_TOOLBAR)
    title_col.pack(side='left', padx=(12, 0))

    # First line: brand + descriptor on the same horizontal row, like
    # "ShadowMountPlus — auto-mount payload" in the mock.
    title_row = tk.Frame(title_col, bg=_BG_TOOLBAR)
    title_row.pack(anchor='w')
    tk.Label(title_row, text='ShadowMountPlus',
             bg=_BG_TOOLBAR, fg=_FG, font=_F_H3,
             anchor='w').pack(side='left')
    tk.Label(title_row, text='  \u2014  auto-mount payload',
             bg=_BG_TOOLBAR, fg=_FG_MUTED, font=_F_LABEL,
             anchor='w').pack(side='left', pady=(2, 0))

    # Second line: path + meta. Three separate Labels with proper gaps
    # avoids the Consolas glyph-collision problem where a single string
    # like "...config.ini | 7 sections | TCP 9021" reads as ".../sections/ICP"
    # because the pipe and the next digit/letter sit too close together.
    sub_row = tk.Frame(title_col, bg=_BG_TOOLBAR)
    sub_row.pack(anchor='w', pady=(2, 0))
    tk.Label(sub_row, text=REMOTE_CONFIG_PATH,
             bg=_BG_TOOLBAR, fg=_FG_MUTED, font=_F_MONO).pack(side='left')
    # 12px gap, em-dash, 12px gap — generous spacing keeps the small
    # mono font readable at the section count
    tk.Label(sub_row, text='      \u2022  7 sections',
             bg=_BG_TOOLBAR, fg=_FG_DIM, font=_F_MONO).pack(side='left')
    tk.Label(sub_row, text='      \u2022  TCP 9021',
             bg=_BG_TOOLBAR, fg=_FG_DIM, font=_F_MONO).pack(side='left')

    # Right-side actions
    actions = tk.Frame(inner, bg=_BG_TOOLBAR)
    actions.pack(side='right')

    # Dirty pill — shown when there are pending edits, hidden otherwise.
    # padx=12 gives a tiny safety margin so text never touches the
    # highlight-border edge regardless of the Tk version's measurement.
    app._smp_dirty_pill = tk.Label(actions, text='',
                                   bg=_WARN_BG, fg=_WARN,
                                   font=_F_MONO,
                                   padx=12, pady=4,
                                   highlightthickness=1,
                                   highlightbackground=_WARN)
    # Don't pack yet — _refresh_dirty_ui() will pack/unpack based on count

    # Status label (network operation feedback)
    status_lbl = tk.Label(actions, textvariable=app._smp_status_var,
                          bg=_BG_TOOLBAR, fg=_FG_MUTED, font=_F_MONO,
                          anchor='e', wraplength=320, justify='right')
    status_lbl.pack(side='right', padx=(8, 12))
    app._smp_status_lbl = status_lbl

    # Action buttons. Save locally + Push to PS5 are NOT here — they
    # live on the savebar at the bottom which only appears when there
    # are pending edits. The top toolbar keeps "global" actions that
    # are independent of the form's dirty state.
    _toolbar_btn(actions, '\u26a1  Send Payload',
                 lambda: _send_payload(app), kind='success').pack(side='right',
                                                                   padx=4)
    _toolbar_btn(actions, '\U0001f4e5  Load from PS5',
                 lambda: _load_from_ps5(app), kind='default').pack(side='right',
                                                                    padx=4)
    _toolbar_btn(actions, '\u23cf  Safe to unplug',
                 lambda: _stop_shadowmount(app), kind='default').pack(side='right',
                                                                       padx=4)
    _toolbar_btn(actions, '\U0001f4dd  Fetch log',
                 lambda: _fetch_debug_log(app), kind='ghost').pack(side='right',
                                                                    padx=4)


def _toolbar_btn(parent, text, cmd, kind='default'):
    """Action button. kind ∈ {default, primary, warn, success, ghost}."""
    bg = _BG_CARD_HEAD
    fg = _FG
    if kind == 'primary':
        bg, fg = _PURPLE, '#ffffff'
    elif kind == 'warn':
        bg, fg = _WARN, '#000000'
    elif kind == 'success':
        bg, fg = _SUCCESS, '#ffffff'
    elif kind == 'ghost':
        bg, fg = _BG_TOOLBAR, _FG_MUTED
    return tk.Button(parent, text=text, command=cmd,
                     bg=bg, fg=fg, font=_F_BUTTON,
                     activebackground=_BG_ROW_HOV, activeforeground=fg,
                     bd=0, padx=12, pady=7, cursor='hand2', relief='flat')


# ════════════════════════════════════════════════════════════════════
# Rail (left nav)
# ════════════════════════════════════════════════════════════════════
def _build_rail(parent, app):
    # Search box — type to filter sections by name or by any field key
    # they contain. Matching rows stay highlighted, non-matching rows
    # dim. Pressing Enter jumps to the first matching section.
    search_frame = tk.Frame(parent, bg=_BG_RAIL)
    search_frame.pack(side='top', fill='x', padx=12, pady=(12, 6))
    app._smp_search_var = tk.StringVar(value='')
    search_entry = tk.Entry(search_frame, textvariable=app._smp_search_var,
                            bg=COLORS['bg_0'], fg=_FG,
                            insertbackground=_FG,
                            bd=0, relief='flat',
                            font=_F_BODY)
    search_entry.pack(fill='x', ipady=4, ipadx=8)
    # Placeholder text (Tk lacks native placeholder; use a focus trick)
    _install_placeholder(search_entry, app._smp_search_var,
                         '\U0001f50d  Filter settings\u2026')

    # Live filter on every keystroke
    def _on_search_change(*_):
        q = (app._smp_search_var.get() or '').strip().lower()
        # Skip while the placeholder text is showing — the placeholder
        # tracker sets a sentinel attr on the entry.
        if getattr(search_entry, '_placeholder_active', False):
            q = ''
        _apply_search_filter(app, q)
    app._smp_search_var.trace_add('write', _on_search_change)

    # Enter key jumps to first match
    def _on_search_enter(_=None):
        q = (app._smp_search_var.get() or '').strip().lower()
        if getattr(search_entry, '_placeholder_active', False) or not q:
            return
        first = _first_matching_section(q)
        if first:
            _select_section(app, first)
    search_entry.bind('<Return>', _on_search_enter)

    # Eyebrow: SECTIONS
    tk.Label(parent, text='SECTIONS',
             bg=_BG_RAIL, fg=_FG_MUTED, font=_F_EYEBROW,
             anchor='w').pack(fill='x', padx=14, pady=(14, 4))

    # Section list
    for key, icon, label, _sub, count, _accent in _SECTIONS:
        row = _make_rail_row(parent, app, key, icon, label, count)
        app._smp_rail_rows[key] = row

    # Spacer pushes the footer to the bottom
    tk.Frame(parent, bg=_BG_RAIL).pack(side='top', fill='both', expand=True)

    # Footer: last-load timestamp
    footer = tk.Frame(parent, bg=COLORS['bg_0'])
    footer.pack(side='bottom', fill='x')
    tk.Frame(footer, bg=_BORDER, height=1).pack(fill='x')
    app._smp_load_footer_var = tk.StringVar(value='Not loaded from PS5 yet')
    tk.Label(footer, textvariable=app._smp_load_footer_var,
             bg=COLORS['bg_0'], fg=_FG_DIM, font=_F_MONO,
             anchor='w', justify='left',
             padx=14, pady=10).pack(fill='x')


def _make_rail_row(parent, app, key, icon, label, count):
    """Clickable rail row. Highlights when active."""
    row = tk.Frame(parent, bg=_BG_RAIL,
                   highlightthickness=0)
    row.pack(side='top', fill='x')

    # Left accent bar (2px wide) — hidden until selected
    accent_bar = tk.Frame(row, bg=_BG_RAIL, width=2)
    accent_bar.pack(side='left', fill='y')

    body = tk.Frame(row, bg=_BG_RAIL)
    body.pack(side='left', fill='x', expand=True)

    inner = tk.Frame(body, bg=_BG_RAIL)
    inner.pack(fill='x', padx=12, pady=7)

    tk.Label(inner, text=icon, bg=_BG_RAIL, fg=_FG_MUTED,
             font=_F_BODY, width=2, anchor='w').pack(side='left')
    lbl = tk.Label(inner, text=label, bg=_BG_RAIL, fg=_FG_MUTED,
                   font=_F_BODY, anchor='w')
    lbl.pack(side='left', fill='x', expand=True, padx=(6, 0))

    # Count badge
    count_bg = tk.Label(inner, text=str(count),
                        bg=_BG_CARD_HEAD, fg=_FG_DIM,
                        font=_F_META, padx=6, pady=0)
    count_bg.pack(side='right')

    # Dirty dot — 6px amber circle drawn on a Canvas. Created hidden;
    # Dirty dot — a unicode bullet glyph rendered as a Label. Bare-tk
    # Label with a font character is sharp at any DPI (unlike a Canvas
    # oval, which is jagged on Windows). _refresh_dirty_ui packs it in
    # / out when the section's dirty count changes.
    dot = tk.Label(inner, text='\u25cf',
                   bg=_BG_RAIL, fg=_WARN,
                   font=(_F_MONO[0], 9))
    # NOT packed yet — _refresh_dirty_ui handles pack/forget.
    row._smp_dirty_dot = dot

    # Stash widget refs on the row so _select_section can re-style them
    row._smp_accent_bar = accent_bar
    row._smp_label = lbl
    row._smp_icon = inner.winfo_children()[0]   # the icon Label
    row._smp_count_bg = count_bg
    row._smp_body = body
    row._smp_inner = inner

    # Click handler — bind to every widget so clicks anywhere on the
    # row work, not just the empty padding around the children.
    def _on_click(_=None, k=key):
        _select_section(app, k)
    for w in (row, body, inner, lbl, count_bg, row._smp_icon):
        w.bind('<Button-1>', _on_click)
        w.configure(cursor='hand2')

    # Hover (subtle background change)
    def _on_enter(_=None):
        if app._smp_active_section != key:
            for w in (row, body, inner):
                w.configure(bg=COLORS['bg_1'])
            lbl.configure(bg=COLORS['bg_1'], fg=_FG)
            count_bg.configure(bg=COLORS['bg_1'])
            row._smp_icon.configure(bg=COLORS['bg_1'])
    def _on_leave(_=None):
        if app._smp_active_section != key:
            for w in (row, body, inner):
                w.configure(bg=_BG_RAIL)
            lbl.configure(bg=_BG_RAIL, fg=_FG_MUTED)
            count_bg.configure(bg=_BG_RAIL)
            row._smp_icon.configure(bg=_BG_RAIL)
    for w in (row, body, inner, lbl, count_bg, row._smp_icon):
        w.bind('<Enter>', _on_enter, add='+')
        w.bind('<Leave>', _on_leave, add='+')

    return row


def _select_section(app, key):
    """Switch the visible section. Updates rail highlight + content."""
    # Update active marker FIRST so the hover handlers respect it
    prev = app._smp_active_section
    app._smp_active_section = key

    # Restyle the previously-selected row back to inactive
    if prev and prev in app._smp_rail_rows:
        _style_rail_row(app._smp_rail_rows[prev], active=False)
    if key in app._smp_rail_rows:
        _style_rail_row(app._smp_rail_rows[key], active=True)

    # Show the matching content frame, hide the others
    for k, frame in app._smp_section_frames.items():
        if k == key:
            frame.pack(fill='x', expand=False, pady=(0, 14))
        else:
            frame.pack_forget()


# ── Search filter helpers ──────────────────────────────────────────
def _section_matches_query(section_key, q):
    """True iff `q` (lowercase) appears in the section's label, sublabel,
    or any of its config keys.
    """
    if not q:
        return True
    for s_key, _ic, label, sublabel, _ct, _ac in _SECTIONS:
        if s_key != section_key:
            continue
        if q in label.lower() or q in sublabel.lower() or q in s_key.lower():
            return True
        break
    # Walk the key-to-section map looking for keys in this section
    for k, sec in _KEY_TO_SECTION.items():
        if sec != section_key:
            continue
        if q in k.lower():
            return True
    return False


def _first_matching_section(q):
    """First section (in canonical _SECTIONS order) that matches q."""
    if not q:
        return None
    for s_key, _ic, _lbl, _sub, _ct, _ac in _SECTIONS:
        if _section_matches_query(s_key, q):
            return s_key
    return None


def _apply_search_filter(app, q):
    """Dim non-matching rail rows; restore them when q is empty.

    On a non-empty query, also jumps to the first matching section so
    the user sees something relevant immediately. Doesn't re-select if
    the currently-active section already matches.
    """
    rail_rows = getattr(app, '_smp_rail_rows', {}) or {}

    if not q:
        # Restore: every row back to its normal styling. _style_rail_row
        # handles active vs inactive.
        active = getattr(app, '_smp_active_section', None)
        for sec_key, row in rail_rows.items():
            _style_rail_row(row, sec_key == active)
        return

    # Apply dim styling to non-matching rows; matching rows keep
    # their normal styling.
    active = getattr(app, '_smp_active_section', None)
    for sec_key, row in rail_rows.items():
        if _section_matches_query(sec_key, q):
            _style_rail_row(row, sec_key == active)
        else:
            # Custom dim: muted fg, no accent bar, smaller-feeling row
            try:
                row._smp_accent_bar.configure(bg=_BG_RAIL)
                for w in (row, row._smp_body, row._smp_inner):
                    w.configure(bg=_BG_RAIL)
                row._smp_label.configure(bg=_BG_RAIL, fg=_FG_DIM)
                row._smp_count_bg.configure(bg=_BG_CARD_HEAD, fg=_FG_DIM)
                row._smp_icon.configure(bg=_BG_RAIL, fg=_FG_DIM)
            except Exception:
                pass

    # Jump to first match if the current section doesn't match
    if active and _section_matches_query(active, q):
        return
    first = _first_matching_section(q)
    if first:
        _select_section(app, first)


def _style_rail_row(row, active):
    bg_active = _PURPLE_SOFT
    if active:
        row._smp_accent_bar.configure(bg=_PURPLE)
        for w in (row, row._smp_body, row._smp_inner):
            w.configure(bg=bg_active)
        row._smp_label.configure(bg=bg_active, fg=_FG)
        row._smp_count_bg.configure(bg=bg_active, fg=_PURPLE_HI)
        row._smp_icon.configure(bg=bg_active, fg=_PURPLE)
        # Dirty dot canvas bg follows row bg (so the dot looks isolated,
        # not on a different surface)
        dot = getattr(row, '_smp_dirty_dot', None)
        if dot is not None:
            dot.configure(bg=bg_active)
    else:
        row._smp_accent_bar.configure(bg=_BG_RAIL)
        for w in (row, row._smp_body, row._smp_inner):
            w.configure(bg=_BG_RAIL)
        row._smp_label.configure(bg=_BG_RAIL, fg=_FG_MUTED)
        row._smp_count_bg.configure(bg=_BG_CARD_HEAD, fg=_FG_DIM)
        row._smp_icon.configure(bg=_BG_RAIL, fg=_FG_MUTED)
        dot = getattr(row, '_smp_dirty_dot', None)
        if dot is not None:
            dot.configure(bg=_BG_RAIL)


# ════════════════════════════════════════════════════════════════════
# Content area — one card per section, vertically stacked. Only the
# active section's card is packed at any one time (see _select_section).
# ════════════════════════════════════════════════════════════════════
def _build_content(parent, app):
    # Scrollable canvas inside `parent`
    canvas = tk.Canvas(parent, bg=_BG_APP, highlightthickness=0)
    vsb    = tk.Scrollbar(parent, orient='vertical', command=canvas.yview,
                          bg=_BG_CARD, troughcolor=_BG_APP)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    inner = tk.Frame(canvas, bg=_BG_APP)
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')

    def _on_inner_config(_=None):
        canvas.configure(scrollregion=canvas.bbox('all'))
    inner.bind('<Configure>', _on_inner_config)

    def _on_canvas_config(e):
        canvas.itemconfig(inner_id, width=e.width)
    canvas.bind('<Configure>', _on_canvas_config)

    def _on_wheel(e):
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units')
    # Only bind mousewheel while pointer is over this canvas
    canvas.bind('<Enter>', lambda _e: canvas.bind_all('<MouseWheel>', _on_wheel))
    canvas.bind('<Leave>', lambda _e: canvas.unbind_all('<MouseWheel>'))

    # Padding wrapper inside the scroll surface
    pad = tk.Frame(inner, bg=_BG_APP)
    pad.pack(fill='x', padx=24, pady=18)

    # One stub card per section
    for key, icon, label, sublabel, count, accent_token in _SECTIONS:
        accent_color = COLORS.get(accent_token, _PURPLE)
        card = _build_section_card(pad, app, key, icon, label, sublabel,
                                   count, accent_color)
        app._smp_section_frames[key] = card


def _build_section_card(parent, app, key, icon, label, sublabel, count, accent):
    """Build a section card (header + placeholder body).
    The body is intentionally empty in Prompt 1 — later prompts port
    each section's fields into the body using SettingRow.
    """
    card = tk.Frame(parent, bg=_BG_CARD,
                    highlightthickness=1, highlightbackground=_BORDER)
    # NOTE: not packed here. _select_section handles pack/forget so
    # only the active section is visible.

    # Header strip
    head = tk.Frame(card, bg=_BG_CARD_HEAD)
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=_BG_CARD_HEAD)
    head_inner.pack(fill='x', padx=14, pady=10)

    # Icon swatch — coloured per section
    swatch = tk.Frame(head_inner, bg=_BG_CARD_HEAD,
                      width=24, height=24)
    swatch.pack(side='left')
    swatch.pack_propagate(False)
    tk.Label(swatch, text=icon, bg=_BG_CARD_HEAD, fg=accent,
             font=_F_BODY).pack(expand=True)

    tk.Label(head_inner, text=label,
             bg=_BG_CARD_HEAD, fg=_FG, font=_F_H3,
             anchor='w').pack(side='left', padx=(10, 0))
    tk.Label(head_inner, text='  \u2014  ' + sublabel,
             bg=_BG_CARD_HEAD, fg=_FG_MUTED, font=_F_MONO,
             anchor='w').pack(side='left')

    meta = tk.Label(head_inner, text='%d SETTINGS' % count,
                    bg=_BG_CARD_HEAD, fg=_FG_DIM, font=_F_META,
                    anchor='e')
    meta.pack(side='right')
    # Stash on the card so _refresh_dirty_ui can rewrite it as
    # "N SETTINGS · M CHANGED" when this section has unsaved edits.
    card._smp_meta_lbl = meta

    # Body — Prompt 3: Kstuff section is fully ported; other sections
    # still show a placeholder until P4.
    body = tk.Frame(card, bg=_BG_CARD)
    body.pack(fill='x')
    card._smp_body = body

    _SECTION_BUILDERS = {
        'kstuff':    _build_kstuff_section,
        'mount':     _build_mount_section,
        'scan':      _build_scan_section,
        'fakelib':   _build_fakelib_section,
        'per_image': _build_per_image_section,
        'advanced':  _build_advanced_section,
        'payload':   _build_payload_section,
    }
    builder = _SECTION_BUILDERS.get(key)
    if builder is not None:
        builder(body, app)
    else:
        # Should never hit this; _SECTIONS and _SECTION_BUILDERS are
        # in sync. Defensive fallback in case a new section is added
        # to _SECTIONS without a matching builder.
        inner = tk.Frame(body, bg=_BG_CARD)
        inner.pack(fill='x', padx=20, pady=20)
        tk.Label(inner,
                 text=('[ no builder registered for section ' + repr(key) + ' ]'),
                 bg=_BG_CARD, fg=_DANGER, font=_F_MONO,
                 anchor='w', justify='left').pack(fill='x')

    return card




def _build_kstuff_section(parent, app):
    """Mount the real 6 Kstuff fields into the section card body.

    Reads initial values from app._settings['shadowmount_config'] if
    present, otherwise falls back to documented defaults. Every change
    routes through _on_smp_change which updates dirty tracking + the
    toolbar/rail/savebar indicators.
    """
    saved = app._settings.get('shadowmount_config', {}) or {}

    def _initial(key):
        """Pick the starting value: saved-locally first, then default."""
        if key in saved:
            v = saved[key]
            # Saved values may already be lists (multiline keys)
            if isinstance(v, list):
                return '\n'.join(v)
            return v
        return DEFAULTS.get(key, '')

    on_change = lambda k, v: _on_smp_change(app, k, v)

    # 1. Master switch
    _register_row(app, 'kstuff_game_auto_toggle',
        SettingRow(parent,
            key='kstuff_game_auto_toggle',
            label='Auto pause/resume kstuff',
            sublabel='master switch for the kstuff handler',
            kind='toggle',
            value=_initial('kstuff_game_auto_toggle'),
            default=DEFAULTS['kstuff_game_auto_toggle'],
            on_change=on_change))

    # 2. Crash detection autotune
    _register_row(app, 'kstuff_crash_detection',
        SettingRow(parent,
            key='kstuff_crash_detection',
            label='Crash detection + autotune',
            sublabel='writes autotune.ini when a crash is detected',
            kind='toggle',
            value=_initial('kstuff_crash_detection'),
            default=DEFAULTS['kstuff_crash_detection'],
            on_change=on_change))

    # 3. Image-backed pause delay
    _register_row(app, 'kstuff_pause_delay_image_seconds',
        SettingRow(parent,
            key='kstuff_pause_delay_image_seconds',
            label='Pause delay \u2014 image-backed launches',
            kind='int',
            value=_initial('kstuff_pause_delay_image_seconds'),
            default=DEFAULTS['kstuff_pause_delay_image_seconds'],
            unit='seconds',
            help='range 0\u20133600 \u00b7 applies to .exfat / .ffpkg mounts',
            on_change=on_change))

    # 4. Direct/non-image pause delay
    _register_row(app, 'kstuff_pause_delay_direct_seconds',
        SettingRow(parent,
            key='kstuff_pause_delay_direct_seconds',
            label='Pause delay \u2014 direct launches',
            kind='int',
            value=_initial('kstuff_pause_delay_direct_seconds'),
            default=DEFAULTS['kstuff_pause_delay_direct_seconds'],
            unit='seconds',
            help='range 0\u20133600 \u00b7 non-image installs',
            on_change=on_change))

    # 5. Skip-list (textarea, wide)
    _register_row(app, 'kstuff_no_pause',
        SettingRow(parent,
            key='kstuff_no_pause',
            label='Skip these title IDs',
            sublabel='one per line \u00b7 PPSA12345 format',
            kind='textarea',
            value=_initial('kstuff_no_pause'),
            default='',
            wide=True,
            on_change=on_change))

    # 6. Per-title delay overrides (textarea, wide)
    _register_row(app, 'kstuff_delay',
        SettingRow(parent,
            key='kstuff_delay',
            label='Per-title pause delay overrides',
            sublabel='format: TITLEID:SECONDS',
            kind='textarea',
            value=_initial('kstuff_delay'),
            default='',
            wide=True,
            on_change=on_change))

    # Take an initial-values snapshot AFTER the rows exist so they all
    # start "clean" (no spurious dirty marks at first render).
    for key in ('kstuff_game_auto_toggle', 'kstuff_crash_detection',
                'kstuff_pause_delay_image_seconds',
                'kstuff_pause_delay_direct_seconds',
                'kstuff_no_pause', 'kstuff_delay'):
        if key in app._smp_rows:
            app._smp_initial_values[key] = app._smp_rows[key].get_value()


# ── helpers shared by P4 section builders ────────────────────────────
def _initial_value(app, key):
    """Pick the starting value for a config-key: saved-locally first,
    documented default second.
    """
    saved = app._settings.get('shadowmount_config', {}) or {}
    if key in saved:
        v = saved[key]
        if isinstance(v, list):
            return '\n'.join(v)
        return v
    return DEFAULTS.get(key, '')


def _snapshot_keys(app, keys):
    """Take a clean-baseline snapshot for the given config-keys. Called
    after every section builder finishes mounting its rows.
    """
    for key in keys:
        if key in app._smp_rows:
            try:
                app._smp_initial_values[key] = app._smp_rows[key].get_value()
            except Exception:
                pass


# ════════════════════════════════════════════════════════════════════
# Mount options section
# ════════════════════════════════════════════════════════════════════
def _build_mount_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    _register_row(app, 'mount_read_only',
        SettingRow(parent,
            key='mount_read_only',
            label='Mount read-only',
            sublabel='global default for all mounted images',
            kind='toggle',
            value=_initial_value(app, 'mount_read_only'),
            default=DEFAULTS['mount_read_only'],
            on_change=on_change))

    _register_row(app, 'force_mount',
        SettingRow(parent,
            key='force_mount',
            label='Force-mount damaged filesystems',
            kind='toggle',
            value=_initial_value(app, 'force_mount'),
            default=DEFAULTS['force_mount'],
            danger=True,
            help='skips integrity checks \u2014 may corrupt data',
            on_change=on_change))

    _register_row(app, 'app_install_all',
        SettingRow(parent,
            key='app_install_all',
            label='Use sceAppInstUtilAppInstallAll',
            sublabel='batch registration of mounted apps',
            kind='toggle',
            value=_initial_value(app, 'app_install_all'),
            default=DEFAULTS['app_install_all'],
            help='auto-enabled on FW 12.00+',
            on_change=on_change))

    _register_row(app, 'exfat_backend',
        SettingRow(parent,
            key='exfat_backend',
            label='exFAT backend',
            kind='select',
            choices=['lvd', 'md'],
            value=_initial_value(app, 'exfat_backend'),
            default=DEFAULTS['exfat_backend'],
            help='lvd = loopback \u00b7 md = device-mapper',
            on_change=on_change))

    _register_row(app, 'ufs_backend',
        SettingRow(parent,
            key='ufs_backend',
            label='UFS backend',
            kind='select',
            choices=['lvd', 'md'],
            value=_initial_value(app, 'ufs_backend'),
            default=DEFAULTS['ufs_backend'],
            help='lvd = loopback \u00b7 md = device-mapper',
            on_change=on_change))

    _snapshot_keys(app, ('mount_read_only', 'force_mount', 'app_install_all',
                         'exfat_backend', 'ufs_backend'))


# ════════════════════════════════════════════════════════════════════
# Scanning section
# ════════════════════════════════════════════════════════════════════
def _build_scan_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    _register_row(app, 'scan_depth',
        SettingRow(parent,
            key='scan_depth',
            label='Scan depth',
            kind='select',
            choices=['1', '2'],
            value=_initial_value(app, 'scan_depth'),
            default=DEFAULTS['scan_depth'],
            help='1 = first-level only \u00b7 2 = one nested level',
            on_change=on_change))

    _register_row(app, 'scan_interval_seconds',
        SettingRow(parent,
            key='scan_interval_seconds',
            label='Scan interval',
            kind='int',
            value=_initial_value(app, 'scan_interval_seconds'),
            default=DEFAULTS['scan_interval_seconds'],
            unit='seconds',
            help='range 1\u20133600 \u00b7 how often to re-scan',
            on_change=on_change))

    _register_row(app, 'stability_wait_seconds',
        SettingRow(parent,
            key='stability_wait_seconds',
            label='Stability wait',
            sublabel='delay before processing new sources',
            kind='int',
            value=_initial_value(app, 'stability_wait_seconds'),
            default=DEFAULTS['stability_wait_seconds'],
            unit='seconds',
            help='prevents mounting half-copied files',
            on_change=on_change))

    _register_row(app, 'scanpath',
        SettingRow(parent,
            key='scanpath',
            label='Custom scan paths',
            sublabel='absolute paths, one per line',
            kind='textarea',
            value=_initial_value(app, 'scanpath'),
            default='',
            wide=True,
            help='/mnt/shadowmnt is always scanned automatically',
            on_change=on_change))

    _snapshot_keys(app, ('scan_depth', 'scan_interval_seconds',
                         'stability_wait_seconds', 'scanpath'))


# ════════════════════════════════════════════════════════════════════
# Fakelib overlays section
# ════════════════════════════════════════════════════════════════════
def _build_fakelib_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    _register_row(app, 'backport_fakelib',
        SettingRow(parent,
            key='backport_fakelib',
            label='Sandbox fakelib watcher',
            sublabel='auto-mounts per-game fakelib folders',
            kind='toggle',
            value=_initial_value(app, 'backport_fakelib'),
            default=DEFAULTS['backport_fakelib'],
            on_change=on_change))

    _register_row(app, 'global_fakelib',
        SettingRow(parent,
            key='global_fakelib',
            label='Global fakelib overlay',
            sublabel='shared folder applied to every game',
            kind='toggle',
            value=_initial_value(app, 'global_fakelib'),
            default=DEFAULTS['global_fakelib'],
            on_change=on_change))

    _register_row(app, 'global_fakelib_path',
        SettingRow(parent,
            key='global_fakelib_path',
            label='Global fakelib path',
            kind='text',
            value=_initial_value(app, 'global_fakelib_path'),
            default=DEFAULTS['global_fakelib_path'],
            help='absolute path on PS5',
            on_change=on_change))

    _register_row(app, 'global_fakelib_priority',
        SettingRow(parent,
            key='global_fakelib_priority',
            label='Priority when both exist',
            kind='select',
            choices=['game', 'global'],
            value=_initial_value(app, 'global_fakelib_priority'),
            default=DEFAULTS['global_fakelib_priority'],
            help='game = per-game wins \u00b7 global = shared wins',
            on_change=on_change))

    _register_row(app, 'global_fakelib_exclude',
        SettingRow(parent,
            key='global_fakelib_exclude',
            label='Exclude these title IDs',
            sublabel='skip the global fakelib for these games',
            kind='textarea',
            value=_initial_value(app, 'global_fakelib_exclude'),
            default='',
            wide=True,
            help='one title ID per line, PPSA12345 format',
            on_change=on_change))

    _snapshot_keys(app, ('backport_fakelib', 'global_fakelib',
                         'global_fakelib_path', 'global_fakelib_priority',
                         'global_fakelib_exclude'))


# ════════════════════════════════════════════════════════════════════
# Per-image overrides section
# ════════════════════════════════════════════════════════════════════
def _build_per_image_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    # Short explanatory paragraph above the textarea — too much detail
    # for SettingRow's sublabel.
    explainer = tk.Frame(parent, bg=_BG_CARD)
    explainer.pack(fill='x', padx=14, pady=(10, 4))
    tk.Label(explainer,
        text=('One rule per line. Last matching rule wins. Filename match '
              'only (no path). Example:\n'
              '    image_ro=PPSA12345.ffpkg\n'
              '    image_rw=My Game Dump.exfat\n'
              '    image_sector=My Game Dump.exfat:65536'),
        bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO,
        anchor='w', justify='left').pack(fill='x')

    _register_row(app, 'per_image_rules',
        SettingRow(parent,
            key='per_image_rules',
            label='Override rules',
            kind='textarea',
            value=_initial_value(app, 'per_image_rules'),
            default='',
            wide=True,
            on_change=on_change))

    _snapshot_keys(app, ('per_image_rules',))


# ════════════════════════════════════════════════════════════════════
# Advanced section (logging + sector sizes)
# ════════════════════════════════════════════════════════════════════
def _build_advanced_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    _register_row(app, 'debug',
        SettingRow(parent,
            key='debug',
            label='Debug logging',
            sublabel='writes to /data/shadowmount/debug.log',
            kind='toggle',
            value=_initial_value(app, 'debug'),
            default=DEFAULTS['debug'],
            on_change=on_change))

    _register_row(app, 'quiet_mode',
        SettingRow(parent,
            key='quiet_mode',
            label='Quiet mode',
            sublabel='suppress informational popups on the PS5',
            kind='toggle',
            value=_initial_value(app, 'quiet_mode'),
            default=DEFAULTS['quiet_mode'],
            on_change=on_change))

    # Subheader for the sector-sizes block
    subhead = tk.Frame(parent, bg=_BG_CARD)
    subhead.pack(fill='x', padx=14, pady=(12, 4))
    tk.Label(subhead, text='SECTOR SIZES',
             bg=_BG_CARD, fg=_FG_MUTED, font=_F_EYEBROW,
             anchor='w').pack(side='left')
    tk.Label(subhead, text='   advanced \u2014 leave defaults unless you know why',
             bg=_BG_CARD, fg=_FG_DIM, font=_F_META,
             anchor='w').pack(side='left')

    for key in ('lvd_exfat_sector_size', 'lvd_ufs_sector_size',
                'lvd_pfs_sector_size', 'md_exfat_sector_size',
                'md_ufs_sector_size'):
        _register_row(app, key,
            SettingRow(parent,
                key=key,
                label=key,                # technical token IS the label
                kind='int',
                value=_initial_value(app, key),
                default=DEFAULTS[key],
                unit='bytes',
                advanced=True,
                on_change=on_change))

    _snapshot_keys(app, ('debug', 'quiet_mode',
                         'lvd_exfat_sector_size', 'lvd_ufs_sector_size',
                         'lvd_pfs_sector_size', 'md_exfat_sector_size',
                         'md_ufs_sector_size'))


# ════════════════════════════════════════════════════════════════════
# Payload section (path picker + port)
# Not strictly a config.ini section — these settings live on `app._settings`
# directly, but we model them as SettingRows for consistency with the rest
# of the tab. _collect_form / _apply_form are aware of these special keys.
# ════════════════════════════════════════════════════════════════════
def _build_payload_section(parent, app):
    on_change = lambda k, v: _on_smp_change(app, k, v)

    # ── ELF path row — text Entry + Browse button ─────────────────
    # SettingRow doesn't support a built-in Browse button, so we mount
    # a SettingRow for the path then a small Browse button next to it.
    # The simplest way: build a custom row that mirrors SettingRow's
    # geometry. For Prompt 4 simplicity we use a plain text SettingRow
    # for the path, then a separate frame below for the Browse button.
    saved_path = str(app._settings.get('smp_payload_path', ''))
    saved_port = str(app._settings.get('smp_payload_port', 9021))

    _register_row(app, 'smp_payload_path',
        SettingRow(parent,
            key='smp_payload_path',
            label='shadowmountplus.elf path',
            sublabel='local path on this PC',
            kind='text',
            value=saved_path,
            default='',
            help='click Browse below to pick a file',
            on_change=on_change))

    # Browse button row — small inline action under the path field
    browse_row = tk.Frame(parent, bg=_BG_CARD)
    browse_row.pack(fill='x', padx=14, pady=(0, 6))
    tk.Frame(browse_row, bg=_BG_CARD, width=280).pack(side='left')   # align under label
    def _browse_elf():
        p = filedialog.askopenfilename(
            title='Locate shadowmountplus.elf',
            filetypes=[('ELF files', '*.elf'), ('All files', '*.*')])
        if p:
            try:
                app._smp_rows['smp_payload_path'].set_value(p)
                # set_value is suppress-flagged so dirty doesn't fire;
                # force it now since the user actually changed it
                _on_smp_change(app, 'smp_payload_path', p)
            except Exception:
                pass
    tk.Button(browse_row, text='\U0001f4c2  Browse\u2026',
              command=_browse_elf,
              bg=_BG_CARD_HEAD, fg=_FG, font=_F_BUTTON,
              activebackground=_BG_ROW_HOV, activeforeground=_FG,
              bd=0, padx=12, pady=5, cursor='hand2',
              relief='flat').pack(side='left', padx=(14, 0))

    _register_row(app, 'smp_payload_port',
        SettingRow(parent,
            key='smp_payload_port',
            label='Payload port',
            kind='int',
            value=saved_port,
            default='9021',
            unit='TCP',
            help='default: 9021 (matches etaHEN / GoldHEN loaders)',
            on_change=on_change))

    _snapshot_keys(app, ('smp_payload_path', 'smp_payload_port'))


def _register_row(app, key, row):
    """Track a SettingRow on the app so collect/apply/dirty can find it."""
    app._smp_rows[key] = row


def _on_smp_change(app, key, value):
    """Single change-callback used by every SettingRow.

    Diffs against the initial-values snapshot, updates the dirty set,
    and refreshes the three UI dirty indicators (toolbar pill, rail
    dots, save bar summary).
    """
    initial = app._smp_initial_values.get(key, None)

    # Normalize both sides for comparison — toggles and textareas
    # need different handling than plain strings.
    if isinstance(initial, bool) or isinstance(value, bool):
        is_dirty = bool(initial) != bool(value)
    else:
        is_dirty = (str(initial if initial is not None else '') !=
                    str(value  if value   is not None else ''))

    if is_dirty:
        app._smp_dirty_keys.add(key)
    else:
        app._smp_dirty_keys.discard(key)

    _refresh_dirty_ui(app)


def _refresh_dirty_ui(app):
    """Refresh all dirty indicators: savebar visibility, toolbar pill,
    rail dots, save bar summary + section header 'CHANGED' meta.
    """
    count = len(app._smp_dirty_keys)

    # 0. Savebar visibility — only shown when there are pending edits.
    # Hides duplicate Push/Save buttons from the top toolbar when the
    # form is clean. Packs above the log strip (its bottom hairline
    # gets packed first so it lands directly above the bar).
    savebar = getattr(app, '_smp_savebar_frame', None)
    savebar_border = getattr(app, '_smp_savebar_border', None)
    if savebar is not None and savebar_border is not None:
        if count > 0:
            # Already visible? winfo_ismapped tells us
            try:
                if not savebar.winfo_ismapped():
                    savebar_border.pack(side='bottom', fill='x')
                    savebar.pack(side='bottom', fill='x')
            except Exception:
                pass
        else:
            try:
                savebar.pack_forget()
                savebar_border.pack_forget()
            except Exception:
                pass

    # 1. Toolbar pill
    pill = getattr(app, '_smp_dirty_pill', None)
    if pill is not None:
        if count > 0:
            # Short text avoids clipping at the highlight-border edge.
            # "1 change" / "3 changes" reads as cleanly as the longer
            # phrasing in the mock and never gets cut off.
            pill.configure(text='\u25cf  %d change%s'
                                % (count, '' if count == 1 else 's'))
            # Pack into the actions row if not already visible.
            try:
                pill.pack(side='right', padx=(8, 4))
            except Exception:
                pass
        else:
            try:
                pill.pack_forget()
            except Exception:
                pass

    # 2. Rail dots — one per section that contains a dirty key
    sections_with_changes = set()
    for k in app._smp_dirty_keys:
        sec = _KEY_TO_SECTION.get(k)
        if sec:
            sections_with_changes.add(sec)
    for sec_key, row in app._smp_rail_rows.items():
        dot = getattr(row, '_smp_dirty_dot', None)
        if dot is not None:
            if sec_key in sections_with_changes:
                try:
                    dot.pack(side='right', padx=(4, 0))
                except Exception:
                    pass
            else:
                try:
                    dot.pack_forget()
                except Exception:
                    pass

    # 3. Save bar summary line
    summary_var = getattr(app, '_smp_savebar_summary', None)
    if summary_var is not None:
        if count == 0:
            summary_var.set('No pending changes')
        else:
            # Build "key=newval (was oldval) · key2=… · +N more"
            entries = []
            for k in sorted(app._smp_dirty_keys)[:3]:
                if k not in app._smp_rows:
                    continue
                new_v = app._smp_rows[k].get_value()
                old_v = app._smp_initial_values.get(k, '')
                # Truncate long values for the summary
                def _trim(v, n=18):
                    s = str(v).replace('\n', '\u23ce')
                    if len(s) > n:
                        s = s[:n-1] + '\u2026'
                    return s or '\u2014'
                entries.append('%s=%s (was %s)'
                               % (k, _trim(new_v), _trim(old_v)))
            summary = '  \u00b7  '.join(entries)
            if count > 3:
                summary += '  \u00b7  +%d more' % (count - 3)
            summary_var.set(summary)

    # 4. Section-header meta — refresh the visible card's meta string
    #    e.g. "6 SETTINGS" -> "6 SETTINGS · 2 CHANGED"
    active = getattr(app, '_smp_active_section', None)
    if active and active in app._smp_section_frames:
        card = app._smp_section_frames[active]
        meta_lbl = getattr(card, '_smp_meta_lbl', None)
        if meta_lbl is not None:
            # Count dirty keys for this section
            sec_dirty = sum(1 for k in app._smp_dirty_keys
                            if _KEY_TO_SECTION.get(k) == active)
            # Look up the base count from _SECTIONS
            base = next((s[4] for s in _SECTIONS if s[0] == active), 0)
            if sec_dirty:
                meta_lbl.configure(
                    text='%d SETTINGS \u00b7 %d CHANGED' % (base, sec_dirty),
                    fg=_WARN)
            else:
                meta_lbl.configure(text='%d SETTINGS' % base, fg=_FG_DIM)


# ════════════════════════════════════════════════════════════════════
# Save bar — sticky, sits above the log strip. Hidden when clean
# (see _refresh_dirty_ui). Shows a summary of pending edits plus the
# Discard / Save local / Push action buttons.
# ════════════════════════════════════════════════════════════════════
def _build_savebar(parent, app):
    inner = tk.Frame(parent, bg=_BG_SAVEBAR)
    inner.pack(fill='x', padx=24, pady=11)

    # Diff summary (left). Clicking it opens the diff modal.
    app._smp_savebar_summary = tk.StringVar(value='No pending changes')
    summary_lbl = tk.Label(inner, textvariable=app._smp_savebar_summary,
                            bg=_BG_SAVEBAR, fg=_FG_MUTED, font=_F_MONO,
                            anchor='w', cursor='hand2')
    summary_lbl.pack(side='left', fill='x', expand=True)
    summary_lbl.bind('<Button-1>', lambda _e: _show_diff_modal(app))

    # Action buttons (right). "Review" appears on the left of the
    # button group when there are changes — gives the user a way to
    # see the full diff before committing.
    _toolbar_btn(inner, 'Push to PS5', lambda: _push_to_ps5(app),
                 kind='primary').pack(side='right', padx=(4, 0))
    _toolbar_btn(inner, 'Save local', lambda: _save_locally(app),
                 kind='warn').pack(side='right', padx=(4, 0))
    _toolbar_btn(inner, 'Review',
                 lambda: _show_diff_modal(app),
                 kind='ghost').pack(side='right', padx=(4, 0))
    _toolbar_btn(inner, 'Discard', lambda: _discard_changes(app),
                 kind='ghost').pack(side='right', padx=(4, 0))


def _show_diff_modal(app):
    """Modal listing every changed key with old → new values.

    Reached from the savebar summary click and from the Review button.
    Lets the user audit exactly what'll be pushed before they push.
    """
    dirty = getattr(app, '_smp_dirty_keys', set())
    if not dirty:
        return

    rows = app._smp_rows
    snap = app._smp_initial_values

    top = tk.Toplevel(app)
    top.title('Review pending changes')
    top.configure(bg=_BG_APP)
    top.geometry('760x440')
    top.transient(app)
    top.grab_set()

    # Header
    hdr = tk.Frame(top, bg=_BG_TOOLBAR)
    hdr.pack(side='top', fill='x')
    tk.Frame(top, bg=_BORDER, height=1).pack(side='top', fill='x')
    hdr_inner = tk.Frame(hdr, bg=_BG_TOOLBAR)
    hdr_inner.pack(fill='x', padx=20, pady=14)
    tk.Label(hdr_inner, text='Review pending changes',
             bg=_BG_TOOLBAR, fg=_FG, font=_F_H3,
             anchor='w').pack(side='left')
    tk.Label(hdr_inner, text='   %d setting%s changed'
                              % (len(dirty), '' if len(dirty)==1 else 's'),
             bg=_BG_TOOLBAR, fg=_FG_MUTED, font=_F_MONO,
             anchor='w').pack(side='left', pady=(2, 0))

    # Body — scrollable list of diff rows
    body_outer = tk.Frame(top, bg=_BG_APP)
    body_outer.pack(side='top', fill='both', expand=True)
    canvas = tk.Canvas(body_outer, bg=_BG_APP, highlightthickness=0)
    vsb = tk.Scrollbar(body_outer, orient='vertical', command=canvas.yview,
                       bg=_BG_CARD, troughcolor=_BG_APP)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)
    body = tk.Frame(canvas, bg=_BG_APP)
    canvas.create_window((0, 0), window=body, anchor='nw')
    def _on_body_config(_=None):
        canvas.configure(scrollregion=canvas.bbox('all'))
    body.bind('<Configure>', _on_body_config)

    inner = tk.Frame(body, bg=_BG_APP)
    inner.pack(fill='x', padx=24, pady=18)

    # Group keys by section so the diff reads naturally
    by_section = {}
    for k in dirty:
        sec = _KEY_TO_SECTION.get(k, '?')
        by_section.setdefault(sec, []).append(k)

    section_labels = {s[0]: s[2] for s in _SECTIONS}

    for sec_key in [s[0] for s in _SECTIONS]:    # canonical order
        if sec_key not in by_section:
            continue
        keys = by_section[sec_key]
        # Section header
        tk.Label(inner,
                 text=section_labels.get(sec_key, sec_key).upper(),
                 bg=_BG_APP, fg=_PURPLE, font=_F_EYEBROW,
                 anchor='w').pack(fill='x', pady=(8, 4))

        for k in sorted(keys):
            new_v = rows[k].get_value() if k in rows else ''
            old_v = snap.get(k, '')
            row = tk.Frame(inner, bg=_BG_CARD,
                           highlightthickness=1,
                           highlightbackground=_BORDER)
            row.pack(fill='x', pady=2)
            row_inner = tk.Frame(row, bg=_BG_CARD)
            row_inner.pack(fill='x', padx=14, pady=8)

            # Key name
            tk.Label(row_inner, text=k,
                     bg=_BG_CARD, fg=_FG, font=_F_LABEL,
                     anchor='w', width=34).pack(side='left')
            # was → new
            def _fmt(v):
                if isinstance(v, bool):
                    return 'on' if v else 'off'
                s = str(v) if v is not None else ''
                if not s:
                    return '\u2014'
                if '\n' in s:
                    s = s.replace('\n', '\u23ce ')
                return s if len(s) <= 28 else s[:25] + '\u2026'
            tk.Label(row_inner, text=_fmt(old_v),
                     bg=_BG_CARD, fg=_FG_DIM, font=_F_MONO,
                     anchor='w').pack(side='left', padx=(4, 4))
            tk.Label(row_inner, text='\u2192',
                     bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO).pack(side='left')
            tk.Label(row_inner, text=_fmt(new_v),
                     bg=_BG_CARD, fg=_WARN, font=_F_MONO,
                     anchor='w').pack(side='left', padx=(4, 0))

    # Footer with Cancel + Save + Push
    tk.Frame(top, bg=_BORDER, height=1).pack(side='bottom', fill='x')
    foot = tk.Frame(top, bg=_BG_SAVEBAR)
    foot.pack(side='bottom', fill='x')
    foot_inner = tk.Frame(foot, bg=_BG_SAVEBAR)
    foot_inner.pack(fill='x', padx=20, pady=11)
    _toolbar_btn(foot_inner, 'Push to PS5',
                 lambda: (top.destroy(), _push_to_ps5(app)),
                 kind='primary').pack(side='right', padx=(4, 0))
    _toolbar_btn(foot_inner, 'Save local',
                 lambda: (top.destroy(), _save_locally(app)),
                 kind='warn').pack(side='right', padx=(4, 0))
    _toolbar_btn(foot_inner, 'Cancel',
                 lambda: top.destroy(),
                 kind='ghost').pack(side='right', padx=(4, 0))


def _discard_changes(app):
    """Restore every dirty SettingRow to its initial value.

    This is "undo my edits since last load/save", NOT "reset to
    documented defaults" — those are different concepts. The defaults
    reset button lives elsewhere (Reset to defaults in the toolbar,
    once we wire it in P5).
    """
    if not getattr(app, '_smp_dirty_keys', None):
        return
    rows = app._smp_rows
    snap = app._smp_initial_values
    # Iterate over a copy because set_value will mutate _smp_dirty_keys
    for key in list(app._smp_dirty_keys):
        if key in rows and key in snap:
            try:
                rows[key].set_value(snap[key])
            except Exception:
                pass
    app._smp_dirty_keys = set()
    _refresh_dirty_ui(app)


# ════════════════════════════════════════════════════════════════════
# Log strip — single line, click to expand (expand is P6 polish).
# ════════════════════════════════════════════════════════════════════
def _build_log_strip(parent, app):
    inner = tk.Frame(parent, bg=_BG_LOG)
    inner.pack(fill='both', expand=True, padx=24, pady=9)

    tk.Label(inner, text='OUTPUT LOG',
             bg=_BG_LOG, fg=_FG_MUTED, font=_F_EYEBROW,
             anchor='w').pack(side='left')

    app._smp_log_status_var = tk.StringVar(value='idle')
    tk.Label(inner, textvariable=app._smp_log_status_var,
             bg=_BG_LOG, fg=_FG_DIM, font=_F_MONO,
             anchor='e').pack(side='right')


# ════════════════════════════════════════════════════════════════════
# SettingRow — single form-row primitive used by every config field.
#
# Layout (standard): 240px label / 220px control / 1fr help / auto badge
# Layout (wide):     240px label / 1fr control+help        / auto badge
#
# Standard rows are one line tall (~32px). Wide rows are used for
# multi-line textareas where the control needs the full remaining width.
# ════════════════════════════════════════════════════════════════════
class SettingRow:
    """Renders one config field as a row inside a section card.

    Required kwargs:
        parent       — the section card body to mount inside
        key          — config.ini key this row binds to
        label        — bold left-hand text
        kind         — 'toggle' | 'int' | 'text' | 'select' | 'textarea'
        value        — current value (str for non-toggle; bool/str for toggle)
        default      — documented default value (used for the badge)

    Optional kwargs:
        sublabel     — small muted text below the label
        help         — inline help text after the control
        unit         — unit suffix shown after numeric inputs (e.g. 'seconds')
        choices      — list[str] for 'select' kind
        danger       — show a red 'dangerous' pill on the label
        advanced     — show a grey 'advanced' pill on the label
        wide         — True for textarea / full-width rows
        on_change    — callback(key, new_value) fired on every edit
    """

    # Track row instances on the parent app so SettingRow.set_value /
    # SettingRow.reset can be called from outside the class without
    # the caller holding the instance reference.

    def __init__(self, parent, *, key, label, kind, value, default,
                 sublabel='', help='', unit='', choices=None,
                 danger=False, advanced=False, wide=False,
                 on_change=None):
        self.key       = key
        self.label     = label
        self.kind      = kind
        self.default   = default
        self.on_change = on_change
        self._wide     = wide or kind == 'textarea'
        self._danger   = danger
        self._advanced = advanced
        self._unit     = unit
        self._choices  = choices

        # Cell containers — created in _build_layout so we can flag
        # the row visually when its value differs from default.
        self._row = tk.Frame(parent, bg=_BG_CARD,
                             highlightthickness=0)
        self._row.pack(fill='x')

        # Left accent bar (2px) — purple/amber when changed
        self._accent = tk.Frame(self._row, bg=_BG_CARD, width=2)
        self._accent.pack(side='left', fill='y')

        # Bottom 1px hairline divider
        self._divider = tk.Frame(self._row.master, bg=_BORDER, height=1)
        # Packed AFTER the row by the caller's iteration order

        # Body holds the 3 or 4 grid columns
        self._body = tk.Frame(self._row, bg=_BG_CARD)
        self._body.pack(side='left', fill='x', expand=True, padx=(14, 14))

        self._build_layout(value, sublabel, help)
        self._refresh_badge()

        # Hover highlight
        def _enter(_=None):
            if not self._is_changed():
                for w in (self._row, self._body):
                    w.configure(bg=_BG_ROW_HOV)
                self._label_lbl.configure(bg=_BG_ROW_HOV)
                if self._sub_lbl is not None:
                    self._sub_lbl.configure(bg=_BG_ROW_HOV)
                self._ctl_cell.configure(bg=_BG_ROW_HOV)
                self._help_cell.configure(bg=_BG_ROW_HOV)
                self._badge_cell.configure(bg=_BG_ROW_HOV)
        def _leave(_=None):
            if not self._is_changed():
                for w in (self._row, self._body):
                    w.configure(bg=_BG_CARD)
                self._label_lbl.configure(bg=_BG_CARD)
                if self._sub_lbl is not None:
                    self._sub_lbl.configure(bg=_BG_CARD)
                self._ctl_cell.configure(bg=_BG_CARD)
                self._help_cell.configure(bg=_BG_CARD)
                self._badge_cell.configure(bg=_BG_CARD)
        self._row.bind('<Enter>', _enter)
        self._row.bind('<Leave>', _leave)

    # ── Public API ──
    def get_value(self):
        if self.kind == 'toggle':
            return bool(self._tk_var.get())
        if self.kind == 'textarea':
            return self._text_widget.get('1.0', 'end').rstrip('\n')
        return self._tk_var.get()

    def set_value(self, v):
        """Set the field value without firing on_change (loading from PS5)."""
        self._suppress_change = True
        try:
            if self.kind == 'toggle':
                self._tk_var.set(bool(v) if not isinstance(v, str)
                                 else str(v).strip().lower() in ('1', 'true',
                                                                  'yes', 'on'))
                self._refresh_toggle_visual()
            elif self.kind == 'textarea':
                self._text_widget.delete('1.0', 'end')
                if v:
                    if isinstance(v, list):
                        v = '\n'.join(v)
                    self._text_widget.insert('1.0', str(v))
            else:
                self._tk_var.set('' if v is None else str(v))
        finally:
            self._suppress_change = False
        self._refresh_badge()

    def reset(self):
        """Restore the documented default and fire on_change."""
        self.set_value(self.default)
        self._fire_change()

    # ── Internals ──
    def _build_layout(self, value, sublabel, help):
        # ── Label cell (280px) ────────────────────────────────────
        # Width matches the mock target. Pill (when present) goes on
        # a second row UNDER the label so a long label like "Force-mount
        # damaged filesystems" plus a DANGEROUS pill don't fight for
        # horizontal space.
        self._label_cell = tk.Frame(self._body, bg=_BG_CARD, width=280)
        self._label_cell.pack(side='left', fill='y')
        self._label_cell.pack_propagate(False)

        label_inner = tk.Frame(self._label_cell, bg=_BG_CARD)
        label_inner.pack(fill='both', expand=True, pady=8)

        # Label first
        self._label_lbl = tk.Label(label_inner, text=self.label,
                                   bg=_BG_CARD, fg=_FG, font=_F_LABEL,
                                   anchor='w',
                                   wraplength=270, justify='left')
        self._label_lbl.pack(anchor='w')

        # Pills (danger / advanced) — separate row below the label
        if self._danger or self._advanced:
            pill_row = tk.Frame(label_inner, bg=_BG_CARD)
            pill_row.pack(anchor='w', pady=(2, 0))
            if self._danger:
                _make_pill(pill_row, 'dangerous', kind='danger').pack(
                    side='left')
            if self._advanced:
                _make_pill(pill_row, 'advanced', kind='advanced').pack(
                    side='left', padx=(4 if self._danger else 0, 0))

        # Sublabel
        self._sub_lbl = None
        if sublabel:
            self._sub_lbl = tk.Label(label_inner, text=sublabel,
                                     bg=_BG_CARD, fg=_FG_MUTED, font=_F_META,
                                     anchor='w')
            self._sub_lbl.pack(fill='x', anchor='w')

        # ── Control cell (220px standard / 1fr wide) ─────────────────
        if self._wide:
            # Wide variant: control gets the remaining width
            self._ctl_cell = tk.Frame(self._body, bg=_BG_CARD)
            self._ctl_cell.pack(side='left', fill='both', expand=True,
                                pady=8, padx=(14, 0))
        else:
            self._ctl_cell = tk.Frame(self._body, bg=_BG_CARD, width=220)
            self._ctl_cell.pack(side='left', fill='y',
                                pady=8, padx=(14, 0))
            self._ctl_cell.pack_propagate(False)

        self._build_control(self._ctl_cell, value)

        # ── Help cell (1fr — only in standard layout) ────────────────
        if self._wide:
            # Wide rows skip the separate help column
            self._help_cell = tk.Frame(self._body, bg=_BG_CARD, width=0)
        else:
            self._help_cell = tk.Frame(self._body, bg=_BG_CARD)
            self._help_cell.pack(side='left', fill='both', expand=True,
                                 pady=8, padx=(10, 0))
            if help:
                tk.Label(self._help_cell, text=help,
                         bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO,
                         anchor='w', justify='left').pack(anchor='w')

        # ── Badge cell (auto) ─────────────────────────────────────────
        self._badge_cell = tk.Frame(self._body, bg=_BG_CARD)
        self._badge_cell.pack(side='right', fill='y', pady=8, padx=(10, 0))
        self._badge_lbl = tk.Label(self._badge_cell, text='',
                                   bg=_BG_CARD, fg=_FG_DIM, font=_F_MONO,
                                   cursor='hand2')
        self._badge_lbl.pack(anchor='e')
        self._badge_lbl.bind('<Button-1>', lambda _e: self.reset())

    def _build_control(self, cell, value):
        self._suppress_change = False
        if self.kind == 'toggle':
            self._build_toggle(cell, value)
        elif self.kind == 'int':
            self._build_int(cell, value)
        elif self.kind == 'text':
            self._build_text(cell, value)
        elif self.kind == 'select':
            self._build_select(cell, value)
        elif self.kind == 'textarea':
            self._build_textarea(cell, value)
        else:
            raise ValueError('unknown SettingRow kind: ' + repr(self.kind))

    # ── Control: toggle (canvas pill) ─────────────────────────────────
    def _build_toggle(self, cell, value):
        # BooleanVar mirrors the on/off state
        initial = bool(value) if not isinstance(value, str) else (
            value.strip().lower() in ('1', 'true', 'yes', 'on'))
        self._tk_var = tk.BooleanVar(value=initial)

        # Flat status pill — no canvas, no curves, so no aliasing.
        # Filled green rectangle with "ON" when active, dark grey with
        # "OFF" when inactive. Danger toggles flip green → red.
        self._toggle_pill = tk.Label(cell, text='ON',
                                      bg=_SUCCESS, fg='#000000',
                                      font=_F_MONO,
                                      padx=10, pady=2,
                                      cursor='hand2',
                                      width=3, anchor='center')
        self._toggle_pill.pack(side='left')
        self._toggle_pill.bind('<Button-1>',
                                lambda _e: self._on_toggle_click())

        # "on"/"off" inline help text alongside the pill (keeps the
        # status readable even when the colour cue is missed)
        self._toggle_label = tk.Label(cell,
                                       text='on' if initial else 'off',
                                       bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO)
        self._toggle_label.pack(side='left', padx=(10, 0))

        self._refresh_toggle_visual()

    def _refresh_toggle_visual(self):
        on = bool(self._tk_var.get())
        if on:
            bg = _DANGER if self._danger else _SUCCESS
            fg = '#ffffff' if self._danger else '#000000'
            text = 'ON'
        else:
            bg = _BG_CARD_HEAD
            fg = _FG_DIM
            text = 'OFF'
        if hasattr(self, '_toggle_pill'):
            self._toggle_pill.configure(text=text, bg=bg, fg=fg)
        if hasattr(self, '_toggle_label'):
            self._toggle_label.configure(text='on' if on else 'off')

    def _on_toggle_click(self):
        self._tk_var.set(not self._tk_var.get())
        self._refresh_toggle_visual()
        self._fire_change()

    # ── Control: int (number entry with optional unit) ────────────────
    def _build_int(self, cell, value):
        self._tk_var = tk.StringVar(value='' if value is None else str(value))
        entry = tk.Entry(cell, textvariable=self._tk_var,
                         bg=COLORS['bg_0'], fg=_FG,
                         insertbackground=_FG,
                         bd=0, relief='flat', font=_F_MONO,
                         width=8,
                         highlightthickness=1,
                         highlightbackground=_BORDER_STRG,
                         highlightcolor=_PURPLE)
        entry.pack(side='left', ipady=4, ipadx=6)
        if self._unit:
            tk.Label(cell, text=self._unit,
                     bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO).pack(
                side='left', padx=(8, 0))
        # Fire change on each keystroke (debouncing is a P5 polish task)
        self._tk_var.trace_add('write',
                                lambda *_: self._fire_change())

    # ── Control: text (free-form single-line) ─────────────────────────
    def _build_text(self, cell, value):
        self._tk_var = tk.StringVar(value='' if value is None else str(value))
        entry = tk.Entry(cell, textvariable=self._tk_var,
                         bg=COLORS['bg_0'], fg=_FG,
                         insertbackground=_FG,
                         bd=0, relief='flat', font=_F_MONO,
                         highlightthickness=1,
                         highlightbackground=_BORDER_STRG,
                         highlightcolor=_PURPLE)
        entry.pack(side='left', fill='x', expand=True, ipady=4, ipadx=6)
        self._tk_var.trace_add('write',
                                lambda *_: self._fire_change())

    # ── Control: select (tk.Menubutton + tk.Menu dropdown) ───────────
    def _build_select(self, cell, value):
        # We use tk.Menubutton instead of ttk.Combobox here. The clam
        # ttk theme renders Combobox values at a fixed Y offset that
        # doesn't match our font metrics, causing the visible text to
        # sit either above or below the field's visible area. A bare
        # tk.Menubutton respects bg/fg/font/padding consistently and
        # renders the value centered in its box.
        choices = list(self._choices or [])
        self._tk_var = tk.StringVar(value='' if value is None else str(value))

        mb = tk.Menubutton(cell,
                           textvariable=self._tk_var,
                           bg=COLORS['bg_0'], fg=_FG,
                           activebackground=_BG_ROW_HOV,
                           activeforeground=_FG,
                           font=_F_MONO,
                           bd=0, relief='flat',
                           padx=10, pady=4,
                           highlightthickness=1,
                           highlightbackground=_BORDER_STRG,
                           highlightcolor=_PURPLE,
                           anchor='w', width=14,
                           cursor='hand2',
                           indicatoron=True,
                           compound='right')
        mb.pack(side='left', anchor='w')

        # Build the dropdown menu — bare tk.Menu themed to match
        menu = tk.Menu(mb, tearoff=0,
                       bg=COLORS['bg_3'], fg=_FG,
                       activebackground=_PURPLE,
                       activeforeground='#ffffff',
                       bd=0, font=_F_MONO)
        for choice in choices:
            def _pick(v=choice):
                self._tk_var.set(v)
                # _tk_var.trace_add below will fire _fire_change
            menu.add_command(label=choice, command=_pick)
        mb.configure(menu=menu)

        self._tk_var.trace_add('write',
                                lambda *_: self._fire_change())

    # ── Control: textarea ─────────────────────────────────────────────
    def _build_textarea(self, cell, value):
        # 3 lines tall by default — matches the 54px target in the mock.
        # Caller can resize later if needed.
        self._text_widget = tk.Text(cell, height=3,
                                     bg=COLORS['bg_0'], fg=_FG,
                                     insertbackground=_FG,
                                     bd=0, relief='flat',
                                     font=_F_MONO, wrap='word',
                                     highlightthickness=1,
                                     highlightbackground=_BORDER_STRG,
                                     highlightcolor=_PURPLE)
        self._text_widget.pack(fill='both', expand=True)
        if value:
            if isinstance(value, list):
                value = '\n'.join(value)
            self._text_widget.insert('1.0', str(value))
        # Track changes via <<Modified>> — fires on any edit
        def _on_modified(_e):
            if not getattr(self, '_suppress_change', False):
                self._fire_change()
            self._text_widget.edit_modified(False)
        self._text_widget.bind('<<Modified>>', _on_modified)

    # ── Change handling + badge ───────────────────────────────────────
    def _fire_change(self):
        if getattr(self, '_suppress_change', False):
            return
        self._refresh_badge()
        if self.on_change:
            try:
                self.on_change(self.key, self.get_value())
            except Exception:
                pass

    def _is_changed(self):
        try:
            current = self.get_value()
        except Exception:
            return False
        default = self.default
        # Normalize for toggle comparison
        if self.kind == 'toggle':
            if isinstance(default, str):
                default = default.strip().lower() in ('1', 'true', 'yes', 'on')
            return bool(current) != bool(default)
        if isinstance(default, list):
            default = '\n'.join(default)
        return str(current) != str(default if default is not None else '')

    def _refresh_badge(self):
        changed = self._is_changed()
        if changed:
            # Off-default: show what the default IS plus a reset arrow.
            # Amber accent strip on the left of the row + amber badge.
            self._accent.configure(bg=_WARN)
            self._badge_lbl.configure(
                text='default: %s  \u21ba' % self._format_default(),
                fg=_WARN)
        else:
            # At-default: short muted "default" label, no value shown.
            # Matches the mock where unchanged rows just say "default".
            self._accent.configure(bg=_BG_CARD)
            self._badge_lbl.configure(
                text='default',
                fg=_FG_DIM)

    def _format_default(self):
        d = self.default
        if self.kind == 'toggle':
            if isinstance(d, str):
                d = d.strip().lower() in ('1', 'true', 'yes', 'on')
            return 'on' if d else 'off'
        if d is None or d == '':
            return '\u2014'   # em dash for empty default
        if isinstance(d, list):
            return '%d items' % len(d)
        s = str(d)
        # Trim long defaults for the badge
        return s if len(s) <= 18 else s[:15] + '\u2026'


def _make_pill(parent, text, kind='advanced'):
    """Small inline pill used on SettingRow labels for danger/advanced flags."""
    if kind == 'danger':
        bg = _blend_hex(COLORS['danger'], COLORS['bg_2'], 0.15)
        fg = COLORS['danger_hi']
        border = _blend_hex(COLORS['danger'], COLORS['bg_2'], 0.30)
    else:
        bg = _BG_CARD_HEAD
        fg = _FG_MUTED
        border = _BORDER_STRG
    return tk.Label(parent, text=text.upper(),
                    bg=bg, fg=fg, font=_F_META,
                    padx=6, pady=0,
                    highlightthickness=1, highlightbackground=border)



def _install_placeholder(entry, var, placeholder):
    """Cheap placeholder text for tk.Entry. Greys out when empty + unfocused.

    Exposes `entry._placeholder_active` for callers (like the search
    filter) that need to know whether the var contents are real or
    just the ghost placeholder.
    """
    real_fg = _FG
    ghost_fg = _FG_DIM
    state = {'ghosted': True}

    def _show_ghost():
        var.set(placeholder)
        entry.configure(fg=ghost_fg)
        state['ghosted'] = True
        entry._placeholder_active = True

    def _hide_ghost():
        if state['ghosted']:
            var.set('')
            entry.configure(fg=real_fg)
            state['ghosted'] = False
            entry._placeholder_active = False

    def _on_focus_in(_=None):
        _hide_ghost()

    def _on_focus_out(_=None):
        if not var.get().strip():
            _show_ghost()

    _show_ghost()
    entry.bind('<FocusIn>', _on_focus_in)
    entry.bind('<FocusOut>', _on_focus_out)



# ════════════════════════════════════════════════════════════════════
# Form <-> dict serialization
# ════════════════════════════════════════════════════════════════════
def _collect_form(app):
    """Read every form widget into a plain dict of settings.

    Prefers SettingRow instances (new layout) over the legacy
    StringVar/BooleanVar/Text-widget plumbing. As sections are ported
    in Prompts 3-4 their keys move from the legacy path to the
    SettingRow path.
    """
    out = {}

    # 1. SettingRow instances (preferred — these are the ported sections)
    rows = getattr(app, '_smp_rows', {}) or {}
    for key, row in rows.items():
        try:
            v = row.get_value()
        except Exception:
            continue
        if isinstance(v, bool):
            out[key] = '1' if v else '0'
        elif v is None:
            out[key] = ''
        else:
            s = str(v).strip() if not isinstance(v, str) else v.strip()
            if s != '':
                out[key] = s

    # 2. Legacy StringVar/BooleanVar (only for keys NOT covered above)
    for key, var in app._smp_vars.items():
        if key in out:
            continue
        try:
            out[key] = var.get().strip()
        except Exception:
            pass
    for key, var in app._smp_check_vars.items():
        if key in out:
            continue
        try:
            out[key] = '1' if var.get() else '0'
        except Exception:
            pass

    # 3. Legacy Text widgets (multiline lists from not-yet-ported sections)
    def _txt(attr, key):
        if key in out:
            return
        w = getattr(app, attr, None)
        if w is None:
            return
        try:
            val = w.get('1.0', 'end').strip()
        except Exception:
            return
        if val:
            out[key] = val
    _txt('_smp_kstuff_no_pause_txt', 'kstuff_no_pause')
    _txt('_smp_kstuff_delay_txt',    'kstuff_delay')
    _txt('_smp_scanpaths_txt',       'scanpath')
    _txt('_smp_global_excludes_txt', 'global_fakelib_exclude')
    _txt('_smp_per_image_txt',       'per_image_rules')
    return out


def _apply_form(app, data):
    """Push a dict of values back into the form widgets.

    Same preference order as _collect_form: SettingRow first, legacy
    second. set_value on a SettingRow is suppress-flagged so this won't
    fire spurious dirty events while loading.
    """
    rows = getattr(app, '_smp_rows', {}) or {}

    # 1. SettingRow instances
    for key, val in data.items():
        if key in rows:
            try:
                rows[key].set_value(val)
            except Exception:
                pass

    # 2. Legacy StringVar/BooleanVar (only for keys NOT handled above)
    bool_keys = set(app._smp_check_vars.keys())
    for key, val in data.items():
        if key in rows:
            continue
        if key in bool_keys:
            s = str(val).strip().lower()
            app._smp_check_vars[key].set(s in ('1', 'true', 'yes', 'on'))
        elif key in app._smp_vars:
            app._smp_vars[key].set(str(val))

    # 3. Legacy Text widgets — only set if no matching SettingRow exists
    def _set_txt(attr, key, val):
        if key in rows:
            return
        w = getattr(app, attr, None)
        if w is None:
            return
        try:
            w.delete('1.0', 'end')
            if val:
                if isinstance(val, list):
                    val = '\n'.join(val)
                w.insert('1.0', str(val))
        except Exception:
            pass
    _set_txt('_smp_kstuff_no_pause_txt', 'kstuff_no_pause',
             data.get('kstuff_no_pause', ''))
    _set_txt('_smp_kstuff_delay_txt',    'kstuff_delay',
             data.get('kstuff_delay', ''))
    _set_txt('_smp_scanpaths_txt',       'scanpath',
             data.get('scanpath', ''))
    _set_txt('_smp_global_excludes_txt', 'global_fakelib_exclude',
             data.get('global_fakelib_exclude', ''))
    _set_txt('_smp_per_image_txt',       'per_image_rules',
             data.get('per_image_rules', ''))

    # After apply, refresh the dirty snapshot so the form is "clean"
    # — load/push/save-local all end in a clean state.
    if hasattr(app, '_smp_initial_values'):
        for key, row in rows.items():
            try:
                app._smp_initial_values[key] = row.get_value()
            except Exception:
                pass
        app._smp_dirty_keys = set()
        _refresh_dirty_ui(app)


def _render_ini(data):
    """Render the collected form data as a config.ini text string,
    matching the format ShadowMountPlus expects.
    """
    lines = [
        '# Generated by exFAT Image Builder',
        '# ShadowMountPlus runtime config',
        '# Edit and re-push from the GUI.',
        '',
    ]

    def emit(key):
        v = data.get(key)
        if v in (None, ''):
            return
        lines.append('%s=%s' % (key, v))

    # Section 1: kstuff
    lines.append('# ── kstuff ─────────────────────────────────────')
    for k in ('kstuff_game_auto_toggle', 'kstuff_crash_detection',
              'kstuff_pause_delay_image_seconds',
              'kstuff_pause_delay_direct_seconds'):
        emit(k)
    for raw in (data.get('kstuff_no_pause', '') or '').splitlines():
        t = raw.strip()
        if t and not t.startswith('#'):
            lines.append('kstuff_no_pause=' + t)
    for raw in (data.get('kstuff_delay', '') or '').splitlines():
        t = raw.strip()
        if t and not t.startswith('#'):
            lines.append('kstuff_delay=' + t)
    lines.append('')

    # Section 2: mount
    lines.append('# ── mount ──────────────────────────────────────')
    for k in ('mount_read_only', 'force_mount', 'app_install_all',
              'exfat_backend', 'ufs_backend'):
        emit(k)
    lines.append('')

    # Section 3: scanning
    lines.append('# ── scanning ───────────────────────────────────')
    for k in ('scan_depth', 'scan_interval_seconds', 'stability_wait_seconds'):
        emit(k)
    for raw in (data.get('scanpath', '') or '').splitlines():
        t = raw.strip()
        if t and not t.startswith('#'):
            lines.append('scanpath=' + t)
    lines.append('')

    # Section 4: fakelib
    lines.append('# ── fakelib ────────────────────────────────────')
    for k in ('backport_fakelib', 'global_fakelib',
              'global_fakelib_path', 'global_fakelib_priority'):
        emit(k)
    for raw in (data.get('global_fakelib_exclude', '') or '').splitlines():
        t = raw.strip()
        if t and not t.startswith('#'):
            lines.append('global_fakelib_exclude=' + t)
    lines.append('')

    # Section 5: per-image rules (already keyed lines)
    rules = (data.get('per_image_rules', '') or '').strip()
    if rules:
        lines.append('# ── per-image overrides ────────────────────')
        for raw in rules.splitlines():
            t = raw.strip()
            if t and not t.startswith('#'):
                # User may have written `key=value` or just `value`.
                # If no '=', skip it as a comment.
                if '=' in t:
                    lines.append(t)
        lines.append('')

    # Section 6: advanced
    lines.append('# ── advanced ───────────────────────────────────')
    for k in ('debug', 'quiet_mode',
              'lvd_exfat_sector_size', 'lvd_ufs_sector_size',
              'lvd_pfs_sector_size',
              'md_exfat_sector_size', 'md_ufs_sector_size'):
        emit(k)

    return '\n'.join(lines) + '\n'


def _parse_ini(text):
    """Parse a config.ini text back into a flat dict matching form keys.

    Repeated keys (kstuff_no_pause, kstuff_delay, scanpath,
    global_fakelib_exclude) are collected into newline-joined strings.
    image_ro / image_rw / image_sector are collected verbatim into
    per_image_rules.
    """
    out = {}
    multi = {
        'kstuff_no_pause':         [],
        'kstuff_delay':            [],
        'scanpath':                [],
        'global_fakelib_exclude':  [],
    }
    per_image = []

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#') or line.startswith(';'):
            continue
        if '=' not in line:
            continue
        k, _, v = line.partition('=')
        k = k.strip()
        v = v.strip()
        if not k:
            continue

        if k in multi:
            multi[k].append(v)
            continue
        if k in ('image_ro', 'image_rw', 'image_sector'):
            per_image.append('%s=%s' % (k, v))
            continue
        out[k] = v

    for k, lst in multi.items():
        if lst:
            out[k] = '\n'.join(lst)
    if per_image:
        out['per_image_rules'] = '\n'.join(per_image)
    return out


# ════════════════════════════════════════════════════════════════════
# Button handlers
# ════════════════════════════════════════════════════════════════════
def _set_status(app, text, color=MUTED):
    """Update both the top-right status label (transient) AND the
    bottom output-log strip (sticky activity line with timestamp).
    """
    from time import strftime
    try:
        app._smp_status_var.set(text)
        app._smp_status_lbl.config(fg=color)
    except Exception:
        pass
    # Persistent log strip: "HH:MM:SS · the same status message"
    log_var = getattr(app, '_smp_log_status_var', None)
    if log_var is not None:
        try:
            log_var.set('%s  \u00b7  %s' % (strftime('%H:%M:%S'), text))
        except Exception:
            pass


def _friendly_ftp_error(action, err):
    """Translate ftplib / socket errors into something a user can act on.
    Returns (short_status, dialog_title, dialog_body) tuple.
    """
    s = str(err)
    low = s.lower()

    # Connection refused — FTP server not running on the PS5
    if '10061' in s or 'refused' in low:
        return (action + ' failed: PS5 FTP server not running',
                'FTP server not running',
                'The PS5 refused the FTP connection.\n\n'
                'The PS5 is reachable on the network, but no FTP server is '
                'listening on that port.\n\n'
                'Most PS5 jailbreaks need FTP started manually after each boot. '
                'Common ways to start it:\n'
                '  \u2022 etaHEN \u2192 Start FTP\n'
                '  \u2022 itemzflow \u2192 enable FTP server\n'
                '  \u2022 Launch your usual FTP payload\n\n'
                'Start FTP on the PS5, then try again.')

    # Timeout — wrong IP, PS5 asleep, or firewall
    if '10060' in s or 'timed out' in low or 'timeout' in low:
        return (action + ' failed: connection timed out',
                'Connection timed out',
                'The PS5 did not respond in time.\n\n'
                'Check:\n'
                '  \u2022 PS5 is powered on (not in rest mode)\n'
                '  \u2022 IP address in FTP tab matches the PS5\n'
                '  \u2022 PS5 and PC are on the same network')

    # No route to host — wrong IP / network down
    if '10065' in s or 'no route' in low or 'unreachable' in low:
        return (action + ' failed: host unreachable',
                'Host unreachable',
                'No network route to the PS5.\n\n'
                'Check the IP address in the FTP tab and that the PS5 is '
                'on the same network as this PC.')

    # 550 = file not found / permission denied on the FTP server
    if '550' in s:
        return (action + ' failed: file/path not accessible on PS5',
                'Path not accessible',
                'The PS5 FTP server rejected the path. The target file or '
                'directory may not exist, or the FTP user may not have '
                'permission to write there.\n\nDetails: ' + s[:200])

    # Generic fallback
    return (action + ' failed: ' + s[:60],
            action + ' failed',
            'Error details:\n\n' + s)


def _save_locally(app):
    data = _collect_form(app)
    app._settings['shadowmount_config'] = data
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass

    # Saving locally clears the dirty state — current values become
    # the new baseline.
    rows = getattr(app, '_smp_rows', {}) or {}
    if hasattr(app, '_smp_initial_values'):
        for key, row in rows.items():
            try:
                app._smp_initial_values[key] = row.get_value()
            except Exception:
                pass
        app._smp_dirty_keys = set()
        _refresh_dirty_ui(app)

    _set_status(app, 'Saved locally \u2713', SUCCESS)


def _reset_defaults(app):
    if not messagebox.askyesno('Reset',
            'Reset all ShadowMountPlus fields to documented defaults?\n\n'
            'This clears your custom values in the form but does NOT '
            'overwrite the config on the PS5.'):
        return
    _apply_form(app, DEFAULTS)
    # Also clear the multiline lists
    for attr in ('_smp_kstuff_no_pause_txt', '_smp_kstuff_delay_txt',
                 '_smp_scanpaths_txt', '_smp_global_excludes_txt',
                 '_smp_per_image_txt'):
        w = getattr(app, attr, None)
        if w is not None:
            try:
                w.delete('1.0', 'end')
            except Exception:
                pass
    _set_status(app, 'Reset to defaults', MUTED)


def _load_from_ps5(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab (or Settings) first.')
        return

    _set_status(app, 'Fetching config from PS5...', ACCENT)

    def worker():
        try:
            ftp = app._ftp_connect()
            try:
                buf = io.BytesIO()
                ftp.retrbinary('RETR ' + REMOTE_CONFIG_PATH, buf.write)
                text = buf.getvalue().decode('utf-8', errors='replace')
            finally:
                try:
                    ftp.quit()
                except Exception:
                    pass
            app.after(0, _on_loaded, app, text)
        except Exception as e:
            err = str(e)
            # 550 here specifically means "file doesn't exist yet" — that's a
            # legitimate first-run state, not a failure. Keep the helpful
            # message.
            if '550' in err or 'no such' in err.lower():
                app.after(0, _set_status, app,
                    'No config on PS5 yet \u2014 push to create it', WARNING)
                app.after(0, messagebox.showinfo, 'Not on PS5',
                    REMOTE_CONFIG_PATH + ' does not exist on the PS5.\n\n'
                    'This is normal if ShadowMountPlus has never been run. '
                    'Edit the form locally and click \u201cPush to PS5\u201d '
                    'to create it.')
            else:
                short, title, body = _friendly_ftp_error('Load', e)
                app.after(0, _set_status, app, short, DANGER)
                app.after(0, messagebox.showerror, title, body)
    threading.Thread(target=worker, daemon=True).start()


def _on_loaded(app, text):
    data = _parse_ini(text)
    _apply_form(app, data)
    _set_status(app, 'Loaded %d keys from PS5 \u2713' % len(data), SUCCESS)

    # Rail footer: "Loaded from PS5 · HH:MM:SS\nN of M settings present"
    footer_var = getattr(app, '_smp_load_footer_var', None)
    if footer_var is not None:
        from time import strftime
        n_rows = len(getattr(app, '_smp_rows', {}) or {})
        footer_var.set('Loaded from PS5 \u00b7 %s\n%d of %d settings present'
                       % (strftime('%H:%M:%S'), len(data), n_rows))


def _push_to_ps5(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab (or Settings) first.')
        return

    data = _collect_form(app)
    # Validate numerics
    for k in ('kstuff_pause_delay_image_seconds',
              'kstuff_pause_delay_direct_seconds',
              'scan_interval_seconds', 'stability_wait_seconds'):
        v = data.get(k, '').strip()
        if v == '':
            continue
        try:
            n = int(v)
            if n < 0 or n > 3600:
                raise ValueError('out of range')
        except ValueError:
            messagebox.showwarning('Invalid value',
                '%s must be a whole number in 0\u20133600. Got: %r' % (k, v))
            return
    # Validate kstuff_delay per-line
    for raw in (data.get('kstuff_delay', '') or '').splitlines():
        t = raw.strip()
        if not t or t.startswith('#'):
            continue
        if ':' not in t:
            messagebox.showwarning('Invalid kstuff_delay rule',
                'Expected TITLEID:SECONDS, got: %r' % t)
            return
        tid, _, sec = t.partition(':')
        try:
            n = int(sec.strip())
            if n < 0 or n > 3600:
                raise ValueError()
        except Exception:
            messagebox.showwarning('Invalid kstuff_delay rule',
                'Seconds must be 0\u20133600, got: %r in rule %r' % (sec, t))
            return

    ini_text = _render_ini(data)
    if not messagebox.askyesno('Push to PS5',
            'Write %d bytes to:\n%s\n\nContinue?'
            % (len(ini_text), REMOTE_CONFIG_PATH)):
        return

    _set_status(app, 'Uploading config to PS5...', ACCENT)
    # Save locally too so reopen restores
    app._settings['shadowmount_config'] = data
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass

    def worker():
        try:
            ftp = app._ftp_connect()
            try:
                # Ensure dir exists — best-effort MKD, ignore failure
                try:
                    ftp.mkd('/data/shadowmount')
                except Exception:
                    pass
                buf = io.BytesIO(ini_text.encode('utf-8'))
                ftp.storbinary('STOR ' + REMOTE_CONFIG_PATH, buf)
            finally:
                try:
                    ftp.quit()
                except Exception:
                    pass
            app.after(0, _set_status, app,
                      'Pushed config to PS5 \u2713', SUCCESS)
        except Exception as e:
            short, title, body = _friendly_ftp_error('Push', e)
            app.after(0, _set_status, app, short, DANGER)
            app.after(0, messagebox.showerror, title, body)
    threading.Thread(target=worker, daemon=True).start()


def _send_payload(app):
    # Read elf path: SettingRow first (new layout), then legacy StringVar,
    # then settings dict, then empty.
    elf_path = ''
    rows = getattr(app, '_smp_rows', {}) or {}
    if 'smp_payload_path' in rows:
        try:
            elf_path = str(rows['smp_payload_path'].get_value()).strip()
        except Exception:
            pass
    if not elf_path:
        legacy_var = getattr(app, '_smp_elf_path_var', None)
        if legacy_var is not None:
            try:
                elf_path = legacy_var.get().strip()
            except Exception:
                pass
    if not elf_path:
        elf_path = str(app._settings.get('smp_payload_path', '')).strip()

    if not elf_path or not os.path.isfile(elf_path):
        messagebox.showwarning('Locate ELF',
            'Pick the shadowmountplus.elf file first using the Browse '
            'button on the Payload section.')
        return

    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab (or Settings) first.')
        return

    # Read port: same priority order.
    port_str = ''
    if 'smp_payload_port' in rows:
        try:
            port_str = str(rows['smp_payload_port'].get_value()).strip()
        except Exception:
            pass
    if not port_str:
        legacy_var = getattr(app, '_smp_elf_port_var', None)
        if legacy_var is not None:
            try:
                port_str = legacy_var.get().strip()
            except Exception:
                pass
    if not port_str:
        port_str = str(app._settings.get('smp_payload_port', 9021))
    try:
        port = int(port_str)
    except (ValueError, TypeError):
        port = 9021
    if not (1 <= port <= 65535):
        messagebox.showwarning('Invalid port',
            'Payload port must be 1\u201365535.')
        return

    # Persist port + path
    app._settings['smp_payload_port'] = port
    app._settings['smp_payload_path'] = elf_path
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass

    size = os.path.getsize(elf_path)
    _set_status(app, 'Sending payload to %s:%d...' % (ip, port), ACCENT)

    def worker():
        try:
            start = time.time()
            sent = 0
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(10)
                s.connect((ip, port))
                s.settimeout(30)
                with open(elf_path, 'rb') as f:
                    while True:
                        chunk = f.read(65536)
                        if not chunk:
                            break
                        s.sendall(chunk)
                        sent += len(chunk)
                        pct = int(sent / size * 100) if size else 0
                        app.after(0, _set_status, app,
                            'Sending payload... %d%%' % pct, ACCENT)
            elapsed = time.time() - start
            app.after(0, _set_status, app,
                'Payload sent \u2713 (%.1f MB in %.1fs)'
                % (size / 1024 / 1024, elapsed), SUCCESS)
        except Exception as e:
            short, title, body = _friendly_ftp_error('Send', e)
            # Payload sends use TCP 9021, not FTP — adjust the body
            # of the 'connection refused' case so the user looks in
            # the right place. Use the same port we resolved earlier
            # in _send_payload (closure-captured) rather than the
            # legacy _smp_elf_port_var which may not exist.
            if '10061' in str(e) or 'refused' in str(e).lower():
                body = ('The PS5 refused the payload connection on port '
                        + str(port) + '.\n\n'
                        'A payload listener (etaHEN, GoldHEN-style loader, '
                        'or PLK Autoloader) must be running on the PS5 and '
                        'listening on that port before you can send an ELF.\n\n'
                        'Start your payload host on the PS5, then try again.')
            app.after(0, _set_status, app, short, DANGER)
            app.after(0, messagebox.showerror, title, body)
    threading.Thread(target=worker, daemon=True).start()


def _fetch_debug_log(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab (or Settings) first.')
        return

    _set_status(app, 'Fetching debug.log from PS5...', ACCENT)

    def worker():
        try:
            ftp = app._ftp_connect()
            try:
                buf = io.BytesIO()
                ftp.retrbinary('RETR ' + REMOTE_DEBUG_LOG, buf.write)
                text = buf.getvalue().decode('utf-8', errors='replace')
            finally:
                try:
                    ftp.quit()
                except Exception:
                    pass
            app.after(0, _show_log_window, app, text)
            app.after(0, _set_status, app,
                'Fetched debug.log (%d bytes) \u2713' % len(text), SUCCESS)
        except Exception as e:
            err = str(e)
            if '550' in err or 'no such' in err.lower():
                app.after(0, _set_status, app,
                    'No debug.log on PS5 yet', WARNING)
                app.after(0, messagebox.showinfo, 'No log yet',
                    REMOTE_DEBUG_LOG + ' does not exist on the PS5.\n\n'
                    'Either ShadowMountPlus has not run yet, or debug logging '
                    'is disabled (set debug=1 in the Advanced section and '
                    'push the config).')
            else:
                short, title, body = _friendly_ftp_error('Fetch', e)
                app.after(0, _set_status, app, short, DANGER)
                app.after(0, messagebox.showerror, title, body)
    threading.Thread(target=worker, daemon=True).start()


def _show_log_window(app, text):
    win = tk.Toplevel(app)
    win.title('ShadowMountPlus debug.log')
    win.configure(bg=BG)
    win.geometry('900x600')

    bar = tk.Frame(win, bg=SURFACE)
    bar.pack(fill='x')
    tk.Label(bar, text=REMOTE_DEBUG_LOG,
             font=('Consolas', 9), bg=SURFACE, fg=MUTED).pack(
                 side='left', padx=10, pady=8)

    def _save_as():
        p = filedialog.asksaveasfilename(
            defaultextension='.log',
            initialfile='shadowmount-debug.log',
            filetypes=[('Log files', '*.log'), ('All files', '*.*')])
        if p:
            try:
                with open(p, 'w', encoding='utf-8') as f:
                    f.write(text)
            except Exception as e:
                messagebox.showerror('Save failed', str(e))

    tk.Button(bar, text='Save as...', command=_save_as,
              bg=SURFACE2, fg=TEXT, font=('Segoe UI', 9),
              activebackground=BORDER, activeforeground=TEXT,
              bd=0, padx=10, pady=4, cursor='hand2',
              relief='flat').pack(side='right', padx=10, pady=6)

    txt = tk.Text(win, bg=BG, fg=TEXT, insertbackground=TEXT,
                  font=('Consolas', 9), bd=0, wrap='word')
    txt.pack(fill='both', expand=True)
    sb = tk.Scrollbar(txt, command=txt.yview,
                      bg=SURFACE2, troughcolor=BG)
    sb.pack(side='right', fill='y')
    txt.configure(yscrollcommand=sb.set)
    txt.insert('1.0', text)
    txt.see('end')
    txt.configure(state='disabled')


# ── Stop ShadowMountPlus (release all mounts so USB drive can be unplugged) ──
REMOTE_STOP_FILE = '/data/shadowmount/STOP'

def _stop_shadowmount(app):
    """Drop the STOP sentinel on the PS5 so ShadowMountPlus releases all
    its mounts. Once the cleanup runs, the user can safely unplug the
    USB/nvme enclosure carrying the game images.

    Re-mounting later means re-sending the ELF payload (the ELF itself
    clears the stale STOP file at startup, so the file does not need
    manual cleanup).
    """
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab (or Settings) first.')
        return

    if not messagebox.askyesno('Stop ShadowMountPlus',
            'This will tell ShadowMountPlus to release ALL mounted games '
            'so you can safely unplug your USB / nvme enclosure.\n\n'
            'After unplugging, ShadowMountPlus will NOT auto-mount anything '
            'until you re-send the payload (click \u201cSend Payload (ELF)\u201d '
            'on this tab once the drive is plugged back in).\n\n'
            'Continue?'):
        return

    _set_status(app, 'Sending STOP signal to PS5...', ACCENT)

    def worker():
        try:
            ftp = app._ftp_connect()
            try:
                # Make sure the directory exists (best-effort)
                try:
                    ftp.mkd('/data/shadowmount')
                except Exception:
                    pass
                # Write an empty sentinel file. ShadowMountPlus only
                # cares about its existence, not contents.
                buf = io.BytesIO(b'')
                ftp.storbinary('STOR ' + REMOTE_STOP_FILE, buf)
            finally:
                try:
                    ftp.quit()
                except Exception:
                    pass

            # Give the ELF a moment to notice the sentinel and run its
            # cleanup. The "[SHUTDOWN] cleanup complete" path takes a
            # second or two per mount; 3s is enough for most libraries.
            app.after(0, _set_status, app,
                'STOP sent \u2014 waiting for cleanup (3s)...', ACCENT)
            time.sleep(3)

            app.after(0, _on_stopped, app)
        except Exception as e:
            short, title, body = _friendly_ftp_error('Stop', e)
            app.after(0, _set_status, app, short, DANGER)
            app.after(0, messagebox.showerror, title, body)

    threading.Thread(target=worker, daemon=True).start()


def _on_stopped(app):
    _set_status(app,
        'ShadowMountPlus stopped \u2713 \u2014 safe to unplug drive',
        SUCCESS)
    messagebox.showinfo('Safe to unplug',
        'ShadowMountPlus has been signalled to stop.\n\n'
        'All game mounts should now be released. You can safely unplug '
        'your USB / nvme enclosure.\n\n'
        'To resume auto-mounting later:\n'
        '  1. Plug the drive back in\n'
        '  2. Click \u201cSend Payload (ELF)\u201d on this tab\n\n'
        'ShadowMountPlus clears the STOP flag automatically on the '
        'next payload run, so no manual cleanup is required.')
