"""MicroMount sub-tab for the PS5 tab.

Provides a GUI for:
  * Editing /data/micromount/config.ini fields (target dir, scan paths,
    scan depth/interval, debug, and the full LVD/PFS mount profile)
  * Loading the current config from the PS5 over FTP
  * Pushing the edited config back to the PS5
  * Sending the micromount.elf payload to the PS5 (TCP port 9021)
  * Fetching /data/micromount/debug.log for inspection

The form mirrors the keys documented in MicroMount's README and the
bundled `config.ini.example` template.  Defaults match the README.

Architecture mirrors tab_shadowmount.py exactly — same SettingRow
primitive, same rail/content/savebar/logstrip layout, same FTP helpers
re-used from the app.  The only deliberate differences are:
  * Teal accent colour instead of purple (distinct brand identity).
  * Different section catalogue and config keys.
  * Different remote paths.
"""

import io
import os
import re
import socket
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox

from tkinter_theme import COLORS, FONTS

from ui.shared.ps5_kit import ControlHero
from ui.shared.page_head import make_themed_button

# ── Colour tokens ────────────────────────────────────────────────────
_BG_APP       = COLORS['bg_1']
_BG_RAIL      = COLORS['bg_0']
_BG_TOOLBAR   = COLORS['bg_2']
_BG_CARD      = COLORS['bg_2']
_BG_CARD_HEAD = COLORS['bg_3']
_BG_ROW_HOV   = COLORS['bg_3']
_BG_SAVEBAR   = COLORS['bg_0']
_BG_LOG       = COLORS['bg_0']
_BORDER       = COLORS['border_2']
_BORDER_STRG  = COLORS['border_3']
_FG           = COLORS['fg_1']
_FG_MUTED     = COLORS['fg_3']
_FG_DIM       = COLORS['fg_5']
_ACCENT       = COLORS['accent']
_SUCCESS      = COLORS['success']
_WARN         = COLORS['warn']
_WARN_BG      = COLORS['warn_bg']
_DANGER       = COLORS['danger']

# Teal — MicroMount brand colour (distinct from ShadowMount+'s purple)
_TEAL         = COLORS['teal']
_TEAL_HI      = COLORS['teal_hi']


def _blend_hex(fg_hex, bg_hex, alpha):
    fg_hex = fg_hex.lstrip('#')
    bg_hex = bg_hex.lstrip('#')
    fr, fg, fb = int(fg_hex[0:2], 16), int(fg_hex[2:4], 16), int(fg_hex[4:6], 16)
    br, bg_, bb = int(bg_hex[0:2], 16), int(bg_hex[2:4], 16), int(bg_hex[4:6], 16)
    r = round(fr * alpha + br * (1 - alpha))
    g = round(fg * alpha + bg_ * (1 - alpha))
    b = round(fb * alpha + bb * (1 - alpha))
    return '#%02x%02x%02x' % (r, g, b)


_TEAL_SOFT   = _blend_hex(_TEAL, COLORS['bg_1'], 0.10)
_TEAL_BORDER = _blend_hex(_TEAL, COLORS['bg_1'], 0.30)

# ── Font tokens ──────────────────────────────────────────────────────
_F_BODY    = FONTS['body']
_F_LABEL   = FONTS['label']
_F_META    = FONTS['meta']
_F_H2      = FONTS['h2']
_F_H3      = FONTS['h3']
_F_EYEBROW = FONTS['eyebrow']
_F_MONO    = FONTS['mono_sm']
_F_BUTTON  = FONTS['button']

# ── Legacy aliases (used as default arg values — must resolve at import) ──
BG       = _BG_APP
SURFACE  = _BG_TOOLBAR
SURFACE2 = _BG_CARD_HEAD
BORDER   = _BORDER
TEXT     = _FG
MUTED    = _FG_MUTED
ACCENT   = _TEAL
SUCCESS  = _SUCCESS
WARNING  = _WARN
DANGER   = _DANGER

# ── Remote paths ──────────────────────────────────────────────────────
REMOTE_CONFIG_PATH = '/data/micromount/config.ini'
REMOTE_DEBUG_LOG   = '/data/micromount/debug.log'

# ── Defaults (from README / config.ini.example) ───────────────────────
DEFAULTS = {
    'target_directory':      '/data/homebrew',
    'scan_depth':            '1',
    'scan_interval_seconds': '30',
    'debug':                 '1',
    # Mount profile
    'lvd_image_type':        '0',
    'lvd_sector_size':       '65536',
    'lvd_secondary_unit':    '65536',
    'lvd_raw_flags':         '0x9',
    'pfs_fstype':            'pfs',
    'pfs_mkeymode':          'AC',
    'pfs_budgetid':          'system',
    'pfs_sigverify':         '0',
    'pfs_playgo':            '0',
    'pfs_disc':              '0',
    'pfs_use_ekpfs':         '1',
    'pfs_read_only':         '1',
    'pfs_force':             '0',
}


# ════════════════════════════════════════════════════════════════════
# Public entry point
# ════════════════════════════════════════════════════════════════════
def build_micromount_tab(parent, app):
    """Construct the MicroMount sub-tab inside `parent`.

    `app` is the main ExFATBuilder instance — we re-use its FTP helpers
    (_ftp_connect, _ftp_ip_var, _ftp_port_var) and settings store.
    """
    for w in parent.winfo_children():
        w.destroy()

    parent.configure(bg=_BG_APP)

    # State init
    if not hasattr(app, '_mm_vars'):
        app._mm_vars = {}
    if not hasattr(app, '_mm_status_var'):
        app._mm_status_var = tk.StringVar(value='')

    app._mm_rows           = {}
    app._mm_initial_values = {}
    app._mm_dirty_keys     = set()
    app._mm_section_frames = {}
    app._mm_rail_rows      = {}
    app._mm_active_section = None

    _build_toolbar(parent, app)

    main = tk.Frame(parent, bg=_BG_APP)
    main.pack(side='top', fill='both', expand=True)

    rail = tk.Frame(main, bg=_BG_RAIL, width=220)
    rail.pack(side='left', fill='y')
    rail.pack_propagate(False)
    _build_rail(rail, app)

    tk.Frame(main, bg=_BORDER, width=1).pack(side='left', fill='y')

    log_strip = tk.Frame(main, bg=_BG_LOG, height=38)
    log_strip.pack(side='bottom', fill='x')
    log_strip.pack_propagate(False)
    _build_log_strip(log_strip, app)

    savebar_border = tk.Frame(main, bg=_BORDER, height=1)
    savebar_border.pack(side='bottom', fill='x')

    savebar = tk.Frame(main, bg=_BG_SAVEBAR)
    savebar.pack(side='bottom', fill='x')
    _build_savebar(savebar, app)

    app._mm_savebar_frame  = savebar
    app._mm_savebar_border = savebar_border
    savebar.pack_forget()
    savebar_border.pack_forget()

    content_outer = tk.Frame(main, bg=_BG_APP)
    content_outer.pack(side='left', fill='both', expand=True)
    _build_content(content_outer, app)

    _select_section(app, 'overview')

    def _kb_save(_=None):
        if app._mm_dirty_keys:
            _save_locally(app)
        return 'break'

    def _kb_push(_=None):
        if app._mm_dirty_keys:
            _push_to_ps5(app)
        return 'break'

    parent.bind('<Control-s>',       _kb_save)
    parent.bind('<Control-Shift-S>', _kb_push)


# ════════════════════════════════════════════════════════════════════
# Section catalogue
# (key, icon, label, sublabel, field_count, accent_token)
# ════════════════════════════════════════════════════════════════════
_SECTIONS = [
    ('core',    '📁', 'Core',            'target dir · scan paths · interval',  4, 'teal'),
    ('lvd',     '💾', 'LVD profile',     'loopback-device mount parameters',     4, 'accent'),
    ('pfs',     '🔒', 'PFS profile',     'filesystem type · key mode · flags',   9, 'accent'),
    ('payload', '📡', 'Payload',         'send micromount.elf · TCP 9021',        2, 'teal'),
]

_KEY_TO_SECTION = {
    # Core
    'target_directory':      'core',
    'scan_depth':            'core',
    'scan_interval_seconds': 'core',
    'debug':                 'core',
    'scanpath':              'core',
    # LVD
    'lvd_image_type':        'lvd',
    'lvd_sector_size':       'lvd',
    'lvd_secondary_unit':    'lvd',
    'lvd_raw_flags':         'lvd',
    # PFS
    'pfs_fstype':            'pfs',
    'pfs_mkeymode':          'pfs',
    'pfs_budgetid':          'pfs',
    'pfs_sigverify':         'pfs',
    'pfs_playgo':            'pfs',
    'pfs_disc':              'pfs',
    'pfs_use_ekpfs':         'pfs',
    'pfs_read_only':         'pfs',
    'pfs_force':             'pfs',
    # Payload (app-settings, not config.ini keys)
    'mm_payload_path':       'payload',
    'mm_payload_port':       'payload',
}


# ════════════════════════════════════════════════════════════════════
# Toolbar
# ════════════════════════════════════════════════════════════════════
def _build_toolbar(parent, app):
    """v3.6.0 PS5 pass: the slim 72px toolbar became a Build-style
    hero card mirroring the ShadowMount+ treatment.

    Backwards compat preserved exactly:
      - app._mm_dirty_pill   Label, packed/unpacked by _refresh_dirty_ui
      - app._mm_status_var   StringVar shown in the hero
      - app._mm_status_lbl   Label whose fg _set_status() reconfigures
    All three actions call the same functions as before. The editor
    below (rail / sections / savebar / log strip) is untouched.
    """
    wrap = tk.Frame(parent, bg=_BG_APP)
    wrap.pack(side='top', fill='x', padx=16, pady=(12, 10))

    hero = ControlHero(
        wrap,
        title='MicroMount',
        subtitle=(REMOTE_CONFIG_PATH + '  \u00b7  auto-mount .ffpfsc payload'),
        stats=[('Target Dir', 'target'),
               ('Scan Paths', 'paths'),
               ('Images Found', 'images'),
               ('Scan Interval', 'interval'),
               ('Payload', 'payload'),
               ('Configuration', 'config')],
        icon='\U0001f5c2', icon_size=64)
    hero.pack(fill='x')
    app._mm_hero = hero

    hero.set_stat('payload', ':' + str(app._settings.get('mm_payload_port',
                                                         '9021') or '9021'))
    hero.set_stat('config', 'Local defaults')
    hero.set_stat('images', '\u2014')
    hero.set_badge('LOCAL', 'wait')

    def _trim(p, n=26):
        p = (p or '').strip()
        return ('\u2026' + p[-(n - 1):]) if len(p) > n else (p or '\u2014')

    # ── Live form mirror — target dir / scan paths / interval read
    # from the SettingRow registry (built after this hero, hence the
    # poll). Presentation only; nothing is written back.
    def _poll_form():
        try:
            if not wrap.winfo_exists():
                return
        except Exception:
            return
        try:
            rows = getattr(app, '_mm_rows', {}) or {}
            r = rows.get('target_directory')
            if r is not None:
                hero.set_stat('target', _trim(r.get_value()))
            r = rows.get('scanpath')
            if r is not None:
                raw = (r.get_value() or '').replace(',', '\n')
                n = len([s for s in raw.split('\n') if s.strip()])
                hero.set_stat('paths', str(n) if n else
                              'SM+ roots')
            r = rows.get('scan_interval_seconds')
            if r is not None:
                v = (r.get_value() or '').strip()
                hero.set_stat('interval', (v + 's') if v else '\u2014')
        except Exception:
            pass
        try:
            wrap.after(1500, _poll_form)
        except Exception:
            pass
    wrap.after(800, _poll_form)

    # ── Status mirror ──
    def _sync_status(*_a):
        try:
            t = app._mm_status_var.get().strip()
            low = t.lower()
            if 'payload' in low:
                warn = ('fail' in low or 'error' in low or '\u2717' in t)
                hero.set_stat('payload',
                              (t[:18] + '\u2026') if len(t) > 19 else t,
                              warn=warn, ok=('\u2713' in t))
        except Exception:
            pass
    app._mm_status_var.trace_add('write', _sync_status)

    # ── Config-source mirror (rail footer var, deferred hookup) ──
    def _hook_footer(tries=0):
        var = getattr(app, '_mm_load_footer_var', None)
        if var is None:
            if tries < 10:
                wrap.after(500, lambda: _hook_footer(tries + 1))
            return

        def _sync(*_a):
            try:
                t = var.get()
                if 'Loaded from PS5' in t:
                    first = t.split('\n')[0]
                    when = first.split('\u00b7')[-1].strip()
                    hero.set_stat('config', 'PS5 \u2713 ' + when, ok=True)
                    hero.set_badge('LOADED FROM PS5', 'ready')
            except Exception:
                pass
        var.trace_add('write', _sync)
    wrap.after(500, _hook_footer)

    # ── Actions row ──
    actions = hero.actions_row()
    make_themed_button(actions, '\U0001f4e5  Load from PS5',
                       command=lambda: _load_from_ps5(app), kind='primary',
                       font_size=9, padx=12, pady=6).pack(side='left')
    make_themed_button(actions, '\U0001f4dd  Fetch log',
                       command=lambda: _fetch_debug_log(app), kind='ghost',
                       font_size=9, padx=12, pady=6
                       ).pack(side='left', padx=(6, 0))
    make_themed_button(actions, '\U0001f4e1  Send Payload',
                       command=lambda: _send_payload(app), kind='success',
                       font_size=9, padx=12, pady=6
                       ).pack(side='left', padx=(6, 0))

    app._mm_dirty_pill = tk.Label(actions, text='',
                                   bg=_WARN_BG, fg=_WARN,
                                   font=_F_MONO,
                                   padx=12, pady=4,
                                   highlightthickness=1,
                                   highlightbackground=_WARN)
    # Don't pack yet — _refresh_dirty_ui() packs/unpacks based on count

    status_lbl = tk.Label(hero.title_row, textvariable=app._mm_status_var,
                          bg=COLORS['bg_2'], fg=_FG_MUTED, font=_F_MONO,
                          anchor='e', wraplength=320, justify='right')
    status_lbl.pack(side='right', anchor='n', padx=(8, 10))
    app._mm_status_lbl = status_lbl

    tk.Frame(parent, bg=_BORDER, height=1).pack(side='top', fill='x')


def _toolbar_btn(parent, text, cmd, kind='default'):
    bg = _BG_CARD_HEAD
    fg = _FG
    if kind == 'primary':
        bg, fg = _TEAL, '#000000'
    elif kind == 'warn':
        bg, fg = _WARN, '#000000'
    elif kind == 'success':
        bg, fg = _SUCCESS, '#000000'
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
    search_frame = tk.Frame(parent, bg=_BG_RAIL)
    search_frame.pack(side='top', fill='x', padx=12, pady=(12, 6))
    app._mm_search_var = tk.StringVar(value='')
    search_entry = tk.Entry(search_frame, textvariable=app._mm_search_var,
                            bg=COLORS['bg_0'], fg=_FG,
                            insertbackground=_FG,
                            bd=0, relief='flat',
                            font=_F_BODY)
    search_entry.pack(fill='x', ipady=4, ipadx=8)
    _install_placeholder(search_entry, app._mm_search_var,
                         '\U0001f50d  Filter settings\u2026')

    def _on_search_change(*_):
        q = (app._mm_search_var.get() or '').strip().lower()
        if getattr(search_entry, '_placeholder_active', False):
            q = ''
        _apply_search_filter(app, q)
    app._mm_search_var.trace_add('write', _on_search_change)

    def _on_search_enter(_=None):
        q = (app._mm_search_var.get() or '').strip().lower()
        if getattr(search_entry, '_placeholder_active', False) or not q:
            return
        first = _first_matching_section(q)
        if first:
            _select_section(app, first)
    search_entry.bind('<Return>', _on_search_enter)

    tk.Label(parent, text='SECTIONS',
             bg=_BG_RAIL, fg=_FG_MUTED, font=_F_EYEBROW,
             anchor='w').pack(fill='x', padx=14, pady=(14, 4))

    ov_row = _make_rail_row(parent, app, 'overview', '\U0001f3e0',
                            'Overview', len(_SECTIONS))
    app._mm_rail_rows['overview'] = ov_row
    for key, icon, label, _sub, count, _accent in _SECTIONS:
        row = _make_rail_row(parent, app, key, icon, label, count)
        app._mm_rail_rows[key] = row

    tk.Frame(parent, bg=_BG_RAIL).pack(side='top', fill='both', expand=True)

    footer = tk.Frame(parent, bg=COLORS['bg_0'])
    footer.pack(side='bottom', fill='x')
    tk.Frame(footer, bg=_BORDER, height=1).pack(fill='x')
    app._mm_load_footer_var = tk.StringVar(value='Not loaded from PS5 yet')
    tk.Label(footer, textvariable=app._mm_load_footer_var,
             bg=COLORS['bg_0'], fg=_FG_DIM, font=_F_MONO,
             anchor='w', justify='left',
             padx=14, pady=10).pack(fill='x')


def _make_rail_row(parent, app, key, icon, label, count):
    row = tk.Frame(parent, bg=_BG_RAIL, highlightthickness=0)
    row.pack(side='top', fill='x')

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

    count_bg = tk.Label(inner, text=str(count),
                        bg=_BG_CARD_HEAD, fg=_FG_DIM,
                        font=_F_META, padx=6, pady=0)
    count_bg.pack(side='right')

    dot = tk.Label(inner, text='\u25cf',
                   bg=_BG_RAIL, fg=_WARN,
                   font=(_F_MONO[0], 9))
    row._mm_dirty_dot  = dot
    row._mm_accent_bar = accent_bar
    row._mm_label      = lbl
    row._mm_icon       = inner.winfo_children()[0]
    row._mm_count_bg   = count_bg
    row._mm_body       = body
    row._mm_inner      = inner

    def _on_click(_=None, k=key):
        _select_section(app, k)
    for w in (row, body, inner, lbl, count_bg, row._mm_icon):
        w.bind('<Button-1>', _on_click)
        w.configure(cursor='hand2')

    def _on_enter(_=None):
        if app._mm_active_section != key:
            for w in (row, body, inner):
                w.configure(bg=COLORS['bg_1'])
            lbl.configure(bg=COLORS['bg_1'], fg=_FG)
            count_bg.configure(bg=COLORS['bg_1'])
            row._mm_icon.configure(bg=COLORS['bg_1'])

    def _on_leave(_=None):
        if app._mm_active_section != key:
            for w in (row, body, inner):
                w.configure(bg=_BG_RAIL)
            lbl.configure(bg=_BG_RAIL, fg=_FG_MUTED)
            count_bg.configure(bg=_BG_RAIL)
            row._mm_icon.configure(bg=_BG_RAIL)

    for w in (row, body, inner, lbl, count_bg, row._mm_icon):
        w.bind('<Enter>', _on_enter, add='+')
        w.bind('<Leave>', _on_leave, add='+')

    return row


def _select_section(app, key):
    prev = app._mm_active_section
    app._mm_active_section = key
    if prev and prev in app._mm_rail_rows:
        _style_rail_row(app._mm_rail_rows[prev], active=False)
    if key in app._mm_rail_rows:
        _style_rail_row(app._mm_rail_rows[key], active=True)
    for k, frame in app._mm_section_frames.items():
        if k == key:
            frame.pack(fill='x', expand=False, pady=(0, 14))
        else:
            frame.pack_forget()

    if key == 'overview':
        fn = getattr(app, '_mm_overview_refresh', None)
        if callable(fn):
            try:
                fn()
            except Exception:
                pass


# ── Search helpers ────────────────────────────────────────────────
def _section_matches_query(section_key, q):
    if not q:
        return True
    for s_key, _ic, label, sublabel, _ct, _ac in _SECTIONS:
        if s_key != section_key:
            continue
        if q in label.lower() or q in sublabel.lower() or q in s_key.lower():
            return True
        break
    for k, sec in _KEY_TO_SECTION.items():
        if sec != section_key:
            continue
        if q in k.lower():
            return True
    return False


def _first_matching_section(q):
    if not q:
        return None
    for s_key, *_ in _SECTIONS:
        if _section_matches_query(s_key, q):
            return s_key
    return None


def _apply_search_filter(app, q):
    rail_rows = getattr(app, '_mm_rail_rows', {}) or {}
    if not q:
        active = getattr(app, '_mm_active_section', None)
        for sec_key, row in rail_rows.items():
            _style_rail_row(row, sec_key == active)
        return
    active = getattr(app, '_mm_active_section', None)
    for sec_key, row in rail_rows.items():
        if _section_matches_query(sec_key, q):
            _style_rail_row(row, sec_key == active)
        else:
            try:
                row._mm_accent_bar.configure(bg=_BG_RAIL)
                for w in (row, row._mm_body, row._mm_inner):
                    w.configure(bg=_BG_RAIL)
                row._mm_label.configure(bg=_BG_RAIL, fg=_FG_DIM)
                row._mm_count_bg.configure(bg=_BG_CARD_HEAD, fg=_FG_DIM)
                row._mm_icon.configure(bg=_BG_RAIL, fg=_FG_DIM)
            except Exception:
                pass
    if active and _section_matches_query(active, q):
        return
    first = _first_matching_section(q)
    if first:
        _select_section(app, first)


def _style_rail_row(row, active):
    if active:
        row._mm_accent_bar.configure(bg=_TEAL)
        for w in (row, row._mm_body, row._mm_inner):
            w.configure(bg=_TEAL_SOFT)
        row._mm_label.configure(bg=_TEAL_SOFT, fg=_FG)
        row._mm_count_bg.configure(bg=_TEAL_SOFT, fg=_TEAL_HI)
        row._mm_icon.configure(bg=_TEAL_SOFT, fg=_TEAL)
        dot = getattr(row, '_mm_dirty_dot', None)
        if dot is not None:
            dot.configure(bg=_TEAL_SOFT)
    else:
        row._mm_accent_bar.configure(bg=_BG_RAIL)
        for w in (row, row._mm_body, row._mm_inner):
            w.configure(bg=_BG_RAIL)
        row._mm_label.configure(bg=_BG_RAIL, fg=_FG_MUTED)
        row._mm_count_bg.configure(bg=_BG_CARD_HEAD, fg=_FG_DIM)
        row._mm_icon.configure(bg=_BG_RAIL, fg=_FG_MUTED)
        dot = getattr(row, '_mm_dirty_dot', None)
        if dot is not None:
            dot.configure(bg=_BG_RAIL)


# ════════════════════════════════════════════════════════════════════
# Content area
# ════════════════════════════════════════════════════════════════════
def _overview_summary_keys(section_key, limit=3):
    """First few config keys of a section, prettified — drives the
    overview tiles' summary lines."""
    out = []
    for k, sec in _KEY_TO_SECTION.items():
        if sec == section_key:
            label = k
            for pre in ('lvd_', 'pfs_', 'mm_', 'scan_'):
                if label.startswith(pre):
                    label = label[len(pre):]
                    break
            out.append((k, label.replace('_', ' ').capitalize()))
            if len(out) >= limit:
                break
    return out


def _build_overview_card(parent, app):
    """v3.6.0 PS5 pass: dashboard landing — one summary tile per
    section (click opens the existing editor) + Recent Activity panel.
    Read-only mirror of the SettingRow values."""
    card = tk.Frame(parent, bg=_BG_CARD,
                    highlightthickness=1, highlightbackground=_BORDER)

    head = tk.Frame(card, bg=_BG_CARD_HEAD)
    head.pack(fill='x')
    hi = tk.Frame(head, bg=_BG_CARD_HEAD)
    hi.pack(fill='x', padx=14, pady=10)
    tk.Label(hi, text='\U0001f3e0', bg=_BG_CARD_HEAD, fg=_TEAL,
             font=_F_BODY).pack(side='left')
    tk.Label(hi, text='Overview', bg=_BG_CARD_HEAD, fg=_FG,
             font=_F_H3).pack(side='left', padx=(10, 0))
    tk.Label(hi, text='  \u2014  configuration at a glance \u00b7 '
                      'click a card to edit',
             bg=_BG_CARD_HEAD, fg=_FG_MUTED, font=_F_MONO
             ).pack(side='left')

    body = tk.Frame(card, bg=_BG_CARD)
    body.pack(fill='x', padx=14, pady=14)
    body.grid_columnconfigure(0, weight=1, uniform='ovc')
    body.grid_columnconfigure(1, weight=1, uniform='ovc')
    body.grid_columnconfigure(2, weight=0, minsize=250)

    value_setters = {}

    def _tile(r, c, key, icon, label, count, accent):
        t = tk.Frame(body, bg=COLORS['bg_3'],
                     highlightbackground=_BORDER, highlightthickness=1,
                     cursor='hand2')
        t.grid(row=r, column=c, sticky='nsew', padx=(0, 10),
               pady=(0, 10))
        ti = tk.Frame(t, bg=COLORS['bg_3'])
        ti.pack(fill='x', padx=12, pady=10)
        hr = tk.Frame(ti, bg=COLORS['bg_3'])
        hr.pack(fill='x')
        tk.Label(hr, text=icon, bg=COLORS['bg_3'], fg=accent,
                 font=_F_BODY).pack(side='left')
        tk.Label(hr, text=label, bg=COLORS['bg_3'], fg=_FG,
                 font=(_F_BODY[0], 10, 'bold')
                 ).pack(side='left', padx=(8, 0))
        tk.Label(hr, text='%d SETTINGS' % count,
                 bg=COLORS['bg_3'], fg=_FG_DIM, font=_F_META
                 ).pack(side='right')
        for k, klabel in _overview_summary_keys(key):
            rrow = tk.Frame(ti, bg=COLORS['bg_3'])
            rrow.pack(fill='x', pady=(4, 0))
            tk.Label(rrow, text=klabel + ':', bg=COLORS['bg_3'],
                     fg=_FG_MUTED, font=_F_MONO,
                     anchor='w').pack(side='left')
            v = tk.Label(rrow, text='\u2014', bg=COLORS['bg_3'],
                         fg=_TEAL, font=(_F_MONO[0], 9, 'bold'),
                         anchor='w')
            v.pack(side='left', padx=(6, 0))
            value_setters[k] = v

        def _open(_e=None, kk=key):
            _select_section(app, kk)
        for w in [t, ti, hr] + list(ti.winfo_children()):
            w.bind('<Button-1>', _open)
            for sub in w.winfo_children():
                sub.bind('<Button-1>', _open)
        t.bind('<Enter>', lambda _e, w=t:
               w.config(highlightbackground=_TEAL))
        t.bind('<Leave>', lambda _e, w=t:
               w.config(highlightbackground=_BORDER))
        return t

    for i, (key, icon, label, _sub, count, accent_token) in \
            enumerate(_SECTIONS):
        _tile(i // 2, i % 2, key, icon, label, count,
              COLORS.get(accent_token, _TEAL))

    rows_needed = (len(_SECTIONS) + 1) // 2
    act = tk.Frame(body, bg=COLORS['bg_3'],
                   highlightbackground=_BORDER, highlightthickness=1)
    act.grid(row=0, column=2, rowspan=max(1, rows_needed),
             sticky='nsew', pady=(0, 10))
    ai = tk.Frame(act, bg=COLORS['bg_3'])
    ai.pack(fill='x', padx=12, pady=10)
    tk.Label(ai, text='RECENT ACTIVITY', bg=COLORS['bg_3'],
             fg=_FG_DIM, font=_F_EYEBROW, anchor='w').pack(fill='x')

    act_vars = {}
    for k, label in [('payload', 'Last payload send'),
                     ('load', 'Last config load'),
                     ('push', 'Last config push'),
                     ('log', 'Last log fetch')]:
        r = tk.Frame(ai, bg=COLORS['bg_3'])
        r.pack(fill='x', pady=(8, 0))
        tk.Label(r, text=label, bg=COLORS['bg_3'], fg=_FG_MUTED,
                 font=_F_MONO, anchor='w').pack(fill='x')
        v = tk.StringVar(value='\u2014')
        tk.Label(r, textvariable=v, bg=COLORS['bg_3'], fg=_TEAL,
                 font=(_F_MONO[0], 9, 'bold'), anchor='w'
                 ).pack(fill='x')
        act_vars[k] = v

    def _on_status(*_a):
        import time as _t
        try:
            txt = app._mm_status_var.get()
            now = _t.strftime('%H:%M:%S')
            if 'Payload sent' in txt and '\u2713' in txt:
                act_vars['payload'].set(now)
            elif 'Pushed config' in txt and '\u2713' in txt:
                act_vars['push'].set(now)
            elif 'Fetched debug.log' in txt and '\u2713' in txt:
                act_vars['log'].set(now)
            elif 'Loaded' in txt and 'from PS5' in txt \
                    and '\u2713' in txt:
                act_vars['load'].set(now)
        except Exception:
            pass
    try:
        app._mm_status_var.trace_add('write', _on_status)
    except Exception:
        pass

    def _refresh():
        rows = getattr(app, '_mm_rows', {}) or {}
        for k, lbl in value_setters.items():
            try:
                if not lbl.winfo_exists():
                    return
                row = rows.get(k)
                if row is None:
                    continue
                val = str(row.get_value())
                if len(val) > 16:
                    val = val[:15] + '\u2026'
                lbl.config(text=val if val.strip() else '\u2014')
            except Exception:
                pass
    app._mm_overview_refresh = _refresh
    card.after(700, _refresh)
    return card


def _build_content(parent, app):
    canvas = tk.Canvas(parent, bg=_BG_APP, highlightthickness=0)
    vsb    = tk.Scrollbar(parent, orient='vertical', command=canvas.yview,
                          bg=_BG_CARD, troughcolor=_BG_APP)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    inner    = tk.Frame(canvas, bg=_BG_APP)
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')

    def _on_inner_config(_=None):
        canvas.configure(scrollregion=canvas.bbox('all'))
    inner.bind('<Configure>', _on_inner_config)

    def _on_canvas_config(e):
        canvas.itemconfig(inner_id, width=e.width)
    canvas.bind('<Configure>', _on_canvas_config)

    def _on_wheel(e):
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units')
    canvas.bind('<Enter>', lambda _e: canvas.bind_all('<MouseWheel>', _on_wheel))
    canvas.bind('<Leave>', lambda _e: canvas.unbind_all('<MouseWheel>'))

    pad = tk.Frame(inner, bg=_BG_APP)
    pad.pack(fill='x', padx=24, pady=18)

    app._mm_section_frames['overview'] = _build_overview_card(pad, app)

    for key, icon, label, sublabel, count, accent_token in _SECTIONS:
        accent_color = COLORS.get(accent_token, _TEAL)
        card = _build_section_card(pad, app, key, icon, label, sublabel,
                                   count, accent_color)
        app._mm_section_frames[key] = card


def _build_section_card(parent, app, key, icon, label, sublabel, count, accent):
    card = tk.Frame(parent, bg=_BG_CARD,
                    highlightthickness=1, highlightbackground=_BORDER)

    # Header
    head = tk.Frame(card, bg=_BG_CARD_HEAD)
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=_BG_CARD_HEAD)
    head_inner.pack(fill='x', padx=14, pady=10)

    swatch = tk.Frame(head_inner, bg=_BG_CARD_HEAD, width=24, height=24)
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
    card._mm_meta_lbl = meta

    body = tk.Frame(card, bg=_BG_CARD)
    body.pack(fill='x')
    card._mm_body = body

    _SECTION_BUILDERS = {
        'core':    _build_core_section,
        'lvd':     _build_lvd_section,
        'pfs':     _build_pfs_section,
        'payload': _build_payload_section,
    }
    builder = _SECTION_BUILDERS.get(key)
    if builder is not None:
        builder(body, app)
    else:
        tk.Label(body,
                 text='[ no builder for section %r ]' % key,
                 bg=_BG_CARD, fg=_DANGER, font=_F_MONO,
                 anchor='w').pack(fill='x', padx=20, pady=20)

    return card


# ════════════════════════════════════════════════════════════════════
# Section builders
# ════════════════════════════════════════════════════════════════════
def _initial_value(app, key):
    saved = app._settings.get('micromount_config', {}) or {}
    if key in saved:
        v = saved[key]
        if isinstance(v, list):
            return '\n'.join(v)
        return v
    return DEFAULTS.get(key, '')


def _snapshot_keys(app, keys):
    for key in keys:
        if key in app._mm_rows:
            try:
                app._mm_initial_values[key] = app._mm_rows[key].get_value()
            except Exception:
                pass


def _register_row(app, key, row):
    app._mm_rows[key] = row


def _on_mm_change(app, key, value):
    initial  = app._mm_initial_values.get(key, None)
    if isinstance(initial, bool) or isinstance(value, bool):
        is_dirty = bool(initial) != bool(value)
    else:
        is_dirty = (str(initial if initial is not None else '') !=
                    str(value   if value   is not None else ''))
    if is_dirty:
        app._mm_dirty_keys.add(key)
    else:
        app._mm_dirty_keys.discard(key)
    _refresh_dirty_ui(app)


def _build_core_section(parent, app):
    on_change = lambda k, v: _on_mm_change(app, k, v)

    _register_row(app, 'target_directory',
        SettingRow(parent,
            key='target_directory',
            label='Target directory',
            sublabel='where MicroMount creates managed mount folders',
            kind='text',
            value=_initial_value(app, 'target_directory'),
            default=DEFAULTS['target_directory'],
            help='absolute path on PS5',
            on_change=on_change))

    _register_row(app, 'scanpath',
        SettingRow(parent,
            key='scanpath',
            label='Scan paths',
            sublabel='absolute paths to scan for .ffpfsc files, one per line',
            kind='textarea',
            value=_initial_value(app, 'scanpath'),
            default='',
            wide=True,
            help='leave blank to use ShadowMountPlus scan roots',
            on_change=on_change))

    _register_row(app, 'scan_depth',
        SettingRow(parent,
            key='scan_depth',
            label='Scan depth',
            kind='select',
            choices=['0', '1', '2'],
            value=_initial_value(app, 'scan_depth'),
            default=DEFAULTS['scan_depth'],
            help='0 = root only \u00b7 1 = one nested level',
            on_change=on_change))

    _register_row(app, 'scan_interval_seconds',
        SettingRow(parent,
            key='scan_interval_seconds',
            label='Scan interval',
            kind='int',
            value=_initial_value(app, 'scan_interval_seconds'),
            default=DEFAULTS['scan_interval_seconds'],
            unit='seconds',
            help='how often to re-scan for new/removed images',
            on_change=on_change))

    _register_row(app, 'debug',
        SettingRow(parent,
            key='debug',
            label='Debug notifications',
            sublabel='per-image popups \u00b7 log always written regardless',
            kind='toggle',
            value=_initial_value(app, 'debug'),
            default=DEFAULTS['debug'],
            on_change=on_change))

    _snapshot_keys(app, ('target_directory', 'scanpath',
                          'scan_depth', 'scan_interval_seconds', 'debug'))


def _build_lvd_section(parent, app):
    on_change = lambda k, v: _on_mm_change(app, k, v)

    # Explanatory note
    note = tk.Frame(parent, bg=_BG_CARD)
    note.pack(fill='x', padx=14, pady=(10, 4))
    tk.Label(note,
        text=('Default profile: {0, 65536, true, 0x9, "pfs", "AC", "system", true, false}\n'
              'Change these only if you know the mount family your images require.'),
        bg=_BG_CARD, fg=_FG_DIM, font=_F_MONO,
        anchor='w', justify='left').pack(fill='x')

    _register_row(app, 'lvd_image_type',
        SettingRow(parent,
            key='lvd_image_type',
            label='lvd_image_type',
            kind='int',
            value=_initial_value(app, 'lvd_image_type'),
            default=DEFAULTS['lvd_image_type'],
            advanced=True,
            on_change=on_change))

    _register_row(app, 'lvd_sector_size',
        SettingRow(parent,
            key='lvd_sector_size',
            label='lvd_sector_size',
            kind='int',
            value=_initial_value(app, 'lvd_sector_size'),
            default=DEFAULTS['lvd_sector_size'],
            unit='bytes',
            advanced=True,
            help='65536 for .ffpfsc \u00b7 512 for exFAT',
            on_change=on_change))

    _register_row(app, 'lvd_secondary_unit',
        SettingRow(parent,
            key='lvd_secondary_unit',
            label='lvd_secondary_unit',
            kind='int',
            value=_initial_value(app, 'lvd_secondary_unit'),
            default=DEFAULTS['lvd_secondary_unit'],
            unit='bytes',
            advanced=True,
            help='defaults to sector size',
            on_change=on_change))

    _register_row(app, 'lvd_raw_flags',
        SettingRow(parent,
            key='lvd_raw_flags',
            label='lvd_raw_flags',
            kind='text',
            value=_initial_value(app, 'lvd_raw_flags'),
            default=DEFAULTS['lvd_raw_flags'],
            advanced=True,
            on_change=on_change))

    _snapshot_keys(app, ('lvd_image_type', 'lvd_sector_size',
                          'lvd_secondary_unit', 'lvd_raw_flags'))


def _build_pfs_section(parent, app):
    on_change = lambda k, v: _on_mm_change(app, k, v)

    _register_row(app, 'pfs_fstype',
        SettingRow(parent,
            key='pfs_fstype',
            label='pfs_fstype',
            kind='select',
            choices=['pfs', 'exfat'],
            value=_initial_value(app, 'pfs_fstype'),
            default=DEFAULTS['pfs_fstype'],
            on_change=on_change))

    _register_row(app, 'pfs_mkeymode',
        SettingRow(parent,
            key='pfs_mkeymode',
            label='pfs_mkeymode',
            kind='select',
            choices=['AC', 'FAKE', 'NONE'],
            value=_initial_value(app, 'pfs_mkeymode'),
            default=DEFAULTS['pfs_mkeymode'],
            advanced=True,
            on_change=on_change))

    _register_row(app, 'pfs_budgetid',
        SettingRow(parent,
            key='pfs_budgetid',
            label='pfs_budgetid',
            kind='select',
            choices=['system', 'user'],
            value=_initial_value(app, 'pfs_budgetid'),
            default=DEFAULTS['pfs_budgetid'],
            advanced=True,
            on_change=on_change))

    for key, lbl in (
        ('pfs_sigverify', 'Signature verification'),
        ('pfs_playgo',    'Playgo streaming'),
        ('pfs_disc',      'Disc mode'),
        ('pfs_use_ekpfs', 'Use EKPFS'),
        ('pfs_read_only', 'Read-only mount'),
        ('pfs_force',     'Force-mount'),
    ):
        _register_row(app, key,
            SettingRow(parent,
                key=key,
                label=lbl,
                sublabel=key,
                kind='toggle',
                value=_initial_value(app, key),
                default=DEFAULTS[key],
                danger=(key == 'pfs_force'),
                advanced=(key not in ('pfs_read_only', 'pfs_use_ekpfs')),
                help='safe to leave at default' if key not in ('pfs_force',) else
                     'skips mount checks \u2014 may corrupt data',
                on_change=on_change))

    _snapshot_keys(app, ('pfs_fstype', 'pfs_mkeymode', 'pfs_budgetid',
                          'pfs_sigverify', 'pfs_playgo', 'pfs_disc',
                          'pfs_use_ekpfs', 'pfs_read_only', 'pfs_force'))


def _build_payload_section(parent, app):
    on_change = lambda k, v: _on_mm_change(app, k, v)

    saved_path = str(app._settings.get('mm_payload_path', ''))
    saved_port = str(app._settings.get('mm_payload_port', 9021))

    _register_row(app, 'mm_payload_path',
        SettingRow(parent,
            key='mm_payload_path',
            label='micromount.elf path',
            sublabel='local path on this PC',
            kind='text',
            value=saved_path,
            default='',
            help='click Browse below to pick a file',
            on_change=on_change))

    browse_row = tk.Frame(parent, bg=_BG_CARD)
    browse_row.pack(fill='x', padx=14, pady=(0, 6))
    tk.Frame(browse_row, bg=_BG_CARD, width=280).pack(side='left')

    def _browse_elf():
        p = filedialog.askopenfilename(
            title='Locate micromount.elf',
            filetypes=[('ELF files', '*.elf'), ('All files', '*.*')])
        if p:
            try:
                app._mm_rows['mm_payload_path'].set_value(p)
                _on_mm_change(app, 'mm_payload_path', p)
            except Exception:
                pass

    tk.Button(browse_row, text='\U0001f4c2  Browse\u2026',
              command=_browse_elf,
              bg=_BG_CARD_HEAD, fg=_FG, font=_F_BUTTON,
              activebackground=_BG_ROW_HOV, activeforeground=_FG,
              bd=0, padx=12, pady=5, cursor='hand2',
              relief='flat').pack(side='left', padx=(14, 0))

    _register_row(app, 'mm_payload_port',
        SettingRow(parent,
            key='mm_payload_port',
            label='Payload port',
            kind='int',
            value=saved_port,
            default='9021',
            unit='TCP',
            help='default: 9021 \u00b7 matches etaHEN / GoldHEN loaders',
            on_change=on_change))

    _snapshot_keys(app, ('mm_payload_path', 'mm_payload_port'))


# ════════════════════════════════════════════════════════════════════
# Dirty-state UI refresh
# ════════════════════════════════════════════════════════════════════
def _refresh_dirty_ui(app):
    count = len(app._mm_dirty_keys)

    savebar = getattr(app, '_mm_savebar_frame', None)
    savebar_border = getattr(app, '_mm_savebar_border', None)
    if savebar is not None and savebar_border is not None:
        if count > 0:
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

    pill = getattr(app, '_mm_dirty_pill', None)
    if pill is not None:
        if count > 0:
            pill.configure(text='\u25cf  %d change%s'
                                % (count, '' if count == 1 else 's'))
            try:
                pill.pack(side='right', padx=(8, 4))
            except Exception:
                pass
        else:
            try:
                pill.pack_forget()
            except Exception:
                pass

    sections_with_changes = set()
    for k in app._mm_dirty_keys:
        sec = _KEY_TO_SECTION.get(k)
        if sec:
            sections_with_changes.add(sec)
    for sec_key, row in app._mm_rail_rows.items():
        dot = getattr(row, '_mm_dirty_dot', None)
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

    summary_var = getattr(app, '_mm_savebar_summary', None)
    if summary_var is not None:
        if count == 0:
            summary_var.set('No pending changes')
        else:
            entries = []
            for k in sorted(app._mm_dirty_keys)[:3]:
                if k not in app._mm_rows:
                    continue
                new_v = app._mm_rows[k].get_value()
                old_v = app._mm_initial_values.get(k, '')
                def _trim(v, n=18):
                    s = str(v).replace('\n', '\u23ce')
                    if len(s) > n:
                        s = s[:n-1] + '\u2026'
                    return s or '\u2014'
                entries.append('%s=%s (was %s)' % (k, _trim(new_v), _trim(old_v)))
            summary = '  \u00b7  '.join(entries)
            if count > 3:
                summary += '  \u00b7  +%d more' % (count - 3)
            summary_var.set(summary)

    active = getattr(app, '_mm_active_section', None)
    if active and active in app._mm_section_frames:
        card = app._mm_section_frames[active]
        meta_lbl = getattr(card, '_mm_meta_lbl', None)
        if meta_lbl is not None:
            sec_dirty = sum(1 for k in app._mm_dirty_keys
                            if _KEY_TO_SECTION.get(k) == active)
            base = next((s[4] for s in _SECTIONS if s[0] == active), 0)
            if sec_dirty:
                meta_lbl.configure(
                    text='%d SETTINGS \u00b7 %d CHANGED' % (base, sec_dirty),
                    fg=_WARN)
            else:
                meta_lbl.configure(text='%d SETTINGS' % base, fg=_FG_DIM)


# ════════════════════════════════════════════════════════════════════
# Save bar
# ════════════════════════════════════════════════════════════════════
def _build_savebar(parent, app):
    inner = tk.Frame(parent, bg=_BG_SAVEBAR)
    inner.pack(fill='x', padx=24, pady=11)

    app._mm_savebar_summary = tk.StringVar(value='No pending changes')
    summary_lbl = tk.Label(inner, textvariable=app._mm_savebar_summary,
                            bg=_BG_SAVEBAR, fg=_FG_MUTED, font=_F_MONO,
                            anchor='w', cursor='hand2')
    summary_lbl.pack(side='left', fill='x', expand=True)

    _toolbar_btn(inner, 'Push to PS5', lambda: _push_to_ps5(app),
                 kind='primary').pack(side='right', padx=(4, 0))
    _toolbar_btn(inner, 'Save local', lambda: _save_locally(app),
                 kind='warn').pack(side='right', padx=(4, 0))
    _toolbar_btn(inner, 'Discard', lambda: _discard_changes(app),
                 kind='ghost').pack(side='right', padx=(4, 0))


def _discard_changes(app):
    if not getattr(app, '_mm_dirty_keys', None):
        return
    for key in list(app._mm_dirty_keys):
        if key in app._mm_rows and key in app._mm_initial_values:
            try:
                app._mm_rows[key].set_value(app._mm_initial_values[key])
            except Exception:
                pass
    app._mm_dirty_keys = set()
    _refresh_dirty_ui(app)


# ════════════════════════════════════════════════════════════════════
# Log strip
# ════════════════════════════════════════════════════════════════════
def _build_log_strip(parent, app):
    inner = tk.Frame(parent, bg=_BG_LOG)
    inner.pack(fill='both', expand=True, padx=24, pady=9)
    tk.Label(inner, text='OUTPUT LOG',
             bg=_BG_LOG, fg=_FG_MUTED, font=_F_EYEBROW,
             anchor='w').pack(side='left')
    app._mm_log_status_var = tk.StringVar(value='idle')
    tk.Label(inner, textvariable=app._mm_log_status_var,
             bg=_BG_LOG, fg=_FG_DIM, font=_F_MONO,
             anchor='e').pack(side='right')


# ════════════════════════════════════════════════════════════════════
# SettingRow — identical primitive to tab_shadowmount's, scoped
# to _mm_ state keys.  Reproduced here so the two tabs are fully
# independent (no cross-import surprises).
# ════════════════════════════════════════════════════════════════════
class SettingRow:
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

        self._row = tk.Frame(parent, bg=_BG_CARD, highlightthickness=0)
        self._row.pack(fill='x')

        self._accent = tk.Frame(self._row, bg=_BG_CARD, width=2)
        self._accent.pack(side='left', fill='y')

        self._divider = tk.Frame(self._row.master, bg=_BORDER, height=1)

        self._body = tk.Frame(self._row, bg=_BG_CARD)
        self._body.pack(side='left', fill='x', expand=True, padx=(14, 14))

        self._build_layout(value, sublabel, help)
        self._refresh_badge()

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

    def get_value(self):
        if self.kind == 'toggle':
            return bool(self._tk_var.get())
        if self.kind == 'textarea':
            return self._text_widget.get('1.0', 'end').rstrip('\n')
        return self._tk_var.get()

    def set_value(self, v):
        self._suppress_change = True
        try:
            if self.kind == 'toggle':
                self._tk_var.set(bool(v) if not isinstance(v, str)
                                 else str(v).strip().lower() in ('1', 'true', 'yes', 'on'))
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
        self.set_value(self.default)
        self._fire_change()

    def _build_layout(self, value, sublabel, help):
        self._label_cell = tk.Frame(self._body, bg=_BG_CARD, width=280)
        self._label_cell.pack(side='left', fill='y')
        self._label_cell.pack_propagate(False)

        label_inner = tk.Frame(self._label_cell, bg=_BG_CARD)
        label_inner.pack(fill='both', expand=True, pady=8)

        self._label_lbl = tk.Label(label_inner, text=self.label,
                                   bg=_BG_CARD, fg=_FG, font=_F_LABEL,
                                   anchor='w', wraplength=270, justify='left')
        self._label_lbl.pack(anchor='w')

        if self._danger or self._advanced:
            pill_row = tk.Frame(label_inner, bg=_BG_CARD)
            pill_row.pack(anchor='w', pady=(2, 0))
            if self._danger:
                _make_pill(pill_row, 'dangerous', kind='danger').pack(side='left')
            if self._advanced:
                _make_pill(pill_row, 'advanced', kind='advanced').pack(
                    side='left', padx=(4 if self._danger else 0, 0))

        self._sub_lbl = None
        if sublabel:
            self._sub_lbl = tk.Label(label_inner, text=sublabel,
                                     bg=_BG_CARD, fg=_FG_MUTED, font=_F_META,
                                     anchor='w')
            self._sub_lbl.pack(fill='x', anchor='w')

        if self._wide:
            self._ctl_cell = tk.Frame(self._body, bg=_BG_CARD)
            self._ctl_cell.pack(side='left', fill='both', expand=True,
                                pady=8, padx=(14, 0))
        else:
            self._ctl_cell = tk.Frame(self._body, bg=_BG_CARD, width=220)
            self._ctl_cell.pack(side='left', fill='y', pady=8, padx=(14, 0))
            self._ctl_cell.pack_propagate(False)

        self._build_control(self._ctl_cell, value)

        if self._wide:
            self._help_cell = tk.Frame(self._body, bg=_BG_CARD, width=0)
        else:
            self._help_cell = tk.Frame(self._body, bg=_BG_CARD)
            self._help_cell.pack(side='left', fill='both', expand=True,
                                 pady=8, padx=(10, 0))
            if help:
                tk.Label(self._help_cell, text=help,
                         bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO,
                         anchor='w', justify='left').pack(anchor='w')

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

    def _build_toggle(self, cell, value):
        initial = bool(value) if not isinstance(value, str) else (
            value.strip().lower() in ('1', 'true', 'yes', 'on'))
        self._tk_var = tk.BooleanVar(value=initial)
        self._toggle_pill = tk.Label(cell, text='ON',
                                      bg=_SUCCESS, fg='#000000',
                                      font=_F_MONO,
                                      padx=10, pady=2,
                                      cursor='hand2',
                                      width=3, anchor='center')
        self._toggle_pill.pack(side='left')
        self._toggle_pill.bind('<Button-1>', lambda _e: self._on_toggle_click())
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

    def _build_int(self, cell, value):
        self._tk_var = tk.StringVar(value='' if value is None else str(value))
        entry = tk.Entry(cell, textvariable=self._tk_var,
                         bg=COLORS['bg_0'], fg=_FG,
                         insertbackground=_FG,
                         bd=0, relief='flat', font=_F_MONO,
                         width=8,
                         highlightthickness=1,
                         highlightbackground=_BORDER_STRG,
                         highlightcolor=_TEAL)
        entry.pack(side='left', ipady=4, ipadx=6)
        if self._unit:
            tk.Label(cell, text=self._unit,
                     bg=_BG_CARD, fg=_FG_MUTED, font=_F_MONO).pack(
                side='left', padx=(8, 0))
        self._tk_var.trace_add('write', lambda *_: self._fire_change())

    def _build_text(self, cell, value):
        self._tk_var = tk.StringVar(value='' if value is None else str(value))
        entry = tk.Entry(cell, textvariable=self._tk_var,
                         bg=COLORS['bg_0'], fg=_FG,
                         insertbackground=_FG,
                         bd=0, relief='flat', font=_F_MONO,
                         highlightthickness=1,
                         highlightbackground=_BORDER_STRG,
                         highlightcolor=_TEAL)
        entry.pack(side='left', fill='x', expand=True, ipady=4, ipadx=6)
        self._tk_var.trace_add('write', lambda *_: self._fire_change())

    def _build_select(self, cell, value):
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
                           highlightcolor=_TEAL,
                           anchor='w', width=14,
                           cursor='hand2',
                           indicatoron=True,
                           compound='right')
        mb.pack(side='left', anchor='w')
        menu = tk.Menu(mb, tearoff=0,
                       bg=COLORS['bg_3'], fg=_FG,
                       activebackground=_TEAL,
                       activeforeground='#000000',
                       bd=0, font=_F_MONO)
        for choice in choices:
            def _pick(v=choice):
                self._tk_var.set(v)
            menu.add_command(label=choice, command=_pick)
        mb.configure(menu=menu)
        self._tk_var.trace_add('write', lambda *_: self._fire_change())

    def _build_textarea(self, cell, value):
        self._text_widget = tk.Text(cell, height=3,
                                     bg=COLORS['bg_0'], fg=_FG,
                                     insertbackground=_FG,
                                     bd=0, relief='flat',
                                     font=_F_MONO, wrap='word',
                                     highlightthickness=1,
                                     highlightbackground=_BORDER_STRG,
                                     highlightcolor=_TEAL)
        self._text_widget.pack(fill='both', expand=True)
        if value:
            if isinstance(value, list):
                value = '\n'.join(value)
            self._text_widget.insert('1.0', str(value))

        def _on_modified(_e):
            if not getattr(self, '_suppress_change', False):
                self._fire_change()
            self._text_widget.edit_modified(False)
        self._text_widget.bind('<<Modified>>', _on_modified)

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
            self._accent.configure(bg=_WARN)
            self._badge_lbl.configure(
                text='default: %s  \u21ba' % self._format_default(),
                fg=_WARN)
        else:
            self._accent.configure(bg=_BG_CARD)
            self._badge_lbl.configure(text='default', fg=_FG_DIM)

    def _format_default(self):
        d = self.default
        if self.kind == 'toggle':
            if isinstance(d, str):
                d = d.strip().lower() in ('1', 'true', 'yes', 'on')
            return 'on' if d else 'off'
        if d is None or d == '':
            return '\u2014'
        if isinstance(d, list):
            return '%d items' % len(d)
        s = str(d)
        return s if len(s) <= 18 else s[:15] + '\u2026'


# ════════════════════════════════════════════════════════════════════
# Pill widget
# ════════════════════════════════════════════════════════════════════
def _make_pill(parent, text, kind='advanced'):
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


# ════════════════════════════════════════════════════════════════════
# Placeholder helper
# ════════════════════════════════════════════════════════════════════
def _install_placeholder(entry, var, placeholder):
    real_fg  = _FG
    ghost_fg = _FG_DIM
    state    = {'ghosted': True}

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

    _show_ghost()
    entry.bind('<FocusIn>',  lambda _: _hide_ghost())
    entry.bind('<FocusOut>', lambda _: (None if var.get().strip()
                                        else _show_ghost()))


# ════════════════════════════════════════════════════════════════════
# Form serialization
# ════════════════════════════════════════════════════════════════════
def _collect_form(app):
    out  = {}
    rows = getattr(app, '_mm_rows', {}) or {}
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
    return out


def _apply_form(app, data):
    rows = getattr(app, '_mm_rows', {}) or {}
    for key, val in data.items():
        if key in rows:
            try:
                rows[key].set_value(val)
            except Exception:
                pass
    if hasattr(app, '_mm_initial_values'):
        for key, row in rows.items():
            try:
                app._mm_initial_values[key] = row.get_value()
            except Exception:
                pass
        app._mm_dirty_keys = set()
        _refresh_dirty_ui(app)


def _render_ini(data):
    """Render form data as a config.ini for MicroMount."""
    lines = [
        '# MicroMount runtime config',
        '# Generated by exFAT Image Builder',
        '# Place as: /data/micromount/config.ini',
        '',
    ]

    def emit(key):
        v = data.get(key)
        if v in (None, ''):
            return
        lines.append('%s=%s' % (key, v))

    lines.append('# ── core ───────────────────────────────────────')
    emit('target_directory')
    emit('scan_depth')
    emit('scan_interval_seconds')
    emit('debug')
    for raw in (data.get('scanpath', '') or '').splitlines():
        t = raw.strip()
        if t and not t.startswith('#'):
            lines.append('scanpath=' + t)
    lines.append('')

    lines.append('# ── LVD mount profile ──────────────────────────')
    for k in ('lvd_image_type', 'lvd_sector_size',
               'lvd_secondary_unit', 'lvd_raw_flags'):
        emit(k)
    lines.append('')

    lines.append('# ── PFS mount profile ──────────────────────────')
    for k in ('pfs_fstype', 'pfs_mkeymode', 'pfs_budgetid',
               'pfs_sigverify', 'pfs_playgo', 'pfs_disc',
               'pfs_use_ekpfs', 'pfs_read_only', 'pfs_force'):
        emit(k)

    return '\n'.join(lines) + '\n'


def _parse_ini(text):
    """Parse a config.ini text back into a flat dict matching form keys."""
    out      = {}
    scanpath = []

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
        if k == 'scanpath':
            scanpath.append(v)
            continue
        # scan_paths is the comma/semicolon-separated variant
        if k == 'scan_paths':
            for part in re.split(r'[,;]', v):
                p = part.strip()
                if p:
                    scanpath.append(p)
            continue
        out[k] = v

    if scanpath:
        out['scanpath'] = '\n'.join(scanpath)
    return out


# ════════════════════════════════════════════════════════════════════
# Status + FTP error helpers
# ════════════════════════════════════════════════════════════════════
def _set_status(app, text, color=MUTED):
    from time import strftime
    try:
        app._mm_status_var.set(text)
        app._mm_status_lbl.config(fg=color)
    except Exception:
        pass
    log_var = getattr(app, '_mm_log_status_var', None)
    if log_var is not None:
        try:
            log_var.set('%s  \u00b7  %s' % (strftime('%H:%M:%S'), text))
        except Exception:
            pass


def _friendly_ftp_error(action, err):
    s   = str(err)
    low = s.lower()
    if '10061' in s or 'refused' in low:
        return (action + ' failed: PS5 FTP server not running',
                'FTP server not running',
                'The PS5 refused the connection.\n\n'
                'Start FTP on the PS5 (etaHEN / itemzflow / payload), '
                'then try again.')
    if '10060' in s or 'timed out' in low or 'timeout' in low:
        return (action + ' failed: connection timed out',
                'Connection timed out',
                'The PS5 did not respond.\n\nCheck: PS5 is on, IP is '
                'correct, same network.')
    if '10065' in s or 'no route' in low or 'unreachable' in low:
        return (action + ' failed: host unreachable',
                'Host unreachable',
                'No route to the PS5.  Check the IP and network.')
    if '550' in s:
        return (action + ' failed: path not accessible on PS5',
                'Path not accessible',
                'The PS5 FTP server rejected the path.\n\nDetails: ' + s[:200])
    return (action + ' failed: ' + s[:60],
            action + ' failed',
            'Error details:\n\n' + s)


# ════════════════════════════════════════════════════════════════════
# Button handlers
# ════════════════════════════════════════════════════════════════════
def _save_locally(app):
    data = _collect_form(app)
    app._settings['micromount_config'] = data
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass
    rows = getattr(app, '_mm_rows', {}) or {}
    if hasattr(app, '_mm_initial_values'):
        for key, row in rows.items():
            try:
                app._mm_initial_values[key] = row.get_value()
            except Exception:
                pass
        app._mm_dirty_keys = set()
        _refresh_dirty_ui(app)
    _set_status(app, 'Saved locally \u2713', SUCCESS)


def _load_from_ps5(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab first.')
        return
    _set_status(app, 'Fetching config from PS5\u2026', ACCENT)

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
            if '550' in err or 'no such' in err.lower():
                app.after(0, _set_status, app,
                    'No config on PS5 yet \u2014 push to create it', WARNING)
                app.after(0, messagebox.showinfo, 'Not on PS5',
                    REMOTE_CONFIG_PATH + ' does not exist on the PS5.\n\n'
                    'Edit the form locally and click "Push to PS5" to create it.')
            else:
                short, title, body = _friendly_ftp_error('Load', e)
                app.after(0, _set_status, app, short, DANGER)
                app.after(0, messagebox.showerror, title, body)

    threading.Thread(target=worker, daemon=True).start()


def _on_loaded(app, text):
    data = _parse_ini(text)
    _apply_form(app, data)
    _set_status(app, 'Loaded %d keys from PS5 \u2713' % len(data), SUCCESS)
    footer_var = getattr(app, '_mm_load_footer_var', None)
    if footer_var is not None:
        from time import strftime
        n_rows = len(getattr(app, '_mm_rows', {}) or {})
        footer_var.set('Loaded from PS5 \u00b7 %s\n%d of %d settings present'
                       % (strftime('%H:%M:%S'), len(data), n_rows))


def _push_to_ps5(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab first.')
        return

    data = _collect_form(app)

    # Validate numerics
    for k in ('scan_interval_seconds', 'lvd_sector_size',
               'lvd_secondary_unit', 'lvd_image_type'):
        v = data.get(k, '').strip()
        if v == '':
            continue
        try:
            n = int(v, 0)   # accept hex (0x9) for lvd_image_type
            if n < 0:
                raise ValueError('negative')
        except ValueError:
            messagebox.showwarning('Invalid value',
                '%s must be a non-negative integer. Got: %r' % (k, v))
            return

    ini_text = _render_ini(data)
    if not messagebox.askyesno('Push to PS5',
            'Write %d bytes to:\n%s\n\nContinue?'
            % (len(ini_text), REMOTE_CONFIG_PATH)):
        return

    _set_status(app, 'Uploading config to PS5\u2026', ACCENT)
    app._settings['micromount_config'] = data
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass

    def worker():
        try:
            ftp = app._ftp_connect()
            try:
                try:
                    ftp.mkd('/data/micromount')
                except Exception:
                    pass
                buf = io.BytesIO(ini_text.encode('utf-8'))
                ftp.storbinary('STOR ' + REMOTE_CONFIG_PATH, buf)
            finally:
                try:
                    ftp.quit()
                except Exception:
                    pass
            app.after(0, _set_status, app, 'Pushed config to PS5 \u2713', SUCCESS)
        except Exception as e:
            short, title, body = _friendly_ftp_error('Push', e)
            app.after(0, _set_status, app, short, DANGER)
            app.after(0, messagebox.showerror, title, body)

    threading.Thread(target=worker, daemon=True).start()


def _send_payload(app):
    elf_path = ''
    rows = getattr(app, '_mm_rows', {}) or {}
    if 'mm_payload_path' in rows:
        try:
            elf_path = str(rows['mm_payload_path'].get_value()).strip()
        except Exception:
            pass
    if not elf_path:
        elf_path = str(app._settings.get('mm_payload_path', '')).strip()

    if not elf_path or not os.path.isfile(elf_path):
        messagebox.showwarning('Locate ELF',
            'Pick the micromount.elf file first using the Browse button '
            'on the Payload section.')
        return

    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab first.')
        return

    port_str = ''
    if 'mm_payload_port' in rows:
        try:
            port_str = str(rows['mm_payload_port'].get_value()).strip()
        except Exception:
            pass
    if not port_str:
        port_str = str(app._settings.get('mm_payload_port', 9021))
    try:
        port = int(port_str)
    except (ValueError, TypeError):
        port = 9021
    if not (1 <= port <= 65535):
        messagebox.showwarning('Invalid port', 'Payload port must be 1\u201365535.')
        return

    app._settings['mm_payload_port'] = port
    app._settings['mm_payload_path'] = elf_path
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass

    size = os.path.getsize(elf_path)
    _set_status(app, 'Sending payload to %s:%d\u2026' % (ip, port), ACCENT)

    def worker():
        try:
            start = time.time()
            sent  = 0
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
                            'Sending payload\u2026 %d%%' % pct, ACCENT)
            elapsed = time.time() - start
            app.after(0, _set_status, app,
                'Payload sent \u2713 (%.1f MB in %.1fs)'
                % (size / 1024 / 1024, elapsed), SUCCESS)
        except Exception as e:
            short, title, body = _friendly_ftp_error('Send', e)
            if '10061' in str(e) or 'refused' in str(e).lower():
                body = ('The PS5 refused the payload connection on port '
                        + str(port) + '.\n\n'
                        'A payload listener (etaHEN, GoldHEN-style loader) '
                        'must be running before you send an ELF.\n\n'
                        'Start your payload host on the PS5, then try again.')
            app.after(0, _set_status, app, short, DANGER)
            app.after(0, messagebox.showerror, title, body)

    threading.Thread(target=worker, daemon=True).start()


def _fetch_debug_log(app):
    ip = app._ftp_ip_var.get().strip()
    if not ip:
        messagebox.showwarning('No IP',
            'Set the PS5 IP address in the FTP tab first.')
        return
    _set_status(app, 'Fetching debug.log from PS5\u2026', ACCENT)

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
                app.after(0, _set_status, app, 'No debug.log on PS5 yet', WARNING)
                app.after(0, messagebox.showinfo, 'No log yet',
                    REMOTE_DEBUG_LOG + ' does not exist.\n\n'
                    'MicroMount has not run yet, or debug=0 in the config.')
            else:
                short, title, body = _friendly_ftp_error('Fetch', e)
                app.after(0, _set_status, app, short, DANGER)
                app.after(0, messagebox.showerror, title, body)

    threading.Thread(target=worker, daemon=True).start()


def _show_log_window(app, text):
    win = tk.Toplevel(app)
    win.title('MicroMount debug.log')
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
            initialfile='micromount-debug.log',
            filetypes=[('Log files', '*.log'), ('All files', '*.*')])
        if p:
            try:
                with open(p, 'w', encoding='utf-8') as f:
                    f.write(text)
            except Exception as e:
                messagebox.showerror('Save failed', str(e))

    tk.Button(bar, text='Save as\u2026', command=_save_as,
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
