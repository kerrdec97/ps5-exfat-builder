"""
ui/tab_y2jb.py — Y2JB (YouTube-to-Jailbreak) sub-tab of PS5.

"Y2JB" here is YouTube to Jailbreak: it installs the YouTube PKGs that
enable jailbreaking via the YouTube app, and patches / blocks their system
updates.

Farewell Edition redesign on the design system:

    PageHeader  Y2JB Tools
    InfoBar     Requires etaHEN with Direct Package Installer V2 …
    ┌ Deployment (IP · FTP · DPI · install) ─┐ ┌ Install queue ───────┐
    │ quick actions                          │ │ current · last · feed│
    └────────────────────────────────────────┘ └──────────────────────┘
    Tabs: Connection · Install · Patch · Files · Autoloader
          (each a stack of cards built by app._y2jb_build_* / here)
    Output log — inset console

Backwards compat: every `_y2jb_*` attribute the install / patch / FTP
callbacks read is preserved with the same name and widget API.
"""

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import filedialog

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.tab_ps5_mgr import (
    ps5_page, page_header, section, two_columns, StatGrid, SubTabs,
    console, legacy_bar, LegacyBar,
)

C = COLORS


def build_y2jb_tab(parent, app):
    from ui.shared.widgets import (Card, Button, InfoBar, SectionTitle,
                                   Divider)
    app._y2jb_gated_buttons = []
    BG2 = C['bg_2']

    body = ps5_page(parent)
    page_header(body, _('Y2JB Tools'),
                _('YouTube to Jailbreak · Install PKG '
                  '· Patch & Block updates'))
    sec = section(body)
    InfoBar(sec, kind='info',
            text=_('Requires etaHEN running with Direct Package Installer '
                   'V2 enabled · FTP server running on PS5 · All three '
                   'regions can be patched.')).pack(fill='x')

    left, right = two_columns(body)

    # ── Deployment card: connection facts + quick actions ──
    dep = Card(left, padding=16)
    dep.pack(fill='both', expand=True)
    strip = StatGrid(dep, [('PS5 IP', 'ip'), ('FTP Port', 'ftp'),
                           ('DPI Port', 'dpi'), ('Install', 'inst')])
    strip.pack(fill='x')
    app._y2jb_hero_strip = strip

    def _mirror_conn(*_a):
        try:
            strip.set('ip', app._y2jb_ps5_ip.get().strip() or '—')
            strip.set('ftp', ':' + (app._y2jb_ftp_port.get().strip()
                                    or '2121'))
            strip.set('dpi', ':' + (app._y2jb_dpi_port.get().strip()
                                    or '12800'))
        except Exception:
            pass
    _mirror_conn()
    strip.set('inst', _('Idle'))
    for v in (app._y2jb_ps5_ip, app._y2jb_ftp_port, app._y2jb_dpi_port):
        try:
            v.trace_add('write', lambda *a: _mirror_conn())
        except Exception:
            pass

    Divider(dep, color=C['border_1']).pack(fill='x', pady=px(16))
    SectionTitle(dep, _('QUICK ACTIONS'), overline=True).pack(
        fill='x', pady=(0, px(8)))
    qbtns = tk.Frame(dep, bg=BG2)
    qbtns.pack(fill='x')

    def _goto_ftp(sub, path=None):
        try:
            getattr(app, '_ps5_subtab_activate', lambda k: None)('ftp')
        except Exception:
            pass

        def _after():
            try:
                if path is not None and hasattr(app, '_br_path'):
                    app._br_path.set(path)
                fn = getattr(app, '_ftp_switch_sub', None)
                if callable(fn):
                    fn(sub)
            except Exception:
                pass
        app.after(200, _after)

    Button(qbtns, text=_('Browse PS5'), icon='folder_open', size='sm',
           command=lambda: _goto_ftp('browser')).pack(side='left')
    Button(qbtns, text=_('Upload Package'), icon='upload', size='sm',
           command=lambda: _goto_ftp('upload')).pack(side='left',
                                                     padx=(px(6), 0))
    Button(qbtns, text=_('Open etaHEN folder'), icon='folder', size='sm',
           command=lambda: _goto_ftp('browser', '/data/etaHEN/')
           ).pack(side='left', padx=(px(6), 0))

    # ── Install queue card ──
    act = Card(right, padding=16)
    act.pack(fill='both', expand=True)
    SectionTitle(act, _('INSTALL QUEUE'), overline=True).pack(
        fill='x', pady=(0, px(8)))
    cur_var = tk.StringVar(value=_('No install running'))
    last_var = tk.StringVar(value=_('Last installed: —'))
    tk.Label(act, textvariable=cur_var, font=FONTS['body_strong'], bg=BG2,
             fg=C['fg_0'], anchor='w').pack(fill='x')
    tk.Label(act, textvariable=last_var, font=FONTS['caption'], bg=BG2,
             fg=C['fg_3'], anchor='w').pack(fill='x', pady=(px(2), 0))
    feed_var = tk.StringVar(value='')
    tk.Label(act, textvariable=feed_var, font=FONTS['code'], bg=BG2,
             fg=C['fg_3'], anchor='w', justify='left').pack(
                 fill='x', pady=(px(8), 0))
    feed = []

    def _wire_install(tries=0):
        step_v = getattr(app, '_y2jb_inst_step', None)
        pct_v = getattr(app, '_y2jb_inst_pct', None)
        if step_v is None or pct_v is None:
            if tries < 12:
                parent.after(400, lambda: _wire_install(tries + 1))
            return

        def _on_step(*_a):
            import time as _t
            try:
                step = step_v.get().strip()
                pct = pct_v.get().strip()
                if step:
                    cur_var.set((pct + '  ' if pct else '') + step)
                    strip.set('inst', pct or _('Running'))
                    feed.insert(0, _t.strftime('%H:%M:%S') + '  ' + step)
                    del feed[3:]
                    feed_var.set('\n'.join(feed))
                    low = step.lower()
                    if '✓' in step or 'complete' in low \
                            or 'installed' in low:
                        last_var.set(_('Last installed') + ': '
                                     + _t.strftime('%H:%M:%S'))
                        strip.set('inst', _('Done ✓'), ok=True)
                else:
                    cur_var.set(_('No install running'))
                    strip.set('inst', _('Idle'))
            except Exception:
                pass
        try:
            step_v.trace_add('write', _on_step)
        except Exception:
            pass
    parent.after(400, _wire_install)

    # ── Output log (built before the tools so they can log while they
    # build; packed after the tabs) ──
    log_sec = tk.Frame(body, bg=C['bg_1'])
    log_card = Card(log_sec, padding=16)
    log_card.pack(fill='x')
    hdr = SectionTitle(log_card, _('OUTPUT LOG'), overline=True)
    hdr.pack(fill='x', pady=(0, px(8)))
    Button(hdr.actions, text=_('Clear'), icon='trash', size='sm',
           variant='subtle', command=app._y2jb_clear_log).pack(side='right')
    well, app._y2jb_log = console(log_card, height=6)
    well.pack(fill='x')
    app._y2jb_log_console = well

    # ── Tool tabs ──
    def _remember(k):
        app._settings['y2jb_active_subtab'] = k
        try:
            save_settings(app._settings)
        except Exception:
            pass

    tabs_sec = section(body)
    app._y2jb_subtabs = SubTabs(tabs_sec, on_change=_remember)
    app._y2jb_subtabs.pack(fill='x')
    conn_pane  = app._y2jb_subtabs.add_tab('connection', _('Connection'),
                                           'plug')
    inst_pane  = app._y2jb_subtabs.add_tab('install', _('Install'),
                                           'package')
    patch_pane = app._y2jb_subtabs.add_tab('patch', _('Patch'), 'shield')
    files_pane = app._y2jb_subtabs.add_tab('files', _('Files'), 'files')
    auto_pane  = app._y2jb_subtabs.add_tab('autoloader', _('Autoloader'),
                                           'file_text')

    app._y2jb_build_connection(conn_pane)
    _build_install_cards(inst_pane, app)
    app._y2jb_build_patch(patch_pane)
    app._y2jb_build_files(files_pane)
    app._y2jb_build_autoloader(auto_pane)

    last = app._settings.get('y2jb_active_subtab', 'connection')
    try:
        app._y2jb_subtabs.show(last)
    except Exception:
        app._y2jb_subtabs.show_first()

    log_sec.pack(fill='x', padx=px(24), pady=(0, px(24)))

    # Gate the install buttons on a non-empty PS5 IP
    try:
        app._y2jb_ps5_ip.trace_add('write', app._y2jb_refresh_gating)
    except Exception:
        pass
    app._y2jb_refresh_gating()


# ─────────────────────────────────────────────────────────────────────────
# Install sub-tab — region cards
#
# Three region cards (USA / EU / JP) with the PPSA id, patchable badge,
# PKG picker, readiness status and Install button, then the install
# progress card. Every attribute the install workflow reads is created
# with the same name:
#   _y2jb_pkg_status[region]  StringVar — written by _y2jb_check_pkg
#   _y2jb_inst_step / _pct    StringVars — written by _y2jb_set_progress
#   _y2jb_inst_bar (+_rect)   bar accepting coords()/itemconfig()
#   _y2jb_inst_pct_val        int        — read by the Configure binding
# ─────────────────────────────────────────────────────────────────────────
def _build_install_cards(parent, app):
    from ui.shared.widgets import Card, Badge, Button, field_label
    row = tk.Frame(parent, bg=C['bg_1'])
    row.pack(fill='x')
    for c in range(3):
        row.grid_columnconfigure(c, weight=1, uniform='y2jb_region')

    app._y2jb_pkg_status = {}
    region_ids = getattr(app, 'Y2JB_REGION_IDS', {})
    BG2 = C['bg_2']

    for col, region in enumerate(('USA', 'EU', 'JP')):
        card = Card(row, padding=16)
        card.grid(row=0, column=col, sticky='nsew',
                  padx=(0 if col == 0 else px(8), 0 if col == 2 else px(8)))
        head = tk.Frame(card, bg=BG2)
        head.pack(fill='x')
        tk.Label(head, text=region, font=FONTS['subtitle'], bg=BG2,
                 fg=C['fg_0']).pack(side='left')
        Badge(head, _('Patchable'), 'success').pack(side='right')
        tk.Label(card, text=region_ids.get(region, ''), font=FONTS['code'],
                 bg=BG2, fg=C['fg_3'], anchor='w').pack(fill='x',
                                                        pady=(px(2), px(12)))

        field_label(card, _('YouTube PKG'), bg=BG2).pack(fill='x')
        field_row = tk.Frame(card, bg=BG2)
        field_row.pack(fill='x', pady=(px(4), 0))
        XEntry(field_row, textvariable=app._y2jb_pkg_paths[region],
               mono=True, state='readonly').pack(side='left', fill='x',
                                                  expand=True)

        def _browse_pkg(r=region):
            p = filedialog.askopenfilename(
                title=_('Select %s YouTube PKG') % r,
                filetypes=[('PKG files', '*.pkg'), ('All files', '*.*')])
            if p:
                app._y2jb_pkg_paths[r].set(p.replace('/', '\\'))
                app._settings['y2jb_pkg_' + r.lower()] = p
                save_settings(app._settings)
                app._y2jb_check_pkg(r)

        from ui.shared.widgets import IconButton
        IconButton(field_row, 'folder_open', _browse_pkg,
                   tooltip=_('Browse'), variant='secondary').pack(
                       side='left', padx=(px(6), 0))

        status_var = tk.StringVar(value='')
        app._y2jb_pkg_status[region] = status_var
        tk.Label(card, textvariable=status_var, font=FONTS['caption'],
                 bg=BG2, fg=C['fg_3'], anchor='w').pack(
                     fill='x', pady=(px(4), px(12)))

        ibtn = Button(card, text=_('Install %s') % region, icon='download',
                      command=lambda reg=region: app._y2jb_install_pkg(reg))
        ibtn.pack(fill='x', side='bottom')
        try:
            Tooltip(ibtn, _('Send the %s YouTube PKG to your PS5 via '
                            "etaHEN's Direct Package Installer V2. The "
                            'install will start automatically on the '
                            'PS5.') % region)
        except Exception:
            pass
        app._y2jb_register_gated_button(ibtn)

    for r in ('USA', 'EU', 'JP'):
        if app._y2jb_pkg_paths[r].get():
            app._y2jb_check_pkg(r)

    prog = Card(parent, title=_('Install progress'),
                subtitle=_('Requires etaHEN with Direct Package Installer '
                           'V2 enabled.'))
    prog.pack(fill='x', pady=(px(16), 0))
    app._y2jb_inst_step = tk.StringVar(value='')
    app._y2jb_inst_pct  = tk.StringVar(value='')
    top = tk.Frame(prog.body, bg=BG2)
    top.pack(fill='x')
    tk.Label(top, textvariable=app._y2jb_inst_step, font=FONTS['body_strong'],
             bg=BG2, fg=C['fg_0'], anchor='w').pack(side='left', fill='x',
                                                     expand=True)
    tk.Label(top, textvariable=app._y2jb_inst_pct, font=FONTS['caption'],
             bg=BG2, fg=C['fg_3'], anchor='e').pack(side='right')
    app._y2jb_inst_bar = legacy_bar(prog.body)
    app._y2jb_inst_bar.pack(fill='x', pady=(px(8), 0))
    app._y2jb_inst_bar_rect = LegacyBar.ITEM
    app._y2jb_inst_bar.bind('<Configure>',
        lambda e: app._y2jb_update_bar(getattr(app, '_y2jb_inst_pct_val', 0)),
        add='+')
    app._y2jb_inst_pct_val = 0
