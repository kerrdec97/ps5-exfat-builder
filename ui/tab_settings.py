"""
ui/tab_settings.py — Settings tab.

Step 8 (v2.1.0): refactored against preview/settings-tab-redesign.html.

Layout:

    ┌─ rail ──────────────┬─ pane ────────────────────────────────────┐
    │ SETTINGS            │ ┌─ pane-head ──────────────────────────── │
    │ ▌ 💾 OSFMount       │ │  PS5 FTP                                 │
    │   🔧 Build          │ │  Connection used by Klog, FTP, payload…  │
    │   📄 Logs           │ ├─ pane-body ──────────────────────────── │
    │ ▌ 📡 PS5 FTP    ←   │ │ ┌─ Connection card ─────────────────┐  │
    │   🔔 Notifications  │ │ │ Console IP   [..............] [Test]│ │
    │   🎨 Theme          │ │ │ FTP port     [2121]    ●Reachable  │ │
    │   💤 Auto-Shutdown  │ │ │ Klog port    [3232]    ●Reachable  │ │
    │ ─                   │ │ │ Auto-reconnect             [toggle]│ │
    │   ↻ Reset to defs   │ │ └────────────────────────────────────┘  │
    └────────────────────┴─────────────────────────────────────────┘

The rail is the upgraded `LeftRail` class. Each section's pane uses
`SettingsCard` to group related controls, with `settings_row()` for
[label] [control] [actions] rows.

Backwards compat: every `app.*` setting var/widget the existing callbacks
read is preserved with the same name. Nothing in the worker/save logic is
changed.
"""

import os

import tkinter as tk
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, LeftRail

from ui.shared.cards import SettingsCard, settings_row, StatusPill
from ui.shared.forms import SegmentedToggle


def build_settings_tab(parent, app):
    """Settings tab — left rail + per-section pane."""
    parent.configure(bg=COLORS['bg_1'])

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
         _('Switch between dark and light variants.')),
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
# Pane scaffolding — sticky header + scrollable body
# ─────────────────────────────────────────────────────────────────────────
def _make_pane(host, title, description):
    """Build the per-section pane: sticky head + scrollable body.

    Returns the body Frame; cards pack into it directly.
    """
    # Sticky head (top of pane)
    head = tk.Frame(host, bg=COLORS['bg_1'])
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=COLORS['bg_1'])
    head_inner.pack(fill='x', padx=32, pady=(20, 16))
    tk.Label(head_inner, text=title,
             font=(FONTS['h2'][0], 18, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'],
             anchor='w'
             ).pack(fill='x')
    tk.Label(head_inner, text=description,
             font=FONTS['body'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w', wraplength=700, justify='left'
             ).pack(fill='x', pady=(4, 0))
    tk.Frame(host, bg=COLORS['border_2'], height=1).pack(fill='x')

    # Scrollable body
    wrap = tk.Frame(host, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True)
    cv = tk.Canvas(wrap, bg=COLORS['bg_1'], highlightthickness=0)
    sb = tk.Scrollbar(wrap, command=cv.yview,
                      bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
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

    # Inner padded body — cards pack here with vertical spacing
    body = tk.Frame(inner, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=32, pady=24)
    return body


# ─────────────────────────────────────────────────────────────────────────
# OSFMOUNT section
# ─────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────
# v3.6.0 pass — dashboard summary strips above each section's settings.
# Presentation only: every value is read from existing vars/widgets or
# from cheap guarded file reads; nothing is written anywhere.
# ─────────────────────────────────────────────────────────────────────────
def _pane_summary(body, items):
    """Bordered horizontal strip of label/value cells at the top of a
    settings pane. `items` = list of (key, caption). Returns
    {key: setter} where setter(text, kind=None) updates the value;
    kind in {'ok','warn','accent',None} colors it."""
    card = tk.Frame(body, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x', pady=(0, 14))
    row = tk.Frame(card, bg=COLORS['bg_2'])
    row.pack(fill='x', padx=4, pady=12)
    n = len(items)
    for i in range(n):
        row.grid_columnconfigure(i, weight=1, uniform='panesum')

    setters = {}
    kinds = {'ok': COLORS['success_hi'], 'warn': COLORS['warn_hi'],
             'accent': COLORS['accent_hi'],
             'teal': COLORS['teal'], None: COLORS['fg_1']}
    for i, (key, caption) in enumerate(items):
        cell = tk.Frame(row, bg=COLORS['bg_2'])
        cell.grid(row=0, column=i, sticky='ew', padx=(14, 4))
        tk.Label(cell, text=caption.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'],
                 anchor='w').pack(fill='x')
        lbl = tk.Label(cell, text='\u2014',
                       font=(FONTS['mono_sm'][0], 11, 'bold'),
                       bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                       anchor='w')
        lbl.pack(fill='x', pady=(2, 0))

        def _set(text, kind=None, _l=lbl):
            try:
                if _l.winfo_exists():
                    _l.config(text=(text if text not in (None, '')
                                    else '\u2014'),
                              fg=kinds.get(kind, COLORS['fg_1']))
            except Exception:
                pass
        setters[key] = _set
    return setters


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

    # Summary strip — Version / Status / Last checked / Architecture
    s = _pane_summary(body, [('version', _('Version')),
                             ('status', _('Status')),
                             ('checked', _('Last checked')),
                             ('arch', _('Architecture')),
                             ('path', _('Detected Path'))])

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
            if len(sp) > 30:
                sp = '\u2026' + sp[-28:]
            s['path'](sp)
        else:
            s['status'](_('Not found'), 'warn')
            s['version']('\u2014')
            s['arch']('\u2014')
            s['path']('\u2014')
        s['checked'](_t.strftime('%d/%m/%Y %H:%M'))

    _refresh_summary()
    try:
        app._osfmount_path_var.trace_add(
            'write', lambda *a: _refresh_summary())
        app._osf_found_var.trace_add(
            'write', lambda *a: _refresh_summary())
    except Exception:
        pass

    card = SettingsCard(body, title=_('OSFMount executable'))
    card.pack(fill='x', pady=(0, 14))

    _row, ctrl, act = settings_row(card.body,
        _('Path to osfmount.com'),
        description=_('Used to mount/unmount disk images during the build.'),
        with_divider=False)

    _dark_entry(ctrl, app._osfmount_path_var, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(act, _('Browse'),
               command=app._browse_osfmount).pack(side='left', padx=(0, 4))
    _ghost_btn(act, _('Detect'),
               command=app._detect_osfmount).pack(side='left')

    # Status row below — shows detection result
    status_wrap = tk.Frame(card.body, bg=COLORS['bg_2'])
    status_wrap.pack(fill='x', padx=18, pady=(0, 14))
    tk.Label(status_wrap, textvariable=app._osf_found_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x')

    app.after(300, app._refresh_osf_status)


# ─────────────────────────────────────────────────────────────────────────
# BUILD section
# ─────────────────────────────────────────────────────────────────────────
def _build_build(body, app):
    # Summary strip — Temp usage / Retries / Retry wait / App version
    s = _pane_summary(body, [('temp', _('Temp Usage')),
                             ('retries', _('Retries')),
                             ('wait', _('Retry Wait')),
                             ('ver', _('App Version'))])
    s['ver']('v' + APP_VERSION, 'accent')

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
                    s['temp'](txt, 'teal')
        except Exception:
            return
        try:
            body.after(2000, _poll_temp)
        except Exception:
            pass
    body.after(800, _poll_temp)

    # ── Temp folder card ──
    card1 = SettingsCard(body, title=_('Temp folder'))
    card1.pack(fill='x', pady=(0, 14))

    _r, ctrl, act = settings_row(card1.body,
        _('Working directory'),
        description=_('Where the builder writes intermediate files. '
                      'Pick a fast SSD with plenty of free space.'),
        with_divider=True)
    _dark_entry(ctrl, app._temp_dir_var, readonly=True
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(act, _('Browse'),
               command=app._browse_temp).pack(side='left', padx=(0, 4))
    _ghost_btn(act, _('Reset'),
               command=lambda: (app._temp_dir_var.set(''),
                                app._settings.update({'temp_dir': ''}),
                                save_settings(app._settings),
                                app._refresh_temp_size())
               ).pack(side='left')

    # Usage + Clear button — second row
    _r, ctrl, act = settings_row(card1.body,
        _('Cleanup'),
        description=_('Manually clear all temp files written by previous builds.'),
        with_divider=False)
    app._temp_usage_lbl = tk.Label(ctrl, text='',
                                   font=FONTS['mono_sm'],
                                   bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                                   anchor='w')
    app._temp_usage_lbl.pack(side='left', fill='x', expand=True)
    _danger_btn(act, _('Clear Temp Files'),
                command=app._clear_temp).pack(side='left')

    # ── Retry card ──
    card2 = SettingsCard(body, title=_('Auto-retry on failure'))
    card2.pack(fill='x', pady=(0, 14))
    _r, ctrl, _act = settings_row(card2.body,
        _('Retries'),
        description=_('How many times to retry a failed image build before giving up.'),
        with_divider=False)
    _retry_seg = tk.Frame(ctrl, bg=COLORS['bg_2'])
    _retry_seg.pack(side='left')
    for n in [0, 1, 2, 3, 5]:
        lbl = _('Off') if n == 0 else (str(n) + 'x')
        tk.Radiobutton(_retry_seg, text=lbl, variable=app._retry_var,
                       value=n,
                       font=FONTS['body'],
                       bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['fg_1'],
                       selectcolor=COLORS['bg_4'],
                       cursor='hand2',
                       command=app._save_extra_settings
                       ).pack(side='left', padx=(0, 12))

    # ── Updates card ──
    card3 = SettingsCard(body, title=_('Updates'))
    card3.pack(fill='x', pady=(0, 14))

    _r, ctrl, _act = settings_row(card3.body,
        _('Check for updates on startup'),
        description=_('Automatically check GitHub for a newer version when the '
                      'app starts. Turn off to skip the check.'),
        with_divider=True)
    if not hasattr(app, '_check_updates_var'):
        app._check_updates_var = tk.BooleanVar(
            value=app._settings.get('check_updates_on_boot', True))

    def _save_update_pref():
        app._settings['check_updates_on_boot'] = bool(app._check_updates_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    tk.Checkbutton(ctrl, text=_('Check on startup'),
                   variable=app._check_updates_var,
                   font=FONTS['body'],
                   bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                   activebackground=COLORS['bg_2'], activeforeground=COLORS['fg_1'],
                   selectcolor=COLORS['bg_4'], cursor='hand2',
                   command=_save_update_pref).pack(side='left')

    _r, ctrl, act = settings_row(card3.body,
        _('Check now'),
        description=_('Manually check GitHub for the latest release right now.'),
        with_divider=False)
    _accent_btn(act, _('Check for updates'),
                command=app._check_for_updates).pack(side='left')

    # ── PFS Convert card ──
    card4 = SettingsCard(body, title=_('PFS Convert'))
    card4.pack(fill='x', pady=(0, 14))

    _r, ctrl, _act = settings_row(card4.body,
        _('Delete source image after a successful convert'),
        description=_('When a .ffpfsc is built from an .exfat/.ffpkg, delete the '
                      'source image to free space. The .ffpfsc no longer needs it. '
                      'Off by default \u2014 only deletes after a verified, freshly '
                      'written output.'),
        with_divider=False)
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

    tk.Checkbutton(ctrl, text=_('Auto-delete source'),
                   variable=app._pfs_del_src_var,
                   font=FONTS['body'],
                   bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                   activebackground=COLORS['bg_2'], activeforeground=COLORS['fg_1'],
                   selectcolor=COLORS['bg_4'], cursor='hand2',
                   command=_save_pfs_del_pref).pack(side='left')

    _r, ctrl, _act = settings_row(card4.body,
        _('Auto-build .ffpfsc after each exFAT/ffpkg build'),
        description=_('Pipeline mode: when building images, each one is converted '
                      'to .ffpfsc as soon as it finishes and the .exfat/.ffpkg is '
                      'then deleted to free space, before the next build starts. '
                      'Replaces the per-build "Convert now?" prompt. Off by default.'),
        with_divider=False)
    if not hasattr(app, '_pipeline_var'):
        app._pipeline_var = tk.BooleanVar(
            value=app._settings.get('pipeline_to_pfs', False))

    def _save_pipeline_pref():
        app._settings['pipeline_to_pfs'] = bool(app._pipeline_var.get())
        try:
            save_settings(app._settings)
        except Exception:
            pass

    tk.Checkbutton(ctrl, text=_('Pipeline build \u2192 .ffpfsc'),
                   variable=app._pipeline_var,
                   font=FONTS['body'],
                   bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                   activebackground=COLORS['bg_2'], activeforeground=COLORS['fg_1'],
                   selectcolor=COLORS['bg_4'], cursor='hand2',
                   command=_save_pipeline_pref).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# LOGS section
# ─────────────────────────────────────────────────────────────────────────
def _build_logs(body, app):
    # Dashboard strip — totals + newest, fed by a directory listing
    # (display-only; refreshed on build and whenever the count var
    # changes, e.g. after Clear all logs).
    s = _pane_summary(body, [('count', _('Total Logs')),
                             ('size', _('Total Size')),
                             ('failed', _('Failed')),
                             ('last', _('Last Log'))])

    table_holder = tk.Frame(body, bg=COLORS['bg_1'])

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
            s['count'](str(total), 'teal')
            s['size']('%.1f MB' % (tsize / 1024**2) if tsize else '0 MB')
            s['failed'](str(failed), 'warn' if failed else None)
            if files:
                newest = max(f[3] for f in files)
                s['last'](_t.strftime('%d/%m %H:%M',
                                      _t.localtime(newest)))
            else:
                s['last']('\u2014')

            # ── Recent logs table (newest 6) ──
            for w in table_holder.winfo_children():
                w.destroy()
            if not files:
                es = tk.Frame(table_holder, bg=COLORS['bg_2'],
                              highlightbackground=COLORS['border_2'],
                              highlightthickness=1)
                es.pack(fill='x')
                tk.Label(es, text='\U0001f4c4',
                         font=('Segoe UI', 26),
                         bg=COLORS['bg_2'], fg=COLORS['fg_6']
                         ).pack(pady=(22, 4))
                tk.Label(es, text=_('No logs available'),
                         font=(FONTS['body'][0], 10, 'bold'),
                         bg=COLORS['bg_2'], fg=COLORS['fg_2']
                         ).pack()
                tk.Label(es,
                         text=_('Build activity will appear here.'),
                         font=FONTS['mono_sm'],
                         bg=COLORS['bg_2'], fg=COLORS['fg_5']
                         ).pack(pady=(2, 22))
                return
            tk.Label(table_holder, text=_('RECENT LOGS'),
                     font=(FONTS['mono_sm'][0], 8, 'bold'),
                     bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                     anchor='w').pack(fill='x', pady=(0, 6))
            tbl = tk.Frame(table_holder, bg=COLORS['bg_2'],
                           highlightbackground=COLORS['border_2'],
                           highlightthickness=1)
            tbl.pack(fill='x')
            for i, (fn, fp, sz, mt) in enumerate(
                    sorted(files, key=lambda f: -f[3])[:6]):
                rbg = COLORS['bg_2'] if i % 2 == 0 else COLORS['bg_3']
                r = tk.Frame(tbl, bg=rbg, cursor='hand2')
                r.pack(fill='x')
                bad = '_FAILED' in fn
                tk.Label(r, text='\u2715' if bad else '\u2713',
                         font=(FONTS['mono_sm'][0], 9, 'bold'),
                         bg=rbg, fg=COLORS['danger_hi'] if bad
                         else COLORS['success_hi'], width=3
                         ).pack(side='left')
                name = fn if len(fn) <= 52 else fn[:50] + '\u2026'
                tk.Label(r, text=name, font=FONTS['mono_sm'],
                         bg=rbg, fg=COLORS['fg_2'], anchor='w'
                         ).pack(side='left', fill='x', expand=True,
                                pady=5)
                tk.Label(r, text='%.0f KB' % max(1, sz / 1024),
                         font=FONTS['mono_sm'],
                         bg=rbg, fg=COLORS['fg_5']
                         ).pack(side='right', padx=(0, 10))
                tk.Label(r, text=_t.strftime('%d/%m %H:%M',
                                             _t.localtime(mt)),
                         font=FONTS['mono_sm'],
                         bg=rbg, fg=COLORS['fg_4']
                         ).pack(side='right', padx=(0, 12))

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

    card = SettingsCard(body, title=_('Log files'))
    card.pack(fill='x', pady=(0, 14))

    _r, ctrl, act = settings_row(card.body,
        _('Logs folder'),
        description=_('Build, upload, and backport logs are written here.'),
        with_divider=True)
    _dark_entry(ctrl, app._logs_dir_var, readonly=True
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(act, _('Browse'),
               command=app._browse_logs_dir).pack(side='left', padx=(0, 4))
    _accent_btn(act, '\U0001f4c2  ' + _('Open'),
                command=app._open_logs_dir).pack(side='left')

    # Stats + clear
    _r, ctrl, act = settings_row(card.body,
        _('Cleanup'),
        description=_('Removes every log file in the folder.'),
        with_divider=False)
    app._logs_count_var = tk.StringVar(value='')
    tk.Label(ctrl, textvariable=app._logs_count_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(side='left', fill='x', expand=True)
    _danger_btn(act, _('Clear all logs'),
                command=app._clear_logs).pack(side='left')

    table_holder.pack(fill='x', pady=(0, 14))

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
# FTP section — most complex
# ─────────────────────────────────────────────────────────────────────────
def _build_ftp(body, app):
    # Summary strip — PS5 IP / FTP port / Status
    s = _pane_summary(body, [('ip', _('PS5 IP')),
                             ('port', _('FTP Port')),
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
            s['auto']('\u2014')
    _mirror_auto()
    try:
        app._ftp_auto_var.trace_add('write', lambda *a: _mirror_auto())
    except Exception:
        pass

    def _mirror_conn(*_a):
        try:
            s['ip'](app._ftp_ip_var.get().strip(), 'accent')
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
    card = SettingsCard(body, title=_('Connection'))
    card.pack(fill='x', pady=(0, 14))

    # Console IP
    _r, ctrl, act = settings_row(card.body,
        _('Console IP'),
        description=_('IPv4 of your PS5 on the local network.'),
        with_divider=True)
    _dark_entry(ctrl, app._ftp_ip_var, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(act, _('Auto-detect'),
               command=app._auto_detect_ip).pack(side='left', padx=(0, 4))
    _accent_btn(act, _('Save'),
                command=app._save_ftp_settings).pack(side='left')

    # FTP port
    _r, ctrl, act = settings_row(card.body,
        _('FTP port'),
        description=_('Default for goldHEN / etaHEN is 2121.'),
        with_divider=True)
    _dark_entry(ctrl, app._ftp_port_var, readonly=False, width=10
                ).pack(side='left')
    # Reachable pill — placeholder; updates only when user clicks Test
    app._ftp_reach_pill = StatusPill(act, kind='wait',
                                     text=_('Not tested'))
    app._ftp_reach_pill.pack(side='left')

    # Games path
    _r, ctrl, act = settings_row(card.body,
        _('PS5 games path'),
        description=_('Folder on the PS5 where uploaded .exfat images land.'),
        with_divider=True)
    _dark_entry(ctrl, app._ftp_path_var, readonly=False
                ).pack(side='left', fill='x', expand=True)
    _ghost_btn(act, _('Default'),
               command=lambda: app._ftp_path_var.set('/data/etaHEN/games/')
               ).pack(side='left')

    # Auto-upload toggle
    _r, ctrl, act = settings_row(card.body,
        _('Auto-upload after build'),
        description=_('Send the built .exfat image to the PS5 automatically.'),
        with_divider=False)
    SegmentedToggle(act, _('On'),
                    var=app._ftp_auto_var,
                    on_change=app._save_ftp_settings).pack(side='left')

    # ── Status banner — info text under the connection card ──
    info_banner = tk.Frame(body, bg=COLORS['accent_08'],
                           highlightbackground=COLORS['accent_lo'],
                           highlightthickness=1)
    info_banner.pack(fill='x', pady=(0, 14))
    info_inner = tk.Frame(info_banner, bg=COLORS['accent_08'])
    info_inner.pack(fill='x', padx=14, pady=10)
    tk.Label(info_inner, text='\u2139',
             font=(FONTS['body'][0], 12, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 10))
    tk.Label(info_inner,
             text=_('Auto-detect uses ARP + mDNS over your local subnet. If '
                    'your PS5 is on a different VLAN you\u2019ll need to '
                    'enter the IP manually.'),
             font=FONTS['body'],
             bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
             anchor='w', justify='left', wraplength=700
             ).pack(side='left', fill='x', expand=True)

    # ── Actions card — Test / Ping / Browse / Games ──
    card2 = SettingsCard(body, title=_('Diagnostics'))
    card2.pack(fill='x', pady=(0, 14))

    actions_row = tk.Frame(card2.body, bg=COLORS['bg_2'])
    actions_row.pack(fill='x', padx=18, pady=14)

    app._ftp_status_var = tk.StringVar(value=_('Not connected'))
    app._ftp_status_lbl = tk.Label(actions_row,
                                    textvariable=app._ftp_status_var,
                                    font=FONTS['mono_sm'],
                                    bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    app._ftp_status_lbl.pack(side='left', padx=(0, 12))

    # Auto-detect status — bound to a separate label per legacy contract
    app._autoip_status = tk.Label(actions_row, text='',
                                  font=FONTS['mono_sm'],
                                  bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    app._autoip_status.pack(side='left')

    btn_row = tk.Frame(card2.body, bg=COLORS['bg_2'])
    btn_row.pack(fill='x', padx=18, pady=(0, 14))

    _accent_btn(btn_row, _('Test Connection'),
                command=app._ftp_test).pack(side='left', padx=(0, 6))
    _ghost_btn(btn_row, _('Ping PS5'),
               command=lambda: app._ftp_ping(
                   lambda ok, msg: (
                       app._ftp_status_var.set(msg),
                       app._ftp_status_lbl.config(
                           fg=COLORS['success_hi'] if ok else COLORS['danger_hi']))
               )).pack(side='left', padx=(0, 6))
    _ghost_btn(btn_row, '\U0001f4c1  ' + _('Browse PS5'),
               command=app._show_ps5_browser).pack(side='left', padx=(0, 6))
    _ghost_btn(btn_row, '\U0001f3ae  ' + _('Games on PS5'),
               command=app._show_installed_games).pack(side='left', padx=(0, 6))

    # Cancel-upload button — same legacy attribute name. Not packed by
    # default; legacy code packs it during an upload.
    app._cancel_btn = tk.Button(btn_row, text='\u2715  ' + _('Cancel Upload'),
                                font=(FONTS['button'][0], 9, 'bold'),
                                bg=COLORS['bg_2'], fg=COLORS['danger_hi'],
                                activebackground=COLORS['bg_3'],
                                activeforeground=COLORS['danger_hi'],
                                relief='flat', bd=0,
                                padx=10, pady=6,
                                cursor='hand2',
                                highlightbackground=COLORS['danger'],
                                highlightthickness=1,
                                command=app._cancel_ftp_upload)
    # Not packed — legacy code does the pack/unpack.


# ─────────────────────────────────────────────────────────────────────────
# NOTIFICATIONS section
# ─────────────────────────────────────────────────────────────────────────
def _build_notifications(body, app):
    # Summary strip — completion sound state
    s = _pane_summary(body, [('sound', _('Completion sound'))])

    def _mirror_sound(*_a):
        try:
            on = bool(app._sound_var.get())
            s['sound'](_('On') if on else _('Off'),
                       'ok' if on else None)
        except Exception:
            pass
    _mirror_sound()
    try:
        app._sound_var.trace_add('write', lambda *a: _mirror_sound())
    except Exception:
        pass

    card = SettingsCard(body, title=_('Sound'))
    card.pack(fill='x', pady=(0, 14))

    _r, ctrl, act = settings_row(card.body,
        _('Play sound on completion'),
        description=_('Audio chime when builds, uploads, or backports finish.'),
        with_divider=False)
    SegmentedToggle(act, _('On'),
                    var=app._sound_var,
                    on_change=app._save_extra_settings).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# THEME section
# ─────────────────────────────────────────────────────────────────────────
def _build_theme(body, app):
    # Summary strip — current theme + live palette preview
    s = _pane_summary(body, [('theme', _('Theme'))])

    def _mirror_theme(*_a):
        try:
            s['theme']((app._theme_var.get() or 'dark').capitalize(),
                       'accent')
        except Exception:
            pass
    _mirror_theme()
    try:
        app._theme_var.trace_add('write', lambda *a: _mirror_theme())
    except Exception:
        pass

    # ── Theme preview cards (v3.6.0) — the two shipped variants as
    # selectable mock previews. Clicking one is identical to clicking
    # the matching radio below (same var, same _toggle_theme call).
    grid = tk.Frame(body, bg=COLORS['bg_1'])
    grid.pack(fill='x', pady=(0, 14))
    grid.grid_columnconfigure(0, weight=1, uniform='thprev')
    grid.grid_columnconfigure(1, weight=1, uniform='thprev')

    cards = {}

    def _theme_card(col, value, title, sub, swatches):
        c = tk.Frame(grid, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=2, cursor='hand2')
        c.grid(row=0, column=col, sticky='nsew',
               padx=(0 if col == 0 else 12, 0))
        ci = tk.Frame(c, bg=COLORS['bg_2'])
        ci.pack(fill='x', padx=14, pady=12)

        # Mini mock window
        mock = tk.Frame(ci, bg=swatches[0],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1, height=64)
        mock.pack(fill='x')
        mock.pack_propagate(False)
        bar = tk.Frame(mock, bg=swatches[1], height=14)
        bar.pack(fill='x')
        dotrow = tk.Frame(mock, bg=swatches[0])
        dotrow.pack(anchor='w', padx=8, pady=8)
        for sc in swatches[2:]:
            d = tk.Frame(dotrow, bg=sc, width=22, height=12)
            d.pack(side='left', padx=(0, 5))
            d.pack_propagate(False)

        hr = tk.Frame(ci, bg=COLORS['bg_2'])
        hr.pack(fill='x', pady=(10, 0))
        tk.Label(hr, text=title, font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_0']
                 ).pack(side='left')
        active = tk.Label(hr, text='\u2713 ' + _('ACTIVE'),
                          font=(FONTS['mono_sm'][0], 8, 'bold'),
                          bg=COLORS['success_bg'],
                          fg=COLORS['success_hi'], padx=8, pady=2)
        tk.Label(ci, text=sub, font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(fill='x')

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
        _bind_all(c)
        cards[value] = (c, active)
        return c

    _theme_card(0, 'dark', _('Dark Purple'),
                _('Deep navy surfaces with the purple accent \u2014 '
                  'the standard look.'),
                [COLORS['bg_1'], COLORS['bg_3'], COLORS['accent'],
                 COLORS['purple'], COLORS['teal'], COLORS['success']])
    _theme_card(1, 'light', _('Light'),
                _('Bright variant of the same layout and accents.'),
                ['#e8e8f0', '#d4d4e0', COLORS['accent'],
                 COLORS['purple'], COLORS['teal'], COLORS['success']])

    def _paint_active(*_a):
        try:
            cur = app._theme_var.get() or 'dark'
            for val, (c, pill) in cards.items():
                if not c.winfo_exists():
                    return
                if val == cur:
                    c.config(highlightbackground=COLORS['accent'])
                    pill.pack(side='right')
                else:
                    c.config(highlightbackground=COLORS['border_2'])
                    pill.pack_forget()
        except Exception:
            pass
    _paint_active()
    try:
        app._theme_var.trace_add('write', lambda *a: _paint_active())
    except Exception:
        pass

    # Palette swatch strip (kept from the earlier pass)
    prev = tk.Frame(body, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    prev.pack(fill='x', pady=(0, 14))
    prow = tk.Frame(prev, bg=COLORS['bg_2'])
    prow.pack(fill='x', padx=18, pady=12)
    tk.Label(prow, text=_('Palette').upper(),
             font=(FONTS['mono_sm'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(0, 14))
    for key in ('bg_1', 'bg_3', 'accent', 'purple', 'teal',
                'success', 'warn', 'danger'):
        sw = tk.Frame(prow, bg=COLORS.get(key, '#000'),
                      width=26, height=18,
                      highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
        sw.pack(side='left', padx=(0, 6))
        sw.pack_propagate(False)

    # v3.6.0: the preview cards above ARE the selector now — clicking
    # one drives the same _theme_var + _toggle_theme path the old
    # radios used, so the radio card was retired.


# ─────────────────────────────────────────────────────────────────────────
# SHUTDOWN section — defers to the existing main-file builder
# ─────────────────────────────────────────────────────────────────────────
def _build_shutdown(body, app):
    """Auto-shutdown section. Summary strip on top (v3.6.0 pass), then
    the existing main-file `_build_shutdown_section` builder below —
    its radios, delay entry, and per-task checkboxes are untouched."""
    s = _pane_summary(body, [('action', _('Action')),
                             ('delay', _('Delay')),
                             ('rules', _('Active Rules'))])

    # Workflow preview — Trigger → Wait → Action, mirrored live
    flowc = tk.Frame(body, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    flowc.pack(fill='x', pady=(0, 14))
    fi = tk.Frame(flowc, bg=COLORS['bg_2'])
    fi.pack(padx=16, pady=12)

    def _flow_chip():
        c = tk.Label(fi, text='\u2014',
                     font=(FONTS['mono_sm'][0], 9, 'bold'),
                     bg=COLORS['bg_3'], fg=COLORS['fg_4'],
                     padx=12, pady=5,
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
        c.pack(side='left')
        return c

    fc_trigger = _flow_chip()
    tk.Label(fi, text='\u2192', font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left', padx=8)
    fc_wait = _flow_chip()
    tk.Label(fi, text='\u2192', font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left', padx=8)
    fc_action = _flow_chip()

    app._build_shutdown_section(body)

    # The vars exist only after the delegated builder ran.
    triggers = [('_shutdown_on_exfat_var', 'exFAT'),
                ('_shutdown_on_ffpkg_var', 'ffpkg'),
                ('_shutdown_on_backport_var', _('Backport')),
                ('_shutdown_on_ftp_var', 'FTP'),
                ('_shutdown_on_pfs_var', 'PFS')]
    action_names = {'none': _('None'),
                    'shutdown': '\u23fb ' + _('Shut down'),
                    'restart': '\U0001f504 ' + _('Restart'),
                    'sleep': '\U0001f4a4 ' + _('Sleep')}

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
            s['rules'](', '.join(names) if names else _('None'),
                       'teal' if names else None)
        except Exception:
            pass
        # workflow preview chips
        try:
            if not fc_trigger.winfo_exists():
                return
            on = act not in ('', 'none') and bool(names)
            fc_trigger.config(
                text=(' + '.join(names) + ' ' + _('complete'))
                if names else _('No triggers'))
            fc_wait.config(text=_('Wait %ss') % (delay or '0'))
            fc_action.config(text=action_names.get(act, act or '\u2014'))
            for c, hot in ((fc_trigger, on), (fc_wait, on),
                           (fc_action, on)):
                if hot:
                    c.config(bg=COLORS['warn_bg'],
                             fg=COLORS['warn_hi'],
                             highlightbackground=COLORS['warn'])
                else:
                    c.config(bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                             highlightbackground=COLORS['border_3'])
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


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _dark_entry(parent, var, readonly=False, width=None):
    """Dark-themed entry box wrapped in a 1px-bordered Frame."""
    wrap = tk.Frame(parent, bg=COLORS['bg_0'],
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    state = 'readonly' if readonly else 'normal'
    kw = dict(textvariable=var,
              font=FONTS['mono_sm'],
              bg=COLORS['bg_0'], fg=COLORS['fg_1'],
              # readonly Entries display via disabledforeground/
              # readonlybackground, NOT fg/bg. Set both so the path
              # text is visible without selection.
              disabledforeground=COLORS['fg_1'],
              readonlybackground=COLORS['bg_0'],
              insertbackground=COLORS['accent'],
              selectbackground=COLORS['accent'],
              selectforeground=COLORS['fg_0'],
              relief='flat', bd=4,
              state=state)
    if width is not None:
        kw['width'] = width
    tk.Entry(wrap, **kw).pack(fill='x')
    return wrap


def _ghost_btn(parent, text, command):
    """Small ghost-style button."""
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


def _accent_btn(parent, text, command):
    """Primary (accent blue) button."""
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
    """Danger (red, ghost) button."""
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
