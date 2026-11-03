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
from ui.shared.widgets import (
    Button as XButton, Check as XCheck, Radio as XRadio,
    TextField as XEntry, Scrollbar as XScrollbar, IconButton, Dropdown,
    Card, Rounded, Badge, InfoBar, PageHeader, Tabs, Segmented, Divider,
    IconLabel, EmptyState as XEmptyState, Tooltip as XTooltip, autowrap)
from ui.shared.scroll import wheel_units
from tkinter import ttk

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from ui.shared.app_dialog import (
    app_confirm, app_error, app_info, app_question, app_warning,
)
from exfat_builder import _, save_settings, SubTabBar, Tooltip
from ui.shared.scroll import attach_scroll
from ui.tab_settings import setting_row, TSwitch

C = COLORS


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
    body = tk.Frame(parent, bg=C['bg_1'])
    body.pack(fill='both', expand=True)
    app._adv_v2_body = body

    # Five panes — only one visible at a time. Each pane is a
    # scrollable host so tall content (e.g. Post-Build's Danger Zone)
    # is always reachable instead of being clipped at the viewport
    # bottom. The scrollregion is recomputed on every <Configure> of
    # the inner frame, so adding cards never pushes content off-screen.
    app._adv_v2_panes = {}
    for key in ('params', 'post', 'lang', 'ampr', 'dlc'):
        pane = tk.Frame(body, bg=C['bg_1'])
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
# Page head — title, description and the page actions
# ─────────────────────────────────────────────────────────────────────
def _build_head(parent, app):
    head = PageHeader(parent, _('Advanced'),
                      _('Power-user configuration for image creation, '
                        'performance tuning and automation.'),
                      bg=C['bg_1'])
    head.pack(fill='x', padx=px(24), pady=(px(20), px(12)))
    app._adv_v2_header = head
    # Save parameters is the primary action of the Build Parameters view;
    # on the other sub-tabs it steps back to secondary (see _show_subtab)
    # so each view keeps a single primary button.
    save_btn = XButton(head.actions, text=_('Save parameters'),
                       variant='primary', icon='save',
                       command=lambda: _save_params(app))
    save_btn.pack(side='right')
    reset_btn = XButton(head.actions, text=_('Reset all'), icon='undo',
                        command=lambda: _reset_all(app))
    reset_btn.pack(side='right', padx=(0, px(6)))
    app._adv_v2_save_btn = save_btn


# ─────────────────────────────────────────────────────────────────────
# Sub-tab bar — design-system underline tabs with counter badges
# ─────────────────────────────────────────────────────────────────────
def _build_subtab_bar(parent, app):
    app._adv_v2_tab_btns = {}     # key -> tab frame
    app._adv_v2_tab_counts = {}   # key -> count badge
    tabs = [
        ('params', '\U0001f527', _('Build Parameters'), 'wrench'),
        ('post',   '\U0001f680', _('Post-Build'), 'rocket'),
        ('lang',   '\U0001f310', _('Language Stripper'), 'globe'),
        ('ampr',   '\U0001f5c2', _('AMPR Index'), 'folders'),
        ('dlc',    '\U0001f5c3', _('DLC-emu'), 'database'),
    ]
    bar = Tabs(parent, bg=C['bg_1'],
               on_select=lambda k: _show_subtab(app, k))
    bar.pack(fill='x', padx=px(24), pady=(0, px(16)))
    app._adv_v2_tabs = bar
    for key, _glyph, label, icon in tabs:
        f = bar.add(key, label, icon)
        app._adv_v2_tab_btns[key] = f
        badge = Badge(bar._tabs[key]['inner'], '', 'warn', icon=False,
                      surface=C['bg_1'])
        # NOT packed yet — _update_tab_count handles pack/forget
        app._adv_v2_tab_counts[key] = badge


def _pill_tab(parent, key, icon, label, app):
    """Kept for compatibility — the sub-tabs are a `Tabs` row now."""
    return app._adv_v2_tab_btns.get(key)


def _show_subtab(app, key):
    """Switch to a sub-tab. Updates the tab indicator, hides other panes,
    shows the chosen pane, persists the choice."""
    for k, pane in app._adv_v2_panes.items():
        if k == key:
            pane.pack(fill='both', expand=True)
        else:
            pane.pack_forget()
    try:
        app._adv_v2_tabs.select(key)
    except Exception:
        pass
    try:
        # (Button maps a legacy bg= colour onto its variant.)
        app._adv_v2_save_btn.configure(
            bg=C['accent'] if key == 'params' else C['bg_3'])
    except Exception:
        pass
    app._settings['adv_active_subtab'] = key
    try:
        save_settings(app._settings)
    except Exception:
        pass


def _style_tab(btn, active):
    """Kept for compatibility — `Tabs.select` paints the active tab."""
    return None


def _update_tab_count(app, key, count):
    """Show / hide / update the warn count badge on a sub-tab."""
    badge = app._adv_v2_tab_counts.get(key)
    if badge is None:
        return
    if count > 0:
        badge.set(str(count))
        badge.pack(side='left', padx=(px(6), 0))
    else:
        badge.pack_forget()


# ─────────────────────────────────────────────────────────────────────
# Scroll host for each sub-tab pane
# ─────────────────────────────────────────────────────────────────────
def _scroll_host(parent):
    """Wrap `parent` in a vertical-scroll canvas and return the inner
    frame to build into (24px page gutter applied). Fixes content taller
    than the viewport being clipped (the scrollregion tracks the inner
    frame's real size)."""
    cv = tk.Canvas(parent, bg=C['bg_1'], highlightthickness=0, bd=0)
    sb = XScrollbar(parent, orient='vertical', command=cv.yview,
                    surface=C['bg_1'])
    cv.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    cv.pack(side='left', fill='both', expand=True)
    outer = tk.Frame(cv, bg=C['bg_1'])
    tag = 'host_' + str(id(outer))
    # Fixed initial width: a hidden pane gets no <Configure>, and an
    # unsized window would follow its content's requested width, which
    # wrapping labels change (layout feedback loop).
    cv.create_window((0, 0), window=outer, anchor='nw', tags=tag,
                     width=px(1060))

    def _on_inner(_e=None, c=cv):
        c.configure(scrollregion=c.bbox('all'))
    outer.bind('<Configure>', _on_inner)
    cv.bind('<Configure>',
            lambda e, c=cv, t=tag: c.itemconfig(t, width=e.width))

    # Scroll only while the pointer is over THIS pane (panes stack;
    # bind_all/unbind_all on Enter/Leave keeps them from fighting).
    def _wheel(e, c=cv):
        c.yview_scroll(wheel_units(c, e.delta), 'units')
    cv.bind('<Enter>', lambda _e, c=cv: c.bind_all('<MouseWheel>', _wheel))
    cv.bind('<Leave>', lambda _e, c=cv: c.unbind_all('<MouseWheel>'))
    inner = tk.Frame(outer, bg=C['bg_1'])
    inner.pack(fill='both', expand=True, padx=(px(24), px(16)),
               pady=(0, px(24)))
    return inner


def _build_legacy_pane(parent, app, builder_fn):
    """Wrap a legacy builder in our scroll frame so it can sit inside a
    sub-tab pane without modification."""
    inner = _scroll_host(parent)
    try:
        builder_fn(inner)
    except Exception as _e:
        # If a legacy builder fails (some have late-init dependencies),
        # at least show a friendly placeholder rather than a blank pane.
        InfoBar(inner, kind='danger', text='(' + str(_e) + ')').pack(
            fill='x')


# ─────────────────────────────────────────────────────────────────────
# Post-Build pane — automation summary + cards of settings rows
# ─────────────────────────────────────────────────────────────────────
def _build_post_summary(parent, app):
    """Automation Summary — enabled-actions checklist + a visual
    Build → Verify → Upload workflow strip. Pure mirror of the
    post-build vars; updates live via traces."""
    card = Card(parent, padding=16)
    card.grid(row=0, column=0, columnspan=2, sticky='ew', pady=(0, px(16)))

    body = tk.Frame(card, bg=C['bg_2'])
    body.pack(fill='x')
    body.grid_columnconfigure(0, weight=0)
    body.grid_columnconfigure(1, weight=0)
    body.grid_columnconfigure(2, weight=1)

    # Left: enabled-actions checklist
    checks = tk.Frame(body, bg=C['bg_2'])
    checks.grid(row=0, column=0, sticky='nw', padx=(0, px(48)))
    tk.Label(checks, text=_('AUTOMATION PIPELINE').upper(),
             font=FONTS['overline'], bg=C['bg_2'], fg=C['fg_4']
             ).pack(anchor='w', pady=(0, px(8)))
    items = [(app._adv_auto_upload_var, _('Upload to PS5')),
             (app._adv_checksum_var, _('Verify checksum')),
             (app._adv_delete_source_var, _('Delete source (exFAT)')),
             (app._adv_delete_source_ffpkg_var,
              _('Delete source (ffpkg)'))]
    rows = []
    for var, label in items:
        r = tk.Frame(checks, bg=C['bg_2'])
        r.pack(anchor='w', pady=(0, px(4)))
        ic = IconLabel(r, 'circle', 16, C['fg_5'], C['bg_2'])
        ic.pack(side='left', padx=(0, px(8)))
        lbl = tk.Label(r, text=label, font=FONTS['body'], bg=C['bg_2'],
                       anchor='w')
        lbl.pack(side='left')
        rows.append((var, ic, lbl))

    # Workflow strip — sits right next to the checklist
    flow_wrap = tk.Frame(body, bg=C['bg_2'])
    flow_wrap.grid(row=0, column=1, sticky='nw')
    tk.Label(flow_wrap, text=_('Current flow').upper(),
             font=FONTS['overline'], bg=C['bg_2'], fg=C['fg_4'], anchor='w'
             ).pack(anchor='w', pady=(0, px(8)))
    flow = tk.Frame(flow_wrap, bg=C['bg_2'])
    flow.pack(anchor='w')
    steps = {}

    def _chip(key, icon, label):
        c = Rounded(flow, bg=C['bg_3'], border=C['border_3'], radius=4,
                    padding=(px(10), px(5)), surface=C['bg_2'])
        c.pack(side='left')
        ic = IconLabel(c, icon, 16, C['fg_3'], C['bg_3'])
        ic.pack(side='left', padx=(0, px(6)))
        lbl = tk.Label(c, text=label, font=FONTS['body'], bg=C['bg_3'],
                       fg=C['fg_3'])
        lbl.pack(side='left')
        steps[key] = (c, ic, lbl, icon)
        return c

    def _arrow():
        IconLabel(flow, 'arrow_right', 16, C['fg_4'], C['bg_2']).pack(
            side='left', padx=px(8))

    _chip('build', 'build', _('Build'))
    _arrow()
    _chip('verify', 'search', _('Verify'))
    _arrow()
    _chip('upload', 'upload', _('Upload'))

    def _sync(*_a):
        try:
            for var, ic, lbl in rows:
                on = bool(var.get())
                ic.set('success' if on else 'circle',
                       fg=C['success'] if on else C['fg_5'])
                lbl.config(fg=C['fg_1'] if on else C['fg_4'])
            states = {'build': True,
                      'verify': bool(app._adv_checksum_var.get()),
                      'upload': bool(app._adv_auto_upload_var.get())}
            for key, (chip, ic, lbl, icon) in steps.items():
                if not chip.winfo_exists():
                    return
                if states[key]:
                    chip.set_style(bg=C['accent_lo'], border=C['accent'])
                    ic.set(icon, fg=C['accent_hi'], bg=C['accent_lo'])
                    lbl.config(bg=C['accent_lo'], fg=C['fg_0'])
                else:
                    chip.set_style(bg=C['bg_3'], border=C['border_3'])
                    ic.set(icon, fg=C['fg_4'], bg=C['bg_3'])
                    lbl.config(bg=C['bg_3'], fg=C['fg_3'])
        except Exception:
            pass

    for var, _i, _l in rows:
        try:
            var.trace_add('write', _sync)
        except Exception:
            pass
    _sync()


def _head_first(card):
    """Give a Card's head actions layout priority over its (wrapping)
    title column, so header badges are never squeezed."""
    try:
        card.head_actions.pack_configure(before=card.title_label.master)
    except Exception:
        pass


def _post_card(parent, row, col, icon, title, desc, accent,
               state_vars=None, danger=False):
    """Automation card with a live On/Off (or Armed) state badge derived
    from the supplied BooleanVars (any True → on). Presentation only —
    the rows inside keep their existing vars and callbacks."""
    card = Card(parent, title=title, subtitle=desc)
    card.grid(row=row, column=col, sticky='nsew',
              padx=(0 if col == 0 else px(8), px(8) if col == 0 else 0),
              pady=(0, px(16)))
    pill = Badge(card.head_actions, _('Off'), 'neutral')
    pill.pack(side='right')
    _head_first(card)
    if danger:
        Badge(card.head_actions, _('DESTRUCTIVE'), 'danger').pack(
            side='right', padx=(0, px(6)))
        _head_first(card)

    if state_vars:
        def _sync(*_a):
            try:
                on = any(bool(v.get()) for v in state_vars)
                if not pill.winfo_exists():
                    return
                if on and danger:
                    pill.set(_('ARMED'), 'danger')
                elif on:
                    pill.set(_('On'), 'success')
                else:
                    pill.set(_('Off'), 'neutral')
            except Exception:
                pass
        for v in state_vars:
            try:
                v.trace_add('write', _sync)
            except Exception:
                pass
        _sync()
    else:
        pill.pack_forget()
    return card.body


def _build_post_pane(parent, app):
    """Automation dashboard — Upload / Verification / Image Output cards
    in a grid with live state badges, and a full-width Danger Zone card
    for the destructive delete-source toggles. (The Logging card was
    removed in v3.6.4 — its toggle wrote a setting no build path
    consumed.) Same SettingRows, same vars, same callbacks."""
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
                   C['accent'],
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
                   C['accent'],
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
                   C['accent_hi'])
    _setting_row(b, app,
        label=_('Image size override (GB)'),
        kind='text',
        var=app._adv_img_size_var,
        default='',
        help_text=_('Blank = auto-size. Set e.g. 90 to force a 90 GB '
                    'image.'),
        on_change=app._adv_save_post_build)

    # ── Danger Zone (full width) ──
    dz = _post_card(parent, 3, 0, '⚠', _('Danger Zone'),
                    _('Destructive cleanup — these delete the '
                      'source folder once a build succeeds. Cannot be '
                      'undone.'),
                    C['danger_hi'],
                    state_vars=[app._adv_delete_source_var,
                                app._adv_delete_source_ffpkg_var],
                    danger=True)
    dz.master.grid_configure(columnspan=2, padx=0)
    _setting_row(dz, app,
        label=_('Delete source folder after exFAT build'),
        kind='toggle',
        var=app._adv_delete_source_var,
        default=False,
        help_text=_('⚠ Cannot be undone. Source folder is removed '
                    'once the .exfat is built successfully.'),
        severity='danger')
    _setting_row(dz, app,
        label=_('Delete source folder after ffpkg build'),
        kind='toggle',
        var=app._adv_delete_source_ffpkg_var,
        default=False,
        help_text=_('⚠ Cannot be undone. Source folder is removed '
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

    card_outer = Card(parent, title=_('AMPR Index Builder'),
                      subtitle=_('Generate ampr_emu.index for an APR title — for an '
                                 'existing dump folder, or a connected PS5. Builds run '
                                 'automatically too (when fakelib/libSceAmpr.sprx is '
                                 'present); this is the manual entry point.'))
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, px(16)))
    body = card_outer.body

    # Stat cells: files indexed / index size / output
    top, (c_files, c_size, c_out) = _stat_cells(body, [
        (_('Files Indexed'), False), (_('Index Size'), False),
        (_('Output'), True)])
    top.pack(fill='x', pady=(0, px(12)))
    c_out.configure(justify='left', wraplength=px(300))

    status_var = tk.StringVar(value=_('Idle.'))
    st_row = tk.Frame(body, bg=C['bg_2'])
    st_row.pack(fill='x', pady=(0, px(12)))
    IconLabel(st_row, 'info', 16, C['fg_4'], C['bg_2']).pack(
        side='left', anchor='n', padx=(0, px(8)))
    _st = tk.Label(st_row, textvariable=status_var, font=FONTS['caption'],
                   bg=C['bg_2'], fg=C['fg_3'], anchor='w', justify='left')
    _st.pack(side='left', fill='x', expand=True)
    autowrap(_st)

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

    Divider(body, color=C['border_1']).pack(fill='x', pady=(0, px(12)))
    btn_row = tk.Frame(body, bg=C['bg_2'])
    btn_row.pack(fill='x')
    btn_folder = XButton(btn_row, text=_('Generate From Folder'),
                         command=_generate_from_folder, variant='primary',
                         icon='folder_open')
    btn_folder.pack(side='left', padx=(0, px(6)))
    btn_ps5 = XButton(btn_row, text=_('Generate From Connected PS5'),
                      command=_generate_from_ps5, icon='wifi')
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
    card_outer = Card(parent, title=_('DLC-emu'), subtitle=_('Build / edit /app0/dlc_emu.ini for a game dump folder. Phase 1 writes only into the app0 folder (packed by your normal build); it never edits images. PSAC is the recommended entry type for most add-on content; every other type stays available for special cases.'))
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, px(16)))
    Badge(card_outer.head_actions, _('PHASE 1'), 'warn').pack(side='right')
    _head_first(card_outer)
    body = card_outer.body
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
    tk.Label(body, text=_('app0 folder:').rstrip(':'), font=FONTS['body'], bg=C['bg_2'], fg=C['fg_2'], anchor='w').pack(fill='x')
    row1 = tk.Frame(body, bg=C['bg_2'])
    row1.pack(fill='x', pady=(px(4), px(4)))
    folder_entry = XEntry(row1, textvariable=folder_var, mono=True)
    folder_entry.pack(side='left', fill='x', expand=True)
    folder_status = tk.StringVar(value='')
    tk.Label(body, textvariable=folder_status, font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3'], anchor='w').pack(fill='x', pady=(0, px(6)))
    strip = tk.Frame(body, bg=C['bg_2'])
    strip.pack(fill='x', pady=(0, px(8)))
    count_var = tk.StringVar(value=_('Entries: 0 / %d') % _D.MAX_ENTRIES)
    size_var = tk.StringVar(value=_('Size: 0 B / 32 KiB'))
    tk.Label(strip, textvariable=count_var, font=FONTS['body_strong'], bg=C['bg_2'], fg=C['fg_0']).pack(side='left', padx=(0, px(16)))
    tk.Label(strip, textvariable=size_var, font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3']).pack(side='left')
    list_wrap = Rounded(body, bg=C['bg_0'], border=C['border_2'], radius=8, padding=(px(4), px(4)), surface=C['bg_2'])
    list_wrap.pack(fill='both', expand=True, pady=(0, px(8)))
    list_canvas = tk.Canvas(list_wrap, bg=C['bg_0'], highlightthickness=0, height=px(260), bd=0)
    list_sb = XScrollbar(list_wrap, command=list_canvas.yview, surface=C['bg_0'])
    list_canvas.configure(yscrollcommand=list_sb.set)
    list_frame = tk.Frame(list_canvas, bg=C['bg_0'])
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
    valid_lbl = tk.Label(body, textvariable=valid_var, font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3'], anchor='w', justify='left')
    valid_lbl.pack(fill='x', pady=(0, px(4)))
    autowrap(valid_lbl)
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
            tk.Label(list_frame, text=_('No entries yet. Click "Add entry" to begin, or load an existing dlc_emu.ini.'), font=FONTS['caption'], bg=C['bg_0'], fg=C['fg_3']).pack(anchor='w', padx=px(12), pady=px(12))
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
        RB = C['bg_2']
        row = Rounded(list_frame, bg=RB, border=C['border_2'], radius=8, padding=(px(12), px(10)), surface=C['bg_0'])
        row.pack(fill='x', padx=px(6), pady=px(4))
        topr = tk.Frame(row, bg=RB)
        topr.pack(fill='x', pady=(0, px(8)))
        tk.Label(topr, text='#%d' % (i + 1), font=FONTS['code'], bg=RB, fg=C['fg_3']).pack(side='left', padx=(0, px(10)))
        sec_var = tk.StringVar(value=entry.section)
        sec_menu = Dropdown(topr, textvariable=sec_var, values=list(_D.SECTION_TYPES), width=8)
        sec_menu.bind('<<ComboboxSelected>>', lambda _e, _i=i, _v=sec_var: _set_section(_i, _v.get()))
        sec_menu.pack(side='left', padx=(0, px(10)))
        btxt, bkind = _badge_text(entry)
        _btone = {'high': 'success', 'medium': 'info', 'low': 'warn', 'manual': 'neutral'}.get(bkind, 'neutral')
        Badge(topr, btxt, _btone, icon=False).pack(side='left')
        IconButton(topr, 'close', lambda _i=i: _remove(_i), tooltip=_('Remove'), size='sm').pack(side='right')
        IconButton(topr, 'copy', lambda _i=i: _duplicate(_i), tooltip=_('Duplicate'), size='sm').pack(side='right')
        IconButton(topr, 'arrow_down', lambda _i=i: _move(_i, 1), tooltip=_('Move down'), size='sm').pack(side='right')
        IconButton(topr, 'arrow_up', lambda _i=i: _move(_i, -1), tooltip=_('Move up'), size='sm').pack(side='right')
        simple = tk.Frame(row, bg=RB)
        simple.pack(fill='x')
        simple.grid_columnconfigure(1, weight=1)
        tk.Label(simple, text=_('Content ID:').rstrip(':'), font=FONTS['body'], bg=RB, fg=C['fg_2']).grid(row=0, column=0, sticky='w', padx=(0, px(10)), pady=px(2))
        cid_var = tk.StringVar(value=entry.get('content_id', ''))
        cid_ent = XEntry(simple, textvariable=cid_var, mono=True)
        cid_ent.grid(row=0, column=1, sticky='ew', pady=px(2))
        status_lbl = tk.Label(simple, font=FONTS['caption'], bg=RB, anchor='w')
        status_lbl.grid(row=1, column=1, sticky='w', pady=(px(4), 0))
        mount_lbl = tk.Label(simple, font=FONTS['caption'], bg=RB, fg=C['fg_3'], anchor='w')
        mount_lbl.grid(row=2, column=1, sticky='w')
        src_lbl = tk.Label(simple, font=FONTS['caption'], bg=RB, fg=C['fg_3'], anchor='w')
        src_lbl.grid(row=3, column=1, sticky='w')
        def _refresh_simple():
            stext, skind = _entry_status(entry)
            status_lbl.configure(text=_('Status:  ') + stext, fg=C['success_hi'] if skind == 'ok' else C['warn_hi'])
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
        adv_bar = tk.Frame(row, bg=RB)
        adv_bar.pack(fill='x', pady=(px(6), 0))
        adv_btn = XButton(adv_bar, text=_('Advanced ▾').rstrip(' ▾▴'), variant='link', size='sm', icon='chevron_right', surface=RB)
        adv_btn.pack(side='left')
        def _build_adv(parent_):
            grid = tk.Frame(parent_, bg=RB)
            for ci in range(2):
                grid.grid_columnconfigure(ci * 2 + 1, weight=1, uniform='dlcf')
            for fi, fld in enumerate(_ADV_FIELDS):
                r, c = divmod(fi, 2)
                tk.Label(grid, text=fld, font=FONTS['code'], bg=RB, fg=C['fg_3']).grid(row=r, column=c * 2, sticky='w', padx=(0, px(8)), pady=px(3))
                if fld == 'download_status':
                    dv = tk.StringVar(value=entry.get(fld, ''))
                    dm = Dropdown(grid, textvariable=dv, values=[''] + list(_D.DOWNLOAD_STATUSES), width=12)
                    dm.bind('<<ComboboxSelected>>', lambda _e, _i=i, _f=fld, _v=dv: (_set_field(_i, _f, _v.get()), _refresh_simple()))
                    dm.grid(row=r, column=c * 2 + 1, sticky='ew', padx=(0, px(16)), pady=px(3))
                else:
                    fv = tk.StringVar(value=entry.get(fld, ''))
                    ent = XEntry(grid, textvariable=fv, mono=True)
                    ent.grid(row=r, column=c * 2 + 1, sticky='ew', padx=(0, px(16)), pady=px(3))
                    fv.trace_add('write', lambda *_a, _i=i, _f=fld, _v=fv: (_set_field(_i, _f, _v.get()), _refresh_simple()))
            return grid
        def _toggle_adv(_e=None):
            if adv_state['open']:
                if adv_state['frame'] is not None:
                    adv_state['frame'].pack_forget()
                    adv_state['frame'].destroy()
                    adv_state['frame'] = None
                adv_state['open'] = False
                adv_btn.configure(text=_('Advanced ▾').rstrip(' ▾▴'), icon='chevron_right')
            else:
                f = _build_adv(row)
                f.pack(fill='x', pady=(px(8), 0))
                adv_state['frame'] = f
                adv_state['open'] = True
                adv_btn.configure(text=_('Advanced ▴').rstrip(' ▾▴'), icon='chevron_down')
        adv_btn.configure(command=_toggle_adv)
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
    XButton(row1, text=_('Browse'), command=_browse_folder, icon='folder_open').pack(side='left', padx=(px(6), 0))
    def _preview():
        text_ini = _D.emit_ini(state['entries'])
        win = tk.Toplevel(parent)
        win.title(_('dlc_emu.ini preview'))
        win.configure(bg=COLORS['bg_2'])
        txt = tk.Text(win, wrap='none', font=FONTS['code'], bg=COLORS['bg_0'], relief='flat', bd=0, highlightthickness=1, highlightbackground=COLORS['border_2'], fg=COLORS['fg_1'], width=80, height=30, insertbackground=COLORS['fg_1'])
        txt.pack(fill='both', expand=True, padx=px(16), pady=px(16))
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
        dlg.configure(bg=COLORS['bg_2'])
        tk.Label(dlg, text=_('Which module build do you want to copy?'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_1']).pack(padx=16, pady=(14, 8))
        var = tk.StringVar(value='nolog')
        for val, lab in ('nolog', _('nolog (no logging — recommended)')), ('log', _('log (writes /app0/dlc_emu.log)')):
            XRadio(dlg, text=lab, value=val, variable=var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_2'], selectcolor=COLORS['bg_3'], activebackground=COLORS['bg_2'], highlightthickness=0).pack(anchor='w', padx=24)
        def _ok():
            variant['v'] = var.get()
            dlg.destroy()
        _ft = tk.Frame(dlg, bg=COLORS['bg_1'])
        _ft.pack(fill='x', side='bottom', pady=(px(14), 0))
        XButton(_ft, text=_('Continue'), command=_ok, variant='primary', width=12).pack(side='right', padx=px(16), pady=px(12))
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
        win.configure(bg=COLORS['bg_2'])
        hdr = tk.Label(win, text=_('Found %d suggestion(s). Tick the ones to add, then Apply selected.') % len(sugg), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w', justify='left', wraplength=620)
        hdr.pack(fill='x', padx=12, pady=(12, 4))
        if info.get('prefix_placeholder'):
            tk.Label(win, text=_('Note: content-id prefix is a placeholder (%s) — no CONTENT_ID/param.json id was found. Edit if needed.') % info.get('prefix', ''), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['warn'], anchor='w', justify='left', wraplength=620).pack(fill='x', padx=12, pady=(0, 4))
        if not info.get('addcont'):
            tk.Label(win, text=_('No addcont folders found. DLC-emu exposes ownership but does not create DLC files. Mount paths must already exist.'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['warn'], anchor='w', justify='left', wraplength=620).pack(fill='x', padx=12, pady=(0, 6))
        wrap = tk.Frame(win, bg=COLORS['bg_2'])
        wrap.pack(fill='both', expand=True, padx=12, pady=(0, 8))
        cv = tk.Canvas(wrap, bg=COLORS['bg_2'], highlightthickness=0, width=620, height=320)
        sb = XScrollbar(wrap, command=cv.yview, bg=COLORS['bg_2'])
        cv.configure(yscrollcommand=sb.set)
        inner = tk.Frame(cv, bg=COLORS['bg_2'])
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
        btnr = tk.Frame(win, bg=COLORS['bg_2'])
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
        XButton(btnr, text=_('Apply selected'), command=_apply_selected, variant='primary').pack(side='right')
        XButton(btnr, text=_('Cancel'), command=win.destroy).pack(side='right', padx=(0, px(6)))
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
        dlg.configure(bg=COLORS['bg_2'])
        dlg.geometry('700x520')
        dlg.resizable(True, True)
        dlg.transient(parent)
        tk.Label(dlg, text=_('SerialStation Lookup'), font=FONTS['title'], bg=COLORS['bg_2'], fg=COLORS['fg_0']).pack(anchor='w', padx=16, pady=(16, 2))
        tk.Label(dlg, text=_('Enter a game name or Title ID (e.g. PPSA04930). Results are unverified candidates — review before saving.'), font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'], justify='left', wraplength=660).pack(anchor='w', padx=16, pady=(0, 10))
        q_row = tk.Frame(dlg, bg=COLORS['bg_2'])
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
        tk.Label(dlg, textvariable=status_var, font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(fill='x', padx=16, pady=(6, 0))
        games_frame = tk.Frame(dlg, bg=COLORS['bg_2'], highlightbackground=COLORS['border_2'], highlightthickness=1)
        games_frame.pack(fill='x', padx=16, pady=(6, 0))
        tk.Label(games_frame, text=_('Games (select one, then Import):'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w', padx=8, pady=(6, 2))
        games_lb = tk.Listbox(games_frame, bg=COLORS['bg_3'], fg=COLORS['fg_1'], selectbackground=COLORS['accent'], selectforeground='#ffffff', font=FONTS['mono_sm'], relief='flat', highlightthickness=0, height=5, activestyle='none')
        games_lb.pack(fill='x', padx=8, pady=(0, 8))
        cid_outer = tk.Frame(dlg, bg=COLORS['bg_2'])
        cid_outer.pack(fill='both', expand=True, padx=16, pady=(6, 0))
        tk.Label(cid_outer, text=_('Content IDs (tick to import):'), font=(FONTS['body_strong'][0], 9), bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(anchor='w')
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
        XButton(q_row, text=_('Search'), command=_do_search, icon='search').pack(side='left')
        q_entry.bind('<Return>', _do_search)
        btn_row = tk.Frame(dlg, bg=COLORS['bg_2'])
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
        XButton(btn_row, text=_('Import selected'), command=_do_import, variant='primary').pack(side='right')
        XButton(btn_row, text=_('Close'), command=dlg.destroy).pack(side='right', padx=(0, px(6)))
    # Toolbar — discovery/import actions above the entry list
    top_actions = tk.Frame(body, bg=C['bg_2'])
    top_actions.pack(fill='x', pady=(0, px(12)), before=strip)
    XButton(top_actions, text=_('Auto Find DLC Entries'), command=_auto_find, icon='search').pack(side='left', padx=(0, px(6)))
    XButton(top_actions, text=_('SerialStation Lookup…'), command=_serialstation_lookup, icon='link').pack(side='left', padx=(0, px(6)))
    XButton(top_actions, text=_('Import DLC-emu Pack…'), command=_import_pack, icon='download').pack(side='left')
    XButton(strip, text=_('Add entry'), icon='plus', size='sm', command=_add).pack(side='right')
    # Footer — preview / export / save (primary rightmost)
    Divider(body, color=C['border_1']).pack(fill='x', pady=(px(8), px(12)))
    btns2 = tk.Frame(body, bg=C['bg_2'])
    btns2.pack(fill='x')
    XButton(btns2, text=_('Save to app0'), command=_save_to_app0, variant='primary', icon='save').pack(side='right')
    XButton(btns2, text=_('Export copy…'), command=_export_copy, icon='download').pack(side='right', padx=(0, px(6)))
    XButton(btns2, text=_('Preview INI'), command=_preview, icon='eye').pack(side='right', padx=(0, px(6)))
    XButton(btns2, text=_('Scan eboot for codes…'), command=_scan_eboot, icon='binary').pack(side='left', padx=(0, px(6)))
    XButton(btns2, text=_('Copy replacement modules…'), command=_copy_modules, icon='package').pack(side='left')
    InfoBar(body, kind='info', text=_('Required runtime files — DLC-emu only works when BOTH are installed\n✓  dlc_emu.ini — created by “Save to app0” (contains the DLC configuration)\n✓  Runtime SPRX modules (libSceAppContent.sprx, libSceGameUpdate.sprx, libSceNpEntitlementAccess.sprx) — installed by “Copy replacement modules…”\nThe INI tells DLC-emu what DLC exists. The SPRX modules make the game use that configuration. Both are required.')).pack(fill='x', pady=(px(16), 0))
    _init_folder = folder_var.get().strip()
    if _init_folder and _os.path.isdir(_init_folder):
        _load_from_folder(_init_folder)
        return
    _render_entries()


# ─────────────────────────────────────────────────────────────────────
# Language Stripper pane — warning, source, quick keep, results
# ─────────────────────────────────────────────────────────────────────
def _stat_cells(parent, specs):
    """Row of caption/value cells (overview style). specs: list of
    (caption, mono). Returns the value labels (configure(text=…))."""
    row = tk.Frame(parent, bg=C['bg_2'])
    out = []
    for i, (caption, mono) in enumerate(specs):
        row.grid_columnconfigure(i, weight=1, uniform='stc%d' % id(row))
        cell = tk.Frame(row, bg=C['bg_2'])
        cell.grid(row=0, column=i, sticky='nw',
                  padx=(0 if i == 0 else px(16), 0))
        tk.Label(cell, text=caption, font=FONTS['caption'], bg=C['bg_2'],
                 fg=C['fg_3'], anchor='w').pack(anchor='w')
        v = tk.Label(cell, text='—',
                     font=FONTS['code'] if mono else FONTS['body_strong'],
                     bg=C['bg_2'], fg=C['fg_0'], anchor='w')
        v.pack(anchor='w', pady=(px(4), 0))
        out.append(v)
    return row, out


def _build_lang_pane(parent, app):
    parent.columnconfigure(0, weight=1)

    card_outer = Card(parent, title=_('Language Stripper'),
                      subtitle=_('Remove unwanted language packs from PS5 '
                                 'game dumps to save space.'))
    card_outer.grid(row=0, column=0, sticky='nsew', pady=(0, px(16)))
    Badge(card_outer.head_actions, _('BETA'), 'warn').pack(side='right')
    _head_first(card_outer)
    body = card_outer.body

    # ── Summary strip ──
    # Selected Folder mirrors the path var live; the scan-derived
    # cells read app._adv_lang_stats when a scan backend populates it
    # (keys: languages, est_saving, files, new_size) and show "—"
    # until then. Presentation only.
    top, (c_folder, c_langs, c_sel, c_save) = _stat_cells(body, [
        (_('Selected Folder'), True), (_('Languages Found'), False),
        (_('Selected'), False), (_('Estimated Saving'), False)])
    top.pack(fill='x', pady=(0, px(4)))

    keep_var = tk.StringVar(value='')
    keep_lbl = tk.Label(body, textvariable=keep_var, font=FONTS['caption'],
                        bg=C['bg_2'], fg=C['fg_2'], anchor='w')
    keep_lbl.pack(fill='x', pady=(0, px(12)))

    def _mirror_folder(*_a):
        try:
            f = (app._adv_lang_folder_var.get() or '').strip()
            if len(f) > 26:
                f = '…' + f[-24:]
            c_folder.config(text=f or '—',
                            font=FONTS['code'] if f else FONTS['body_strong'])
        except Exception:
            pass

    def _refresh_lang_stats():
        try:
            if not c_langs.winfo_exists():
                return
            st = getattr(app, '_adv_lang_stats', None) or {}
            c_langs.config(text=str(st.get('languages', '—')))
            c_sel.config(text=str(st.get('selected', '—')))
            c_save.config(text=str(st.get('est_saving', '—')))
            keep = st.get('keep')
            keep_var.set((_('Keep') + ':  ' + ' / '.join(keep))
                         if keep else '')
            if hasattr(app, '_adv_lang_bottom_cells'):
                bc = app._adv_lang_bottom_cells
                bc['save'].config(
                    text=str(st.get('est_saving', '—')))
                bc['files'].config(text=str(st.get('files', '—')))
                bc['new'].config(text=str(st.get('new_size', '—')))
        except Exception:
            pass
        try:
            body.after(2000, _refresh_lang_stats)
        except Exception:
            pass

    app._adv_lang_refresh_stats = _refresh_lang_stats
    body.after(600, _refresh_lang_stats)

    # Warning
    warn = InfoBar(body, kind='danger',
                   title=_('Deletes files permanently — back up games first.'),
                   text=_('  May not work on all games').strip())
    warn.pack(fill='x', pady=(0, px(16)))

    # Mode (same variable/values as the old radios)
    if not hasattr(app, '_adv_lang_mode_var'):
        app._adv_lang_mode_var = tk.StringVar(
            value=app._settings.get('adv_lang_mode', 'single'))
    Segmented(body, [('single', _('Single game folder')),
                     ('scan', _('Scan all games in a folder'))],
              variable=app._adv_lang_mode_var).pack(anchor='w',
                                                    pady=(0, px(12)))

    # Games folder field
    tk.Label(body, text=_('Games folder:').rstrip(':'), font=FONTS['body'],
             bg=C['bg_2'], fg=C['fg_2'], anchor='w').pack(fill='x')
    folder_row = tk.Frame(body, bg=C['bg_2'])
    folder_row.pack(fill='x', pady=(px(4), px(12)))
    if not hasattr(app, '_adv_lang_folder_var'):
        app._adv_lang_folder_var = tk.StringVar(
            value=app._settings.get('adv_lang_folder', ''))
    _mirror_folder()
    try:
        app._adv_lang_folder_var.trace_add(
            'write', lambda *a: _mirror_folder())
    except Exception:
        pass
    XEntry(folder_row, textvariable=app._adv_lang_folder_var, mono=True
           ).pack(side='left', fill='x', expand=True)
    XButton(folder_row, text=_('Scan'), icon='search',
            command=lambda: _lang_scan(app)).pack(side='right',
                                                  padx=(px(6), 0))
    XButton(folder_row, text=_('Browse'), icon='folder_open',
            command=lambda: _lang_browse(app)).pack(side='right',
                                                    padx=(px(6), 0))

    # Quick-keep row
    chip_row = tk.Frame(body, bg=C['bg_2'])
    chip_row.pack(fill='x', pady=(0, px(12)))
    tk.Label(chip_row, text=_('Quick keep:').rstrip(':'),
             font=FONTS['body'], bg=C['bg_2'], fg=C['fg_2']
             ).pack(side='left', padx=(0, px(10)))
    chips = [
        ('GB', _('EN only'),  ['en']),
        ('JP', _('JA only'),  ['ja']),
        ('DE', _('DE only'),  ['de']),
        ('FR', _('FR only'),  ['fr']),
        ('ES', _('ES only'),  ['es']),
        ('',   _('EN+JA'),    ['en', 'ja']),
        ('EU', _('EU only'),  ['en', 'de', 'fr', 'es', 'it']),
    ]
    for _prefix, label, keep_codes in chips:
        XButton(chip_row, text=label, size='sm',
                command=lambda codes=keep_codes: _lang_quick_keep(app, codes)
                ).pack(side='left', padx=(0, px(6)))

    # Result area (inset well) — empty state replaced when results arrive
    result_outer = Rounded(body, bg=C['bg_0'], border=C['border_2'],
                           radius=8, padding=(px(8), px(8)),
                           surface=C['bg_2'], height=px(200))
    result_outer.pack(fill='both', expand=True, pady=(0, px(16)))
    result_outer.pack_propagate(False)
    app._adv_lang_result_frame = result_outer
    _show_lang_empty_state(app)

    # ── Bottom summary strip — scan-derived, "—" until a scan
    # populates app._adv_lang_stats ──
    bot, (b_save, b_files, b_new) = _stat_cells(body, [
        (_('Space To Save'), False), (_('Files To Remove'), False),
        (_('New Estimated Size'), False)])
    bot.pack(fill='x', pady=(0, px(16)))
    app._adv_lang_bottom_cells = {'save': b_save, 'files': b_files,
                                  'new': b_new}

    # Footer: Keep all / Keep none on the left; destructive action right
    Divider(body, color=C['border_1']).pack(fill='x', pady=(0, px(12)))
    footer = tk.Frame(body, bg=C['bg_2'])
    footer.pack(fill='x')
    XButton(footer, text=_('Keep all'), icon='check',
            command=lambda: _lang_keep_all(app)).pack(side='left',
                                                      padx=(0, px(6)))
    XButton(footer, text=_('Keep none'), icon='close',
            command=lambda: _lang_keep_none(app)).pack(side='left')
    strip_btn = XButton(footer, text=_('Strip Selected Languages'),
                        variant='danger', icon='trash',
                        command=lambda: _lang_strip(app))
    strip_btn.pack(side='right')


def _show_lang_empty_state(app):
    """Render the empty-state placeholder in the result area."""
    frame = app._adv_lang_result_frame
    for w in list(frame.winfo_children()):
        if w in getattr(frame, '_xb_deco', []):
            continue
        w.destroy()
    XEmptyState(frame, icon='globe', title=_('No scan yet'),
                text=_('Select a games folder and click Scan to see '
                       'language packs.'), bg=C['bg_0']
                ).pack(fill='both', expand=True)
    try:
        frame.raise_decorations()
    except Exception:
        pass


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
# Build Parameters pane — 2x2 card grid + full-width assistant row
# ─────────────────────────────────────────────────────────────────────
def _build_params_pane(parent, app):
    # A 2x2 card grid on top with the recommendation assistant beneath it
    # (three columns: setup recommendation · current profile · why these
    # defaults).
    grid = tk.Frame(parent, bg=C['bg_1'])
    grid.pack(fill='x')
    grid.columnconfigure(0, weight=1, uniform='card')
    grid.columnconfigure(1, weight=1, uniform='card')

    _build_card_fs    (grid, app, row=0, col=0)
    _build_card_image (grid, app, row=0, col=1)
    _build_card_perf  (grid, app, row=1, col=0)
    _build_card_ffpkg (grid, app, row=1, col=1)

    assistant = tk.Frame(parent, bg=C['bg_1'])
    assistant.pack(fill='x')
    assistant.columnconfigure(0, weight=3, uniform='asst')
    assistant.columnconfigure(1, weight=2, uniform='asst')
    assistant.columnconfigure(2, weight=2, uniform='asst')

    col0 = tk.Frame(assistant, bg=C['bg_1'])
    col0.grid(row=0, column=0, sticky='nsew', padx=(0, px(16)))
    _build_recco_card(col0, app)
    col1 = tk.Frame(assistant, bg=C['bg_1'])
    col1.grid(row=0, column=1, sticky='nsew', padx=(0, px(16)))
    _build_profile_card(col1, app)
    col2 = tk.Frame(assistant, bg=C['bg_1'])
    col2.grid(row=0, column=2, sticky='nsew')
    _build_help_card(col2, app)


# ─────────────────────────────────────────────────────────────────────
# Cards — each is a titled Card with a body of setting rows
# ─────────────────────────────────────────────────────────────────────
def _card_shell(parent, row, col, icon, title, subtitle):
    card = Card(parent, title=title, subtitle=subtitle)
    card.grid(row=row, column=col, sticky='nsew',
              padx=(0 if col == 0 else px(8), px(8) if col == 0 else 0),
              pady=(0, px(16)))
    return card.body


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
        '⚡', _('Performance'),
        _('Copy concurrency, retries, benchmark.'))

    # Recommended threads meta line — only if HW detected
    hw = app._adv_v2_hw
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
    meta = None
    if hw:
        meta = _('Recommended: 1 — single-threaded avoids '
                 'fragmenting the image')

    _setting_row(body, app,
        label=_('Threads'),
        kind='int',
        var=app._adv_threads_var,
        default='1',
        help_text=_('1 = sequential — recommended, avoids fragmenting the image. Higher copies faster but fragments output.'),
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

    # Write benchmark — a setting row with a value badge + Run button
    ctrl = setting_row(body, _('Write speed'),
                       _('1 GiB sequential test on the temp drive.'))
    bench_val = app._settings.get('adv_write_benchmark', '—')
    bench_pill = Badge(ctrl, bench_val, 'neutral', mono=True)
    bench_pill.pack(side='left', padx=(0, px(8)))
    XButton(ctrl, text=_('Run'), icon='play', size='sm',
            command=lambda: _run_benchmark(app, bench_pill)
            ).pack(side='left')


# ── Image options ────────────────────────────────────────────────────
def _build_card_image(parent, app, row, col):
    body = _card_shell(parent, row, col,
        '⚙', _('Image options'),
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
# SettingRow primitive — label + control + restore-default link
# ─────────────────────────────────────────────────────────────────────
def _setting_row(parent, app, label, kind, var=None,
                 default=None, options=None, help_text=None, meta=None,
                 value_text=None, severity=None, on_change=None):
    """Build one setting row inside a card body.

    kind:
      'select'  — dropdown with [(value, display)] options
      'int'     — text field validated to integer
      'text'    — free-text field
      'toggle'  — on/off switch (saves immediately)
      'locked'  — read-only field with a lock icon
    severity: None or 'danger' — danger rows get a danger badge next to
      the label and danger-tinted help text.
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

    help_plain = help_text
    if help_text and severity == 'danger':
        from ui.icons import strip_glyphs
        help_plain = strip_glyphs(help_text)
    ctrl_col = setting_row(parent, label,
                           None if severity == 'danger' else help_plain)
    lcol = ctrl_col.text_col
    bg = C['bg_2']
    if severity == 'danger':
        ctrl_col.label.pack_forget()
        lrow = tk.Frame(lcol, bg=bg)
        lrow.pack(fill='x')
        tk.Label(lrow, text=label, font=FONTS['body'], bg=bg, fg=C['fg_1'],
                 anchor='w').pack(side='left')
        Badge(lrow, _('destructive'), 'danger', icon=False).pack(
            side='left', padx=(px(8), 0))
        if help_plain:
            hrow = tk.Frame(lcol, bg=bg)
            hrow.pack(fill='x', pady=(px(2), 0))
            IconLabel(hrow, 'warning', 12, C['danger'], bg).pack(
                side='left', anchor='n', pady=(px(2), 0), padx=(0, px(6)))
            hl = tk.Label(hrow, text=help_plain, font=FONTS['caption'],
                          bg=bg, fg=C['danger_hi'], anchor='w',
                          justify='left')
            hl.pack(side='left', fill='x', expand=True)
            autowrap(hl)
    if meta:
        mrow = tk.Frame(lcol, bg=bg)
        mrow.pack(fill='x', pady=(px(4), 0))
        IconLabel(mrow, 'lightbulb', 12, C['fg_3'], bg).pack(
            side='left', padx=(0, px(6)))
        tk.Label(mrow, text=meta, font=FONTS['caption'], bg=bg,
                 fg=C['fg_2'], anchor='w').pack(side='left')

    ctrl_widget = None

    if kind == 'select':
        display_var = tk.StringVar(value='')
        # Resolve current value to its display label
        cur = var.get() if var else default
        display_map = dict(options)
        display_var.set(display_map.get(cur, cur))
        values = [d for (_v, d) in options]
        ctrl_widget = Dropdown(ctrl_col, textvariable=display_var,
                               values=values, width=12)
        ctrl_widget.pack(side='right', padx=(0, px(3)))

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

        # Keep the display in step with programmatic changes (reset /
        # restore default).
        def _sync_disp(*_a):
            try:
                display_var.set(display_map.get(var.get(), var.get()))
            except Exception:
                pass
        if var is not None:
            try:
                var.trace_add('write', _sync_disp)
            except Exception:
                pass

    elif kind == 'int' or kind == 'text':
        ctrl_widget = XEntry(ctrl_col, textvariable=var, width=10)
        ctrl_widget.pack(side='right', padx=(0, px(3)))

        def _entry_save(_e=None):
            try:
                _save_handler()
            except Exception:
                pass
        ctrl_widget.bind('<FocusOut>', _entry_save)
        ctrl_widget.bind('<Return>',   _entry_save)

    elif kind == 'toggle':
        def _flip():
            try:
                _save_handler()
            except Exception:
                pass
        ctrl_widget = TSwitch(ctrl_col, variable=var, command=_flip)
        ctrl_widget.pack(side='right')

    elif kind == 'locked':
        lock_pill = IconLabel(ctrl_col, 'lock', 16, C['fg_3'], bg)
        lock_pill.pack(side='right', padx=(px(8), 0))
        XTooltip(lock_pill, _('Locked. 4096 produces broken images on Windows.'))
        ent = XEntry(ctrl_col, width=10)
        ent.insert(0, value_text or '')
        ent.configure(state='disabled')
        ent.pack(side='right')
        ctrl_widget = lock_pill

    # Inline "Restore default" link — only shown when the value differs
    # from its default (sits just left of the control)
    if default is not None and var is not None:
        def_inline = tk.Frame(lcol, bg=bg)

        def _update_def_indicator(*_a):
            try:
                cur = var.get()
            except Exception:
                return
            try:
                if not def_inline.winfo_exists():
                    return
            except Exception:
                return
            for w in def_inline.winfo_children():
                w.destroy()
            if str(cur) == str(default):
                def_inline.pack_forget()
                return
            def_inline.pack(anchor='w', pady=(px(4), 0))

            def _revert():
                var.set(default)
                try:
                    _save_handler()
                except Exception:
                    pass
                _update_def_indicator()
            # Off-default — revert link
            XButton(def_inline,
                    text=_('Restore default') + ' (' + str(default) + ')',
                    variant='link', size='sm', icon='undo', surface=bg,
                    command=_revert).pack(side='left')
        _update_def_indicator()
        try:
            var.trace_add('write', _update_def_indicator)
        except Exception:
            pass
    return ctrl_widget


# ─────────────────────────────────────────────────────────────────────
# Assistant row — Recommended-for-your-setup · profile · why defaults
# ─────────────────────────────────────────────────────────────────────
def _overline(parent, text, bg=None):
    bg = bg or C['bg_2']
    lbl = tk.Label(parent, text=text.upper(), font=FONTS['overline'],
                   bg=bg, fg=C['fg_4'], anchor='w')
    return lbl


def _build_profile_card(parent, app):
    """Current Build Profile — live mirror of the five key params with
    per-row status icons and an overall Optimal / Custom / Risky badge.
    Success = matches the hardware recommendation, warn = custom,
    danger = the known-risky 4096 sector size. Read-only."""
    hw = getattr(app, '_adv_v2_hw', None)
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
    rec_threads = str(hw['recommended_threads']) if hw else None

    card = Card(parent, padding=16)
    card.pack(fill='x', pady=(0, px(16)))

    hrow = tk.Frame(card, bg=C['bg_2'])
    hrow.pack(fill='x', pady=(0, px(10)))
    _overline(hrow, _('CURRENT BUILD PROFILE')).pack(side='left')
    status_pill = Badge(hrow, '', 'neutral')
    status_pill.pack(side='right')

    body = tk.Frame(card, bg=C['bg_2'])
    body.pack(fill='x')

    specs = [
        ('cluster', _('Cluster Size'), app._adv_cluster_var),
        ('sector', _('Sector Size'), app._adv_sector_var),
        ('threads', _('Threads'), app._adv_threads_var),
        ('retries', _('Robocopy file retries'), app._adv_retries_var),
        ('inode', _('Bytes / Inode'), app._adv_ffpkg_inode_var),
    ]
    rows = {}
    for key, label, var in specs:
        r = tk.Frame(body, bg=C['bg_2'])
        r.pack(fill='x', pady=(0, px(6)))
        dot = IconLabel(r, 'dot', 16, C['fg_5'], C['bg_2'])
        dot.pack(side='left', padx=(0, px(6)))
        tk.Label(r, text=label, font=FONTS['body'], bg=C['bg_2'],
                 fg=C['fg_2'], anchor='w').pack(side='left')
        val = tk.Label(r, text='—', font=FONTS['code'],
                       bg=C['bg_2'], fg=C['fg_0'], anchor='e')
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

    dotc = {'ok': ('success', C['success']), 'custom': ('warning', C['warn']),
            'risky': ('error', C['danger']), 'neutral': ('dot', C['fg_5'])}

    def _sync(*_a):
        try:
            worst = 'ok'
            for key, (var, dot, val) in rows.items():
                if not val.winfo_exists():
                    return
                raw = (var.get() or '').strip()
                val.config(text=raw or '—')
                j = _judge(key, raw)
                ic, col = dotc[j]
                dot.set(ic, fg=col)
                if j == 'risky':
                    worst = 'risky'
                elif j == 'custom' and worst != 'risky':
                    worst = 'custom'
            if worst == 'risky':
                status_pill.set(_('RISKY'), 'danger')
            elif worst == 'custom':
                status_pill.set(_('CUSTOM'), 'warn')
            else:
                status_pill.set(_('OPTIMAL'), 'success')
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
    # hw may be None (detection still running in the background) or a
    # {'_failed': True} sentinel (detection finished but found nothing). Only
    # treat it as real hardware when it actually carries a model.
    _detecting = (hw is None)
    if not isinstance(hw, dict) or not hw.get('model'):
        hw = None
    card = Card(parent, padding=16)
    card.pack(fill='x', pady=(0, px(16)))
    bg = C['bg_2']

    # Header
    head = tk.Frame(card, bg=bg)
    head.pack(fill='x')
    IconLabel(head, 'lightbulb', 16, C['fg_3'], bg).pack(
        side='left', anchor='n', padx=(0, px(10)))
    head_col = tk.Frame(head, bg=bg)
    head_col.pack(side='left', fill='x', expand=True)
    _overline(head_col, _('RECOMMENDED FOR YOUR SETUP')).pack(anchor='w')
    if _detecting:
        sub_text = _('Detecting hardware…')
    elif hw:
        sub_text = _('Detected: %s · %s') % (hw['model'], hw['kind'])
    else:
        sub_text = _('HW detection failed')
    sub = tk.Label(head_col, text=sub_text, font=FONTS['body'], bg=bg,
                   fg=C['fg_2'], justify='left', anchor='w')
    sub.pack(fill='x', pady=(px(4), 0))
    autowrap(sub)

    # Compact HW stat cells under the header
    if hw:
        perf_map = {'NVMe Gen4 SSD': (_('Excellent'), 'success'),
                    'SATA SSD': (_('Good'), 'success'),
                    'HDD': (_('Limited'), 'warn')}
        perf, perf_tone = perf_map.get(hw.get('kind', ''),
                                       (_('Unknown'), 'neutral'))
        statrow, cells = _stat_cells(card, [(_('Threads'), False),
                                            (_('Sector'), False),
                                            (_('Performance'), False),
                                            (_('Health'), False)])
        statrow.pack(fill='x', pady=(px(14), 0))
        cells[0].config(text=str(hw.get('recommended_threads', '—')))
        cells[1].config(text='512')
        cells[2].config(text=perf)
        cells[3].config(text=_('Optimal'))

    if hw:
        # Detail list
        Divider(card, color=C['border_1']).pack(fill='x',
                                                pady=(px(14), px(10)))
        details = tk.Frame(card, bg=bg)
        details.pack(fill='x', pady=(0, px(10)))
        cur_threads = app._adv_threads_var.get()
        _recco_row(details, 'THREADS', str(hw['recommended_threads']),
                   cur_threads, str(hw['recommended_threads']))
        _recco_row(details, 'CLUSTER', _('Auto (64K)'),
                   app._adv_cluster_var.get(), 'Auto')
        _recco_row(details, 'SECTOR', '512',
                   app._adv_sector_var.get(), '512')
        bench_val = app._settings.get('adv_write_benchmark', '—')
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
        XButton(card, text=_('Apply recommended'), icon='zap',
                command=_apply_reco).pack(anchor='w', pady=(px(4), 0))


def _recco_row(parent, eyebrow, recommended_val, current_val, matches_val):
    bg = C['bg_2']
    row = tk.Frame(parent, bg=bg)
    row.pack(fill='x', pady=(0, px(6)))
    tk.Label(row, text=eyebrow.capitalize(), font=FONTS['body'], bg=bg,
             fg=C['fg_3'], width=12, anchor='w').pack(side='left')
    tk.Label(row, text=str(recommended_val), font=FONTS['body_strong'],
             bg=bg, fg=C['fg_0'], anchor='w').pack(side='left')
    if current_val is not None and matches_val is not None:
        if str(current_val) == str(matches_val):
            Badge(row, _('matches'), 'success').pack(side='left',
                                                     padx=(px(8), 0))
        else:
            Badge(row, _('currently ') + str(current_val), 'warn').pack(
                side='left', padx=(px(8), 0))


def _build_help_card(parent, app):
    card = Card(parent, padding=16)
    card.pack(fill='x')
    bg = C['bg_2']
    hrow = tk.Frame(card, bg=bg)
    hrow.pack(fill='x', pady=(0, px(8)))
    IconLabel(hrow, 'help', 16, C['fg_3'], bg).pack(side='left',
                                                    padx=(0, px(8)))
    _overline(hrow, _('WHY THESE DEFAULTS?')).pack(side='left')
    text1 = _(
        'PS5 expects 64K clusters and 512-byte sectors — anything '
        'else needs config.ini edits or the image won\'t mount.')
    l1 = tk.Label(card, text=text1, font=FONTS['caption'], bg=bg,
                  fg=C['fg_2'], anchor='w', justify='left')
    l1.pack(fill='x', pady=(0, px(6)))
    autowrap(l1)
    text2 = _('More threads = faster but more fragmentation.')
    l2 = tk.Label(card, text=text2, font=FONTS['caption'], bg=bg,
                  fg=C['fg_2'], anchor='w', justify='left')
    l2.pack(fill='x')
    autowrap(l2)


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
    return XButton(parent, text=text, command=command)


def _accent_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='primary')


def _danger_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='danger')
