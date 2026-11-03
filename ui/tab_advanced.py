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
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.scroll import wheel_units
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from ui.shared.app_dialog import (
    app_confirm, app_error, app_info, app_question, app_warning,
)
from exfat_builder import _, save_settings, SubTabBar, Tooltip
from ui.shared.scroll import attach_scroll


# ─────────────────────────────────────────────────────────────────────
# Hardware detection (for Recommended-for-your-setup card)
# ─────────────────────────────────────────────────────────────────────
def _detect_hw():
    """Returns a dict {model, kind, recommended_threads} or None.

    recommended_threads is always 1: multi-threaded robocopy (/MT)
    fragments the output image, which hurts it once mounted on the
    PS5. Single-threaded is the right call regardless of how fast
    the source drive is. The detected model/kind is still used for
    display (device name, performance/health labels).

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
                        'recommended_threads': 1}
            if 'ssd' in lm or 'solid' in lm or 'sata' in lb:
                return {'model': model, 'kind': 'SATA SSD',
                        'recommended_threads': 1}
            if 'hdd' in lm:
                return {'model': model, 'kind': 'HDD',
                        'recommended_threads': 1}
            # Heuristic fallback from model name
            if any(x in lname for x in (
                    'sn850', 'sn770', '980 pro', '990 pro', 'rocket',
                    'firecuda 530', 'fireblade', 'samsung 970',
                    'samsung 980', 'samsung 990', 'kingston kc', 'wd_black')):
                return {'model': model, 'kind': 'NVMe Gen4 SSD',
                        'recommended_threads': 1}
            if any(x in lname for x in ('ssd', 'crucial mx', 'samsung 870')):
                return {'model': model, 'kind': 'SATA SSD',
                        'recommended_threads': 1}
            return {'model': model, 'kind': 'Unknown drive',
                    'recommended_threads': 1}
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
                    'recommended_threads': 1}
        if 'ssd' in lm or 'solid' in (media or '').lower():
            return {'model': model, 'kind': 'SATA SSD',
                    'recommended_threads': 1}
        if 'hdd' in (media or '').lower() or 'fixed hard disk' in (media or '').lower():
            return {'model': model, 'kind': 'HDD',
                    'recommended_threads': 1}
        return {'model': model, 'kind': 'Unknown drive',
                'recommended_threads': 1}
    except Exception:
        return None


# ─────────────────────────────────────────────────────────────────────
# Main entry
# ─────────────────────────────────────────────────────────────────────
def build_advanced_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State for dirty tracking — keyed by sub-tab id
    app._adv_v2_dirty = {'params': set(), 'post': set(), 'lang': set()}
    # v3.6.3 STARTUP FIX: _detect_hw() shells out to PowerShell
    # (Get-PhysicalDisk) which costs ~1.5s of cold-start — it was the single
    # biggest chunk of app launch time (advanced ≈ 1685ms). It's only used to
    # DISPLAY a drive model + a recommended-threads hint, so it must not block
    # startup. Detect in the background: render the card immediately with a
    # "detecting" placeholder, then rebuild the params pane once the result
    # arrives. Cached at the app level so it only ever runs once per session.
    if getattr(app, '_adv_v2_hw_cached', None) is not None:
        # Already detected earlier this session — reuse, no thread.
        app._adv_v2_hw = app._adv_v2_hw_cached
    else:
        app._adv_v2_hw = None  # card renders a "detecting…" state meanwhile

        def _detect_hw_async():
            try:
                result = _detect_hw()
            except Exception:
                result = None

            def _apply():
                try:
                    app._adv_v2_hw_cached = result if result is not None \
                        else {'_failed': True}
                    app._adv_v2_hw = result
                    # Rebuild just the params pane so the recco card now shows
                    # real hardware. Guard: pane may be gone if tab rebuilt.
                    pane = (getattr(app, '_adv_v2_panes', {}) or {}).get(
                        'params')
                    if pane is not None and pane.winfo_exists():
                        for w in pane.winfo_children():
                            w.destroy()
                        _build_params_pane(_scroll_host(pane), app)
                except Exception:
                    pass
            try:
                app.after(0, _apply)
            except Exception:
                pass

        import threading as _thr
        _thr.Thread(target=_detect_hw_async, daemon=True).start()

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
    for key in ('params', 'post', 'lang', 'ampr', 'dlc'):
        pane = tk.Frame(body, bg=COLORS['bg_1'])
        app._adv_v2_panes[key] = pane

    _build_params_pane(_scroll_host(app._adv_v2_panes['params']), app)
    _build_post_pane(_scroll_host(app._adv_v2_panes['post']), app)
    _build_lang_pane(_scroll_host(app._adv_v2_panes['lang']), app)
    _build_ampr_pane(_scroll_host(app._adv_v2_panes['ampr']), app)
    _build_dlc_pane(_scroll_host(app._adv_v2_panes['dlc']), app)

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
             font=(FONTS['body'][0], 14)
             ).pack(expand=True)
    title_col = tk.Frame(left, bg=COLORS['bg_1'])
    title_col.pack(side='left', padx=(10, 0))
    tk.Label(title_col, text=_('Advanced'),
             font=(FONTS['body_strong'][0], 16),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Power-user configuration for image creation, '
                    'performance tuning and automation.'),
             font=(FONTS['body'][0], 10),
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
        ('ampr',   '\U0001f5c2', _('AMPR Index')),
        ('dlc',    '\U0001f5c3', _('DLC-emu')),
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
                  font=(FONTS['body'][0], 11),
                  bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    ic.pack(side='left', padx=(0, 6))
    lbl = tk.Label(inner, text=label,
                   font=(FONTS['body'][0], 10),
                   bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    lbl.pack(side='left')
    # Counter badge (hidden by default)
    count = tk.Label(inner, text='',
                     font=(FONTS['body_strong'][0], 9),
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
        c.yview_scroll(wheel_units(c, e.delta), 'units')
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
    attach_scroll(cv)
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
             font=(FONTS['body_strong'][0], 9),
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
             font=(FONTS['body_strong'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(anchor='w', pady=(0, 6))
    flow = tk.Frame(flow_wrap, bg=COLORS['bg_2'])
    flow.pack(anchor='w')
    steps = {}

    def _chip(key, icon, label):
        c = tk.Label(flow, text=icon + '  ' + label,
                     font=(FONTS['body_strong'][0], 9),
                     bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                     padx=9, pady=4,
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
        c.pack(side='left')
        steps[key] = c
        return c

    def _arrow():
        tk.Label(flow, text='\u2192',
                 font=(FONTS['body_strong'][0], 11),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(side='left', padx=6)

    _chip('build', '\U0001f528', _('Build'))
    _arrow()
    _chip('verify', '\U0001f50d', _('Verify'))
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
    tk.Label(hpad, text=icon, font=(FONTS['body'][0], 11),
             bg=hbg, fg=accent).pack(side='left', padx=(0, 8))
    tcol = tk.Frame(hpad, bg=hbg)
    tcol.pack(side='left', fill='x', expand=True)
    trow = tk.Frame(tcol, bg=hbg)
    trow.pack(anchor='w', fill='x')
    tk.Label(trow, text=title, font=(FONTS['body_strong'][0], 10),
             bg=hbg, fg=COLORS['danger_hi'] if danger
             else COLORS['fg_0'], anchor='w').pack(side='left')
    if danger:
        tk.Label(trow, text=_('DESTRUCTIVE'),
                 font=(FONTS['body_strong'][0], 9),
                 bg=COLORS['danger'], fg='#000000',
                 padx=6, pady=1).pack(side='left', padx=(8, 0))
    tk.Label(tcol, text=desc, font=FONTS['mono_sm'],
             bg=hbg, fg=COLORS['fg_5'], anchor='w',
             wraplength=420, justify='left').pack(anchor='w')

    pill = tk.Label(hpad, text='OFF',
                    font=(FONTS['body_strong'][0], 9),
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
    dashboard — Upload / Verification / Image Output cards
    in a grid with live state pills, and a full-width Danger Zone
    card for the destructive delete-source toggles. (The Logging card
    was removed in v3.6.4 — its toggle wrote a setting no build path
    consumed.) Same six
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

    # ── Image Output ──
    # (The former "Logging" card here was removed in v3.6.4: the
    # "Save robocopy log" toggle wrote a setting that no build path ever
    # consumed — no /LOG flag was ever passed to robocopy — so it was a
    # control that did nothing. The backing var is left defined elsewhere
    # for settings-compat; only the dead UI is gone.)
    b = _post_card(parent, 2, 0, '\U0001f4bf', _('Image Output'),
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


def _build_ampr_pane(parent, app):
    """AMPR Index Builder (issue #43): a thin UI wrapper around the existing
    ui.build_ampr_index engine (build_index_local / build_index_ftp). Nothing
    new is computed here — it exposes the tool that already runs
    automatically during a Build (gated on fakelib/libSceAmpr.sprx) so users
    can generate ampr_emu.index for an existing dump folder, or push one to a
    connected PS5 over FTP. No build, format, or engine changes."""
    import threading as _thr
    from tkinter import filedialog

    parent.columnconfigure(0, weight=1)
    parent.rowconfigure(0, weight=1)

    card_outer = tk.Frame(parent, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, 12))

    head = tk.Frame(card_outer, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=14, pady=(12, 4))
    tk.Label(head, text='\U0001f5c2',
             font=(FONTS['body'][0], 14),
             bg=COLORS['bg_2'], fg=COLORS['purple']
             ).pack(side='left', padx=(0, 8))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=_('AMPR Index Builder'),
             font=(FONTS['body_strong'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Generate ampr_emu.index for an APR title — for an '
                    'existing dump folder, or a connected PS5. Builds run '
                    'automatically too (when fakelib/libSceAmpr.sprx is '
                    'present); this is the manual entry point.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w',
             justify='left', wraplength=720
             ).pack(anchor='w')

    body = tk.Frame(card_outer, bg=COLORS['bg_2'])
    body.pack(fill='both', expand=True, padx=14, pady=(0, 12))

    # Stat cells: files indexed / index size / output
    top = tk.Frame(body, bg=COLORS['bg_2'])
    top.pack(fill='x', pady=(4, 8))
    for i in range(3):
        top.grid_columnconfigure(i, weight=1, uniform='amprtop')

    def _cell(col, caption, fg):
        cell = tk.Frame(top, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        cell.grid(row=0, column=col, sticky='ew',
                  padx=(0 if col == 0 else 10, 0))
        tk.Label(cell, text=caption.upper(),
                 font=(FONTS['body_strong'][0], 9),
                 bg=COLORS['bg_3'], fg=COLORS['fg_5']
                 ).pack(anchor='w', padx=12, pady=(9, 0))
        v = tk.Label(cell, text='—',
                     font=(FONTS['body_strong'][0], 11),
                     bg=COLORS['bg_3'], fg=fg, anchor='w')
        v.pack(fill='x', padx=12, pady=(2, 9))
        return v

    c_files = _cell(0, _('Files Indexed'), COLORS['purple_hi'])
    c_size = _cell(1, _('Index Size'), COLORS['accent_hi'])
    c_out = _cell(2, _('Output'), COLORS['fg_1'])

    status_var = tk.StringVar(value=_('Idle.'))
    tk.Label(body, textvariable=status_var,
             font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=720
             ).pack(fill='x', pady=(2, 8))

    busy = {'on': False}

    def _set_busy(on):
        busy['on'] = on
        _state = 'disabled' if on else 'normal'
        for b in (btn_folder, btn_ps5):
            try:
                b.configure(state=_state)
            except Exception:
                pass

    def _reset_cells():
        for v in (c_files, c_size, c_out):
            v.configure(text='—')

    def _fmt_bytes(n):
        try:
            n = int(n)
        except Exception:
            return '—'
        if n >= 1048576:
            return '%.1f MB' % (n / 1048576.0)
        if n >= 1024:
            return '%.0f KB' % (n / 1024.0)
        return '%d B' % n

    def _generate_from_folder():
        if busy['on']:
            return
        folder = filedialog.askdirectory(
            title=_('Select the app0 / dump folder to index'))
        if not folder:
            return
        # Gate on the same marker the automatic build path uses.
        sprx = os.path.join(folder, 'fakelib', 'libSceAmpr.sprx')
        if not os.path.isfile(sprx):
            if not app_confirm(app,
                    _('Not an APR title?'),
                    _('This folder has no fakelib/libSceAmpr.sprx, so it does '
                      'not appear to be an APR title.\n\n'
                      'Generate ampr_emu.index anyway?')):
                return

        _reset_cells()
        _set_busy(True)
        status_var.set(_('Generating ampr_emu.index in %s…') % folder)

        def _progress(msg):
            # Called from the worker thread by the engine — marshal to the
            # UI thread.
            try:
                app.after(0, lambda m=msg: status_var.set(m))
            except Exception:
                pass

        def _work():
            from pathlib import Path
            import time as _time
            out = Path(folder) / 'ampr_emu.index'
            rc = 1
            err = ''
            stats = {}
            t0 = _time.monotonic()
            try:
                from ui.build_ampr_index import build_index_local
                rc = build_index_local(Path(folder), out, True,
                                       progress=_progress, stats_out=stats)
            except Exception as e:
                err = str(e)

            def _done():
                _set_busy(False)
                if rc == 0 and out.is_file():
                    try:
                        sz = out.stat().st_size
                    except Exception:
                        sz = 0
                    _el = int(round(_time.monotonic() - t0))
                    _el_str = '%dm %02ds' % (_el // 60, _el % 60) if _el >= 60 else '%ds' % _el
                    _folders = stats.get('folders')
                    _files = stats.get('files')
                    c_size.configure(text=_fmt_bytes(sz))
                    c_out.configure(text=str(out))
                    status_var.set(
                        _('Complete — %s files indexed in %s.')
                        % (_files if _files is not None else '?', _el_str))
                    try:
                        app._log('[AMPR] Manual index written: %s '
                                 '(%s folders, %s files, %d bytes, %s)\n'
                                 % (out, _folders, _files, sz, _el_str))
                    except Exception:
                        pass
                    app_info(app,
                        _('AMPR index created'),
                        _('Wrote ampr_emu.index.\n\n'
                          'Folders indexed:  %s\n'
                          'Files indexed:    %s\n'
                          'Elapsed:          %s\n'
                          'Output:           %s')
                        % (_folders if _folders is not None else '—',
                           _files if _files is not None else '—',
                           _el_str, out))
                else:
                    status_var.set(_('Index generation failed (rc=%s)%s.')
                                   % (rc, (' — ' + err) if err else ''))
                    app_error(app,
                        _('AMPR index failed'),
                        _('Could not generate the index (rc=%s).%s')
                        % (rc, ('\n\n' + err) if err else ''))
            app.after(0, _done)

        _thr.Thread(target=_work, daemon=True).start()

    def _generate_from_ps5():
        if busy['on']:
            return
        ip = ''
        port = '2121'
        try:
            ip = (app._ftp_ip_var.get() or '').strip()
            port = (app._ftp_port_var.get() or '2121').strip()
        except Exception:
            pass
        if not ip:
            app_warning(app,
                _('No PS5 connection'),
                _('Set the PS5 FTP IP in the PS5 tab first.'))
            return

        # Start the picker where the user already is: the PS5 Browser's
        # current folder if it has one, else the configured FTP path.
        # The picker itself (app._ampr_pick_ps5_folder) opens its own
        # short-lived FTP connection and validates the choice is a real
        # app0 folder (eboot.bin / sce_sys present) before returning.
        default_path = '/'
        try:
            _br = getattr(app, '_br_path', None)
            _brp = _br.get().strip() if _br is not None else ''
            if _brp and _brp != '/':
                default_path = _brp
            else:
                default_path = (app._ftp_path_var.get() or '/').strip() or '/'
        except Exception:
            default_path = '/'

        # Modal picker — returns the validated app0 path or None. All the
        # browsing happens one level at a time; nothing recursive runs
        # until the user confirms below.
        game_path = app._ampr_pick_ps5_folder(default_path)
        if not game_path:
            return
        game_path = game_path.strip()

        if not app_confirm(app,
                _('Generate and upload?'),
                _('Build ampr_emu.index from:\n  ftp://%s:%s%s\n\n'
                  'and upload it to that folder on the PS5?')
                % (ip, port, game_path)):
            return

        _reset_cells()
        _set_busy(True)
        status_var.set(_('Indexing PS5 over FTP…'))

        def _progress(msg):
            # Engine progress callback — arrives on the worker thread. The
            # engine reports every N files / folders so a long walk over a
            # wrong (too broad) root is visible rather than a silent hang;
            # the app0 check above is what prevents that walk in the first
            # place.
            try:
                app.after(0, lambda m=msg: status_var.set(m))
            except Exception:
                pass

        def _probe_app0(host, prt, path):
            """Shallow one-level FTP listing of `path`. Returns
            (is_app0, n_child_dirs, listed, error). `is_app0` is True when the
            folder directly contains eboot.bin or sce_sys. Never raises — used
            to confirm a game app0 folder was chosen before the recursive walk,
            so a broad parent (e.g. /data/etaHEN/games) is not scanned whole."""
            import ftplib as _ftplib
            f = None
            names = set()
            n_dirs = 0
            listed = False
            try:
                f = _ftplib.FTP()
                f.connect(str(host), int(prt), timeout=15)
                f.login()
                try:
                    for nm, facts in f.mlsd(path):
                        if nm in ('.', '..'):
                            continue
                        listed = True
                        names.add(nm.lower())
                        if (facts.get('type', '') or '').lower() == 'dir':
                            n_dirs += 1
                except _ftplib.all_errors:
                    # No MLSD — fall back to a plain NLST (names only).
                    try:
                        for r in f.nlst(path):
                            nm = r.rsplit('/', 1)[-1]
                            if nm in ('', '.', '..'):
                                continue
                            listed = True
                            names.add(nm.lower())
                    except _ftplib.all_errors:
                        pass
                is_app0 = 'eboot.bin' in names or 'sce_sys' in names
                return is_app0, n_dirs, listed, ''
            except Exception as _e:
                return False, 0, listed, str(_e)
            finally:
                if f is not None:
                    try:
                        f.quit()
                    except Exception:
                        try:
                            f.close()
                        except Exception:
                            pass

        def _work():
            from pathlib import Path
            import tempfile as _tf
            import time as _time
            rc = 1
            err = ''
            stats = {}
            t0 = _time.monotonic()
            out = Path(_tf.gettempdir()) / 'ampr_emu.index'

            # Defence in depth: the picker already validated the folder, but
            # re-check right before the recursive walk (the user may have
            # typed a path, or the console contents may have changed) so a
            # broad parent folder is never indexed by mistake. This is one
            # shallow listing — not the recursive walk.
            _progress(_('Checking folder…'))
            _is_app0, _n_dirs, _listed, _verr = _probe_app0(ip, port, game_path)
            if not _is_app0:
                def _bad():
                    _set_busy(False)
                    if not _listed:
                        status_var.set(
                            _('Could not read %s on the PS5.') % game_path)
                        app_error(app,
                            _('Cannot read PS5 folder'),
                            _('Could not list this folder on the PS5:\n  %s\n\n%s')
                            % (game_path, _verr or _('The folder was empty or unreadable.')))
                        return
                    status_var.set(
                        _('Not a game app0 folder — nothing indexed.'))
                    app_error(app,
                        _('Not a game app0 folder'),
                        _('This looks like a parent folder, not a game app0 '
                          'folder. Open the game folder first.\n\n'
                          'No eboot.bin or sce_sys was found in:\n  %s\n\n'
                          '%d subfolder(s) present. Browse into the specific '
                          'game first, then try again.')
                        % (game_path, _n_dirs))
                app.after(0, _bad)
                return
            try:
                from ui.build_ampr_index import build_index_ftp
                root_url = f'ftp://{ip!s}:{port!s}{game_path!s}'
                rc = build_index_ftp(root_url, out, True, True,
                                     progress=_progress, stats_out=stats)
            except Exception as e:
                err = str(e)

            def _done():
                _set_busy(False)
                if rc == 0:
                    try:
                        sz = out.stat().st_size if out.is_file() else 0
                    except Exception:
                        sz = 0
                    _el = int(round(_time.monotonic() - t0))
                    _el_str = '%dm %02ds' % (_el // 60, _el % 60) if _el >= 60 else '%ds' % _el
                    _folders = stats.get('folders')
                    _files = stats.get('files')
                    c_size.configure(text=_fmt_bytes(sz))
                    c_out.configure(text=_('PS5: %s') % game_path)
                    status_var.set(
                        _('Complete — %s files indexed in %s.')
                        % (_files if _files is not None else '?', _el_str))
                    try:
                        app._log(f'[AMPR] Index uploaded to PS5 '
                                 f'{ip!s}{game_path!s} ({_folders!s} folders, '
                                 f'{_files!s} files, {_el_str!s})\n')
                    except Exception:
                        pass
                    app_info(app,
                        _('AMPR index uploaded'),
                        _('Uploaded ampr_emu.index to the PS5.\n\n'
                          'Folders indexed:  %s\n'
                          'Files indexed:    %s\n'
                          'Elapsed:          %s\n'
                          'Output:           ftp://%s:%s%s')
                        % (_folders if _folders is not None else '—',
                           _files if _files is not None else '—',
                           _el_str, ip, port, game_path))
                else:
                    status_var.set(_('PS5 index failed (rc=%s)%s.')
                                   % (rc, (' — ' + err) if err else ''))
                    app_error(app,
                        _('AMPR index failed'),
                        _('Could not index the PS5 (rc=%s).%s')
                        % (rc, ('\n\n' + err) if err else ''))
            app.after(0, _done)

        _thr.Thread(target=_work, daemon=True).start()

    btn_row = tk.Frame(body, bg=COLORS['bg_2'])
    btn_row.pack(fill='x', pady=(2, 2))
    btn_folder = XButton(btn_row, text='\U0001f4c1  ' + _('Generate From Folder'),
                           command=_generate_from_folder,
                           font=(FONTS['body'][0], 10),
                           bg=COLORS['accent'], fg='#ffffff',
                           activebackground=COLORS['accent'],
                           activeforeground='#ffffff',
                           relief='flat', bd=0, padx=16, pady=8,
                           cursor='hand2', highlightthickness=0)
    btn_folder.pack(side='left', padx=(0, 8))
    btn_ps5 = XButton(btn_row,
                        text='\U0001f4e1  ' + _('Generate From Connected PS5'),
                        command=_generate_from_ps5,
                        font=(FONTS['body'][0], 10),
                        bg=COLORS['bg_3'], fg=COLORS['fg_1'],
                        activebackground=COLORS['bg_4'],
                        activeforeground=COLORS['fg_0'],
                        relief='flat', bd=0, padx=16, pady=8,
                        cursor='hand2', highlightthickness=1,
                        highlightbackground=COLORS['border_2'])
    btn_ps5.pack(side='left')


def _build_dlc_pane(parent, app):
    """DLC-emu INI builder/editor (Phase 1). A thin Advanced tool that
    creates/edits /app0/dlc_emu.ini for a chosen app0 folder. Models the file
    as an ORDERED ENTRY LIST (repeated [SECTION] headers) via ui.dlc_emu_ini
    — never configparser. Phase 1 writes only into the app0 folder (same
    proven path as the AMPR tool); it never edits images or auto-injects
    anything. Module copy is a separate, explicit, confirmed action that
    copies from a user-selected dlc_emu package (modules are NOT bundled)."""
    import threading as _thr
    import os as _os
    from tkinter import filedialog, simpledialog
    from ui import dlc_emu_ini as _D
    parent.columnconfigure(0, weight=1)
    parent.rowconfigure(0, weight=1)
    card_outer = tk.Frame(parent, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, 12))
    head = tk.Frame(card_outer, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=14, pady=(12, 4))
    tk.Label(head, text='🗃', font=(FONTS['body'][0], 14), bg=COLORS['bg_2'], fg=COLORS['purple']).pack(side='left', padx=(0, 8))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=_('DLC-emu'), font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(anchor='w')
    tk.Label(title_col, text=_('Build / edit /app0/dlc_emu.ini for a game dump folder. Phase 1 writes only into the app0 folder (packed by your normal build); it never edits images. PSAC is the recommended entry type for most add-on content; every other type stays available for special cases.'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w', justify='left', wraplength=760).pack(anchor='w')
    tk.Label(head, text=_('PHASE 1'), font=(FONTS['body_strong'][0], 9), bg=COLORS['warn_bg'], fg=COLORS['warn'], padx=8, pady=2, highlightbackground=COLORS['warn'], highlightthickness=1).pack(side='right')
    body = tk.Frame(card_outer, bg=COLORS['bg_2'])
    body.pack(fill='both', expand=True, padx=14, pady=(0, 12))
    state = {'entries': [], 'app0': ''}
    folder_var = tk.StringVar(value='')
    try:
        _prefill = ''
        for _attr in '_sdkfix_src_var', '_dumps_src_var':
            try:
                _v = (getattr(app, _attr).get() or '').strip()
            except Exception:
                _v = ''
            if _v and _os.path.isdir(_v):
                _prefill = _v
                break
        if _prefill:
            folder_var.set(_prefill)
    except Exception:
        pass
    row1 = tk.Frame(body, bg=COLORS['bg_2'])
    row1.pack(fill='x', pady=(4, 4))
    tk.Label(row1, text=_('app0 folder:'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left', padx=(0, 6))
    folder_entry = XEntry(row1, textvariable=folder_var, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
    folder_entry.pack(side='left', fill='x', expand=True, padx=(0, 6))
    folder_status = tk.StringVar(value='')
    tk.Label(body, textvariable=folder_status, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(fill='x', pady=(0, 6))
    strip = tk.Frame(body, bg=COLORS['bg_2'])
    strip.pack(fill='x', pady=(0, 6))
    count_var = tk.StringVar(value=_('Entries: 0 / %d') % _D.MAX_ENTRIES)
    size_var = tk.StringVar(value=_('Size: 0 B / 32 KiB'))
    tk.Label(strip, textvariable=count_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['accent_hi']).pack(side='left', padx=(0, 18))
    tk.Label(strip, textvariable=size_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['purple_hi']).pack(side='left')
    list_wrap = tk.Frame(body, bg=COLORS['bg_1'], highlightbackground=COLORS['border_2'], highlightthickness=1)
    list_wrap.pack(fill='both', expand=True, pady=(0, 6))
    list_canvas = tk.Canvas(list_wrap, bg=COLORS['bg_1'], highlightthickness=0, height=240)
    list_sb = XScrollbar(list_wrap, command=list_canvas.yview, bg=COLORS['bg_2'])
    list_canvas.configure(yscrollcommand=list_sb.set)
    list_frame = tk.Frame(list_canvas, bg=COLORS['bg_1'])
    _win = list_canvas.create_window((0, 0), window=list_frame, anchor='nw')
    list_frame.bind('<Configure>', lambda e: (list_canvas.configure(scrollregion=list_canvas.bbox('all')), list_canvas.itemconfig(_win, width=list_canvas.winfo_width())))
    list_canvas.bind('<Configure>', lambda e: list_canvas.itemconfig(_win, width=e.width))
    def _dlc_wheel(e, _c=list_canvas):
        _c.yview_scroll(wheel_units(_c, e.delta), 'units')
    list_canvas.bind('<Enter>', lambda _e, _c=list_canvas: _c.bind_all('<MouseWheel>', _dlc_wheel))
    list_canvas.bind('<Leave>', lambda _e, _c=list_canvas: _c.unbind_all('<MouseWheel>'))
    list_sb.pack(side='right', fill='y')
    list_canvas.pack(side='left', fill='both', expand=True)
    valid_var = tk.StringVar(value='')
    valid_lbl = tk.Label(body, textvariable=valid_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w', justify='left', wraplength=760)
    valid_lbl.pack(fill='x', pady=(0, 6))
    _ENTRY_FIELDS = [*('content_id', 'label', 'mount_point', 'entitlement_key', 'service_label', 'download_status')]
    def _recompute():
        n = len(state['entries'])
        count_var.set(_('Entries: %d / %d') % (n, _D.MAX_ENTRIES))
        sz = _D.ini_byte_size(state['entries'])
        size_var.set(_('Size: %d B / 32 KiB') % sz)
        def _mx(p):
            try:
                base = state['app0']
                rel = p[len('/app0/'):] if p.startswith('/app0/') else p.lstrip('/')
                return bool(base) and _os.path.isdir(_os.path.join(base, rel))
            except Exception:
                return True
        errs, warns = _D.validate_all(state['entries'], mount_exists=_mx if state['app0'] else None)
        if errs:
            valid_lbl.configure(fg=COLORS.get('danger_hi', '#e0584b'))
            valid_var.set(_('%d error(s): ') % len(errs) + ' | '.join(errs[:6]) + (' …' if len(errs) > 6 else ''))
        elif warns:
            valid_lbl.configure(fg=COLORS['warn'])
            valid_var.set(_('%d warning(s): ') % len(warns) + ' | '.join(warns[:6]) + (' …' if len(warns) > 6 else ''))
        else:
            valid_lbl.configure(fg=COLORS.get('success_hi', '#4caf50'))
            valid_var.set(_('✓ Valid — %d entr%s, %d bytes.') % (n, 'y' if n == 1 else 'ies', sz))
    def _render_entries():
        for w in list_frame.winfo_children():
            w.destroy()
        if not state['entries']:
            tk.Label(list_frame, text=_('No entries yet. Click "Add entry" to begin, or load an existing dlc_emu.ini.'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_5']).pack(anchor='w', padx=12, pady=12)
            _recompute()
        else:
            for i, e in enumerate(state['entries']):
                _make_entry_row(i, e)
            _recompute()
    _ADV_FIELDS = [*('label', 'entitlement_label', 'service_label', 'entitlement_key', 'download_status', 'active_flag', 'active_date', 'inactive_date', 'use_count', 'use_limit', 'consumable')]
    def _entry_status(entry):
        """(text, kind) for the simple-card status line. kind in
        'ok'|'warn'. Pure function of the entry + its provenance."""
        issues = _D.validate_entry(entry)
        err = [m for sev, m in issues if sev == 'error']
        if err:
            return '⚠ ' + err[0], 'warn'
        if getattr(entry, 'placeholder', False):
            return _('⚠ Review — placeholder ID'), 'warn'
        sec = (entry.section or '').upper()
        if sec in ('PSAC', 'PSAL') and (entry.get('download_status', 'INSTALLED') == 'INSTALLED' and not entry.get('mount_point', '')):
            return _('⚠ No addcont folder detected'), 'warn'
        return _('✓ Ready'), 'ok'
    def _badge_text(entry):
        conf = getattr(entry, 'confidence', None)
        src = getattr(entry, 'source', None)
        if not conf:
            return _('MANUAL'), 'manual'
        label = {'high': 'HIGH', 'medium': 'MEDIUM', 'low': 'LOW'}.get(conf, conf.upper())
        return f"{label!s} — {src or ''!s}", conf
    def _make_entry_row(i, entry):
        row = tk.Frame(list_frame, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
        row.pack(fill='x', padx=8, pady=4)
        topr = tk.Frame(row, bg=COLORS['bg_2'])
        topr.pack(fill='x', padx=8, pady=(6, 2))
        tk.Label(topr, text='#%d' % (i + 1), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(side='left', padx=(0, 8))
        sec_var = tk.StringVar(value=entry.section)
        sec_menu = tk.OptionMenu(topr, sec_var, *_D.SECTION_TYPES, command=(lambda v, _i=i: _set_section(_i, v)))
        try:
            sec_menu.configure(font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, highlightthickness=0)
        except Exception:
            pass
        sec_menu.pack(side='left', padx=(0, 8))
        btxt, bkind = _badge_text(entry)
        _badge_fg = {'high': COLORS.get('success_hi', '#4caf50'), 'medium': COLORS['accent_hi'], 'low': COLORS['warn'], 'manual': COLORS['fg_5']}.get(bkind, COLORS['fg_5'])
        tk.Label(topr, text=btxt, font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=_badge_fg).pack(side='left', padx=(2, 0))
        def _mini(txt, cmd):
            return XButton(topr, text=txt, command=cmd, font=(FONTS['body'][0], 9), bg=COLORS['bg_3'], fg=COLORS['fg_2'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=8, pady=2, cursor='hand2', highlightthickness=0)
        _mini('✕', lambda _i=i: _remove(_i)).pack(side='right')
        _mini('⧉', lambda _i=i: _duplicate(_i)).pack(side='right', padx=(0, 4))
        _mini('↓', lambda _i=i: _move(_i, 1)).pack(side='right', padx=(0, 4))
        _mini('↑', lambda _i=i: _move(_i, -1)).pack(side='right', padx=(0, 4))
        simple = tk.Frame(row, bg=COLORS['bg_2'])
        simple.pack(fill='x', padx=8, pady=(0, 4))
        simple.grid_columnconfigure(1, weight=1)
        tk.Label(simple, text=_('Content ID:'), font=(FONTS['body'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).grid(row=0, column=0, sticky='e', padx=(0, 6), pady=2)
        cid_var = tk.StringVar(value=entry.get('content_id', ''))
        cid_ent = XEntry(simple, textvariable=cid_var, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
        cid_ent.grid(row=0, column=1, sticky='ew', padx=(0, 8), pady=2)
        status_lbl = tk.Label(simple, font=(FONTS['body'][0], 9), bg=COLORS['bg_2'], anchor='w')
        status_lbl.grid(row=1, column=1, sticky='w', padx=(0, 8))
        mount_lbl = tk.Label(simple, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w')
        mount_lbl.grid(row=2, column=1, sticky='w', padx=(0, 8))
        src_lbl = tk.Label(simple, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w')
        src_lbl.grid(row=3, column=1, sticky='w', padx=(0, 8), pady=(0, 2))
        def _refresh_simple():
            stext, skind = _entry_status(entry)
            status_lbl.configure(text=_('Status:  ') + stext, fg=COLORS.get('success_hi', '#4caf50') if skind == 'ok' else COLORS['warn'])
            mp = entry.get('mount_point', '')
            mount_lbl.configure(text=_('Mount:  ') + (mp if mp else _('No mount path detected')))
            src = getattr(entry, 'source', None)
            src_lbl.configure(text=_('Source:  ') + (src if src else _('manual')))
        def _on_cid(*_a, _i=i, _v=cid_var):
            _set_field(_i, 'content_id', _v.get())
            _refresh_simple()
        cid_var.trace_add('write', _on_cid)
        _refresh_simple()
        adv_state = {'open': False, 'frame': None}
        adv_bar = tk.Frame(row, bg=COLORS['bg_2'])
        adv_bar.pack(fill='x', padx=8, pady=(0, 6))
        adv_btn = tk.Label(adv_bar, text=_('Advanced ▾'), cursor='hand2', font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['accent_hi'])
        adv_btn.pack(side='left')
        def _build_adv(parent_):
            grid = tk.Frame(parent_, bg=COLORS['bg_2'])
            for ci in range(2):
                grid.grid_columnconfigure(ci * 2 + 1, weight=1, uniform='dlcf')
            for fi, fld in enumerate(_ADV_FIELDS):
                r, c = divmod(fi, 2)
                tk.Label(grid, text=fld, font=(FONTS['body'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_5']).grid(row=r, column=c * 2, sticky='e', padx=(0, 4), pady=2)
                if fld == 'download_status':
                    dv = tk.StringVar(value=entry.get(fld, ''))
                    dm = tk.OptionMenu(grid, dv, '', *_D.DOWNLOAD_STATUSES, command=(lambda v, _i=i, _f=fld: (_set_field(_i, _f, v), _refresh_simple())))
                    try:
                        dm.configure(font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], relief='flat', bd=0, highlightthickness=1, highlightbackground=COLORS['border_2'])
                    except Exception:
                        pass
                    dm.grid(row=r, column=c * 2 + 1, sticky='ew', padx=(0, 12), pady=2)
                else:
                    fv = tk.StringVar(value=entry.get(fld, ''))
                    ent = XEntry(grid, textvariable=fv, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
                    ent.grid(row=r, column=c * 2 + 1, sticky='ew', padx=(0, 12), pady=2)
                    fv.trace_add('write', lambda *_a, _i=i, _f=fld, _v=fv: (_set_field(_i, _f, _v.get()), _refresh_simple()))
            return grid
        def _toggle_adv(_e=None):
            if adv_state['open']:
                if adv_state['frame'] is not None:
                    adv_state['frame'].pack_forget()
                    adv_state['frame'].destroy()
                    adv_state['frame'] = None
                adv_state['open'] = False
                adv_btn.configure(text=_('Advanced ▾'))
            else:
                f = _build_adv(row)
                f.pack(fill='x', padx=8, pady=(0, 6))
                adv_state['frame'] = f
                adv_state['open'] = True
                adv_btn.configure(text=_('Advanced ▴'))
        adv_btn.bind('<Button-1>', _toggle_adv)
    def _set_section(i, v):
        if 0 <= i < len(state['entries']):
            state['entries'][i].section = v
            _recompute()

    def _set_field(i, f, v):
        if 0 <= i < len(state['entries']):
            state['entries'][i].set(f, v)
            _recompute()

    def _add():
        if len(state['entries']) >= _D.MAX_ENTRIES:
            app_warning(app, _('Limit reached'), _('Maximum %d entries.') % _D.MAX_ENTRIES)
        else:
            state['entries'].append(_D.DlcEntry('PSAC', {}))
            _render_entries()
    def _remove(i):
        if 0 <= i < len(state['entries']):
            del state['entries'][i]
            _render_entries()

    def _duplicate(i):
        if 0 <= i < len(state['entries']):
            state['entries'].insert(i + 1, state['entries'][i].copy())
            _render_entries()

    def _move(i, delta):
        j = i + delta
        if 0 <= i < len(state['entries']):
            if 0 <= j < len(state['entries']):
                state['entries'][i], state['entries'][j] = \
                    state['entries'][j], state['entries'][i]
                _render_entries()

    def _set_folder(path):
        state['app0'] = path
        ini = _os.path.join(path, 'dlc_emu.ini') if path else ''
        if path and _os.path.isdir(path):
            if ini and _os.path.isfile(ini):
                folder_status.set(_('dlc_emu.ini found — loaded.'))
                return
            folder_status.set(_('No dlc_emu.ini yet — starting fresh.'))
        else:
            folder_status.set('')
    def _browse_folder():
        p = filedialog.askdirectory(title=_('Select the app0 / dump folder'))
        if not p:
            return
        folder_var.set(p)
        _load_from_folder(p)
    def _load_from_folder(path):
        _set_folder(path)
        ini = _os.path.join(path, 'dlc_emu.ini')
        if _os.path.isfile(ini):
            try:
                with open(ini, 'r', encoding='utf-8', errors='replace') as f:
                    state['entries'] = _D.parse_ini(f.read())
            except Exception as e:
                app_error(app, _('Load failed'), str(e))
                state['entries'] = []
        _render_entries()
    XButton(row1, text=_('Browse'), command=_browse_folder, font=(FONTS['body'][0], 9), bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=12, pady=4, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2']).pack(side='left')
    def _preview():
        text_ini = _D.emit_ini(state['entries'])
        win = tk.Toplevel(parent)
        win.title(_('dlc_emu.ini preview'))
        win.configure(bg=COLORS['bg_1'])
        txt = tk.Text(win, wrap='none', font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_1'], width=80, height=30, insertbackground=COLORS['fg_1'])
        txt.pack(fill='both', expand=True, padx=10, pady=10)
        txt.insert('1.0', text_ini)
        txt.configure(state='disabled')
    def _confirm_and_save(dest_path, into_app0):
        errs, warns = _D.validate_all(state['entries'])
        if errs and not app_confirm(app, _('Validation errors'), _('There are %d error(s):\n\n%s\n\nSave anyway?') % (len(errs), '\n'.join(errs[:10]))):
            return
        risky = sum(1 for e in state['entries'] if getattr(e, 'confidence', None) == 'low' or getattr(e, 'placeholder', False))
        if risky and not app_confirm(app, _('Low-confidence entries'), _('Some entries are low-confidence or use placeholder IDs. They may not unlock real DLC unless corrected. Save anyway?')):
            return
        try:
            if _os.path.isfile(dest_path):
                bak = dest_path + '.bak'
                try:
                    import shutil as _sh
                    _sh.copy2(dest_path, bak)
                    app._log('[DLC] Backed up existing dlc_emu.ini -> %s\n' % bak)
                except Exception as e:
                    app._log('[DLC] Backup failed (%s) — aborting save.\n' % e)
                    app_error(app, _('Backup failed'), _('Could not back up the existing file. Nothing was written.\n\n%s') % e)
                    return
            with open(dest_path, 'w', encoding='utf-8', newline='') as f:
                f.write(_D.emit_ini(state['entries']))
            app._log('[DLC] Wrote %s (%d entries)\n' % (dest_path, len(state['entries'])))
            app_info(app, _('Saved'), _('Wrote dlc_emu.ini:\n%s') % dest_path)
        except Exception as e:
            app_error(app, _('Save failed'), str(e))
    def _save_to_app0():
        path = folder_var.get().strip()
        if not path or not _os.path.isdir(path):
            app_warning(app, _('No folder'), _('Select a valid app0 folder first.'))
        else:
            state['app0'] = path
            _confirm_and_save(_os.path.join(path, 'dlc_emu.ini'), True)
    def _export_copy():
        p = filedialog.asksaveasfilename(title=_('Export dlc_emu.ini copy'), defaultextension='.ini', initialfile='dlc_emu.ini', filetypes=[('INI', '*.ini'), (_('All files'), '*.*')])
        if not p:
            return
        _confirm_and_save(p, False)
    def _scan_eboot():
        p = filedialog.askopenfilename(title=_('Select a binary (e.g. eboot.bin) to scan for DLC codes'))
        if not p:
            return
        prefix = simpledialog.askstring(_('content_id prefix'), _('Prefix for generated PSAC content_ids\n(20 chars, e.g. UP9000-PPSA01234_00-):'), initialvalue='UP9000-PPSA01234_00-')
        if not prefix:
            return
        def _work():
            found = []
            err = ''
            try:
                found = _D.scan_binary_for_codes(p, content_prefix=prefix)
            except Exception as e:
                err = str(e)
            def _done():
                if err:
                    app_error(app, _('Scan failed'), err)
                elif not found:
                    app_info(app, _('No codes'), _('No DLC codes found in that file.'))
                elif app_confirm(app, _('Add codes?'), _('Found %d unique code(s). Add them as PSAC entries?') % len(found)):
                    state['entries'].extend(found)
                    _render_entries()
            app.after(0, _done)
        _thr.Thread(target=_work, daemon=True).start()
    def _copy_modules():
        pkg = filedialog.askdirectory(title=_('Select the downloaded dlc_emu package folder (contains log/ and nolog/)'))
        if not pkg:
            return
        variant = {'v': None}
        dlg = tk.Toplevel(parent)
        dlg.title(_('Module variant'))
        dlg.configure(bg=COLORS['bg_1'])
        tk.Label(dlg, text=_('Which module build do you want to copy?'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_1']).pack(padx=16, pady=(14, 8))
        var = tk.StringVar(value='nolog')
        for val, lab in ('nolog', _('nolog (no logging — recommended)')), ('log', _('log (writes /app0/dlc_emu.log)')):
            XRadio(dlg, text=lab, value=val, variable=var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_3'], activebackground=COLORS['bg_1'], highlightthickness=0).pack(anchor='w', padx=24)
        def _ok():
            variant['v'] = var.get()
            dlg.destroy()
        XButton(dlg, text=_('Continue'), command=_ok, font=(FONTS['body'][0], 9), bg=COLORS['accent'], fg='#ffffff', relief='flat', bd=0, padx=14, pady=6, cursor='hand2').pack(pady=12)
        dlg.transient(parent)
        dlg.grab_set()
        parent.wait_window(dlg)
        if not variant['v']:
            return
        app0 = folder_var.get().strip()
        if not app0 or not _os.path.isdir(app0):
            app_warning(app, _('No folder'), _('Select a valid app0 folder first (modules copy into it).'))
        else:
            srcdir = _os.path.join(pkg, variant['v'])
            if not _os.path.isdir(srcdir):
                app_error(app, _('Package not found'), _('No "%s" folder inside the selected package.') % variant['v'])
            else:
                dstdir = _os.path.join(app0, 'fakelib')
                mods = [f for f in _os.listdir(srcdir) if f.lower().endswith(('.sprx', '.prx'))]
                if not mods:
                    app_error(app, _('No modules'), _('No .sprx/.prx files in %s.') % srcdir)
                else:
                    if not app_confirm(app, _('Copy modules?'), _('Copy %d module(s) from:\n  %s\n\ninto:\n  %s\n\nExisting same-named modules will be backed up (.bak). Continue?') % (len(mods), srcdir, dstdir)):
                        return
                    def _work():
                        import shutil as _sh
                        copied = 0
                        err = ''
                        try:
                            _os.makedirs(dstdir, exist_ok=True)
                            for m in mods:
                                s = _os.path.join(srcdir, m)
                                d = _os.path.join(dstdir, m)
                                if _os.path.isfile(d):
                                    try:
                                        _sh.copy2(d, d + '.bak')
                                    except Exception:
                                        pass
                                _sh.copy2(s, d)
                                copied += 1
                        except Exception as e:
                            err = str(e)
                        def _done():
                            if err:
                                app_error(app, _('Copy failed'), err)
                            else:
                                app._log('[DLC] Copied %d %s module(s) into %s\n' % (copied, variant['v'], dstdir))
                                app_info(app, _('Modules copied'), _('Copied %d %s module(s) into:\n%s') % (copied, variant['v'], dstdir))
                        app.after(0, _done)
                    _thr.Thread(target=_work, daemon=True).start()
    def _auto_find():
        path = folder_var.get().strip()
        if not path or not _os.path.isdir(path):
            app_warning(app, _('No folder'), _('Select a valid app0 folder first.'))
        else:
            state['app0'] = path
            def _work():
                sugg, info = [], {}
                err = ''
                try:
                    sugg, info = _D.auto_find_suggestions(path, scan_eboot=True, existing_entries=state['entries'])
                except Exception as e:
                    err = str(e)
                def _done():
                    if err:
                        app_error(app, _('Auto Find failed'), err)
                    elif not sugg:
                        note = ' '.join(info.get('notes', [])) or _('No local DLC metadata found. Add entries manually.')
                        app_info(app, _('Auto Find'), note)
                    else:
                        _show_suggestions(sugg, info)
                app.after(0, _done)
            _thr.Thread(target=_work, daemon=True).start()
    def _show_suggestions(sugg, info):
        win = tk.Toplevel(parent)
        win.title(_('Suggested DLC entries'))
        win.configure(bg=COLORS['bg_1'])
        hdr = tk.Label(win, text=_('Found %d suggestion(s). Tick the ones to add, then Apply selected.') % len(sugg), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_1'], anchor='w', justify='left', wraplength=620)
        hdr.pack(fill='x', padx=12, pady=(12, 4))
        if info.get('prefix_placeholder'):
            tk.Label(win, text=_('Note: content-id prefix is a placeholder (%s) — no CONTENT_ID/param.json id was found. Edit if needed.') % info.get('prefix', ''), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['warn'], anchor='w', justify='left', wraplength=620).pack(fill='x', padx=12, pady=(0, 4))
        if not info.get('addcont'):
            tk.Label(win, text=_('No addcont folders found. DLC-emu exposes ownership but does not create DLC files. Mount paths must already exist.'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['warn'], anchor='w', justify='left', wraplength=620).pack(fill='x', padx=12, pady=(0, 6))
        wrap = tk.Frame(win, bg=COLORS['bg_1'])
        wrap.pack(fill='both', expand=True, padx=12, pady=(0, 8))
        cv = tk.Canvas(wrap, bg=COLORS['bg_1'], highlightthickness=0, width=620, height=320)
        sb = XScrollbar(wrap, command=cv.yview, bg=COLORS['bg_2'])
        cv.configure(yscrollcommand=sb.set)
        inner = tk.Frame(cv, bg=COLORS['bg_1'])
        win_id = cv.create_window((0, 0), window=inner, anchor='nw')
        inner.bind('<Configure>', lambda e: (cv.configure(scrollregion=cv.bbox('all')), cv.itemconfig(win_id, width=cv.winfo_width())))
        cv.bind('<Configure>', lambda e: cv.itemconfig(win_id, width=e.width))
        def _sw(e, _c=cv):
            _c.yview_scroll(wheel_units(_c, e.delta), 'units')
        cv.bind('<Enter>', lambda _e, _c=cv: _c.bind_all('<MouseWheel>', _sw))
        cv.bind('<Leave>', lambda _e, _c=cv: _c.unbind_all('<MouseWheel>'))
        sb.pack(side='right', fill='y')
        cv.pack(side='left', fill='both', expand=True)
        _conf_color = {'high': COLORS.get('success_hi', '#4caf50'), 'medium': COLORS['accent_hi'], 'low': COLORS['warn']}
        checks = []
        for s in sugg:
            rowf = tk.Frame(inner, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
            rowf.pack(fill='x', padx=4, pady=3)
            var = tk.IntVar(value=1 if s.confidence in ('high', 'medium') else 0)
            checks.append((var, s))
            XCheck(rowf, variable=var, bg=COLORS['bg_2'], activebackground=COLORS['bg_2'], selectcolor=COLORS['bg_4'], highlightthickness=0).pack(side='left', padx=(6, 4))
            tk.Label(rowf, text=s.confidence.upper(), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=_conf_color.get(s.confidence, COLORS['fg_3']), width=7).pack(side='left')
            cid = s.entry.get('content_id', '')
            mp = s.entry.get('mount_point', '')
            txt = f"{s.entry.section!s}  [{cid!s}]  {s.source!s}{'  -> ' + mp if mp else ''!s}"
            tk.Label(rowf, text=txt, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w', justify='left').pack(side='left', fill='x', expand=True, padx=(6, 8), pady=4)
        btnr = tk.Frame(win, bg=COLORS['bg_1'])
        btnr.pack(fill='x', padx=12, pady=(0, 12))
        def _apply_selected():
            added = 0
            for var, s in checks:
                if var.get():
                    state['entries'].append(s.entry.copy())
                    added += 1
            win.destroy()
            if added:
                _render_entries()
                try:
                    app._log('[DLC] Auto Find added %d entr%s\n' % (added, 'y' if added == 1 else 'ies'))
                except Exception:
                    pass
        XButton(btnr, text=_('Apply selected'), command=_apply_selected, font=(FONTS['body'][0], 10), bg=COLORS['accent'], fg='#ffffff', relief='flat', bd=0, padx=16, pady=8, cursor='hand2').pack(side='left')
        XButton(btnr, text=_('Cancel'), command=win.destroy, font=(FONTS['body'][0], 10), bg=COLORS['bg_3'], fg=COLORS['fg_1'], relief='flat', bd=0, padx=16, pady=8, cursor='hand2').pack(side='left', padx=(8, 0))
        win.transient(parent)
    def _import_pack():
        from tkinter import filedialog as _fd
        choice = app_question(app, _('Import from'), _('Import from a ZIP file? (No = pick a folder instead)'))
        if choice == 'yes':
            pkg = _fd.askopenfilename(title=_('Select dlc_emu pack (.zip)'), filetypes=[('ZIP', '*.zip'), (_('All files'), '*.*')])
        else:
            pkg = _fd.askdirectory(title=_('Select dlc_emu pack folder'))
        if not pkg:
            return
        def _work():
            ini_txt = None
            mods = {}
            err = ''
            try:
                ini_txt = _D.find_ini_in_package(pkg)
                mods = _D.find_modules_in_package(pkg)
            except Exception as e:
                err = str(e)
            def _done():
                if err:
                    app_error(app, _('Import failed'), err)
                elif not ini_txt:
                    app_warning(app, _('No INI found'), _('No dlc_emu.ini was found in that package.'))
                else:
                    imported = _D.parse_ini(ini_txt)
                    nmods = sum(len(v) for k, v in mods.items() if k != '__zip__')
                    if not app_confirm(app, _('Import entries?'), _('Found %d entr%s in the package%s.\n\nAdd them to the editor? (You can then preview and save to app0.)') % (len(imported), 'y' if len(imported) == 1 else 'ies', _(' and %d module file(s)') % nmods if nmods else '')):
                        return
                    have = {e.get('content_id', '') for e in state['entries']}
                    added = 0
                    for e in imported:
                        if e.get('content_id', '') not in have:
                            state['entries'].append(e.copy())
                            added += 1
                    _render_entries()
                    try:
                        app._log('[DLC] Imported %d entr%s from pack %s\n' % (added, 'y' if added == 1 else 'ies', pkg))
                    except Exception:
                        pass
                    if nmods:
                        app_info(app, _('Entries imported'), _('Added %d entr%s. The package also contains module files — use "Copy replacement modules" to install them (separate, confirmed step).') % (added, 'y' if added == 1 else 'ies'))
            app.after(0, _done)
        _thr.Thread(target=_work, daemon=True).start()
    def _serialstation_lookup():
        """Open a dialog to search the SerialStation API by game name or Title ID,
        then import selected content_ids as PSAL DlcEntry candidates."""
        import urllib.request
        import urllib.parse
        import urllib.error
        import json as _json
        _SS_BASE = 'https://api.serialstation.com/v1'
        _SS_GAMES = _SS_BASE + '/games/'
        _SS_CIDS = _SS_BASE + '/content-ids/'
        _UA = 'exFAT-Builder-DLC/1.0'
        def _get_json(url, params, timeout=15):
            full = url + ('?' + urllib.parse.urlencode(params) if params else '')
            req = urllib.request.Request(full, headers={'User-Agent': _UA, 'Accept': 'application/json'})
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    return _json.loads(resp.read().decode('utf-8'))
            except urllib.error.HTTPError as exc:
                raise OSError('HTTP %s from SerialStation' % exc.code)
            except Exception as exc:
                raise OSError('SerialStation request failed: %s' % exc)
        def _search_games(name, limit=100):
            data = _get_json(_SS_GAMES, {'name': name, 'limit': limit})
            return data.get('items', []) if isinstance(data, dict) else []
        def _get_content_ids(title_id, limit=100, max_pages=200):
            import time as _time
            all_items, offset = [], 0
            for _ in range(max_pages):
                data = _get_json(_SS_CIDS, {'title_id': title_id, 'limit': limit, 'offset': offset})
                items = data.get('items', []) if isinstance(data, dict) else []
                all_items.extend(items)
                if len(items) < limit:
                    break
                offset += limit
                _time.sleep(0.15)
            return all_items
        dlg = tk.Toplevel(parent)
        dlg.title(_('SerialStation Lookup'))
        dlg.configure(bg=COLORS['bg_1'])
        dlg.geometry('700x520')
        dlg.resizable(True, True)
        dlg.transient(parent)
        tk.Label(dlg, text='🔗  ' + _('SerialStation Lookup'), font=(FONTS['body_strong'][0], 11), bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w', padx=16, pady=(16, 2))
        tk.Label(dlg, text=_('Enter a game name or Title ID (e.g. PPSA04930). Results are unverified candidates — review before saving.'), font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], justify='left', wraplength=660).pack(anchor='w', padx=16, pady=(0, 10))
        q_row = tk.Frame(dlg, bg=COLORS['bg_1'])
        q_row.pack(fill='x', padx=16)
        q_entry = XEntry(q_row, font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_1'], insertbackground=COLORS['fg_1'], relief='flat', highlightthickness=1, highlightbackground=COLORS['border_2'])
        q_entry.pack(side='left', fill='x', expand=True, padx=(0, 8))
        q_entry.focus_set()
        try:
            _ppsa_guess = _D.detect_ppsa(app0=(folder_var.get() or '').strip() or state.get('app0', ''), entries=state.get('entries', []), extra=[getattr(app, '_dlc_mounted_title_id', ''), getattr(app, '_mounted_title_id', '')])
            if _ppsa_guess and not q_entry.get().strip():
                q_entry.insert(0, _ppsa_guess)
                q_entry.icursor('end')
        except Exception:
            pass
        status_var = tk.StringVar(value='')
        tk.Label(dlg, textvariable=status_var, font=FONTS['mono_sm'], bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w').pack(fill='x', padx=16, pady=(6, 0))
        games_frame = tk.Frame(dlg, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
        games_frame.pack(fill='x', padx=16, pady=(6, 0))
        tk.Label(games_frame, text=_('Games (select one, then Import):'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=8, pady=(6, 2))
        games_lb = tk.Listbox(games_frame, bg=COLORS['bg_3'], fg=COLORS['fg_1'], selectbackground=COLORS['accent'], selectforeground='#ffffff', font=FONTS['mono_sm'], relief='flat', highlightthickness=0, height=5, activestyle='none')
        games_lb.pack(fill='x', padx=8, pady=(0, 8))
        cid_outer = tk.Frame(dlg, bg=COLORS['bg_1'])
        cid_outer.pack(fill='both', expand=True, padx=16, pady=(6, 0))
        tk.Label(cid_outer, text=_('Content IDs (tick to import):'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_1'], fg=COLORS['fg_4']).pack(anchor='w')
        cid_wrap = tk.Frame(cid_outer, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
        cid_wrap.pack(fill='both', expand=True)
        cid_canvas = tk.Canvas(cid_wrap, bg=COLORS['bg_2'], highlightthickness=0, height=160)
        cid_sb = XScrollbar(cid_wrap, command=cid_canvas.yview, bg=COLORS['bg_3'])
        cid_canvas.configure(yscrollcommand=cid_sb.set)
        cid_inner = tk.Frame(cid_canvas, bg=COLORS['bg_2'])
        _cid_win = cid_canvas.create_window((0, 0), window=cid_inner, anchor='nw')
        cid_inner.bind('<Configure>', lambda e: (cid_canvas.configure(scrollregion=cid_canvas.bbox('all')), cid_canvas.itemconfig(_cid_win, width=cid_canvas.winfo_width())))
        cid_canvas.bind('<Configure>', lambda e: cid_canvas.itemconfig(_cid_win, width=e.width))
        def _cid_wheel(ev, _c=cid_canvas):
            _c.yview_scroll(wheel_units(_c, ev.delta), 'units')
        cid_canvas.bind('<Enter>', lambda _e, _c=cid_canvas: _c.bind_all('<MouseWheel>', _cid_wheel))
        cid_canvas.bind('<Leave>', lambda _e, _c=cid_canvas: _c.unbind_all('<MouseWheel>'))
        cid_sb.pack(side='right', fill='y')
        cid_canvas.pack(side='left', fill='both', expand=True)
        _ss = {'game_rows': [], 'cid_checks': []}
        def _clear_cids():
            for w in cid_inner.winfo_children():
                w.destroy()
            _ss['cid_checks'].clear()
        def _populate_cids(items):
            _clear_cids()
            if not items:
                tk.Label(cid_inner, text=_('No content IDs found for this title.'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_5']).pack(anchor='w', padx=10, pady=8)
            else:
                have = {e.get('content_id', '') for e in state['entries']}
                for it in items:
                    cid = (it.get('content_id') or '').strip()
                    name = (it.get('name') or '').strip()
                    if not cid:
                        continue
                    already = cid in have
                    var = tk.IntVar(value=0 if already else 1)
                    row = tk.Frame(cid_inner, bg=COLORS['bg_2'])
                    row.pack(fill='x', padx=4, pady=1)
                    XCheck(row, variable=var, bg=COLORS['bg_2'], activebackground=COLORS['bg_2'], selectcolor=COLORS['bg_4'], highlightthickness=0).pack(side='left')
                    lbl = ('[already loaded]  ' if already else '') + cid
                    if name:
                        lbl += '  —  ' + name
                    tk.Label(row, text=lbl, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'] if already else COLORS['fg_1'], anchor='w').pack(side='left', fill='x', expand=True, padx=(4, 8))
                    _ss['cid_checks'].append((var, cid, name, already))
        def _fetch_cids(title_id):
            status_var.set(_('Fetching content IDs for %s …') % title_id)
            dlg.update_idletasks()
            def _work():
                items, err = [], ''
                try:
                    items = _get_content_ids(title_id)
                except OSError as exc:
                    err = str(exc)
                def _done():
                    if err:
                        status_var.set(_('Error: ') + err)
                        _clear_cids()
                    else:
                        status_var.set(_('Found %d content ID(s) for %s. Tick the ones you own, then Import.') % (len(items), title_id))
                        _populate_cids(items)
                dlg.after(0, _done)
            import threading as _thr2
            _thr2.Thread(target=_work, daemon=True).start()
        def _on_game_select(_event=None):
            sel = games_lb.curselection()
            if not sel:
                return
            idx = sel[0]
            if idx >= len(_ss['game_rows']):
                return
            title_id = _ss['game_rows'][idx][1]
            _fetch_cids(title_id)
        games_lb.bind('<<ListboxSelect>>', _on_game_select)
        def _do_search(_event=None):
            q = q_entry.get().strip()
            if not q:
                return
            games_lb.delete(0, 'end')
            _clear_cids()
            _ss['game_rows'].clear()
            status_var.set(_('Searching …'))
            dlg.update_idletasks()
            import re as _re3
            if _re3.match('^[A-Za-z]{4}[0-9]{5}$', q.strip()):
                tid = q.strip().upper()
                _ss['game_rows'].append((tid, tid))
                games_lb.insert('end', tid + '  (direct Title ID)')
                games_lb.selection_set(0)
                _fetch_cids(tid)
            else:
                def _work():
                    games, err = [], ''
                    try:
                        games = _search_games(q)
                    except OSError as exc:
                        err = str(exc)
                    def _done():
                        if err:
                            status_var.set(_('Error: ') + err)
                        elif not games:
                            status_var.set(_('No games found for “%s”.') % q)
                        else:
                            for g in games:
                                gname = g.get('name', '')
                                for tid in g.get('title_ids') or []:
                                    label = f"{gname!s}  ({tid!s})"
                                    _ss['game_rows'].append((gname, tid))
                                    games_lb.insert('end', label)
                            status_var.set(_('Found %d title(s). Select one to load its content IDs.') % len(_ss['game_rows']))
                    dlg.after(0, _done)
                import threading as _thr2
                _thr2.Thread(target=_work, daemon=True).start()
        XButton(q_row, text=_('Search'), command=_do_search, font=(FONTS['body'][0], 9), bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=12, pady=5, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2']).pack(side='left')
        q_entry.bind('<Return>', _do_search)
        btn_row = tk.Frame(dlg, bg=COLORS['bg_1'])
        btn_row.pack(fill='x', padx=16, pady=10)
        def _do_import():
            to_add = [(cid, name) for var, cid, name, already in _ss['cid_checks'] if var.get() and not already]
            if not to_add:
                app_info(app, _('Nothing to import'), _('No new content IDs are ticked.'), parent=dlg)
            else:
                have = {e.get('content_id', '') for e in state['entries']}
                added = 0
                for cid, name in to_add:
                    if cid in have:
                        continue
                    entry = _D.DlcEntry('PSAL', {'content_id': cid})
                    try:
                        entry.confidence = 'unverified'
                        entry.source = 'SerialStation'
                        if name:
                            entry.source = 'SerialStation — ' + name
                    except Exception:
                        pass
                    state['entries'].append(entry)
                    have.add(cid)
                    added += 1
                dlg.destroy()
                if added:
                    _render_entries()
                    try:
                        app._log('[DLC] SerialStation imported %d content ID(s)\n' % added)
                    except Exception:
                        pass
        XButton(btn_row, text=_('Import selected'), command=_do_import, font=(FONTS['body'][0], 10), bg=COLORS['accent'], fg='#ffffff', activebackground=COLORS['accent'], activeforeground='#ffffff', relief='flat', bd=0, padx=16, pady=8, cursor='hand2', highlightthickness=0).pack(side='left')
        XButton(btn_row, text=_('Close'), command=dlg.destroy, font=(FONTS['body'][0], 10), bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=14, pady=8, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2']).pack(side='left', padx=(8, 0))
    top_actions = tk.Frame(body, bg=COLORS['bg_2'])
    top_actions.pack(fill='x', pady=(2, 6))
    XButton(top_actions, text='🔎  ' + _('Auto Find DLC Entries'), command=_auto_find, font=(FONTS['body'][0], 10), bg=COLORS['accent'], fg='#ffffff', activebackground=COLORS['accent'], activeforeground='#ffffff', relief='flat', bd=0, padx=16, pady=9, cursor='hand2', highlightthickness=0).pack(side='left', padx=(0, 6))
    XButton(top_actions, text='🔗  ' + _('SerialStation Lookup…'), command=_serialstation_lookup, font=(FONTS['body'][0], 10), bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=16, pady=9, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2']).pack(side='left', padx=(0, 6))
    XButton(top_actions, text='📥  ' + _('Import DLC-emu Pack…'), command=_import_pack, font=(FONTS['body'][0], 10), bg=COLORS['bg_3'], fg=COLORS['fg_1'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=16, pady=9, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2']).pack(side='left')
    btns = tk.Frame(body, bg=COLORS['bg_2'])
    btns.pack(fill='x', pady=(2, 2))
    def _abtn(txt, cmd, primary=False):
        return XButton(btns, text=txt, command=cmd, font=(FONTS['body'][0], 10), bg=COLORS['accent'] if primary else COLORS['bg_3'], fg='#ffffff' if primary else COLORS['fg_1'], activebackground=COLORS['accent'] if primary else COLORS['bg_4'], activeforeground='#ffffff' if primary else COLORS['fg_0'], relief='flat', bd=0, padx=14, pady=8, cursor='hand2', highlightthickness=0 if primary else 1, highlightbackground=COLORS['border_2'])
    _abtn('+  ' + _('Add entry'), _add).pack(side='left', padx=(0, 6))
    _abtn('👁  ' + _('Preview INI'), _preview).pack(side='left', padx=(0, 6))
    _abtn('💾  ' + _('Save to app0'), _save_to_app0, primary=True).pack(side='left', padx=(0, 6))
    _abtn('⬇  ' + _('Export copy…'), _export_copy).pack(side='left', padx=(0, 6))
    btns2 = tk.Frame(body, bg=COLORS['bg_2'])
    btns2.pack(fill='x', pady=(6, 0))
    def _abtn2(txt, cmd):
        return XButton(btns2, text=txt, command=cmd, font=(FONTS['body'][0], 9), bg=COLORS['bg_3'], fg=COLORS['fg_2'], activebackground=COLORS['bg_4'], relief='flat', bd=0, padx=12, pady=6, cursor='hand2', highlightthickness=1, highlightbackground=COLORS['border_2'])
    _abtn2('🔍  ' + _('Scan eboot for codes…'), _scan_eboot).pack(side='left', padx=(0, 6))
    _abtn2('📦  ' + _('Copy replacement modules…'), _copy_modules).pack(side='left')
    tk.Label(body, text=_('Required runtime files — DLC-emu only works when BOTH are installed\n✓  dlc_emu.ini — created by “Save to app0” (contains the DLC configuration)\n✓  Runtime SPRX modules (libSceAppContent.sprx, libSceGameUpdate.sprx, libSceNpEntitlementAccess.sprx) — installed by “Copy replacement modules…”\nThe INI tells DLC-emu what DLC exists. The SPRX modules make the game use that configuration. Both are required.'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_5'], justify='left', anchor='w', wraplength=760).pack(fill='x', anchor='w', pady=(10, 2))
    _init_folder = folder_var.get().strip()
    if _init_folder and _os.path.isdir(_init_folder):
        _load_from_folder(_init_folder)
        return
    _render_entries()


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
             font=(FONTS['body'][0], 14),
             bg=COLORS['bg_2'], fg=COLORS['purple']
             ).pack(side='left', padx=(0, 8))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=_('Language Stripper'),
             font=(FONTS['body_strong'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Remove unwanted language packs from PS5 game dumps to save space.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(anchor='w')
    # BETA pill on the right of the header
    tk.Label(head, text=_('BETA'),
             font=(FONTS['body_strong'][0], 9),
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
                 font=(FONTS['body_strong'][0], 9),
                 bg=COLORS['bg_3'], fg=COLORS['fg_5']
                 ).pack(anchor='w', padx=12, pady=(9, 0))
        v = tk.Label(cell, text='\u2014',
                     font=(FONTS['body_strong'][0], 11),
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
                            font=(FONTS['body_strong'][0], 10))
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
             font=(FONTS['body_strong'][0], 11),
             bg=COLORS['danger_bg'], fg=COLORS['danger_hi']
             ).pack(side='left', padx=(0, 8))
    tk.Label(bi,
             text=_('Deletes files permanently \u2014 back up games first.'),
             font=(FONTS['body'][0], 10),
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
    XRadio(mode_row, text=_('Single game folder'),
                    variable=app._adv_lang_mode_var, value='single',
                    font=(FONTS['body'][0], 10),
                    bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                    activebackground=COLORS['bg_2'],
                    selectcolor=COLORS['bg_3'],
                    cursor='hand2'
                    ).pack(side='left', padx=(0, 16))
    XRadio(mode_row, text=_('Scan all games in a folder'),
                    variable=app._adv_lang_mode_var, value='scan',
                    font=(FONTS['body'][0], 10),
                    bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                    activebackground=COLORS['bg_2'],
                    selectcolor=COLORS['bg_3'],
                    cursor='hand2'
                    ).pack(side='left')

    # Games folder field
    folder_row = tk.Frame(body, bg=COLORS['bg_2'])
    folder_row.pack(fill='x', pady=(0, 8))
    tk.Label(folder_row, text=_('Games folder:'),
             font=(FONTS['body'][0], 10),
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
    XEntry(folder_ef, textvariable=app._adv_lang_folder_var,
             font=(FONTS['body'][0], 10),
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
             font=(FONTS['body'][0], 9),
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
            font=(FONTS['body'][0], 9),
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
    strip_btn = XButton(footer,
        text='\U0001f5d1  ' + _('Strip Selected Languages'),
        font=(FONTS['body_strong'][0], 11),
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
             font=(FONTS['body'][0], 28),
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
            return
        except Exception as e:
            app_error(app, 'Scan failed', str(e))
            return
    app_info(app, 'Not available',
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
            app_error(app, 'Strip failed', str(e))


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
             font=(FONTS['body'][0], 15),
             bg=COLORS['bg_2'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 9))
    title_col = tk.Frame(head, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=title,
             font=(FONTS['body_strong'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(anchor='w')
    tk.Label(title_col, text=subtitle,
             font=(FONTS['body'][0], 10),
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
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
    meta = None
    if hw:
        meta = _('Recommended: 1 \u2014 single-threaded avoids '
                 'fragmenting the image')

    _setting_row(body, app,
        label=_('Threads'),
        kind='int',
        var=app._adv_threads_var,
        default='1',
        help_text=_('1 = sequential \u2014 recommended, avoids fragmenting the image. Higher copies faster but fragments output.'),
        meta=meta)

    _setting_row(body, app,
        label=_('Robocopy file retry count (/R)'),
        kind='int',
        var=app._adv_retries_var,
        default='3',
        help_text=_('How many times robocopy retries a single failed '
                    'file copy during a build. Separate from the '
                    'whole-build auto-retry.'))

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
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
             anchor='w').pack(anchor='w')
    tk.Label(bench_lcol, text=_('1 GiB sequential test on the temp drive.'),
             font=(FONTS['body'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             anchor='w').pack(anchor='w', pady=(3, 0))
    bench_ctrl = tk.Frame(bench_row, bg=COLORS['bg_2'])
    bench_ctrl.pack(side='left', padx=(12, 12))
    bench_val = app._settings.get('adv_write_benchmark', '\u2014')
    bench_pill = tk.Label(bench_ctrl, text=bench_val,
        font=(FONTS['body_strong'][0], 10),
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
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
             anchor='w').pack(side='left')
    if severity == 'danger':
        tk.Label(label_row, text=_('destructive'),
                 font=(FONTS['body_strong'][0], 9),
                 bg=COLORS['danger_bg'], fg=COLORS['danger_hi'],
                 padx=6, pady=1,
                 highlightbackground=COLORS['danger'], highlightthickness=1
                 ).pack(side='left', padx=(8, 0))
    if help_text:
        help_fg = COLORS['danger_hi'] if severity == 'danger' else COLORS['fg_5']
        tk.Label(lcol, text=help_text,
                 font=(FONTS['body'][0], 9),
                 bg=COLORS['bg_2'], fg=help_fg,
                 anchor='w', justify='left', wraplength=340
                 ).pack(anchor='w', pady=(3, 0))
    if meta:
        tk.Label(lcol, text='\U0001f4a1  ' + meta,
                 font=(FONTS['body'][0], 9),
                 bg=COLORS['bg_2'], fg=COLORS['accent_hi'],
                 anchor='w').pack(anchor='w', pady=(2, 0))

    # Column 2: control
    ctrl_col = tk.Frame(row, bg=COLORS['bg_2'])
    ctrl_col.pack(side='right', padx=(12, 0))
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
                                    font=(FONTS['body'][0], 10), width=14)
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
        ctrl_widget = XEntry(ef, textvariable=var,
                 font=(FONTS['body'][0], 10),
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
                              font=(FONTS['body_strong'][0], 9),
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
                        font=(FONTS['body_strong'][0], 9),
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
                 font=(FONTS['body'][0], 10),
                 padx=8, pady=3, width=8, anchor='w'
                 ).pack()
        lock_pill = tk.Label(ctrl_col, text='\U0001f512',
                              font=(FONTS['body'][0], 10),
                              bg=COLORS['bg_2'], fg=COLORS['warn'])
        lock_pill.pack(side='left', padx=(6, 0))
        ctrl_widget = lock_pill

    # Column 3: inline "Restore default" badge — only shown when the
    # value differs from its default (sits just left of the control)
    if default is not None and var is not None:
        def_inline = tk.Frame(row, bg=COLORS['bg_2'])
        def_inline.pack(side='right', padx=(0, 4))

        def _update_def_indicator(*_a):
            try:
                cur = var.get()
            except Exception:
                return
            for w in def_inline.winfo_children():
                w.destroy()
            if str(cur) == str(default):
                return
            # Off-default — clickable revert badge
            badge = tk.Label(def_inline,
                text='\u21bb  ' + _('Restore default') + ' (' + str(default) + ')',
                font=(FONTS['body_strong'][0], 9),
                bg=COLORS['warn_bg'], fg=COLORS['warn'],
                padx=8, pady=2, cursor='hand2',
                highlightbackground=COLORS['warn'], highlightthickness=1)
            badge.pack(side='right')
            def _revert(_e=None):
                var.set(default)
                try:
                    _save_handler()
                except Exception:
                    pass
                _update_def_indicator()
            badge.bind('<Button-1>', _revert)
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
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
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
             font=(FONTS['body_strong'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left')
    status_pill = tk.Label(hrow, text='',
                           font=(FONTS['body_strong'][0], 9),
                           padx=8, pady=2)
    status_pill.pack(side='right')

    body = tk.Frame(inner, bg=COLORS['bg_2'])
    body.pack(fill='x', pady=(8, 0))

    specs = [
        ('cluster', _('Cluster Size'), app._adv_cluster_var),
        ('sector', _('Sector Size'), app._adv_sector_var),
        ('threads', _('Threads'), app._adv_threads_var),
        ('retries', _('Robocopy file retries'), app._adv_retries_var),
        ('inode', _('Bytes / Inode'), app._adv_ffpkg_inode_var),
    ]
    rows = {}
    for key, label, var in specs:
        r = tk.Frame(body, bg=COLORS['bg_2'])
        r.pack(fill='x', pady=(0, 5))
        dot = tk.Label(r, text='\u25cf',
                       font=(FONTS['body'][0], 9),
                       bg=COLORS['bg_2'], fg=COLORS['fg_5'])
        dot.pack(side='left', padx=(0, 6))
        tk.Label(r, text=label, font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
                 ).pack(side='left')
        val = tk.Label(r, text='\u2014',
                       font=(FONTS['body_strong'][0], 9),
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
    # v3.6.3: hw may be None (detection still running in the background) or a
    # {'_failed': True} sentinel (detection finished but found nothing). Only
    # treat it as real hardware when it actually carries a model. This keeps
    # the async deferral from ever raising a KeyError here.
    _detecting = (hw is None)
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
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
             font=(FONTS['body'][0], 14),
             bg=COLORS['accent_08'], fg=COLORS['accent_hi']
             ).pack(side='left', padx=(0, 8))
    head_col = tk.Frame(head, bg=COLORS['accent_08'])
    head_col.pack(side='left', fill='x', expand=True)
    tk.Label(head_col, text=_('RECOMMENDED FOR YOUR SETUP'),
             font=(FONTS['body_strong'][0], 9),
             bg=COLORS['accent_08'], fg=COLORS['accent']
             ).pack(anchor='w')
    if _detecting:
        sub_text = _('Detecting hardware\u2026')
    elif hw:
        sub_text = _('Detected: %s \u00b7 %s') % (hw['model'], hw['kind'])
    else:
        sub_text = _('HW detection failed')
    tk.Label(head_col, text=sub_text,
             font=(FONTS['body'][0], 9),
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
                     font=(FONTS['body_strong'][0], 9),
                     bg=COLORS['bg_2'], fg=COLORS['fg_5']
                     ).pack(anchor='w', padx=8, pady=(5, 0))
            tk.Label(cell, text=val,
                     font=(FONTS['body_strong'][0], 10),
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
             font=(FONTS['body_strong'][0], 9),
             bg=COLORS['accent_08'], fg=COLORS['fg_5'],
             width=10, anchor='w').pack(side='left')
    tk.Label(row, text=str(recommended_val),
             font=(FONTS['body_strong'][0], 10),
             bg=COLORS['accent_08'], fg=COLORS['fg_1'],
             anchor='w').pack(side='left')
    if current_val is not None and matches_val is not None:
        if str(current_val) == str(matches_val):
            tk.Label(row, text='\u2713  ' + _('matches'),
                     font=(FONTS['body'][0], 9),
                     bg=COLORS['accent_08'], fg=COLORS['success_hi']
                     ).pack(side='left', padx=(8, 0))
        else:
            tk.Label(row, text=_('currently ') + str(current_val),
                     font=(FONTS['body_strong'][0], 9),
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
             font=(FONTS['body_strong'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(anchor='w')
    text1 = _(
        'PS5 expects 64K clusters and 512-byte sectors \u2014 anything '
        'else needs config.ini edits or the image won\'t mount.')
    tk.Label(inner, text=text1,
             font=(FONTS['body'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=300
             ).pack(anchor='w', pady=(6, 4))
    text2 = _('More threads = faster but more fragmentation.')
    tk.Label(inner, text=text2,
             font=(FONTS['body'][0], 9),
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=300
             ).pack(anchor='w')


# ─────────────────────────────────────────────────────────────────────
# Toolbar actions: Reset all + Save parameters + Run benchmark
# ─────────────────────────────────────────────────────────────────────
def _reset_all(app):
    if not app_confirm(app, 'Reset all',
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
        app_error(app, 'Save failed', str(e))


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
    return XButton(parent, text=text,
                     font=(FONTS['body'][0], 9),
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
    return XButton(parent, text=text,
                     font=(FONTS['body_strong'][0], 9),
                     bg=COLORS['accent'], fg=COLORS['fg_0'],
                     activebackground=COLORS['accent_hi'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     command=command)


def _danger_btn(parent, text, command):
    return XButton(parent, text=text,
                     font=(FONTS['body_strong'][0], 9),
                     bg=COLORS['danger'], fg=COLORS['fg_0'],
                     activebackground=COLORS['danger_hi'],
                     activeforeground=COLORS['fg_0'],
                     disabledforeground=COLORS['fg_5'],
                     relief='flat', bd=0,
                     padx=12, pady=6,
                     cursor='hand2',
                     command=command)
