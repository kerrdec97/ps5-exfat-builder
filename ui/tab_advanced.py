"""
ui/tab_advanced.py — Advanced tab (v2.9.0 redesign — Variant B).

Step 51 (v2.9.0): full redesign per design_handoff_exfat_advanced/.

Replaces the flat-form layout with:
  - Pill-button sub-tabs with counter badges (count = #fields differing
    from their default in that sub-tab)
  - 2x2 card grid for Build Parameters: Filesystem layout / Performance
    / Image options / ffpkg parameters
  - 300px side rail with "Recommended for your setup" (HW-detected) +
    "Why these defaults?" help
  - "Reset all" + "Save parameters" anchored right of the sub-tab bar

Layout:

    [ page head: ⚙ Advanced  + "Power-user knobs — defaults are safe"  ]
    [ sub-tab pills + ↻ Reset all + 💾 Save parameters                   ]
    ┌─ body (grid: 1fr / 300px) ────────────────────────────────────────┐
    │ ┌── card grid (2x2) ───────┐ ┌── side rail ──┐                    │
    │ │ Filesystem layout │ Perf │ │ Recommended    │                    │
    │ │───────────────────│──────│ │ for your setup │                    │
    │ │ Image options    │ ffpkg │ │ Why these defs│                    │
    │ └──────────────────┘───────┘ └───────────────┘                    │
    └───────────────────────────────────────────────────────────────────┘

Post-Build and Language Stripper sub-tabs still delegate to the existing
`app._build_advanced_post_build` and `app._build_advanced_lang_strip`
builders — the only sub-tab being structurally rewritten in v2.9.0 is
Build Parameters. The other two keep their content, just inside the new
shell.

Backwards-compat: every `app._adv_*` variable the rest of the code
reads is preserved. Existing callbacks (`_save_adv_params`,
`_adv_clamp_threads`, `_run_write_benchmark`) are invoked unchanged.
"""

import os
import sys
import subprocess
import tkinter as tk
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, SubTabBar, Tooltip


# ─────────────────────────────────────────────────────────────────────
# Hardware detection (for Recommended-for-your-setup card)
# ─────────────────────────────────────────────────────────────────────
def _detect_hw():
    """Returns a dict {model, kind, recommended_threads} or None.

    Best-effort: tries wmic Get-PhysicalDisk first (modern, reliable),
    falls back to wmic diskdrive (legacy, sometimes returns garbage on
    newer Windows). Silent fallback on every error.
    """
    if not sys.platform.startswith('win'):
        return None
    # Try Get-PhysicalDisk via PowerShell first — gives clean MediaType
    try:
        out = subprocess.check_output(
            ['powershell', '-NoProfile', '-Command',
             "Get-PhysicalDisk | Select-Object -First 1 FriendlyName,MediaType,BusType | Format-List"],
            text=True, timeout=5, stderr=subprocess.DEVNULL,
            creationflags=0x08000000)
        model = ''
        media = ''
        bus = ''
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('FriendlyName'):
                model = line.split(':', 1)[-1].strip()
            elif line.startswith('MediaType'):
                media = line.split(':', 1)[-1].strip()
            elif line.startswith('BusType'):
                bus = line.split(':', 1)[-1].strip()
        if model:
            lb = (bus or '').lower()
            lm = (media or '').lower()
            lname = model.lower()
            if 'nvme' in lb or 'nvme' in lname:
                return {'model': model, 'kind': 'NVMe Gen4 SSD',
                        'recommended_threads': 32}
            if 'ssd' in lm or 'solid' in lm or 'sata' in lb:
                return {'model': model, 'kind': 'SATA SSD',
                        'recommended_threads': 16}
            if 'hdd' in lm:
                return {'model': model, 'kind': 'HDD',
                        'recommended_threads': 1}
            # Heuristic fallback from model name
            if any(x in lname for x in (
                    'sn850', 'sn770', '980 pro', '990 pro', 'rocket',
                    'firecuda 530', 'fireblade', 'samsung 970',
                    'samsung 980', 'samsung 990', 'kingston kc', 'wd_black')):
                return {'model': model, 'kind': 'NVMe Gen4 SSD',
                        'recommended_threads': 32}
            if any(x in lname for x in ('ssd', 'crucial mx', 'samsung 870')):
                return {'model': model, 'kind': 'SATA SSD',
                        'recommended_threads': 16}
            return {'model': model, 'kind': 'Unknown drive',
                    'recommended_threads': 4}
    except Exception:
        pass
    # Fallback: legacy wmic
    try:
        out = subprocess.check_output(
            ['wmic', 'diskdrive', 'get', 'Model,MediaType', '/format:list'],
            text=True, timeout=3, stderr=subprocess.DEVNULL,
            creationflags=0x08000000)
        model = ''
        media = ''
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('Model=') and not model:
                model = line[len('Model='):]
            elif line.startswith('MediaType=') and not media:
                media = line[len('MediaType='):]
        if not model:
            return None
        lm = model.lower()
        if 'nvme' in lm or any(x in lm for x in (
                'sn850', 'sn770', '980 pro', '990 pro', 'rocket', 'firecuda 530')):
            return {'model': model, 'kind': 'NVMe Gen4 SSD',
                    'recommended_threads': 32}
        if 'ssd' in lm or 'solid' in (media or '').lower():
            return {'model': model, 'kind': 'SATA SSD',
                    'recommended_threads': 16}
        if 'hdd' in (media or '').lower() or 'fixed hard disk' in (media or '').lower():
            return {'model': model, 'kind': 'HDD',
                    'recommended_threads': 1}
        return {'model': model, 'kind': 'Unknown drive',
                'recommended_threads': 4}
    except Exception:
        return None


# ─────────────────────────────────────────────────────────────────────
# Main entry
# ─────────────────────────────────────────────────────────────────────
def build_advanced_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State for dirty tracking — keyed by sub-tab id
    app._adv_v2_dirty = {'params': set(), 'post': set(), 'lang': set()}
    app._adv_v2_hw = _detect_hw()

    _build_head(parent, app)
    _build_subtab_bar(parent, app)
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=24, pady=(0, 12))
    app._adv_v2_body = body

    # Three panes — only one visible at a time. Each pane is a
    # scrollable host so tall content (e.g. Post-Build's Danger Zone)
    # is always reachable instead of being clipped at the viewport
    # bottom. The scrollregion is recomputed on every <Configure> of
    # the inner frame, so adding cards never pushes content off-screen.
    app._adv_v2_panes = {}
    for key in ('params', 'post', 'lang'):
        pane = tk.Frame(body, bg=COLORS['bg_1'])
        app._adv_v2_panes[key] = pane

    _build_params_pane(_scroll_host(app._adv_v2_panes['params']), app)
    _build_post_pane(_scroll_host(app._adv_v2_panes['post']), app)
    _build_lang_pane(_scroll_host(app._adv_v2_panes['lang']), app)

    # Show last-used sub-tab
    last = app._settings.get('adv_active_subtab', 'params')
    if last not in app._adv_v2_panes:
        last = 'params'
    _show_subtab(app, last)


# ─────────────────────────────────────────────────────────────────────
# Page head
# ─────────────────────────────────────────────────────────────────────
def _build_head(parent, app):
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(18, 14))

    # Left: 32x32 accent tile + h2 + subtitle
    left = tk.Frame(head, bg=COLORS['bg_1'])
    left.pack(side='left', fill='x', expand=True)
    icon_tile = tk.Frame(left, bg=COLORS['accent_15'],
                         width=32, height=32)
    icon_tile.pack(side='left')
    icon_tile.pack_propagate(False)
    tk.Label(icon_tile, text='\u2699',
             bg=COLORS['accent_15'], fg=COLORS['accent'],
             font=('Segoe UI', 14)
             ).pack(expand=True)
    title_col = tk.Frame(left, bg=COLORS['bg_1'])
    title_col.pack(side='left', padx=(10, 0))
    tk.Label(title_col, text=_('Advanced'),
             font=(FONTS['h2'][0], 16, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Power-user configuration for image creation, '
                    'performance tuning and automation.'),
             font=(FONTS['mono_sm'][0], 10),
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(anchor='w')


# ─────────────────────────────────────────────────────────────────────
# Sub-tab bar (pill buttons + global Reset/Save actions on the right)
# ─────────────────────────────────────────────────────────────────────
def _build_subtab_bar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_1'])
    bar.pack(fill='x', padx=24, pady=(4, 8))

    app._adv_v2_tab_btns = {}     # key -> outer frame
    app._adv_v2_tab_counts = {}   # key -> count label (or None)
    tabs = [
        ('params', '\U0001f527', _('Build Parameters')),
        ('post',   '\U0001f680', _('Post-Build')),
        ('lang',   '\U0001f310', _('Language Stripper')),
    ]
    for key, icon, label in tabs:
        btn = _pill_tab(bar, key, icon, label, app)
        btn.pack(side='left', padx=(0, 6))
        app._adv_v2_tab_btns[key] = btn

    # Right side: spacer + Reset all + Save parameters
    spacer = tk.Frame(bar, bg=COLORS['bg_1'])
    spacer.pack(side='left', fill='x', expand=True)

    reset_btn = _ghost_btn(bar, '\u21bb  ' + _('Reset all'),
        command=lambda: _reset_all(app))
    reset_btn.pack(side='left', padx=(0, 6))
    save_btn = _accent_btn(bar, '\U0001f4be  ' + _('Save parameters'),
        command=lambda: _save_params(app))
    save_btn.pack(side='left')


def _pill_tab(parent, key, icon, label, app):
    """Create a pill-button sub-tab with optional counter badge."""
    outer = tk.Frame(parent, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1,
                     cursor='hand2')
    inner = tk.Frame(outer, bg=COLORS['bg_2'])
    inner.pack(padx=14, pady=7)
    ic = tk.Label(inner, text=icon,
                  font=('Segoe UI', 11),
                  bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    ic.pack(side='left', padx=(0, 6))
    lbl = tk.Label(inner, text=label,
                   font=('Segoe UI', 10),
                   bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    lbl.pack(side='left')
    # Counter badge (hidden by default)
    count = tk.Label(inner, text='',
                     font=(FONTS['mono_sm'][0], 8, 'bold'),
                     bg=COLORS['warn'], fg='#1a0e00',
                     padx=5, pady=1)
    # NOT packed yet — _update_tab_count handles pack/forget
    app._adv_v2_tab_counts[key] = count
    # Click handler — applied to ALL constituents so clicks register
    # anywhere on the pill, not just the bg frame
    def _click(_e=None):
        _show_subtab(app, key)
    for w in (outer, inner, ic, lbl, count):
        w.bind('<Button-1>', _click)
    # Stash refs for the active-state toggle
    outer._smp_inner = inner
    outer._smp_ic    = ic
    outer._smp_lbl   = lbl
    outer._smp_count = count
    outer._smp_key   = key
    return outer


def _show_subtab(app, key):
    """Switch to a sub-tab. Updates pill active state, hides other panes,
    shows the chosen pane, persists the choice."""
    for k, pane in app._adv_v2_panes.items():
        if k == key:
            pane.pack(fill='both', expand=True)
        else:
            pane.pack_forget()
    for k, btn in app._adv_v2_tab_btns.items():
        active = (k == key)
        _style_tab(btn, active)
    app._settings['adv_active_subtab'] = key
    try:
        save_settings(app._settings)
    except Exception:
        pass


def _style_tab(btn, active):
    """Apply active/inactive styling to a pill-button sub-tab."""
    if active:
        bg = COLORS['accent_08']
        fg_ic  = COLORS['accent_hi']
        fg_lbl = COLORS['accent_hi']
        border = COLORS['accent_lo']
        weight = 'bold'
    else:
        bg = COLORS['bg_2']
        fg_ic  = COLORS['fg_4']
        fg_lbl = COLORS['fg_4']
        border = COLORS['border_2']
        weight = 'normal'
    btn.configure(bg=bg, highlightbackground=border)
    btn._smp_inner.configure(bg=bg)
    btn._smp_ic.configure(bg=bg, fg=fg_ic)
    fnt = btn._smp_lbl.cget('font')
    if isinstance(fnt, str):
        fnt_family = 'Segoe UI'
        fnt_size = 10
    else:
        try:
            fnt_family, fnt_size = fnt[0], fnt[1]
        except Exception:
            fnt_family, fnt_size = 'Segoe UI', 10
    btn._smp_lbl.configure(bg=bg, fg=fg_lbl,
                            font=(fnt_family, fnt_size, weight))


def _update_tab_count(app, key, count):
    """Show / hide / update the warn-tinted count badge on a sub-tab."""
    lbl = app._adv_v2_tab_counts.get(key)
    if lbl is None:
        return
    if count > 0:
        lbl.config(text=str(count))
        lbl.pack(side='left', padx=(6, 0))
    else:
        lbl.pack_forget()


# ─────────────────────────────────────────────────────────────────────
# Legacy pane (post-build + lang-stripper) — kept inside scroll wrapper
# ─────────────────────────────────────────────────────────────────────
def _scroll_host(parent):
    """Wrap `parent` in a vertical-scroll canvas and return the inner
    frame to build into. Fixes content taller than the viewport being
    clipped (the scrollregion tracks the inner frame's real size)."""
    cv = tk.Canvas(parent, bg=COLORS['bg_1'], highlightthickness=0)
    sb = ttk.Scrollbar(parent, orient='vertical', command=cv.yview)
    cv.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    cv.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(cv, bg=COLORS['bg_1'])
    tag = 'host_' + str(id(inner))
    cv.create_window((0, 0), window=inner, anchor='nw', tags=tag)

    def _on_inner(_e=None, c=cv):
        c.configure(scrollregion=c.bbox('all'))
    inner.bind('<Configure>', _on_inner)
    cv.bind('<Configure>',
            lambda e, c=cv, t=tag: c.itemconfig(t, width=e.width))

    # Scroll only while the pointer is over THIS pane (3 panes stack;
    # bind_all/unbind_all on Enter/Leave keeps them from fighting).
    def _wheel(e, c=cv):
        c.yview_scroll(int(-1 * (e.delta / 120)), 'units')
    cv.bind('<Enter>', lambda _e, c=cv: c.bind_all('<MouseWheel>', _wheel))
    cv.bind('<Leave>', lambda _e, c=cv: c.unbind_all('<MouseWheel>'))
    return inner


def _build_legacy_pane(parent, app, builder_fn):
    """Wrap the existing legacy builder in our scroll frame so it can
    sit inside the new sub-tab pane without modification."""
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True)
    cv = tk.Canvas(wrap, bg=COLORS['bg_1'], highlightthickness=0)
    sb = ttk.Scrollbar(wrap, command=cv.yview)
    cv.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    cv.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(cv, bg=COLORS['bg_1'])
    tag = 'inner_' + str(id(inner))
    cv.create_window((0, 0), window=inner, anchor='nw', tags=tag)
    inner.bind('<Configure>',
        lambda e, c=cv: c.configure(scrollregion=c.bbox('all')))
    cv.bind('<Configure>',
        lambda e, c=cv, t=tag: c.itemconfig(t, width=e.width))
    cv.bind('<MouseWheel>',
        lambda e, c=cv: c.yview_scroll(int(-1 * (e.delta / 120)), 'units'))
    try:
        builder_fn(inner)
    except Exception as _e:
        # If a legacy builder fails (some have late-init dependencies),
        # at least show a friendly placeholder rather than a blank pane.
        tk.Label(inner,
                 text='(' + str(_e) + ')',
                 font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['danger']
                 ).pack(padx=24, pady=24)


# ─────────────────────────────────────────────────────────────────────
# Post-Build pane (v2.9.0) — single card with 6 SettingRows
# ─────────────────────────────────────────────────────────────────────
def _build_post_summary(parent, app):
    """Automation Summary — enabled-actions checklist + a visual
    Build → Verify → Log → Upload workflow strip. Pure mirror of the
    five post-build vars; updates live via traces."""
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.grid(row=0, column=0, columnspan=2, sticky='ew',
              padx=(0, 12), pady=(0, 12))
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=16, pady=(10, 12))

    tk.Label(inner, text=_('AUTOMATION PIPELINE'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(anchor='w')

    body = tk.Frame(inner, bg=COLORS['bg_2'])
    body.pack(fill='x', pady=(8, 0))
    # checklist + flow hug the left; a trailing spacer absorbs the
    # slack instead of stranding the flow at the far edge.
    body.grid_columnconfigure(0, weight=0)
    body.grid_columnconfigure(1, weight=0)
    body.grid_columnconfigure(2, weight=1)

    # Left: enabled-actions checklist
    checks = tk.Frame(body, bg=COLORS['bg_2'])
    checks.grid(row=0, column=0, sticky='nw', padx=(0, 40))
    items = [(app._adv_auto_upload_var, _('Upload to PS5')),
             (app._adv_robo_log_var, _('Save copy logs')),
             (app._adv_checksum_var, _('Verify checksum')),
             (app._adv_delete_source_var, _('Delete source (exFAT)')),
             (app._adv_delete_source_ffpkg_var,
              _('Delete source (ffpkg)'))]
    rows = []
    for var, label in items:
        r = tk.Label(checks, text='', font=FONTS['mono_sm'],
                     bg=COLORS['bg_2'], anchor='w')
        r.pack(anchor='w', pady=(0, 2))
        rows.append((var, label, r))

    # Workflow strip — sits right next to the checklist
    flow_wrap = tk.Frame(body, bg=COLORS['bg_2'])
    flow_wrap.grid(row=0, column=1, sticky='nw')
    tk.Label(flow_wrap, text=_('Current flow'),
             font=(FONTS['mono_sm'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(anchor='w', pady=(0, 6))
    flow = tk.Frame(flow_wrap, bg=COLORS['bg_2'])
    flow.pack(anchor='w')
    steps = {}

    def _chip(key, icon, label):
        c = tk.Label(flow, text=icon + '  ' + label,
                     font=(FONTS['mono_sm'][0], 9, 'bold'),
                     bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                     padx=9, pady=4,
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
        c.pack(side='left')
        steps[key] = c
        return c

    def _arrow():
        tk.Label(flow, text='\u2192',
                 font=(FONTS['body'][0], 11, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(side='left', padx=6)

    _chip('build', '\U0001f528', _('Build'))
    _arrow()
    _chip('verify', '\U0001f50d', _('Verify'))
    _arrow()
    _chip('log', '\U0001f4dd', _('Log'))
    _arrow()
    _chip('upload', '\U0001f4e1', _('Upload'))

    def _sync(*_a):
        try:
            for var, label, lbl in rows:
                on = bool(var.get())
                lbl.config(
                    text=('\u2713  ' if on else '\u2717  ') + label,
                    fg=COLORS['success_hi'] if on else COLORS['fg_5'])
            states = {'build': True,
                      'verify': bool(app._adv_checksum_var.get()),
                      'log': bool(app._adv_robo_log_var.get()),
                      'upload': bool(app._adv_auto_upload_var.get())}
            for key, chip in steps.items():
                if not chip.winfo_exists():
                    return
                if states[key]:
                    chip.config(bg=COLORS['accent_08'],
                                fg=COLORS['accent_hi'],
                                highlightbackground=COLORS['accent_lo'])
                else:
                    chip.config(bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                                highlightbackground=COLORS['border_3'])
        except Exception:
            pass

    for var, _l, _r in rows:
        try:
            var.trace_add('write', _sync)
        except Exception:
            pass
    _sync()


def _post_card(parent, row, col, icon, title, desc, accent,
               state_vars=None, danger=False):
    """Automation card shell with a live ON/OFF state pill derived
    from the supplied BooleanVars (any True → ON). Presentation only —
    the rows inside keep their existing vars and callbacks."""
    # Danger cards keep the normal surface so the shared setting rows
    # blend; the red lives in the border, title, and pill.
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=(COLORS['danger'] if danger
                                         else COLORS['border_2']),
                    highlightthickness=2 if danger else 1)
    card.grid(row=row, column=col, sticky='nsew',
              padx=(0, 12), pady=(0, 12))
    bgc = COLORS['bg_2']

    hbg = COLORS['danger_bg'] if danger else bgc
    head = tk.Frame(card, bg=hbg)
    head.pack(fill='x')
    hpad = tk.Frame(head, bg=hbg)
    hpad.pack(fill='x', padx=14, pady=(12, 8 if danger else 4))
    tk.Label(hpad, text=icon, font=('Segoe UI', 13),
             bg=hbg, fg=accent).pack(side='left', padx=(0, 8))
    tcol = tk.Frame(hpad, bg=hbg)
    tcol.pack(side='left', fill='x', expand=True)
    trow = tk.Frame(tcol, bg=hbg)
    trow.pack(anchor='w', fill='x')
    tk.Label(trow, text=title, font=('Segoe UI', 10, 'bold'),
             bg=hbg, fg=COLORS['danger_hi'] if danger
             else COLORS['fg_0'], anchor='w').pack(side='left')
    if danger:
        tk.Label(trow, text=_('DESTRUCTIVE'),
                 font=(FONTS['mono_sm'][0], 7, 'bold'),
                 bg=COLORS['danger'], fg='#000000',
                 padx=6, pady=1).pack(side='left', padx=(8, 0))
    tk.Label(tcol, text=desc, font=FONTS['mono_sm'],
             bg=hbg, fg=COLORS['fg_5'], anchor='w',
             wraplength=420, justify='left').pack(anchor='w')

    pill = tk.Label(hpad, text='OFF',
                    font=(FONTS['mono_sm'][0], 8, 'bold'),
                    bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                    padx=8, pady=2,
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    pill.pack(side='right')

    if state_vars:
        def _sync(*_a):
            try:
                on = any(bool(v.get()) for v in state_vars)
                if not pill.winfo_exists():
                    return
                if on and danger:
                    pill.config(text='ARMED', bg=COLORS['danger_bg'],
                                fg=COLORS['danger_hi'],
                                highlightbackground=COLORS['danger'])
                elif on:
                    pill.config(text='ON', bg=COLORS['success_bg'],
                                fg=COLORS['success_hi'],
                                highlightbackground=COLORS['success'])
                else:
                    pill.config(text='OFF', bg=COLORS['bg_3'],
                                fg=COLORS['fg_5'],
                                highlightbackground=COLORS['border_3'])
            except Exception:
                pass
        for v in state_vars:
            try:
                v.trace_add('write', _sync)
            except Exception:
                pass
        _sync()

    body = tk.Frame(card, bg=bgc)
    body.pack(fill='x', padx=14, pady=(0, 12))
    return body


def _build_post_pane(parent, app):
    """v3.6.0: the single post-build form became an automation
    dashboard — Upload / Verification / Logging / Image Output cards
    in a 2x2 grid with live state pills, and a full-width Danger Zone
    card for the destructive delete-source toggles. Same six
    SettingRows, same vars, same callbacks."""
    parent.columnconfigure(0, weight=1, uniform='postc')
    parent.columnconfigure(1, weight=1, uniform='postc')

    # Vars that may not exist yet
    if not hasattr(app, '_adv_img_size_var'):
        app._adv_img_size_var = tk.StringVar(
            value=app._settings.get('adv_img_size_override', ''))

    # ── Automation Summary (full width, row 0) ──
    _build_post_summary(parent, app)

    # ── Upload ──
    b = _post_card(parent, 1, 0, '\U0001f4e1', _('Upload'),
                   _('Push finished images to the PS5 automatically.'),
                   COLORS['accent'],
                   state_vars=[app._adv_auto_upload_var])
    _setting_row(b, app,
        label=_('Auto-upload .exfat to PS5 via FTP'),
        kind='toggle',
        var=app._adv_auto_upload_var,
        default=False,
        help_text=_('Uses FTP settings from the PS5 tab. Fires after '
                    'each successful build.'),
        on_change=app._adv_save_post_build)

    # ── Verification ──
    b = _post_card(parent, 1, 1, '\U0001f50d', _('Verification'),
                   _('Integrity checks after every copy pass.'),
                   COLORS['teal'],
                   state_vars=[app._adv_checksum_var])
    _setting_row(b, app,
        label=_('Checksum verification (SHA-256)'),
        kind='toggle',
        var=app._adv_checksum_var,
        default=False,
        help_text=_('Slower, more accurate than the default size '
                    'check.'),
        on_change=app._adv_save_post_build)

    # ── Logging ──
    b = _post_card(parent, 2, 0, '\U0001f4dd', _('Logging'),
                   _('Keep a verbose copy log next to each build.'),
                   COLORS['purple'],
                   state_vars=[app._adv_robo_log_var])
    _setting_row(b, app,
        label=_('Save robocopy log to file'),
        kind='toggle',
        var=app._adv_robo_log_var,
        default=False,
        help_text=_('Verbose copy log saved alongside the build for '
                    'debugging.'),
        on_change=app._adv_save_post_build)

    # ── Image Output ──
    b = _post_card(parent, 2, 1, '\U0001f4bf', _('Image Output'),
                   _('Override how the output image is sized.'),
                   COLORS['accent_hi'])
    _setting_row(b, app,
        label=_('Image size override (GB)'),
        kind='text',
        var=app._adv_img_size_var,
        default='',
        help_text=_('Blank = auto-size. Set e.g. 90 to force a 90 GB '
                    'image.'),
        on_change=app._adv_save_post_build)

    # ── Danger Zone (full width) ──
    dz = _post_card(parent, 3, 0, '\u26a0', _('Danger Zone'),
                    _('Destructive cleanup \u2014 these delete the '
                      'source folder once a build succeeds. Cannot be '
                      'undone.'),
                    COLORS['danger_hi'],
                    state_vars=[app._adv_delete_source_var,
                                app._adv_delete_source_ffpkg_var],
                    danger=True)
    dz.master.grid_configure(columnspan=2)
    _setting_row(dz, app,
        label=_('Delete source folder after exFAT build'),
        kind='toggle',
        var=app._adv_delete_source_var,
        default=False,
        help_text=_('\u26a0 Cannot be undone. Source folder is removed '
                    'once the .exfat is built successfully.'),
        severity='danger')
    _setting_row(dz, app,
        label=_('Delete source folder after ffpkg build'),
        kind='toggle',
        var=app._adv_delete_source_ffpkg_var,
        default=False,
        help_text=_('\u26a0 Cannot be undone. Source folder is removed '
                    'once the .ffpkg is built successfully.'),
        severity='danger')


# ─────────────────────────────────────────────────────────────────────
# Language Stripper pane (v2.9.0) — banner + radios + browse + chips
# ─────────────────────────────────────────────────────────────────────
def _build_lang_pane(parent, app):
    parent.columnconfigure(0, weight=1)
    parent.rowconfigure(0, weight=1)

    card_outer = tk.Frame(parent, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, 12))

    # Card header
    head = tk.Frame(card_outer, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=14, pady=(12, 4))
    tk.Label(head, text='\U0001f310',
             font=('Segoe UI', 14),
             bg=COLORS['bg_2'], fg=COLORS['purple']
             ).pack(side='left', padx=(0, 8))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=_('Language Stripper'),
             font=('Segoe UI', 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Remove unwanted language packs from PS5 game dumps to save space.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(anchor='w')
    # BETA pill on the right of the header
    tk.Label(head, text=_('BETA'),
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['warn_bg'], fg=COLORS['warn'],
             padx=8, pady=2,
             highlightbackground=COLORS['warn'], highlightthickness=1
             ).pack(side='right')

    body = tk.Frame(card_outer, bg=COLORS['bg_2'])
    body.pack(fill='both', expand=True, padx=14, pady=(0, 12))

    # ── Feature-page summary strip (v3.6.0) ──
    # Selected Folder mirrors the path var live; the scan-derived
    # cells read app._adv_lang_stats when a scan backend populates it
    # (keys: languages, est_saving, files, new_size) and show "\u2014"
    # until then. Presentation only.
    top = tk.Frame(body, bg=COLORS['bg_2'])
    top.pack(fill='x', pady=(0, 4))
    for i in range(4):
        top.grid_columnconfigure(i, weight=1, uniform='langtop')

    def _lang_cell(parent_, col, caption, fg, ncols, key):
        cell = tk.Frame(parent_, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        cell.grid(row=0, column=col, sticky='ew',
                  padx=(0 if col == 0 else 10, 0))
        tk.Label(cell, text=caption.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_3'], fg=COLORS['fg_5']
                 ).pack(anchor='w', padx=12, pady=(9, 0))
        v = tk.Label(cell, text='\u2014',
                     font=(FONTS['h2'][0], 13, 'bold'),
                     bg=COLORS['bg_3'], fg=fg, anchor='w')
        v.pack(fill='x', padx=12, pady=(2, 9))
        return v

    c_folder = _lang_cell(top, 0, _('Selected Folder'),
                          COLORS['fg_1'], 4, 'folder')
    c_langs = _lang_cell(top, 1, _('Languages Found'),
                         COLORS['purple_hi'], 4, 'languages')
    c_sel = _lang_cell(top, 2, _('Selected'),
                       COLORS['accent_hi'], 4, 'selected')
    c_save = _lang_cell(top, 3, _('Estimated Saving'),
                        COLORS['success_hi'], 4, 'est_saving')

    keep_var = tk.StringVar(value='')
    keep_lbl = tk.Label(body, textvariable=keep_var,
                        font=FONTS['mono_sm'],
                        bg=COLORS['bg_2'], fg=COLORS['teal'],
                        anchor='w')
    keep_lbl.pack(fill='x', pady=(0, 8))

    def _mirror_folder(*_a):
        try:
            f = (app._adv_lang_folder_var.get() or '').strip()
            if len(f) > 34:
                f = '\u2026' + f[-32:]
            c_folder.config(text=f or '\u2014',
                            font=(FONTS['mono_sm'][0], 10, 'bold'))
        except Exception:
            pass

    def _refresh_lang_stats():
        try:
            if not c_langs.winfo_exists():
                return
            st = getattr(app, '_adv_lang_stats', None) or {}
            c_langs.config(text=str(st.get('languages', '\u2014')))
            c_sel.config(text=str(st.get('selected', '\u2014')))
            c_save.config(text=str(st.get('est_saving', '\u2014')))
            keep = st.get('keep')
            keep_var.set((_('Keep') + ':  ' + ' / '.join(keep))
                         if keep else '')
            if hasattr(app, '_adv_lang_bottom_cells'):
                bc = app._adv_lang_bottom_cells
                bc['save'].config(
                    text=str(st.get('est_saving', '\u2014')))
                bc['files'].config(text=str(st.get('files', '\u2014')))
                bc['new'].config(text=str(st.get('new_size', '\u2014')))
        except Exception:
            pass
        try:
            body.after(2000, _refresh_lang_stats)
        except Exception:
            pass

    app._adv_lang_refresh_stats = _refresh_lang_stats
    body.after(600, _refresh_lang_stats)

    # Banner row — red-tinted
    banner = tk.Frame(body, bg=COLORS['danger_bg'],
                      highlightbackground=COLORS['danger'],
                      highlightthickness=1)
    banner.pack(fill='x', pady=(0, 12))
    bi = tk.Frame(banner, bg=COLORS['danger_bg'])
    bi.pack(fill='x', padx=12, pady=8)
    tk.Label(bi, text='\u26a0',
             font=('Segoe UI', 12, 'bold'),
             bg=COLORS['danger_bg'], fg=COLORS['danger_hi']
             ).pack(side='left', padx=(0, 8))
    tk.Label(bi,
             text=_('Deletes files permanently \u2014 back up games first.'),
             font=('Segoe UI', 10),
             bg=COLORS['danger_bg'], fg=COLORS['danger_hi']
             ).pack(side='left')
    tk.Label(bi,
             text=_('  May not work on all games'),
             font=FONTS['mono_sm'],
             bg=COLORS['danger_bg'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 0))

    # Mode radios
    mode_row = tk.Frame(body, bg=COLORS['bg_2'])
    mode_row.pack(fill='x', pady=(0, 8))
    if not hasattr(app, '_adv_lang_mode_var'):
        app._adv_lang_mode_var = tk.StringVar(
            value=app._settings.get('adv_lang_mode', 'single'))
    tk.Radiobutton(mode_row, text=_('Single game folder'),
                    variable=app._adv_lang_mode_var, value='single',
                    font=('Segoe UI', 10),
                    bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                    activebackground=COLORS['bg_2'],
                    selectcolor=COLORS['bg_3'],
                    cursor='hand2'
                    ).pack(side='left', padx=(0, 16))
    tk.Radiobutton(mode_row, text=_('Scan all games in a folder'),
                    variable=app._adv_lang_mode_var, value='scan',
                    font=('Segoe UI', 10),
                    bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                    activebackground=COLORS['bg_2'],
                    selectcolor=COLORS['bg_3'],
                    cursor='hand2'
                    ).pack(side='left')

    # Games folder field
    folder_row = tk.Frame(body, bg=COLORS['bg_2'])
    folder_row.pack(fill='x', pady=(0, 8))
    tk.Label(folder_row, text=_('Games folder:'),
             font=('Segoe UI', 10),
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(side='left', padx=(0, 8))
    if not hasattr(app, '_adv_lang_folder_var'):
        app._adv_lang_folder_var = tk.StringVar(
            value=app._settings.get('adv_lang_folder', ''))
    _mirror_folder()
    try:
        app._adv_lang_folder_var.trace_add(
            'write', lambda *a: _mirror_folder())
    except Exception:
        pass
    folder_ef = tk.Frame(folder_row, bg=COLORS['field_bg'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    folder_ef.pack(side='left', fill='x', expand=True)
    tk.Entry(folder_ef, textvariable=app._adv_lang_folder_var,
             font=(FONTS['mono_sm'][0], 10),
             bg=COLORS['field_bg'], fg=COLORS['field_fg'],
             insertbackground=COLORS['field_fg'],
             relief='flat', bd=4
             ).pack(fill='x')
    _ghost_btn(folder_row, '\U0001f4c1  ' + _('Browse'),
        command=lambda: _lang_browse(app)
        ).pack(side='left', padx=(6, 0))
    _accent_btn(folder_row, '\U0001f50d  ' + _('Scan'),
        command=lambda: _lang_scan(app)
        ).pack(side='left', padx=(6, 0))

    # Quick-keep chip row
    chip_row = tk.Frame(body, bg=COLORS['bg_2'])
    chip_row.pack(fill='x', pady=(0, 12))
    tk.Label(chip_row, text=_('Quick keep:'),
             font=(FONTS['mono_sm'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(0, 8))
    chips = [
        ('GB', _('EN only'),  ['en']),
        ('JP', _('JA only'),  ['ja']),
        ('DE', _('DE only'),  ['de']),
        ('FR', _('FR only'),  ['fr']),
        ('ES', _('ES only'),  ['es']),
        ('',   _('EN+JA'),    ['en', 'ja']),
        ('EU', _('EU only'),  ['en', 'de', 'fr', 'es', 'it']),
    ]
    for prefix, label, keep_codes in chips:
        text = prefix + ' ' + label if prefix else label
        chip = tk.Label(chip_row, text=text,
            font=(FONTS['mono_sm'][0], 9),
            bg=COLORS['bg_3'], fg=COLORS['fg_2'],
            padx=8, pady=3,
            highlightbackground=COLORS['border_3'],
            highlightthickness=1, cursor='hand2')
        chip.pack(side='left', padx=(0, 4))
        chip.bind('<Button-1>',
            lambda _e, codes=keep_codes: _lang_quick_keep(app, codes))
        # Hover effect
        def _enter(_e, c=chip):
            c.config(bg=COLORS['accent_08'],
                     highlightbackground=COLORS['accent_lo'])
        def _leave(_e, c=chip):
            c.config(bg=COLORS['bg_3'],
                     highlightbackground=COLORS['border_3'])
        chip.bind('<Enter>', _enter)
        chip.bind('<Leave>', _leave)

    # Result area (200px min height, dashed border)
    result_outer = tk.Frame(body, bg=COLORS['bg_3'],
                            highlightbackground=COLORS['border_3'],
                            highlightthickness=1, height=200)
    result_outer.pack(fill='both', expand=True, pady=(0, 8))
    result_outer.pack_propagate(False)
    # Empty state — replaced when scan results arrive
    app._adv_lang_result_frame = result_outer
    _show_lang_empty_state(app)

    # ── Bottom summary strip (v3.6.0) — scan-derived, "\u2014" until
    # a scan populates app._adv_lang_stats ──
    bot = tk.Frame(body, bg=COLORS['bg_2'])
    bot.pack(fill='x', pady=(0, 10))
    for i in range(3):
        bot.grid_columnconfigure(i, weight=1, uniform='langbot')
    app._adv_lang_bottom_cells = {
        'save': _lang_cell(bot, 0, _('Space To Save'),
                           COLORS['success_hi'], 3, 'est_saving'),
        'files': _lang_cell(bot, 1, _('Files To Remove'),
                            COLORS['warn_hi'], 3, 'files'),
        'new': _lang_cell(bot, 2, _('New Estimated Size'),
                          COLORS['teal'], 3, 'new_size'),
    }

    # Footer: Keep all / Keep none on the left; large primary strip
    footer = tk.Frame(body, bg=COLORS['bg_2'])
    footer.pack(fill='x')
    _ghost_btn(footer, '\u2713  ' + _('Keep all'),
        command=lambda: _lang_keep_all(app)
        ).pack(side='left', padx=(0, 4))
    _ghost_btn(footer, '\u2717  ' + _('Keep none'),
        command=lambda: _lang_keep_none(app)
        ).pack(side='left')
    strip_btn = tk.Button(footer,
        text='\U0001f5d1  ' + _('Strip Selected Languages'),
        font=(FONTS['button'][0], 11, 'bold'),
        bg=COLORS['danger'], fg=COLORS['fg_0'],
        activebackground=COLORS['danger_hi'],
        activeforeground=COLORS['fg_0'],
        relief='flat', bd=0, padx=22, pady=10,
        cursor='hand2',
        command=lambda: _lang_strip(app))
    strip_btn.pack(side='right')


def _show_lang_empty_state(app):
    """Render the empty-state placeholder in the result area."""
    frame = app._adv_lang_result_frame
    for w in frame.winfo_children():
        w.destroy()
    inner = tk.Frame(frame, bg=COLORS['bg_3'])
    inner.pack(expand=True)
    tk.Label(inner, text='\U0001f310',
             font=('Segoe UI', 28),
             bg=COLORS['bg_3'], fg=COLORS['fg_6']
             ).pack(pady=(0, 6))
    tk.Label(inner,
             text=_('Select a games folder and click Scan to see language packs.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_5'],
             wraplength=400, justify='center'
             ).pack()


# ─────────────────────────────────────────────────────────────────────
# Language Stripper callbacks — delegate to existing app helpers
# ─────────────────────────────────────────────────────────────────────
def _lang_browse(app):
    """Open a folder picker, set the result on _adv_lang_folder_var."""
    from tkinter import filedialog
    folder = filedialog.askdirectory(title='Select games folder')
    if folder:
        app._adv_lang_folder_var.set(folder)
        app._settings['adv_lang_folder'] = folder
        try:
            save_settings(app._settings)
        except Exception:
            pass


def _lang_scan(app):
    """Trigger a language scan. Delegates to existing helper if present;
    otherwise shows a placeholder message."""
    fn = getattr(app, '_adv_lang_scan', None)
    if fn is None:
        # Fall back to existing _build_advanced_lang_strip's internals
        fn = getattr(app, '_lang_scan', None)
    if fn:
        try:
            fn()
        except Exception as e:
            from tkinter import messagebox
            messagebox.showerror('Scan failed', str(e))
    else:
        from tkinter import messagebox
        messagebox.showinfo('Not available',
            'Language scan not implemented yet in this build.')


def _lang_quick_keep(app, codes):
    """Quick-keep chip click. Sets the desired keep-codes on app state."""
    fn = getattr(app, '_adv_lang_quick_keep', None)
    if fn:
        try:
            fn(codes)
            return
        except Exception:
            pass


def _lang_keep_all(app):
    fn = getattr(app, '_adv_lang_keep_all', None)
    if fn:
        try: fn()
        except Exception: pass


def _lang_keep_none(app):
    fn = getattr(app, '_adv_lang_keep_none', None)
    if fn:
        try: fn()
        except Exception: pass


def _lang_strip(app):
    fn = getattr(app, '_adv_lang_strip', None)
    if fn:
        try: fn()
        except Exception as e:
            from tkinter import messagebox
            messagebox.showerror('Strip failed', str(e))


# ─────────────────────────────────────────────────────────────────────
# Build Parameters pane — the redesigned 2x2 grid + side rail
# ─────────────────────────────────────────────────────────────────────
def _build_params_pane(parent, app):
    # v3.6.0 layout: a 2x2 card grid on top with a full-width
    # recommendation assistant beneath it (replaces the cramped 300px
    # right sidebar that wrapped text and unbalanced the cards on
    # smaller displays / Windows scaling).
    grid = tk.Frame(parent, bg=COLORS['bg_1'])
    grid.pack(fill='x')
    grid.columnconfigure(0, weight=1, uniform='card')
    grid.columnconfigure(1, weight=1, uniform='card')

    _build_card_fs    (grid, app, row=0, col=0)
    _build_card_image (grid, app, row=0, col=1)
    _build_card_perf  (grid, app, row=1, col=0)
    _build_card_ffpkg (grid, app, row=1, col=1)

    # Recommendation Configuration Assistant — full width, three
    # columns (setup recommendation · current profile · why these
    # defaults) so nothing has to wrap into a narrow rail.
    assistant = tk.Frame(parent, bg=COLORS['bg_1'])
    assistant.pack(fill='x', pady=(0, 4))
    assistant.columnconfigure(0, weight=3, uniform='asst')
    assistant.columnconfigure(1, weight=2, uniform='asst')
    assistant.columnconfigure(2, weight=2, uniform='asst')

    col0 = tk.Frame(assistant, bg=COLORS['bg_1'])
    col0.grid(row=0, column=0, sticky='nsew', padx=(0, 12))
    _build_recco_card(col0, app)
    col1 = tk.Frame(assistant, bg=COLORS['bg_1'])
    col1.grid(row=0, column=1, sticky='nsew', padx=(0, 12))
    _build_profile_card(col1, app)
    col2 = tk.Frame(assistant, bg=COLORS['bg_1'])
    col2.grid(row=0, column=2, sticky='nsew')
    _build_help_card(col2, app)


# ─────────────────────────────────────────────────────────────────────
# Cards — each is a Frame with a header + body of SettingRows
# ─────────────────────────────────────────────────────────────────────
def _card_shell(parent, row, col, icon, title, subtitle):
    outer = tk.Frame(parent, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    outer.grid(row=row, column=col, sticky='nsew',
               padx=(0 if col == 0 else 6, 6 if col == 0 else 0),
               pady=(0, 12))

    head = tk.Frame(outer, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=16, pady=(14, 6))
    tk.Label(head, text=icon,
             font=('Segoe UI', 15),
             bg=COLORS['bg_2'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 9))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=title,
             font=('Segoe UI', 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col, text=subtitle,
             font=(FONTS['mono_sm'][0], 10),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(anchor='w', pady=(1, 0))

    body = tk.Frame(outer, bg=COLORS['bg_2'])
    body.pack(fill='x', padx=16, pady=(2, 14))
    return body


# ── Filesystem layout ────────────────────────────────────────────────
def _build_card_fs(parent, app, row, col):
    body = _card_shell(parent, row, col,
        '\U0001f9f1', _('Filesystem layout'),
        _('How exFAT and ffpkg lay out the image.'))

    _setting_row(body, app,
        label=_('Cluster size'),
        kind='select',
        var=app._adv_cluster_var,
        options=[('Auto', 'Auto (64K)'),
                 ('32768', '32K'),
                 ('65536', '64K'),
                 ('131072', '128K'),
                 ('262144', '256K')],
        default='Auto',
        help_text=_('PS5 expects 64K. Auto = 64K.'))

    _setting_row(body, app,
        label=_('Sector size'),
        kind='select',
        var=app._adv_sector_var,
        options=[('512', '512'), ('4096', '4096')],
        default='512',
        help_text=_('PS5 expects 512. 4096 needs config.ini edits.'))



# ── Performance ──────────────────────────────────────────────────────
def _build_card_perf(parent, app, row, col):
    body = _card_shell(parent, row, col,
        '\u26a1', _('Performance'),
        _('Copy concurrency, retries, benchmark.'))

    # Recommended threads meta line — only if HW detected
    hw = app._adv_v2_hw
    meta = None
    if hw:
        meta = (_('Recommended: %d (%s detected)')
                % (hw['recommended_threads'], hw['kind']))

    _setting_row(body, app,
        label=_('Threads'),
        kind='int',
        var=app._adv_threads_var,
        default='1',
        help_text=_('1 = sequential (safest). HDD 1-4, SATA SSD 8-16, NVMe 32-64.'),
        meta=meta)

    _setting_row(body, app,
        label=_('Retries (/R)'),
        kind='int',
        var=app._adv_retries_var,
        default='3',
        help_text=_('Retry count for a failed file copy.'))

    _setting_row(body, app,
        label=_('Retry wait (/W, s)'),
        kind='int',
        var=app._adv_retry_wait_var,
        default='3',
        help_text=_('Seconds to wait between retries.'))

    # Write benchmark pill + button — laid out like a _setting_row so
    # the controls line up with the entries above
    bench_row = tk.Frame(body, bg=COLORS['bg_2'])
    bench_row.pack(fill='x', pady=(8, 0))
    bench_lcol = tk.Frame(bench_row, bg=COLORS['bg_2'])
    bench_lcol.pack(side='left', fill='x', expand=True)
    tk.Label(bench_lcol, text=_('Write speed'),
             font=('Segoe UI', 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
             anchor='w').pack(anchor='w')
    tk.Label(bench_lcol, text=_('1 GiB sequential test on the temp drive.'),
             font=(FONTS['mono_sm'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             anchor='w').pack(anchor='w', pady=(3, 0))
    bench_ctrl = tk.Frame(bench_row, bg=COLORS['bg_2'])
    bench_ctrl.pack(side='left', padx=(12, 12))
    bench_val = app._settings.get('adv_write_benchmark', '\u2014')
    bench_pill = tk.Label(bench_ctrl, text=bench_val,
        font=(FONTS['mono_sm'][0], 10, 'bold'),
        bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
        padx=8, pady=2,
        highlightbackground=COLORS['accent_lo'], highlightthickness=1)
    bench_pill.pack(side='left', padx=(0, 6))
    _ghost_btn(bench_ctrl, '\u25b6  ' + _('Run'),
        command=lambda: _run_benchmark(app, bench_pill)
        ).pack(side='left')


# ── Image options ────────────────────────────────────────────────────
def _build_card_image(parent, app, row, col):
    body = _card_shell(parent, row, col,
        '\u2699', _('Image options'),
        _('Hygiene flags & verification.'))

    _setting_row(body, app,
        label=_('Skip post-build re-mount check'),
        kind='toggle',
        var=app._adv_skip_verify_var,
        default=False,
        help_text=_('Skip the verify step after build. Faster, riskier.'))

    _setting_row(body, app,
        label=_('Exclude hidden files'),
        kind='toggle',
        var=app._adv_excl_hidden_var,
        default=False,
        help_text=_('Skip files marked hidden in the source folder.'))

    # Image size override — text input
    if not hasattr(app, '_adv_img_size_var'):
        app._adv_img_size_var = tk.StringVar(
            value=app._settings.get('adv_img_size_override', ''))
    _setting_row(body, app,
        label=_('Image size override (GB)'),
        kind='text',
        var=app._adv_img_size_var,
        default='',
        help_text=_('Blank = auto-size. Set e.g. 90 for a 90 GB image.'))


# ── ffpkg parameters ─────────────────────────────────────────────────
def _build_card_ffpkg(parent, app, row, col):
    body = _card_shell(parent, row, col,
        '\U0001f4e6', _('ffpkg parameters'),
        _('UFS2Tool newfs settings.'))

    _setting_row(body, app,
        label=_('ffpkg block size (-b)'),
        kind='select',
        var=app._adv_ffpkg_block_var,
        options=[('32768', '32K'), ('65536', '64K'), ('131072', '128K')],
        default='65536')

    _setting_row(body, app,
        label=_('ffpkg fragment (-f)'),
        kind='select',
        var=app._adv_ffpkg_frag_var,
        options=[('32768', '32K'), ('65536', '64K')],
        default='65536')

    # ffpkg sector — locked at 512
    _setting_row(body, app,
        label=_('ffpkg sector size (-S)'),
        kind='locked',
        var=None,
        value_text='512',
        help_text=_('Locked. 4096 produces broken images on Windows.'))

    _setting_row(body, app,
        label=_('Min Free % (-m)'),
        kind='int',
        var=app._adv_ffpkg_minfree_var,
        default='0',
        help_text=_('0 = use full space (PS5 default).'))

    _setting_row(body, app,
        label=_('Bytes / inode (-i)'),
        kind='select',
        var=app._adv_ffpkg_inode_var,
        options=[('131072', '128K'),
                 ('262144', '256K'),
                 ('524288', '512K')],
        default='262144')


# ─────────────────────────────────────────────────────────────────────
# SettingRow primitive — label + control + default indicator
# ─────────────────────────────────────────────────────────────────────
def _setting_row(parent, app, label, kind, var=None,
                 default=None, options=None, help_text=None, meta=None,
                 value_text=None, severity=None, on_change=None):
    """Build one setting row inside a card body.

    kind:
      'select'  — combobox with [(value, display)] options
      'int'     — text entry validated to integer
      'text'    — free-text entry
      'toggle'  — boolean toggle pill (ON/OFF flat label)
      'locked'  — read-only display with a lock pill
    severity: None or 'danger' — danger rows get a danger pill next to
      the label and warn-tinted help text.
    on_change: optional zero-arg callback invoked on every save. If
      provided, it's called INSTEAD of app._save_adv_params.
    """
    # Pick the save handler for this row
    if on_change is not None:
        _save_handler = on_change
    else:
        _save_handler = lambda: (
            app._save_adv_params()
            if hasattr(app, '_save_adv_params') else None)

    row = tk.Frame(parent, bg=COLORS['bg_2'])
    row.pack(fill='x', pady=(2, 0))
    sep = tk.Frame(parent, bg=COLORS['border_1'], height=1)
    sep.pack(fill='x', pady=(10, 10))

    # Column 1: label + help + meta
    lcol = tk.Frame(row, bg=COLORS['bg_2'])
    lcol.pack(side='left', fill='x', expand=True)
    # Label + optional danger pill
    label_row = tk.Frame(lcol, bg=COLORS['bg_2'])
    label_row.pack(anchor='w', fill='x')
    tk.Label(label_row, text=label,
             font=('Segoe UI', 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
             anchor='w').pack(side='left')
    if severity == 'danger':
        tk.Label(label_row, text=_('destructive'),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['danger_bg'], fg=COLORS['danger_hi'],
                 padx=6, pady=1,
                 highlightbackground=COLORS['danger'], highlightthickness=1
                 ).pack(side='left', padx=(8, 0))
    if help_text:
        help_fg = COLORS['danger_hi'] if severity == 'danger' else COLORS['fg_5']
        tk.Label(lcol, text=help_text,
                 font=(FONTS['mono_sm'][0], 9),
                 bg=COLORS['bg_2'], fg=help_fg,
                 anchor='w', justify='left', wraplength=340
                 ).pack(anchor='w', pady=(3, 0))
    if meta:
        tk.Label(lcol, text='\U0001f4a1  ' + meta,
                 font=(FONTS['mono_sm'][0], 9),
                 bg=COLORS['bg_2'], fg=COLORS['accent_hi'],
                 anchor='w').pack(anchor='w', pady=(2, 0))

    # Column 2: control
    ctrl_col = tk.Frame(row, bg=COLORS['bg_2'])
    ctrl_col.pack(side='left', padx=(12, 12))
    ctrl_widget = None

    if kind == 'select':
        # Light-on-dark Combobox (light field is a Windows-form holdover)
        display_var = tk.StringVar(value='')
        # Resolve current value to its display label
        cur = var.get() if var else default
        display_map = dict(options)
        display_var.set(display_map.get(cur, cur))
        values = [d for (_v, d) in options]
        ctrl_widget = ttk.Combobox(ctrl_col, textvariable=display_var,
                                    values=values, state='readonly',
                                    font=(FONTS['mono_sm'][0], 10), width=14)
        ctrl_widget.pack()
        # Sync display back to the real var
        def _on_pick(_e=None):
            sel = display_var.get()
            for v, d in options:
                if d == sel:
                    var.set(v)
                    break
            try:
                _save_handler()
            except Exception:
                pass
        ctrl_widget.bind('<<ComboboxSelected>>', _on_pick)

    elif kind == 'int' or kind == 'text':
        ef = tk.Frame(ctrl_col, bg=COLORS['field_bg'],
                      highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
        ef.pack()
        ctrl_widget = tk.Entry(ef, textvariable=var,
                 font=(FONTS['mono_sm'][0], 10),
                 bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                 insertbackground=COLORS['field_fg'],
                 relief='flat', bd=4, width=12)
        ctrl_widget.pack()
        def _entry_save(_e=None):
            try:
                _save_handler()
            except Exception:
                pass
        ctrl_widget.bind('<FocusOut>', _entry_save)
        ctrl_widget.bind('<Return>',   _entry_save)

    elif kind == 'toggle':
        # Status chip + flat ON/OFF pill (v3.6.0: every toggle row
        # shows its state in words next to the control)
        is_on = bool(var.get()) if var else False
        status_lbl = tk.Label(ctrl_col,
                              text=('\u2713 ' + _('Enabled')) if is_on
                              else ('\u25cb ' + _('Disabled')),
                              font=(FONTS['mono_sm'][0], 8, 'bold'),
                              bg=COLORS['success_bg'] if is_on
                              else COLORS['bg_3'],
                              fg=COLORS['success_hi'] if is_on
                              else COLORS['fg_5'],
                              padx=8, pady=2,
                              highlightbackground=(
                                  COLORS['success'] if is_on
                                  else COLORS['border_3']),
                              highlightthickness=1)
        status_lbl.pack(side='left', padx=(0, 10))
        pill = tk.Label(ctrl_col,
                        text='ON' if is_on else 'OFF',
                        bg=COLORS['success'] if is_on else COLORS['bg_5'],
                        fg='#000000' if is_on else COLORS['fg_5'],
                        font=(FONTS['mono_sm'][0], 9, 'bold'),
                        padx=10, pady=2,
                        width=3, anchor='center',
                        cursor='hand2')
        pill.pack(side='left')
        def _paint(on):
            pill.config(text='ON' if on else 'OFF',
                        bg=COLORS['success'] if on else COLORS['bg_5'],
                        fg='#000000' if on else COLORS['fg_5'])
            try:
                status_lbl.config(
                    text=('\u2713 ' + _('Enabled')) if on
                    else ('\u25cb ' + _('Disabled')),
                    bg=COLORS['success_bg'] if on else COLORS['bg_3'],
                    fg=COLORS['success_hi'] if on else COLORS['fg_5'],
                    highlightbackground=(COLORS['success'] if on
                                         else COLORS['border_3']))
            except Exception:
                pass
        def _flip(_e=None):
            new = not bool(var.get())
            var.set(new)
            _paint(new)
            try:
                _save_handler()
            except Exception:
                pass
        pill.bind('<Button-1>', _flip)
        if var is not None:
            try:
                var.trace_add('write',
                              lambda *a: _paint(bool(var.get())))
            except Exception:
                pass
        ctrl_widget = pill

    elif kind == 'locked':
        # Read-only field + 🔒 pill
        ef = tk.Frame(ctrl_col, bg=COLORS['field_bg'],
                      highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
        ef.pack(side='left')
        tk.Label(ef, text=value_text or '',
                 bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                 font=(FONTS['mono_sm'][0], 10),
                 padx=8, pady=3, width=8, anchor='w'
                 ).pack()
        lock_pill = tk.Label(ctrl_col, text='\U0001f512',
                              font=('Segoe UI', 10),
                              bg=COLORS['bg_2'], fg=COLORS['warn'])
        lock_pill.pack(side='left', padx=(6, 0))
        ctrl_widget = lock_pill

    # Column 3: default indicator (right-aligned)
    def_col = tk.Frame(row, bg=COLORS['bg_2'], width=120)
    def_col.pack(side='right')
    def_col.pack_propagate(False)
    if default is not None and var is not None:
        def _update_def_indicator(*_a):
            try:
                cur = var.get()
            except Exception:
                return
            cur_s = str(cur)
            def_s = str(default)
            for w in def_col.winfo_children():
                w.destroy()
            if cur_s == def_s:
                tk.Label(def_col,
                         text=_('default: ') + def_s,
                         font=(FONTS['mono_sm'][0], 9),
                         bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                         anchor='e').pack(side='right')
            else:
                # Off-default — clickable revert badge
                def_btn = tk.Label(def_col,
                    text='\u21bb  ' + _('default ') + def_s,
                    font=(FONTS['mono_sm'][0], 9, 'bold'),
                    bg=COLORS['bg_2'], fg=COLORS['warn_hi'],
                    cursor='hand2', anchor='e')
                def_btn.pack(side='right')
                def _revert(_e=None):
                    var.set(default)
                    try:
                        _save_handler()
                    except Exception:
                        pass
                    _update_def_indicator()
                def_btn.bind('<Button-1>', _revert)
        _update_def_indicator()
        try:
            var.trace_add('write', _update_def_indicator)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────
# Side rail — Recommended-for-your-setup + Why-these-defaults
# ─────────────────────────────────────────────────────────────────────
def _build_profile_card(parent, app):
    """Current Build Profile — live mirror of the five key params with
    per-row status dots and an overall Optimal / Custom / Risky pill.
    Green = matches the hardware recommendation, yellow = custom,
    red = the known-risky 4096 sector size. Read-only."""
    hw = getattr(app, '_adv_v2_hw', None)
    rec_threads = str(hw['recommended_threads']) if hw else None

    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x', pady=(0, 12))
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=14, pady=12)

    hrow = tk.Frame(inner, bg=COLORS['bg_2'])
    hrow.pack(fill='x')
    tk.Label(hrow, text=_('CURRENT BUILD PROFILE'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left')
    status_pill = tk.Label(hrow, text='',
                           font=(FONTS['mono_sm'][0], 8, 'bold'),
                           padx=8, pady=2)
    status_pill.pack(side='right')

    body = tk.Frame(inner, bg=COLORS['bg_2'])
    body.pack(fill='x', pady=(8, 0))

    specs = [
        ('cluster', _('Cluster Size'), app._adv_cluster_var),
        ('sector', _('Sector Size'), app._adv_sector_var),
        ('threads', _('Threads'), app._adv_threads_var),
        ('retries', _('Retry Count'), app._adv_retries_var),
        ('inode', _('Bytes / Inode'), app._adv_ffpkg_inode_var),
    ]
    rows = {}
    for key, label, var in specs:
        r = tk.Frame(body, bg=COLORS['bg_2'])
        r.pack(fill='x', pady=(0, 5))
        dot = tk.Label(r, text='\u25cf',
                       font=(FONTS['mono_sm'][0], 9),
                       bg=COLORS['bg_2'], fg=COLORS['fg_5'])
        dot.pack(side='left', padx=(0, 6))
        tk.Label(r, text=label, font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
                 ).pack(side='left')
        val = tk.Label(r, text='\u2014',
                       font=(FONTS['mono_sm'][0], 9, 'bold'),
                       bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='e')
        val.pack(side='right')
        rows[key] = (var, dot, val)

    def _judge(key, raw):
        low = (raw or '').strip().lower()
        if key == 'sector':
            if low == '4096':
                return 'risky'
            return 'ok' if low in ('512', '') else 'custom'
        if key == 'cluster':
            return 'ok' if ('auto' in low or '64' in low or not low) \
                else 'custom'
        if key == 'threads':
            if rec_threads is None:
                return 'neutral'
            return 'ok' if low == rec_threads else 'custom'
        return 'neutral'

    dotc = {'ok': COLORS['success_hi'], 'custom': COLORS['warn_hi'],
            'risky': COLORS['danger_hi'], 'neutral': COLORS['fg_5']}

    def _sync(*_a):
        try:
            worst = 'ok'
            for key, (var, dot, val) in rows.items():
                if not val.winfo_exists():
                    return
                raw = (var.get() or '').strip()
                val.config(text=raw or '\u2014')
                j = _judge(key, raw)
                dot.config(fg=dotc[j])
                if j == 'risky':
                    worst = 'risky'
                elif j == 'custom' and worst != 'risky':
                    worst = 'custom'
            if worst == 'risky':
                status_pill.config(text=_('RISKY'),
                                   bg=COLORS['danger_bg'],
                                   fg=COLORS['danger_hi'])
            elif worst == 'custom':
                status_pill.config(text=_('CUSTOM'),
                                   bg=COLORS['warn_bg'],
                                   fg=COLORS['warn_hi'])
            else:
                status_pill.config(text=_('OPTIMAL'),
                                   bg=COLORS['success_bg'],
                                   fg=COLORS['success_hi'])
        except Exception:
            pass

    for key, (var, _d, _v) in rows.items():
        try:
            var.trace_add('write', _sync)
        except Exception:
            pass
    _sync()


def _build_recco_card(parent, app):
    hw = app._adv_v2_hw
    card = tk.Frame(parent, bg=COLORS['accent_08'],
                    highlightbackground=COLORS['accent_lo'],
                    highlightthickness=1)
    card.pack(fill='x', pady=(0, 12))
    inner = tk.Frame(card, bg=COLORS['accent_08'])
    inner.pack(fill='x', padx=14, pady=12)

    # Header
    head = tk.Frame(inner, bg=COLORS['accent_08'])
    head.pack(fill='x')
    tk.Label(head, text='\U0001f4a1',
             font=('Segoe UI', 14),
             bg=COLORS['accent_08'], fg=COLORS['accent_hi']
             ).pack(side='left', padx=(0, 8))
    head_col = tk.Frame(head, bg=COLORS['accent_08'])
    head_col.pack(side='left', fill='x', expand=True)
    tk.Label(head_col, text=_('RECOMMENDED FOR YOUR SETUP'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent']
             ).pack(anchor='w')
    sub_text = _('Detected: %s \u00b7 %s') % (
        hw['model'], hw['kind']) if hw else _('HW detection failed')
    tk.Label(head_col, text=sub_text,
             font=(FONTS['mono_sm'][0], 9),
             bg=COLORS['accent_08'], fg=COLORS['fg_4'],
             wraplength=320, justify='left'
             ).pack(anchor='w', pady=(2, 0))

    # v3.6.0: compact HW stat cells under the header
    if hw:
        perf_map = {'NVMe Gen4 SSD': (_('Excellent'),
                                      COLORS['success_hi']),
                    'SATA SSD': (_('Good'), COLORS['teal']),
                    'HDD': (_('Limited'), COLORS['warn_hi'])}
        perf, perf_fg = perf_map.get(hw.get('kind', ''),
                                     (_('Unknown'), COLORS['fg_4']))
        statrow = tk.Frame(inner, bg=COLORS['accent_08'])
        statrow.pack(fill='x', pady=(10, 2))
        for i in range(2):
            statrow.grid_columnconfigure(i, weight=1, uniform='hwst')

        def _hw_cell(r, c, cap, val, fg):
            cell = tk.Frame(statrow, bg=COLORS['bg_2'],
                            highlightbackground=COLORS['border_2'],
                            highlightthickness=1)
            cell.grid(row=r, column=c, sticky='ew',
                      padx=(0 if c == 0 else 6, 0),
                      pady=(0 if r == 0 else 6, 0))
            tk.Label(cell, text=cap.upper(),
                     font=(FONTS['mono_sm'][0], 7, 'bold'),
                     bg=COLORS['bg_2'], fg=COLORS['fg_5']
                     ).pack(anchor='w', padx=8, pady=(5, 0))
            tk.Label(cell, text=val,
                     font=(FONTS['mono_sm'][0], 10, 'bold'),
                     bg=COLORS['bg_2'], fg=fg, anchor='w'
                     ).pack(fill='x', padx=8, pady=(0, 5))

        _hw_cell(0, 0, _('Threads'),
                 str(hw.get('recommended_threads', '\u2014')),
                 COLORS['accent_hi'])
        _hw_cell(0, 1, _('Sector'), '512', COLORS['fg_1'])
        _hw_cell(1, 0, _('Performance'), perf, perf_fg)
        _hw_cell(1, 1, _('Health'), _('Optimal'), COLORS['success_hi'])

    if hw:
        # Detail list
        details = tk.Frame(inner, bg=COLORS['accent_08'])
        details.pack(fill='x', pady=(10, 10))
        cur_threads = app._adv_threads_var.get()
        _recco_row(details, 'THREADS', str(hw['recommended_threads']),
                   cur_threads, str(hw['recommended_threads']))
        _recco_row(details, 'CLUSTER', _('Auto (64K)'),
                   app._adv_cluster_var.get(), 'Auto')
        _recco_row(details, 'SECTOR', '512',
                   app._adv_sector_var.get(), '512')
        bench_val = app._settings.get('adv_write_benchmark', '\u2014')
        _recco_row(details, 'WRITE SPEED', bench_val, None, None)

        # CTA button
        def _apply_reco():
            app._adv_threads_var.set(str(hw['recommended_threads']))
            try:
                app._save_adv_params()
            except Exception:
                pass
            try:
                app._adv_clamp_threads()
            except Exception:
                pass
        _accent_btn(inner,
            '\u26a1  ' + _('Apply recommended'),
            command=_apply_reco
            ).pack(fill='x', pady=(0, 0))


def _recco_row(parent, eyebrow, recommended_val, current_val, matches_val):
    row = tk.Frame(parent, bg=COLORS['accent_08'])
    row.pack(fill='x', pady=(0, 6))
    tk.Label(row, text=eyebrow,
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['fg_5'],
             width=10, anchor='w').pack(side='left')
    tk.Label(row, text=str(recommended_val),
             font=(FONTS['mono_sm'][0], 10, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['fg_1'],
             anchor='w').pack(side='left')
    if current_val is not None and matches_val is not None:
        if str(current_val) == str(matches_val):
            tk.Label(row, text='\u2713  ' + _('matches'),
                     font=(FONTS['mono_sm'][0], 9),
                     bg=COLORS['accent_08'], fg=COLORS['success_hi']
                     ).pack(side='left', padx=(8, 0))
        else:
            tk.Label(row, text=_('currently ') + str(current_val),
                     font=(FONTS['mono_sm'][0], 9, 'bold'),
                     bg=COLORS['warn_bg'], fg=COLORS['warn'],
                     padx=6, pady=1
                     ).pack(side='left', padx=(8, 0))


def _build_help_card(parent, app):
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x')
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=14, pady=12)
    tk.Label(inner, text=_('WHY THESE DEFAULTS?'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(anchor='w')
    text1 = _(
        'PS5 expects 64K clusters and 512-byte sectors \u2014 anything '
        'else needs config.ini edits or the image won\'t mount.')
    tk.Label(inner, text=text1,
             font=(FONTS['mono_sm'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=300
             ).pack(anchor='w', pady=(6, 4))
    text2 = _('More threads = faster but more fragmentation.')
    tk.Label(inner, text=text2,
             font=(FONTS['mono_sm'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=300
             ).pack(anchor='w')


# ─────────────────────────────────────────────────────────────────────
# Toolbar actions: Reset all + Save parameters + Run benchmark
# ─────────────────────────────────────────────────────────────────────
def _reset_all(app):
    from tkinter import messagebox
    if not messagebox.askyesno('Reset all',
            'Restore every Advanced setting to its default?'):
        return
    defaults = {
        '_adv_cluster_var':       'Auto',
        '_adv_sector_var':        '512',
        '_adv_threads_var':       '1',
        '_adv_retries_var':       '3',
        '_adv_retry_wait_var':    '3',
        '_adv_skip_verify_var':   False,
        '_adv_excl_hidden_var':   False,
        '_adv_img_size_var':      '',
        '_adv_ffpkg_block_var':   '65536',
        '_adv_ffpkg_frag_var':    '65536',
        '_adv_ffpkg_minfree_var': '0',
        '_adv_ffpkg_inode_var':   '262144',
    }
    for attr, val in defaults.items():
        v = getattr(app, attr, None)
        if v is not None:
            try:
                v.set(val)
            except Exception:
                pass
    try:
        app._save_adv_params()
    except Exception:
        pass


def _save_params(app):
    try:
        app._save_adv_params()
        app._set_status(_('Saved.'), COLORS['success'])
    except Exception as e:
        from tkinter import messagebox
        messagebox.showerror('Save failed', str(e))


def _run_benchmark(app, bench_pill):
    """Hook into the existing _run_write_benchmark callback if available;
    otherwise show a placeholder. Updates the pill in place."""
    fn = getattr(app, '_run_write_benchmark', None)
    if fn is None:
        bench_pill.config(text='(no benchmark)')
        return
    try:
        fn()
        # The callback should update app._settings['adv_write_benchmark'];
        # poll it back into the pill
        app.after(500, lambda: bench_pill.config(
            text=app._settings.get('adv_write_benchmark', '\u2014')))
    except Exception as e:
        bench_pill.config(text='(error: %s)' % str(e)[:24])


# ─────────────────────────────────────────────────────────────────────
# Button helpers
# ─────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9),
                     bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                     activebackground=COLORS['bg_5'],
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
                     padx=12, pady=6,
                     cursor='hand2',
                     command=command)


def _danger_btn(parent, text, command):
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9, 'bold'),
                     bg=COLORS['danger'], fg=COLORS['fg_0'],
                     activebackground=COLORS['danger_hi'],
                     activeforeground=COLORS['fg_0'],
                     disabledforeground=COLORS['fg_5'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     command=command)
