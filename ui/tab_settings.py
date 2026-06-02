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
def _build_osfmount(body, app):
    app._osf_found_var = tk.StringVar(value='')

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


# ─────────────────────────────────────────────────────────────────────────
# LOGS section
# ─────────────────────────────────────────────────────────────────────────
def _build_logs(body, app):
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

    app.after(500, app._refresh_logs_count)


# ─────────────────────────────────────────────────────────────────────────
# FTP section — most complex
# ─────────────────────────────────────────────────────────────────────────
def _build_ftp(body, app):
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
    card = SettingsCard(body, title=_('Color theme'))
    card.pack(fill='x', pady=(0, 14))

    _r, ctrl, _act = settings_row(card.body,
        _('Theme'),
        description=_('Switches the app between dark and light variants.'),
        with_divider=False)

    seg = tk.Frame(ctrl, bg=COLORS['bg_2'])
    seg.pack(side='left')
    for label, val in [(_('Dark'), 'dark'), (_('Light'), 'light')]:
        tk.Radiobutton(seg, text=label, variable=app._theme_var,
                       value=val,
                       font=FONTS['body'],
                       bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['fg_1'],
                       selectcolor=COLORS['bg_4'],
                       cursor='hand2',
                       command=app._toggle_theme
                       ).pack(side='left', padx=(0, 16))


# ─────────────────────────────────────────────────────────────────────────
# SHUTDOWN section — defers to the existing main-file builder
# ─────────────────────────────────────────────────────────────────────────
def _build_shutdown(body, app):
    """Auto-shutdown section. Delegated to the main file's
    `_build_shutdown_section` for now — it's a complex builder with radios,
    delay entry, and per-task checkboxes that we don't want to rewrite this
    turn. The visual is acceptable inside the new pane scaffolding."""
    app._build_shutdown_section(body)


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
