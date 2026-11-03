"""
ui/tab_settings.py — Settings tab (design-system pass, Farewell Edition).

Layout (reads like Windows 11 Settings):

    ┌─ rail ──────────────┬─ pane ──────────────────────────────────────┐
    │ SETTINGS            │  PS5 FTP                         (header)   │
    │ ▌ OSFMount          │  Connection used by Klog, FTP browser…      │
    │   Build             │  ┌ overview ─────────────────────────────┐  │
    │   Logs              │  │ PS5 IP   FTP port   Auto upload  …     │  │
    │ ▌ PS5 FTP     ←     │  └───────────────────────────────────────┘  │
    │   Notifications     │  ┌ Connection ───────────────────────────┐  │
    │   Theme             │  │ Console IP          [field] [Detect]  │  │
    │   Auto-Shutdown     │  │ ───────────────────────────────────── │  │
    │                     │  │ Auto-upload after build     ( ●) On   │  │
    └─────────────────────┴──┴───────────────────────────────────────┴──┘

The rail is the main file's `LeftRail`. Each pane is a page header plus a
stack of design-system cards; each card is a list of settings rows (label
and description on the left, control on the right, hairline dividers).
Instant on/off settings use a Switch, choices a Dropdown, paths a field
with Browse.

Backwards compat: every `app.*` setting var/widget the existing callbacks
read is preserved with the same name. Nothing in the worker/save logic is
changed. The small row/switch/overview helpers at the top are shared with
ui/tab_language.py and ui/tab_advanced.py.
"""

import os

import tkinter as tk
from ui.shared.widgets import (
    Button as XButton, Check as XCheck, Radio as XRadio,
    TextField as XEntry, Scrollbar as XScrollbar,
    Switch, Dropdown, Badge, Card, Rounded, InfoBar, PageHeader, Divider,
    IconLabel, EmptyState as XEmptyState, Tooltip as XTooltip, autowrap)
from tkinter import ttk

from tkinter_theme import COLORS, FONTS, px, surface_of

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, LeftRail

from ui.shared.scroll import attach_scroll

C = COLORS


# ─────────────────────────────────────────────────────────────────────────
# Shared helpers (also used by tab_language / tab_advanced)
# ─────────────────────────────────────────────────────────────────────────
class TSwitch(Switch):
    """Design-system Switch whose On/Off state text goes through _()."""

    def __init__(self, master=None, tooltip=None, **kw):
        super().__init__(master, **kw)
        if self._lbl is not None:
            self._lbl.configure(width=max(3, len(_('On')), len(_('Off'))))
        if tooltip:
            XTooltip(self._track, tooltip)

    def _draw(self):
        super()._draw()
        if getattr(self, '_lbl', None) is not None:
            self._lbl.configure(text=_('On') if self.is_on() else _('Off'))


def setting_row(parent, label, description=None, stacked=False):
    """One settings row inside a card body.

    Label (body, fg_1) and description (caption, fg_3) on the left, the
    returned control frame on the right — or, with stacked=True, the
    control frame spans the width under the text (paths, radio lists).
    Rows after the first are separated by a border_1 hairline.
    """
    bg = surface_of(parent)
    n = getattr(parent, '_xs_rows', 0)
    parent._xs_rows = n + 1
    if n:
        Divider(parent, color=C['border_1']).pack(fill='x')
    row = tk.Frame(parent, bg=bg)
    row.pack(fill='x', pady=(px(12) if n else px(2), px(12)))
    if stacked:
        txt = tk.Frame(row, bg=bg)
        txt.pack(fill='x')
        ctrl = tk.Frame(row, bg=bg)
        ctrl.pack(fill='x', pady=(px(10), 0))
    else:
        ctrl = tk.Frame(row, bg=bg)
        ctrl.pack(side='right', padx=(px(24), 0))
        txt = tk.Frame(row, bg=bg)
        txt.pack(side='left', fill='x', expand=True)
    lbl = tk.Label(txt, text=label, font=FONTS['body'], bg=bg, fg=C['fg_1'],
                   anchor='w', justify='left')
    lbl.pack(fill='x')
    autowrap(lbl)
    if description:
        d = tk.Label(txt, text=description, font=FONTS['caption'], bg=bg,
                     fg=C['fg_3'], anchor='w', justify='left')
        d.pack(fill='x', pady=(px(2), 0))
        autowrap(d)
    ctrl.row = row
    ctrl.text_col = txt
    ctrl.label = lbl
    return ctrl


def settings_card(parent, title, subtitle=None, pady=(0, 16)):
    """A titled design-system Card packed full width; returns the card
    (rows go into card.body, badges into card.head_actions)."""
    card = Card(parent, title=title, subtitle=subtitle)
    card.pack(fill='x', pady=(px(pady[0]), px(pady[1])))
    try:
        # Head actions (badges) get layout priority over the title column.
        card.head_actions.pack_configure(before=card.title_label.master)
    except Exception:
        pass
    return card


def overview_strip(parent, items, pady=(0, 16)):
    """Overview card of caption/value cells at the top of a pane.

    `items` = list of (key, caption). Returns {key: setter} where
    setter(text, kind=None) updates the value; kind 'ok'/'warn' shows the
    value as a status badge (icon + word), anything else as plain fg_0
    text (`mono=True` cells use the code font for paths/IDs)."""
    card = Card(parent, padding=16)
    card.pack(fill='x', pady=(px(pady[0]), px(pady[1])))
    grid = tk.Frame(card, bg=C['bg_2'])
    grid.pack(fill='x')
    setters = {}
    for i, item in enumerate(items):
        key, caption = item[0], item[1]
        mono = len(item) > 2 and item[2]
        grid.columnconfigure(i, weight=1, uniform='ovw')
        cell = tk.Frame(grid, bg=C['bg_2'])
        cell.grid(row=0, column=i, sticky='nw',
                  padx=(0 if i == 0 else px(16), 0))
        tk.Label(cell, text=caption, font=FONTS['caption'], bg=C['bg_2'],
                 fg=C['fg_3'], anchor='w').pack(anchor='w')
        val = tk.Label(cell, text='—',
                       font=FONTS['code'] if mono else FONTS['body_strong'],
                       bg=C['bg_2'], fg=C['fg_0'], anchor='w')
        val.pack(anchor='w', pady=(px(4), 0))
        badge = Badge(cell, '', 'neutral')

        def _set(text, kind=None, _v=val, _b=badge,
                 _vf=FONTS['code'] if mono else FONTS['body_strong']):
            try:
                if not _v.winfo_exists():
                    return
                text = text if text not in (None, '') else '—'
                if kind in ('ok', 'warn'):
                    _v.pack_forget()
                    _b.set(str(text), 'success' if kind == 'ok' else 'warn')
                    _b.pack(anchor='w', pady=(px(4), 0))
                else:
                    _b.pack_forget()
                    _v.config(text=text,
                              font=_vf if text != '\u2014'
                              else FONTS['body_strong'])
                    _v.pack(anchor='w', pady=(px(4), 0))
            except Exception:
                pass
        setters[key] = _set
    return setters


def status_line(parent, var, bg=None):
    """Caption status line bound to a StringVar whose text may start with
    a legacy ✓/✗ glyph: the glyph becomes a status icon."""
    bg = bg or surface_of(parent)
    row = tk.Frame(parent, bg=bg)
    ic = IconLabel(row, 'circle', 16, C['fg_4'], bg)
    lbl = tk.Label(row, text='', font=FONTS['caption'], bg=bg, fg=C['fg_3'],
                   anchor='w', justify='left')
    lbl.pack(side='left', fill='x', expand=True)
    autowrap(lbl)

    def _sync(*_a):
        try:
            if not lbl.winfo_exists():
                return
            t = (var.get() or '').strip()
            if not t:
                ic.pack_forget()
                lbl.config(text='')
                return
            if t[:1] in ('✓', '✔'):
                ic.set('success', fg=C['success'])
                t = t[1:].strip()
            elif t[:1] in ('✗', '✕', '✖'):
                ic.set('warning', fg=C['warn'])
                t = t[1:].strip()
            else:
                ic.set('info', fg=C['fg_4'])
            ic.pack(side='left', padx=(0, px(8)), before=lbl)
            lbl.config(text=t)
        except Exception:
            pass
    _sync()
    try:
        var.trace_add('write', _sync)
    except Exception:
        pass
    return row


def _field(parent, var, readonly=False, width=None, mono=True):
    """Rounded 32px design-system field (paths are monospace)."""
    kw = dict(textvariable=var, mono=mono)
    if readonly:
        kw['state'] = 'readonly'
    if width is not None:
        kw['width'] = width
    return XEntry(parent, **kw)


def _btn(parent, text, command, variant='secondary', icon=None, size='md'):
    return XButton(parent, text=text, command=command, variant=variant,
                   icon=icon, size=size)


# Legacy helper names kept (design-system widgets now).
def _dark_entry(parent, var, readonly=False, width=None):
    return _field(parent, var, readonly=readonly, width=width)


def _ghost_btn(parent, text, command):
    return _btn(parent, text, command)


def _accent_btn(parent, text, command):
    return _btn(parent, text, command, variant='primary')


def _danger_btn(parent, text, command):
    return _btn(parent, text, command, variant='danger')


# ─────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────
def build_settings_tab(parent, app):
    """Settings tab — left rail + per-section pane."""
    parent.configure(bg=C['bg_1'])

    # ── Left rail ──
    app._settings_rail = LeftRail(parent, rail_width=210)
    rail = app._settings_rail
    rail.add_section_label(_('SETTINGS'))

    section_specs = [
        ('osfmount',      '\U0001f4be   OSFMount',
         _('OSFMount'),
         _('Tell the app where your OSFMount executable lives.')),
        ('build',         '\U0001f527   Build',
         _('Build'),
         _('Temp folder, retry policy, and disk-image build defaults.')),
        ('logs',          '\U0001f4c4   Logs',
         _('Logs'),
         _('Where build/upload logs are written, and how to clear them.')),
        ('ftp',           '\U0001f4e1   PS5 FTP',
         _('PS5 FTP'),
         _('Connection used by Klog, FTP browser, payload sender, '
           'and remote sync.')),
        ('notifications', '\U0001f514   Notifications',
         _('Notifications'),
         _('Sound and toast preferences for completed tasks.')),
        ('theme',         '\U0001f3a8   Theme',
         _('Theme'),
         _('The app ships with a dark theme. More variants are planned.')),
        ('shutdown',      '\U0001f4a4   Auto-Shutdown',
         _('Auto-Shutdown'),
         _('Automatically shut down, restart, or sleep the PC after '
           'a task completes.')),
    ]
    panes = {}
    for key, rail_label, head_title, head_desc in section_specs:
        host = rail.add_section(key, rail_label)
        pane = _make_pane(host, head_title, head_desc)
        panes[key] = pane

    # Build each section
    _build_osfmount(panes['osfmount'], app)
    _build_build(panes['build'], app)
    _build_logs(panes['logs'], app)
    _build_ftp(panes['ftp'], app)
    _build_notifications(panes['notifications'], app)
    _build_theme(panes['theme'], app)
    _build_shutdown(panes['shutdown'], app)

    # Restore last selected rail entry
    last = app._settings.get('settings_active_rail', 'osfmount')
    try:
        rail.show(last)
    except Exception:
        rail.show_first()

    # Persist rail choice
    def _remember(k):
        app._settings['settings_active_rail'] = k
        try:
            save_settings(app._settings)
        except Exception:
            pass
    for key, btn, _frame in rail._sections:
        btn.bind('<Button-1>',
            lambda e, k=key: (rail.show(k), _remember(k)),
            add='+')


# ─────────────────────────────────────────────────────────────────────────
# Pane scaffolding — page header + scrollable card stack
# ─────────────────────────────────────────────────────────────────────────
_PANE_MAX_W = 1040


def _make_pane(host, title, description):
    """Build the per-section pane: page header + scrollable body.

    Returns the body Frame; cards pack into it directly. `body.header` is
    the PageHeader (its `.actions` slot takes header buttons/badges)."""
    host.configure(bg=C['bg_1'])
    head = PageHeader(host, title, description, bg=C['bg_1'])
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    wrap = tk.Frame(host, bg=C['bg_1'])
    wrap.pack(fill='both', expand=True)
    cv = tk.Canvas(wrap, bg=C['bg_1'], highlightthickness=0, bd=0)
    sb = XScrollbar(wrap, command=cv.yview, surface=C['bg_1'])
    cv.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    cv.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(cv, bg=C['bg_1'])
    tag = 'inner_' + str(id(inner))
    # A fixed initial width: while the pane is hidden the canvas gets no
    # <Configure>, and an unsized window would follow its content's
    # requested width — which wrapping labels change (layout feedback loop).
    cv.create_window((0, 0), window=inner, anchor='nw', tags=tag,
                     width=px(860))
    inner.bind('<Configure>',
        lambda e, c=cv: c.configure(scrollregion=c.bbox('all')))
    cv.bind('<Configure>',
        lambda e, c=cv, t=tag: c.itemconfig(
            t, width=min(e.width, px(_PANE_MAX_W))))
    attach_scroll(cv)

    body = tk.Frame(inner, bg=C['bg_1'])
    body.pack(fill='both', expand=True, padx=px(24), pady=(0, px(24)))
    body.header = head
    return body


# Back-compat name for the old summary strip.
def _pane_summary(body, items):
    return overview_strip(body, items)


# ─────────────────────────────────────────────────────────────────────────
# OSFMOUNT section
# ─────────────────────────────────────────────────────────────────────────
def _osf_file_version(path):
    """File version of a Windows PE (osfmount.com) via the version
    API. Returns 'a.b.c' or None. Fully guarded — returns None on any
    non-Windows host or unexpected binary."""
    try:
        import ctypes
        size = ctypes.windll.version.GetFileVersionInfoSizeW(path, None)
        if not size:
            return None
        buf = ctypes.create_string_buffer(size)
        if not ctypes.windll.version.GetFileVersionInfoW(
                path, None, size, buf):
            return None
        r = ctypes.c_void_p()
        ln = ctypes.c_uint()
        if not ctypes.windll.version.VerQueryValueW(
                buf, '\\', ctypes.byref(r), ctypes.byref(ln)):
            return None
        if not r.value or ln.value < 16:
            return None
        arr = ctypes.cast(r, ctypes.POINTER(ctypes.c_uint32 * 4)).contents
        ms, ls = arr[2], arr[3]
        return '%d.%d.%d' % (ms >> 16, ms & 0xFFFF, ls >> 16)
    except Exception:
        return None


def _pe_arch(path):
    """CPU architecture of a PE binary from its COFF header. Two tiny
    reads; guarded; returns 'x64'/'x86'/'ARM64' or None."""
    try:
        with open(path, 'rb') as f:
            if f.read(2) != b'MZ':
                return None
            f.seek(0x3C)
            peoff = int.from_bytes(f.read(4), 'little')
            f.seek(peoff)
            if f.read(4) != b'PE\x00\x00':
                return None
            machine = int.from_bytes(f.read(2), 'little')
        return {0x8664: 'x64', 0x014C: 'x86',
                0xAA64: 'ARM64'}.get(machine)
    except Exception:
        return None


def _build_osfmount(body, app):
    app._osf_found_var = tk.StringVar(value='')

    # Overview — Version / Status / Last checked / Architecture / Path
    s = overview_strip(body, [('version', _('Version')),
                              ('status', _('Status')),
                              ('checked', _('Last checked')),
                              ('arch', _('Architecture')),
                              ('path', _('Detected Path'), True)])

    def _refresh_summary(*_a):
        import time as _t
        try:
            path = (app._osfmount_path_var.get() or '').strip()
        except Exception:
            path = ''
        found = bool(path) and os.path.isfile(path)
        if found:
            s['status'](_('Detected'), 'ok')
            s['version'](_osf_file_version(path))
            s['arch'](_pe_arch(path))
            sp = path
            if len(sp) > 22:
                sp = '…' + sp[-21:]
            s['path'](sp)
        else:
            s['status'](_('Not found'), 'warn')
            s['version']('—')
            s['arch']('—')
            s['path']('—')
        s['checked'](_t.strftime('%d/%m/%Y %H:%M'))

    _refresh_summary()
    try:
        app._osfmount_path_var.trace_add(
            'write', lambda *a: _refresh_summary())
        app._osf_found_var.trace_add(
            'write', lambda *a: _refresh_summary())
    except Exception:
        pass

    card = settings_card(body, _('OSFMount executable'))
    ctrl = setting_row(card.body, _('Path to osfmount.com'),
        _('Used to mount/unmount disk images during the build.'),
        stacked=True)
    _field(ctrl, app._osfmount_path_var).pack(side='left', fill='x',
                                             expand=True)
    _btn(ctrl, _('Detect'), app._detect_osfmount, icon='search').pack(
        side='right', padx=(px(6), 0))
    _btn(ctrl, _('Browse'), app._browse_osfmount, icon='folder_open').pack(
        side='right', padx=(px(6), 0))

    # Detection result under the field
    status_line(ctrl.row, app._osf_found_var).pack(fill='x', pady=(px(10), 0))

    app.after(300, app._refresh_osf_status)


# ─────────────────────────────────────────────────────────────────────────
# BUILD section
# ─────────────────────────────────────────────────────────────────────────
def _build_build(body, app):
    # Overview — Temp usage / Retries / Retry wait / App version
    s = overview_strip(body, [('temp', _('Temp Usage'), True),
                              ('retries', _('Retries')),
                              ('wait', _('Retry Wait')),
                              ('ver', _('App Version'), True)])
    s['ver']('v' + APP_VERSION)

    def _mirror_retry(*_a):
        try:
            s['retries'](app._adv_retries_var.get().strip() or '3')
            s['wait']((app._adv_retry_wait_var.get().strip() or '3')
                      + 's')
        except Exception:
            pass
    _mirror_retry()
    try:
        app._adv_retries_var.trace_add(
            'write', lambda *a: _mirror_retry())
        app._adv_retry_wait_var.trace_add(
            'write', lambda *a: _mirror_retry())
    except Exception:
        pass

    def _poll_temp():
        try:
            if not body.winfo_exists():
                return
            lbl = getattr(app, '_temp_usage_lbl', None)
            if lbl is not None and lbl.winfo_exists():
                txt = lbl.cget('text')
                if txt:
                    s['temp'](txt)
        except Exception:
            return
        try:
            body.after(2000, _poll_temp)
        except Exception:
            pass
    body.after(800, _poll_temp)

    # ── Temp folder card ──
    card1 = settings_card(body, _('Temp folder'))
    ctrl = setting_row(card1.body, _('Working directory'),
        _('Where the builder writes intermediate files. '
          'Pick a fast SSD with plenty of free space.'),
        stacked=True)
    _field(ctrl, app._temp_dir_var, readonly=True).pack(
        side='left', fill='x', expand=True)
    _btn(ctrl, _('Reset'),
         lambda: (app._temp_dir_var.set(''),
                  app._settings.update({'temp_dir': ''}),
                  save_settings(app._settings),
                  app._refresh_temp_size()),
         icon='undo').pack(side='right', padx=(px(6), 0))
    _btn(ctrl, _('Browse'), app._browse_temp, icon='folder_open').pack(
        side='right', padx=(px(6), 0))

    # Usage + Clear button — second row
    ctrl = setting_row(card1.body, _('Cleanup'),
        _('Manually clear all temp files written by previous builds.'))
    app._temp_usage_lbl = tk.Label(ctrl, text='', font=FONTS['code'],
                                   bg=C['bg_2'], fg=C['fg_3'], anchor='e')
    app._temp_usage_lbl.pack(side='left', padx=(0, px(12)))
    _btn(ctrl, _('Clear Temp Files'), app._clear_temp, variant='danger',
         icon='trash').pack(side='left')

    # ── Retry card ──
    card2 = settings_card(body, _('Auto-retry on failure'))
    ctrl = setting_row(card2.body, _('Retries'),
        _('How many times to retry a failed image build before giving up.'))
    _retry_opts = [(n, _('Off') if n == 0 else (str(n) + 'x'))
                   for n in [0, 1, 2, 3, 5]]
    _retry_disp = tk.StringVar()

    def _retry_sync(*_a):
        try:
            cur = int(app._retry_var.get())
        except Exception:
            cur = 0
        _retry_disp.set(dict(_retry_opts).get(cur, str(cur)))
    _retry_sync()

    def _retry_pick(_e=None):
        for n, lbl in _retry_opts:
            if lbl == _retry_disp.get():
                app._retry_var.set(n)
                break
        app._save_extra_settings()
    dd = Dropdown(ctrl, textvariable=_retry_disp,
                  values=[l for _n, l in _retry_opts], width=8)
    dd.pack(side='left', padx=(0, px(3)))
    dd.bind('<<ComboboxSelected>>', _retry_pick)
    try:
        app._retry_var.trace_add('write', _retry_sync)
    except Exception:
        pass

    # ── Updates card ──
    card3 = settings_card(body, _('Updates'))
    if not hasattr(app, '_check_updates_var'):
        app._check_updates_var = tk.BooleanVar(
            value=app._settings.get('check_updates_on_boot', True))

    def _save_update_pref():
        app._settings['check_updates_on_boot'] = bool(app._check_updates_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    ctrl = setting_row(card3.body, _('Check for updates on startup'),
        _('Automatically check GitHub for a newer version when the '
          'app starts. Turn off to skip the check.'))
    TSwitch(ctrl, variable=app._check_updates_var,
            command=_save_update_pref,
            tooltip=_('Check on startup')).pack(side='left')

    ctrl = setting_row(card3.body, _('Check now'),
        _('Manually check GitHub for the latest release right now.'))
    _btn(ctrl, _('Check for updates'), app._check_for_updates,
         icon='refresh').pack(side='left')

    # ── PFS Convert card ──
    card4 = settings_card(body, _('PFS Convert'))
    if not hasattr(app, '_pfs_del_src_var'):
        app._pfs_del_src_var = tk.BooleanVar(
            value=app._settings.get('pfs_delete_source_after_convert', False))

    def _save_pfs_del_pref():
        app._settings['pfs_delete_source_after_convert'] = bool(
            app._pfs_del_src_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    ctrl = setting_row(card4.body,
        _('Delete source image after a successful convert'),
        _('When a .ffpfsc is built from an .exfat/.ffpkg, delete the '
          'source image to free space. The .ffpfsc no longer needs it. '
          'Off by default — only deletes after a verified, freshly '
          'written output.'))
    TSwitch(ctrl, variable=app._pfs_del_src_var, command=_save_pfs_del_pref,
            tooltip=_('Auto-delete source')).pack(side='left')

    if not hasattr(app, '_pipeline_var'):
        app._pipeline_var = tk.BooleanVar(
            value=app._settings.get('pipeline_to_pfs', False))

    def _save_pipeline_pref():
        app._settings['pipeline_to_pfs'] = bool(app._pipeline_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    ctrl = setting_row(card4.body,
        _('Auto-build .ffpfsc after each exFAT/ffpkg build'),
        _('Pipeline mode: when building images, each one is converted '
          'to .ffpfsc as soon as it finishes and the .exfat/.ffpkg is '
          'then deleted to free space, before the next build starts. '
          'Replaces the per-build "Convert now?" prompt. Off by default.'))
    TSwitch(ctrl, variable=app._pipeline_var, command=_save_pipeline_pref,
            tooltip=_('Pipeline build → .ffpfsc')).pack(side='left')

    # ── EXTRACTION card (issue #38): optional robocopy multithread ──
    # Applies ONLY to .exfat image extraction (OSFMount mount + robocopy).
    # The ffpkg/PFS (UFS2Tool/Dokan) extraction path is unaffected.
    card_ext = settings_card(body, _('Extraction (.exfat images)'))
    if not hasattr(app, '_exfat_extract_mt_var'):
        app._exfat_extract_mt_var = tk.BooleanVar(
            value=app._settings.get('exfat_extract_robocopy_mt', False))

    def _save_extract_mt_pref():
        app._settings['exfat_extract_robocopy_mt'] = bool(
            app._exfat_extract_mt_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    ctrl = setting_row(card_ext.body,
        _('Use Robocopy multithread extraction'),
        _('When extracting an .exfat image to a folder, copy '
          'multiple files in parallel with robocopy /MT. Off by '
          'default (Classic single-thread extraction). Faster on '
          'NVMe/SSD destinations; on slow or USB drives the '
          'classic single-thread mode is usually safer. Windows '
          'only. The .ffpkg/.ffpfs/.ffpfsc extraction path is '
          'not affected by this setting.'))
    TSwitch(ctrl, variable=app._exfat_extract_mt_var,
            command=_save_extract_mt_pref,
            tooltip=_('Extraction multithread (/MT)')).pack(side='left')

    if not hasattr(app, '_exfat_extract_threads_var'):
        app._exfat_extract_threads_var = tk.StringVar(
            value=str(app._settings.get('exfat_extract_mt_threads', 32)))
    ctrl2 = setting_row(card_ext.body,
        _('Extraction copy threads (/MT:N)'),
        _('Number of parallel copy threads (robocopy /MT:N) when '
          'multithread extraction is enabled. Default 32. Valid '
          'range 1–128. Separate from Build Copy Threads.'))
    _thr_entry = _field(ctrl2, app._exfat_extract_threads_var, width=6)
    _thr_entry.pack(side='left', padx=(0, px(3)))

    def _save_extract_threads(*_a):
        raw = app._exfat_extract_threads_var.get().strip()
        try:
            n = int(raw)
        except Exception:
            n = 32
        n = max(1, min(128, n))
        # Normalise the field if the user typed something out of range/garbage.
        if str(n) != raw:
            try:
                app._exfat_extract_threads_var.set(str(n))
            except Exception:
                pass
        app._settings['exfat_extract_mt_threads'] = n
        try:
            save_settings(app._settings)
        except Exception:
            pass

    _thr_entry.bind('<FocusOut>', _save_extract_threads)
    _thr_entry.bind('<Return>', _save_extract_threads)

    # ── PFS convert backend card ──
    # Remove this whole card + ui/experimental_mkpfs.py + the pipeline branch
    # to fully revert the external mkpfs-compatible tool path.
    card_exp = settings_card(body, _('PFS convert backend'))
    if not hasattr(app, '_pfs_backend_var'):
        app._pfs_backend_var = tk.StringVar(
            value=str(app._settings.get('pfs_backend', 'lazy')).strip().lower()
            or 'lazy')

    def _save_pfs_backend():
        val = (app._pfs_backend_var.get() or 'lazy').strip().lower()
        if val not in ('lazy', 'mkpfs'):
            val = 'lazy'
        app._settings['pfs_backend'] = val
        try:
            save_settings(app._settings)
        except Exception:
            pass

    ctrl = setting_row(card_exp.body, _('Backend'),
        _('Two backends are available for the image → '
          '.ffpfsc convert/compression stage. Lazy_MkPFS (by '
          'Nazky) is the default. MkPFS (by PSBrew) is bundled '
          'and selectable but EXPERIMENTAL — it additionally '
          'supports OSFMount-free extract/verify; validate it on '
          'your own images/console before relying on it. If the '
          'chosen backend fails, the tool can fall back to the '
          'other. This only affects the image → .ffpfsc '
          'convert stage; the folder → exFAT/ffpkg build '
          'stage is unchanged.'),
        stacked=True)
    for _val, _lbl in (('lazy', _('Lazy_MkPFS by Nazky (Default)')),
                       ('mkpfs', _('MkPFS by PSBrew (Experimental — native, '
                                   'OSFMount-free extract/verify)'))):
        XRadio(ctrl, text=_lbl, variable=app._pfs_backend_var, value=_val,
               command=_save_pfs_backend).pack(anchor='w', pady=(0, px(4)))

    if not hasattr(app, '_exp_mkpfs_dir_var'):
        app._exp_mkpfs_dir_var = tk.StringVar(
            value=app._settings.get('experimental_mkpfs_folder', ''))
    ctrl2 = setting_row(card_exp.body,
        _('Override Lazy_MkPFS folder (Advanced)'),
        _('Lazy_MkPFS is bundled with the app and used by default '
          '— you do not need to set anything here. Leave this '
          'empty to use the bundled copy. Power users can point '
          'this at a different Lazy_MkPFS checkout (a folder '
          'containing main_file.py) to override the bundled one; '
          'if the override is missing or invalid the app falls '
          'back to the bundled copy, and if that fails, to '
          'current mkpfs.'),
        stacked=True)
    _exp_entry = _field(ctrl2, app._exp_mkpfs_dir_var)
    _exp_entry.pack(side='left', fill='x', expand=True)

    def _save_exp_dir(*_a):
        app._settings['experimental_mkpfs_folder'] = \
            app._exp_mkpfs_dir_var.get().strip()
        try:
            save_settings(app._settings)
        except Exception:
            pass

    def _browse_exp_dir():
        d = filedialog.askdirectory(
            title=_('Select a Lazy_MkPFS folder to override the bundled copy'))
        if d:
            app._exp_mkpfs_dir_var.set(d)
            _save_exp_dir()

    app._exp_mkpfs_dir_var.trace_add('write', _save_exp_dir)
    _btn(ctrl2, _('Browse…'), _browse_exp_dir, icon='folder_open').pack(
        side='right', padx=(px(6), 0))


# ─────────────────────────────────────────────────────────────────────────
# LOGS section
# ─────────────────────────────────────────────────────────────────────────
def _build_logs(body, app):
    # Overview — totals + newest, fed by a directory listing
    # (display-only; refreshed on build and whenever the count var
    # changes, e.g. after Clear all logs).
    s = overview_strip(body, [('count', _('Total Logs')),
                              ('size', _('Total Size')),
                              ('failed', _('Failed')),
                              ('last', _('Last Log'))])

    table_holder = tk.Frame(body, bg=C['bg_1'])

    def _refresh_dashboard(*_a):
        import time as _t
        try:
            d = app._get_logs_dir()
            files = []
            for fn in os.listdir(d):
                if not fn.endswith('.log'):
                    continue
                fp = os.path.join(d, fn)
                try:
                    st_ = os.stat(fp)
                    files.append((fn, fp, st_.st_size, st_.st_mtime))
                except OSError:
                    pass
        except Exception:
            files = []
        try:
            if not table_holder.winfo_exists():
                return
            total = len(files)
            tsize = sum(f[2] for f in files)
            failed = sum(1 for f in files if '_FAILED' in f[0])
            s['count']('{:,}'.format(total))
            s['size']('%.1f MB' % (tsize / 1024**2) if tsize else '0 MB')
            if failed:
                s['failed']('{:,}'.format(failed), 'warn')
            else:
                s['failed']('0')
            if files:
                newest = max(f[3] for f in files)
                s['last'](_t.strftime('%d/%m %H:%M',
                                      _t.localtime(newest)))
            else:
                s['last']('—')

            # ── Recent logs list (newest 6) ──
            for w in table_holder.winfo_children():
                w.destroy()
            tk.Label(table_holder, text=_('RECENT LOGS').upper(),
                     font=FONTS['overline'], bg=C['bg_1'], fg=C['fg_4'],
                     anchor='w').pack(fill='x', pady=(0, px(8)))
            card = Card(table_holder, padding=16)
            card.pack(fill='x')
            card.body = card
            if not files:
                es = XEmptyState(card.body, icon='file_text',
                                 title=_('No logs available'),
                                 text=_('Build activity will appear here.'))
                es.pack(fill='x')
                es.configure(height=px(180))
                return
            lst = tk.Frame(card.body, bg=C['bg_2'])
            lst.pack(fill='x')
            for i, (fn, fp, sz, mt) in enumerate(
                    sorted(files, key=lambda f: -f[3])[:6]):
                if i:
                    Divider(lst, color=C['border_1']).pack(fill='x')
                r = tk.Frame(lst, bg=C['bg_2'], cursor='hand2')
                r.pack(fill='x')
                bad = '_FAILED' in fn
                IconLabel(r, 'error' if bad else 'success', 16,
                          C['danger'] if bad else C['success'],
                          C['bg_2']).pack(side='left', padx=(0, px(10)))
                name = fn if len(fn) <= 56 else fn[:54] + '…'
                tk.Label(r, text=name, font=FONTS['code'],
                         bg=C['bg_2'], fg=C['fg_1'], anchor='w'
                         ).pack(side='left', fill='x', expand=True,
                                pady=px(9))
                tk.Label(r, text='%.0f KB' % max(1, sz / 1024),
                         font=FONTS['code'], bg=C['bg_2'], fg=C['fg_3'],
                         width=9, anchor='e').pack(side='right')
                tk.Label(r, text=_t.strftime('%d/%m %H:%M',
                                             _t.localtime(mt)),
                         font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3']
                         ).pack(side='right', padx=(0, px(16)))

                def _open(_e=None, path=fp):
                    try:
                        os.startfile(path)
                    except Exception:
                        pass
                for w in [r] + r.winfo_children():
                    w.bind('<Double-Button-1>', _open)
        except Exception:
            pass

    app._logs_dashboard_refresh = _refresh_dashboard

    card = settings_card(body, _('Log files'))
    ctrl = setting_row(card.body, _('Logs folder'),
        _('Build, upload, and backport logs are written here.'),
        stacked=True)
    _field(ctrl, app._logs_dir_var, readonly=True).pack(
        side='left', fill='x', expand=True)
    _btn(ctrl, _('Open'), app._open_logs_dir, icon='folder_open').pack(
        side='right', padx=(px(6), 0))
    _btn(ctrl, _('Browse'), app._browse_logs_dir, icon='folder').pack(
        side='right', padx=(px(6), 0))

    # Stats + clear
    ctrl = setting_row(card.body, _('Cleanup'),
        _('Removes every log file in the folder.'))
    app._logs_count_var = tk.StringVar(value='')
    tk.Label(ctrl, textvariable=app._logs_count_var, font=FONTS['caption'],
             bg=C['bg_2'], fg=C['fg_3'], anchor='e'
             ).pack(side='left', padx=(0, px(12)))
    _btn(ctrl, _('Clear all logs'), app._clear_logs, variant='danger',
         icon='trash').pack(side='left')

    table_holder.pack(fill='x', pady=(0, px(16)))

    _refresh_dashboard()
    try:
        # _refresh_logs_count rewrites this var after builds/clears —
        # piggyback on it to keep the dashboard current.
        app._logs_count_var.trace_add(
            'write', lambda *a: _refresh_dashboard())
        app._logs_dir_var.trace_add(
            'write', lambda *a: _refresh_dashboard())
    except Exception:
        pass

    app.after(500, app._refresh_logs_count)


# ─────────────────────────────────────────────────────────────────────────
# FTP section
# ─────────────────────────────────────────────────────────────────────────
class _ReachBadge(Badge):
    """Badge standing in for the old StatusPill: keeps .cget('text')
    and .set(kind, text) working for any reader of `_ftp_reach_pill`."""
    _KIND_TONE = {'active': 'info', 'done': 'success', 'wait': 'neutral',
                  'fail': 'danger'}

    def set(self, kind=None, text=None, icon='__keep__'):
        if kind in self._KIND_TONE and text is not None:
            return super().set(text, self._KIND_TONE[kind], icon)
        return super().set(kind, text, icon)

    def cget(self, key):
        if key == 'text':
            return getattr(self, '_text', '')
        return super().cget(key)


def _build_ftp(body, app):
    # Overview — PS5 IP / FTP port / Auto upload / Status
    s = overview_strip(body, [('ip', _('PS5 IP'), True),
                              ('port', _('FTP Port'), True),
                              ('auto', _('Auto Upload')),
                              ('status', _('Status'))])

    # Auto Upload mirrors this pane's own auto-upload-after-build
    # toggle (the control lives in the Connection card below).
    def _mirror_auto(*_a):
        try:
            on = bool(app._ftp_auto_var.get())
            s['auto'](_('On') if on else _('Off'),
                      'ok' if on else None)
        except Exception:
            s['auto']('—')
    _mirror_auto()
    try:
        app._ftp_auto_var.trace_add('write', lambda *a: _mirror_auto())
    except Exception:
        pass

    def _mirror_conn(*_a):
        try:
            s['ip'](app._ftp_ip_var.get().strip())
        except Exception:
            pass
        try:
            s['port'](app._ftp_port_var.get().strip())
        except Exception:
            pass
    try:
        app._ftp_ip_var.trace_add('write', lambda *a: _mirror_conn())
        app._ftp_port_var.trace_add('write', lambda *a: _mirror_conn())
    except Exception:
        pass

    def _poll_pill():
        try:
            if not body.winfo_exists():
                return
            pill = getattr(app, '_ftp_reach_pill', None)
            if pill is not None and pill.winfo_exists():
                txt = (pill.cget('text') or '').strip()
                low = txt.lower()
                kind = ('ok' if 'reach' in low and 'not' not in low
                        else 'warn' if ('unreach' in low or
                                        'fail' in low or 'not' in low
                                        and 'tested' not in low)
                        else None)
                s['status'](txt, kind)
        except Exception:
            return
        try:
            body.after(1500, _poll_pill)
        except Exception:
            pass

    body.after(600, lambda: (_mirror_conn(), _poll_pill()))

    # ── Connection card ──
    card = settings_card(body, _('Connection'))

    # Console IP
    ctrl = setting_row(card.body, _('Console IP'),
        _('IPv4 of your PS5 on the local network.'))
    _field(ctrl, app._ftp_ip_var, width=18).pack(side='left')
    _btn(ctrl, _('Auto-detect'), app._auto_detect_ip, icon='search').pack(
        side='left', padx=(px(6), 0))
    _btn(ctrl, _('Save'), app._save_ftp_settings, variant='primary',
         icon='save').pack(side='left', padx=(px(6), 0))

    # FTP port
    ctrl = setting_row(card.body, _('FTP port'),
        _('Default for goldHEN / etaHEN is 2121.'))
    # Reachable badge — placeholder; updates only when user clicks Test
    app._ftp_reach_pill = _ReachBadge(ctrl, _('Not tested'), 'neutral')
    app._ftp_reach_pill.pack(side='left', padx=(0, px(12)))
    _field(ctrl, app._ftp_port_var, width=8).pack(side='left',
                                                  padx=(0, px(3)))

    # Games path
    ctrl = setting_row(card.body, _('PS5 games path'),
        _('Folder on the PS5 where uploaded .exfat images land.'),
        stacked=True)
    _field(ctrl, app._ftp_path_var).pack(side='left', fill='x', expand=True)
    _btn(ctrl, _('Default'),
         lambda: app._ftp_path_var.set('/data/etaHEN/games/'),
         icon='undo').pack(side='right', padx=(px(6), 0))

    # Auto-upload toggle
    ctrl = setting_row(card.body, _('Auto-upload after build'),
        _('Send the built .exfat image to the PS5 automatically.'))
    TSwitch(ctrl, variable=app._ftp_auto_var,
            command=app._save_ftp_settings).pack(side='left')

    # ── Info bar under the connection card ──
    InfoBar(body, kind='info',
            text=_('Auto-detect uses ARP + mDNS over your local subnet. If '
                   'your PS5 is on a different VLAN you’ll need to '
                   'enter the IP manually.')
            ).pack(fill='x', pady=(0, px(16)))

    # ── Diagnostics card — Test / Ping / Browse / Games ──
    card2 = settings_card(body, _('Diagnostics'))

    status_row = tk.Frame(card2.body, bg=C['bg_2'])
    status_row.pack(fill='x', pady=(px(2), px(12)))
    tk.Label(status_row, text=_('Status'), font=FONTS['caption'],
             bg=C['bg_2'], fg=C['fg_3']).pack(side='left', padx=(0, px(12)))
    app._ftp_status_var = tk.StringVar(value=_('Not connected'))
    app._ftp_status_lbl = tk.Label(status_row,
                                    textvariable=app._ftp_status_var,
                                    font=FONTS['body'],
                                    bg=C['bg_2'], fg=C['fg_2'])
    app._ftp_status_lbl.pack(side='left', padx=(0, px(16)))

    # Auto-detect status — bound to a separate label per legacy contract
    app._autoip_status = tk.Label(status_row, text='', font=FONTS['caption'],
                                  bg=C['bg_2'], fg=C['fg_3'])
    app._autoip_status.pack(side='left')

    Divider(card2.body, color=C['border_1']).pack(fill='x')
    btn_row = tk.Frame(card2.body, bg=C['bg_2'])
    btn_row.pack(fill='x', pady=(px(12), 0))

    _btn(btn_row, _('Test Connection'), app._ftp_test, icon='plug').pack(
        side='left', padx=(0, px(6)))
    _btn(btn_row, _('Ping PS5'),
         lambda: app._ftp_ping(
             lambda ok, msg: (
                 app._ftp_status_var.set(msg),
                 app._ftp_status_lbl.config(
                     fg=C['success_hi'] if ok else C['danger_hi']))),
         icon='network').pack(side='left', padx=(0, px(6)))
    _btn(btn_row, '\U0001f4c1  ' + _('Browse PS5'),
         app._show_ps5_browser, icon='folder').pack(side='left',
                                                    padx=(0, px(6)))
    _btn(btn_row, '\U0001f3ae  ' + _('Games on PS5'),
         app._show_installed_games, icon='game').pack(side='left',
                                                      padx=(0, px(6)))

    # Cancel-upload button — same legacy attribute name. Not packed by
    # default; legacy code packs it during an upload.
    app._cancel_btn = XButton(btn_row, text=_('Cancel Upload'),
                              variant='danger', icon='close',
                              command=app._cancel_ftp_upload)
    # Not packed — legacy code does the pack/unpack.


# ─────────────────────────────────────────────────────────────────────────
# NOTIFICATIONS section
# ─────────────────────────────────────────────────────────────────────────
def _build_notifications(body, app):
    card = settings_card(body, _('Sound'))

    # Completion-sound state, mirrored as a badge in the card head
    state_badge = Badge(card.head_actions, '', 'neutral')
    state_badge.pack(side='right')

    def _mirror_sound(*_a):
        try:
            on = bool(app._sound_var.get())
            state_badge.set(_('Completion sound') + ': ' +
                            (_('On') if on else _('Off')),
                            'success' if on else 'neutral')
        except Exception:
            pass
    _mirror_sound()
    try:
        app._sound_var.trace_add('write', lambda *a: _mirror_sound())
    except Exception:
        pass

    ctrl = setting_row(card.body, _('Play sound on completion'),
        _('Audio chime when builds, uploads, or backports finish.'))
    TSwitch(ctrl, variable=app._sound_var,
            command=app._save_extra_settings).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# THEME section
# ─────────────────────────────────────────────────────────────────────────
def _build_theme(body, app):
    card = settings_card(body, _('Theme'))
    theme_badge = Badge(card.head_actions, '', 'neutral')
    theme_badge.pack(side='right')

    def _mirror_theme(*_a):
        try:
            theme_badge.set((app._theme_var.get() or 'dark').capitalize(),
                            'neutral')
        except Exception:
            pass
    _mirror_theme()
    try:
        app._theme_var.trace_add('write', lambda *a: _mirror_theme())
    except Exception:
        pass

    # ── Theme preview tiles — the shipped variant as a selectable mock
    # preview. Clicking one drives the same _theme_var + _toggle_theme
    # path the old radios used.
    grid = tk.Frame(card.body, bg=C['bg_2'])
    grid.pack(fill='x', pady=(px(2), 0))
    cards = {}

    def _theme_card(col, value, title, sub, swatches):
        tile = Rounded(grid, bg=C['bg_3'], border=C['border_3'], radius=8,
                       padding=(px(14), px(12)), surface=C['bg_2'])
        tile.grid(row=0, column=col, sticky='nw',
                  padx=(0 if col == 0 else px(12), 0))
        tile.configure(cursor='hand2')

        # Mini mock window
        mock = tk.Frame(tile, bg=swatches[0], width=px(260), height=px(92),
                        highlightbackground=C['border_3'],
                        highlightthickness=1)
        mock.pack(anchor='w')
        mock.pack_propagate(False)
        side = tk.Frame(mock, bg=swatches[1], width=px(56))
        side.pack(side='left', fill='y')
        main = tk.Frame(mock, bg=swatches[0])
        main.pack(side='left', fill='both', expand=True, padx=px(10),
                  pady=px(10))
        tk.Frame(main, bg=swatches[2], height=px(16)).pack(fill='x')
        dots = tk.Frame(main, bg=swatches[0])
        dots.pack(anchor='w', pady=(px(10), 0))
        for sc in swatches[3:]:
            d = tk.Frame(dots, bg=sc, width=px(26), height=px(12))
            d.pack(side='left', padx=(0, px(6)))
            d.pack_propagate(False)

        hr = tk.Frame(tile, bg=C['bg_3'])
        hr.pack(fill='x', pady=(px(10), 0))
        tk.Label(hr, text=title, font=FONTS['body_strong'],
                 bg=C['bg_3'], fg=C['fg_0']).pack(side='left')
        active = Badge(hr, _('ACTIVE'), 'success')
        sub_lbl = tk.Label(tile, text=sub, font=FONTS['caption'],
                           bg=C['bg_3'], fg=C['fg_3'], anchor='w',
                           justify='left', wraplength=px(260))
        sub_lbl.pack(fill='x', pady=(px(2), 0))

        def _pick(_e=None, v=value):
            try:
                app._theme_var.set(v)
                app._toggle_theme()
            except Exception:
                pass

        def _bind_all(w):
            w.bind('<Button-1>', _pick)
            for ch in w.winfo_children():
                _bind_all(ch)
        _bind_all(tile)
        cards[value] = (tile, active, [hr, sub_lbl])
        return tile

    _theme_card(0, 'dark', _('Dark Purple'),
                _('Deep navy surfaces with the purple accent — '
                  'the standard look.'),
                [C['bg_1'], C['bg_0'], C['bg_3'], C['accent'],
                 C['success'], C['warn'], C['danger']])
    # v3.6.3: the light variant was removed. The app's UI is built on the
    # COLORS{} design-token system; a proper light theme (a full light token
    # palette) is planned for a future release rather than shipping a
    # misleading half-working toggle.

    def _paint_active(*_a):
        try:
            cur = app._theme_var.get() or 'dark'
            for val, (tile, pill, kids) in cards.items():
                if not tile.winfo_exists():
                    return
                if val == cur:
                    tile.set_style(bg=C['accent_08'], border=C['accent'])
                    bgc = C['accent_08']
                    pill.pack(side='right')
                else:
                    tile.set_style(bg=C['bg_3'], border=C['border_3'])
                    bgc = C['bg_3']
                    pill.pack_forget()
                for k in kids:
                    k.configure(bg=bgc)
                    for ch in k.winfo_children():
                        try:
                            ch.configure(bg=bgc)
                        except Exception:
                            pass
                pill.configure(bg=bgc)
                pill.set()
        except Exception:
            pass
    _paint_active()
    try:
        app._theme_var.trace_add('write', lambda *a: _paint_active())
    except Exception:
        pass

    # Palette swatch row
    card.body._xs_rows = 1
    grid.pack_configure(pady=(px(2), px(12)))
    ctrl = setting_row(card.body, _('Palette'),
        None)
    for key in ('bg_0', 'bg_1', 'bg_2', 'bg_3', 'accent', 'success',
                'warn', 'danger'):
        sw = tk.Frame(ctrl, bg=C.get(key, '#000'), width=px(24),
                      height=px(18), highlightbackground=C['border_3'],
                      highlightthickness=1)
        sw.pack(side='left', padx=(0, px(6)))
        sw.pack_propagate(False)
        XTooltip(sw, key)


# ─────────────────────────────────────────────────────────────────────────
# SHUTDOWN section — the main-file `_build_shutdown_section` builds the
# controls; this adds the overview and the live workflow preview.
# ─────────────────────────────────────────────────────────────────────────
def _build_shutdown(body, app):
    s = overview_strip(body, [('action', _('Action')),
                              ('delay', _('Delay')),
                              ('rules', _('Active Rules'))])

    # Workflow preview — Trigger → Wait → Action, mirrored live
    flowc = settings_card(body, _('Current flow'))
    fi = tk.Frame(flowc.body, bg=C['bg_2'])
    fi.pack(anchor='w', pady=(px(2), 0))

    def _flow_chip():
        chip = Rounded(fi, bg=C['bg_3'], border=C['border_3'], radius=4,
                       padding=(px(12), px(6)), surface=C['bg_2'])
        chip.pack(side='left')
        lbl = tk.Label(chip, text='—', font=FONTS['body'],
                       bg=C['bg_3'], fg=C['fg_3'])
        lbl.pack()
        chip.lbl = lbl
        return chip

    def _arrow():
        IconLabel(fi, 'arrow_right', 16, C['fg_4'], C['bg_2']).pack(
            side='left', padx=px(10))

    fc_trigger = _flow_chip()
    _arrow()
    fc_wait = _flow_chip()
    _arrow()
    fc_action = _flow_chip()

    app._build_shutdown_section(body)

    # The vars exist only after the delegated builder ran.
    triggers = [('_shutdown_on_unibuild_var', 'Build tab'),
                ('_shutdown_on_exfat_var', 'exFAT'),
                ('_shutdown_on_ffpkg_var', 'ffpkg'),
                ('_shutdown_on_backport_var', _('Backport')),
                ('_shutdown_on_ftp_var', 'FTP'),
                ('_shutdown_on_pfs_var', 'PFS')]
    action_names = {'none': _('None'),
                    'shutdown': _('Shut down'),
                    'restart': _('Restart'),
                    'sleep': _('Sleep')}

    def _mirror_shutdown(*_a):
        act = 'none'
        try:
            act = app._shutdown_action_var.get()
            s['action'](action_names.get(act, act),
                        None if act == 'none' else 'warn')
        except Exception:
            pass
        delay = ''
        try:
            delay = app._shutdown_delay_var.get().strip()
            s['delay'](delay + 's')
        except Exception:
            pass
        names = []
        try:
            names = [lbl for attr, lbl in triggers
                     if bool(getattr(app, attr).get())]
            s['rules'](', '.join(names) if names else _('None'))
        except Exception:
            pass
        # workflow preview chips
        try:
            if not fc_trigger.winfo_exists():
                return
            on = act not in ('', 'none') and bool(names)
            fc_trigger.lbl.config(
                text=(' + '.join(names) + ' ' + _('complete'))
                if names else _('No triggers'))
            fc_wait.lbl.config(text=_('Wait %ss') % (delay or '0'))
            fc_action.lbl.config(text=action_names.get(act, act or '—'))
            for chip in (fc_trigger, fc_wait, fc_action):
                if on:
                    chip.set_style(bg=C['warn_bg'], border=C['warn'])
                    chip.lbl.config(bg=C['warn_bg'], fg=C['warn_hi'])
                else:
                    chip.set_style(bg=C['bg_3'], border=C['border_3'])
                    chip.lbl.config(bg=C['bg_3'], fg=C['fg_3'])
        except Exception:
            pass

    _mirror_shutdown()
    try:
        app._shutdown_action_var.trace_add(
            'write', lambda *a: _mirror_shutdown())
        app._shutdown_delay_var.trace_add(
            'write', lambda *a: _mirror_shutdown())
        for attr, _lbl in triggers:
            v = getattr(app, attr, None)
            if v is not None:
                v.trace_add('write', lambda *a: _mirror_shutdown())
    except Exception:
        pass
