"""
ui/tab_y2jb.py — Y2JB (YouTube-to-Jailbreak) tab.

Step 14 (v2.1.6): chrome restyle against the design system.

Important note about the mock: `preview/y2jb-tab-redesign.html` describes
a "Year-to-Jailbreak" exploit-chain launcher (HTTP server + payload bus +
boot sequence). This is a DIFFERENT FEATURE that just shares the Y2JB
acronym. The implementation in this codebase is "YouTube to Jailbreak" —
it installs YouTube PKGs that enable jailbreaking via the YouTube app,
plus patches/blocks system updates.

This refactor restyles the EXISTING tab with design tokens. Building the
mock's exploit-chain launcher would be a multi-turn networking project,
not a UI refactor.

Layout (unchanged from existing):
    ┌─ page head ──────────────────────────────────────────────┐
    │ 🎬 Y2JB Tools  — YouTube Jailbreak · Install · Patch    │
    ├─ info banner ────────────────────────────────────────────┤
    │ ℹ Requires etaHEN with Direct Package Installer V2…     │
    ├─ sub-tab bar (upgraded SubTabBar with underline) ───────┤
    │ Connection · Install · Patch · Files · Autoloader        │
    ├─ active sub-tab content (existing builders, untouched) ─┤
    │ ...                                                      │
    ├─ output log (ConsoleView, terminal styled) ─────────────┤
    └──────────────────────────────────────────────────────────┘

Backwards compat: every `_y2jb_*` attribute the existing 20+ callbacks
read is preserved. The 5 sub-tab content builders (`_y2jb_build_*`) keep
their current visual — they pack into the SubTabBar's panes which we now
give a design-tokens chrome.
"""

import tkinter as tk
from tkinter import filedialog

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, SubTabBar

from ui.shared.log_view import ConsoleView
from ui.shared.cards import Card
from ui.shared.page_head import make_themed_button
from ui.shared.ps5_kit import StatStrip


def build_y2jb_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # ── Page head ──
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 6))
    tk.Label(head, text='\U0001f3ac  ' + _('Y2JB Tools'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')
    tk.Label(head,
             text='\u2014  ' + _('YouTube Jailbreak · Install PKG · '
                                  'Patch & Block updates'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(12, 0), pady=(2, 0))

    # ── Info banner ──
    banner = tk.Frame(parent, bg=COLORS['accent_08'],
                      highlightbackground=COLORS['accent_lo'],
                      highlightthickness=1)
    banner.pack(fill='x', padx=24, pady=(0, 10))
    bi = tk.Frame(banner, bg=COLORS['accent_08'])
    bi.pack(fill='x', padx=12, pady=8)
    tk.Label(bi, text='\u2139',
             font=(FONTS['body'][0], 12, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 8))
    tk.Label(bi,
             text=_('Requires etaHEN running with Direct Package Installer '
                    'V2 enabled · FTP server running on PS5 · All three '
                    'regions can be patched.'),
             font=FONTS['body'],
             bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
             anchor='w', justify='left', wraplength=1100
             ).pack(side='left', fill='x', expand=True)

    # ── Deployment hero strip (v3.6.0 PS5 pass) ──
    # Connection facts mirrored live from the existing _y2jb_* vars;
    # the Install cell mirrors the live install progress.
    hero_wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    hero_wrap.pack(fill='x', padx=24, pady=(0, 10))
    strip = StatStrip(hero_wrap, [('PS5 IP', 'ip'),
                                  ('FTP Port', 'ftp'),
                                  ('DPI Port', 'dpi'),
                                  ('Install', 'inst')])
    strip.pack(fill='x')
    app._y2jb_hero_strip = strip

    def _mirror_conn(*_a):
        try:
            strip.set('ip', app._y2jb_ps5_ip.get().strip() or '\u2014',
                      'accent')
            strip.set('ftp', ':' + (app._y2jb_ftp_port.get().strip()
                                    or '2121'))
            strip.set('dpi', ':' + (app._y2jb_dpi_port.get().strip()
                                    or '12800'), 'purple')
        except Exception:
            pass
    _mirror_conn()
    strip.set('inst', _('Idle'))
    for v in (app._y2jb_ps5_ip, app._y2jb_ftp_port, app._y2jb_dpi_port):
        try:
            v.trace_add('write', lambda *a: _mirror_conn())
        except Exception:
            pass

    # ── Quick actions + install activity row ──
    qa_row = tk.Frame(parent, bg=COLORS['bg_1'])
    qa_row.pack(fill='x', padx=24, pady=(0, 10))
    qa_row.grid_columnconfigure(0, weight=1)
    qa_row.grid_columnconfigure(1, weight=1)

    qa = tk.Frame(qa_row, bg=COLORS['bg_2'],
                  highlightbackground=COLORS['border_2'],
                  highlightthickness=1)
    qa.grid(row=0, column=0, sticky='nsew', padx=(0, 10))
    qi = tk.Frame(qa, bg=COLORS['bg_2'])
    qi.pack(fill='x', padx=14, pady=12)
    tk.Label(qi, text=_('QUICK ACTIONS'),
             font=(FONTS['mono_sm'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(anchor='w', pady=(0, 8))
    qbtns = tk.Frame(qi, bg=COLORS['bg_2'])
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

    make_themed_button(qbtns, '\U0001f4c2  ' + _('Browse PS5'),
                       command=lambda: _goto_ftp('browser'),
                       kind='ghost', font_size=9, padx=12, pady=6
                       ).pack(side='left')
    make_themed_button(qbtns, '\u2191  ' + _('Upload Package'),
                       command=lambda: _goto_ftp('upload'),
                       kind='ghost', font_size=9, padx=12, pady=6
                       ).pack(side='left', padx=(8, 0))
    make_themed_button(qbtns, '\U0001f4e1  ' + _('Open etaHEN folder'),
                       command=lambda: _goto_ftp('browser',
                                                 '/data/etaHEN/'),
                       kind='ghost', font_size=9, padx=12, pady=6
                       ).pack(side='left', padx=(8, 0))

    # Install activity panel — current install, last install, feed.
    act = tk.Frame(qa_row, bg=COLORS['bg_2'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    act.grid(row=0, column=1, sticky='nsew')
    ai = tk.Frame(act, bg=COLORS['bg_2'])
    ai.pack(fill='both', expand=True, padx=14, pady=12)
    tk.Label(ai, text=_('INSTALL QUEUE'),
             font=(FONTS['mono_sm'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(anchor='w', pady=(0, 6))

    cur_var = tk.StringVar(value=_('No install running'))
    last_var = tk.StringVar(value=_('Last installed: \u2014'))
    tk.Label(ai, textvariable=cur_var,
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['teal'],
             anchor='w').pack(fill='x')
    tk.Label(ai, textvariable=last_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w').pack(fill='x', pady=(2, 0))
    feed_var = tk.StringVar(value='')
    tk.Label(ai, textvariable=feed_var,
             font=(FONTS['mono_sm'][0], 8),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             anchor='w', justify='left').pack(fill='x', pady=(4, 0))

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
                    strip.set('inst', pct or _('Running'), 'warn')
                    feed.insert(0, _t.strftime('%H:%M:%S') + '  ' + step)
                    del feed[3:]
                    feed_var.set('\n'.join(feed))
                    low = step.lower()
                    if '\u2713' in step or 'complete' in low \
                            or 'installed' in low:
                        last_var.set(_('Last installed') + ': '
                                     + _t.strftime('%H:%M:%S'))
                        strip.set('inst', _('Done \u2713'), 'ok')
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

    # ── Persistent Output Log (built first, packed at bottom) ──
    log_outer = tk.Frame(parent, bg=COLORS['bg_1'])
    log_outer.pack(fill='x', side='bottom', padx=24, pady=(0, 12))

    log_hdr = tk.Frame(log_outer, bg=COLORS['bg_1'])
    log_hdr.pack(fill='x', pady=(6, 4))
    tk.Label(log_hdr, text=_('OUTPUT LOG'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(side='left')
    tk.Button(log_hdr, text=_('Clear'),
              font=FONTS['meta'],
              bg=COLORS['bg_1'], fg=COLORS['fg_5'],
              activebackground=COLORS['bg_2'],
              activeforeground=COLORS['fg_2'],
              relief='flat', bd=0,
              cursor='hand2',
              command=app._y2jb_clear_log
              ).pack(side='right')

    cv = ConsoleView(log_outer)
    cv.pack(fill='x')
    # Cap the underlying Text widget to 6 visible lines — the legacy log
    # was 6 lines tall. ConsoleView's `height=` Frame kwarg is unreliable
    # here because Text's natural size dominates the frame; setting the
    # Text widget's `height` directly is the only reliable cap.
    cv.text.configure(height=6)
    app._y2jb_log = cv.text
    app._y2jb_log_console = cv

    # ── Sub-tab bar (uses the upgraded SubTabBar with underline pattern) ──
    app._y2jb_subtabs = SubTabBar(parent, font_size=10, pad=(16, 9))
    conn_pane  = app._y2jb_subtabs.add_tab('connection',
                                            '\U0001f50c  ' + _('Connection'))
    inst_pane  = app._y2jb_subtabs.add_tab('install',
                                            '\U0001f4e4  ' + _('Install'))
    patch_pane = app._y2jb_subtabs.add_tab('patch',
                                            '\U0001f6e1  ' + _('Patch'))
    files_pane = app._y2jb_subtabs.add_tab('files',
                                            '\U0001f4e1  ' + _('Files'))
    auto_pane  = app._y2jb_subtabs.add_tab('autoloader',
                                            '\U0001f4c4  ' + _('Autoloader'))

    # Wrap each pane in a scrollable canvas (same helper as before)
    def make_scroll(pane):
        wrap = tk.Frame(pane, bg=COLORS['bg_1'])
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
        return inner

    # Build each sub-tab using the existing main-file builders.
    # v3.6.0 PS5 pass: the Install sub-tab is built by this module's
    # region-card builder (_build_install_cards) instead of the legacy
    # stacked-field builder. Same attributes, same install workflow —
    # presentation only.
    app._y2jb_build_connection(make_scroll(conn_pane))
    _build_install_cards(make_scroll(inst_pane), app)
    app._y2jb_build_patch(make_scroll(patch_pane))
    app._y2jb_build_files(make_scroll(files_pane))
    app._y2jb_build_autoloader(make_scroll(auto_pane))

    # Show last selected sub-tab
    last = app._settings.get('y2jb_active_subtab', 'connection')
    try:
        app._y2jb_subtabs.show(last)
    except Exception:
        app._y2jb_subtabs.show_first()

    # Persist sub-tab choice between sessions
    def _remember(k):
        app._settings['y2jb_active_subtab'] = k
        try:
            save_settings(app._settings)
        except Exception:
            pass
    for key, btn, _frame in app._y2jb_subtabs._tabs:
        btn.bind('<Button-1>',
            lambda e, k=key: (app._y2jb_subtabs.show(k), _remember(k)),
            add='+')


# ─────────────────────────────────────────────────────────────────────────
# Install sub-tab — region cards (v3.6.0 PS5 pass)
#
# Replaces the legacy stacked region fields with three side-by-side
# region cards (USA / EU / JP), each carrying the region's PPSA id,
# patchable badge, PKG file picker, readiness status, and an Install
# button. Every attribute the install workflow reads is created with
# the exact same name and widget type as before:
#
#   _y2jb_pkg_status[region]  StringVar — written by _y2jb_check_pkg
#   _y2jb_inst_step / _pct    StringVars — written by _y2jb_set_progress
#   _y2jb_inst_bar (+_rect)   Canvas     — driven by _y2jb_update_bar
#   _y2jb_inst_pct_val        int        — read by the Configure binding
#
# The install workflow itself (_y2jb_install_pkg, _y2jb_check_pkg,
# DPI handshake, FTP) is untouched.
# ─────────────────────────────────────────────────────────────────────────
def _build_install_cards(parent, app):
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='x', padx=24, pady=(0, 12))

    # ── Region cards row ──
    row = tk.Frame(wrap, bg=COLORS['bg_1'])
    row.pack(fill='x')
    for c in range(3):
        row.grid_columnconfigure(c, weight=1, uniform='y2jb_region')

    app._y2jb_pkg_status = {}
    region_ids = getattr(app, 'Y2JB_REGION_IDS', {})

    for col, region in enumerate(('USA', 'EU', 'JP')):
        card = Card(row)
        card.grid(row=0, column=col, sticky='nsew',
                  padx=(0 if col == 0 else 10, 0))

        body = card.body

        # Head — region name + PPSA chip + patchable badge
        head = tk.Frame(body, bg=COLORS['bg_2'])
        head.pack(fill='x')
        tk.Label(head, text=region,
                 font=(FONTS['h3'][0], 13, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_0']
                 ).pack(side='left')
        tk.Label(head, text='\u2713 ' + _('Patchable'),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['success_bg'], fg=COLORS['success_hi'],
                 padx=8, pady=2
                 ).pack(side='right')

        tk.Label(body, text=region_ids.get(region, ''),
                 font=(FONTS['mono_sm'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w'
                 ).pack(fill='x', pady=(2, 10))

        # PKG file row — readonly entry + Browse (same behavior as the
        # legacy builder: persist to settings + re-check readiness)
        tk.Label(body, text=_('YouTube PKG').upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(fill='x')
        field_row = tk.Frame(body, bg=COLORS['bg_2'])
        field_row.pack(fill='x', pady=(3, 0))
        ef = tk.Frame(field_row, bg=COLORS['field_bg'],
                      highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
        ef.pack(side='left', fill='x', expand=True)
        tk.Entry(ef, textvariable=app._y2jb_pkg_paths[region],
                 font=(FONTS['mono_sm'][0], 9),
                 bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                 insertbackground=COLORS['field_fg'],
                 disabledforeground=COLORS['field_fg'],
                 readonlybackground=COLORS['field_bg'],
                 relief='flat', bd=4,
                 state='readonly').pack(fill='x')

        def _browse_pkg(r=region):
            p = filedialog.askopenfilename(
                title=_('Select %s YouTube PKG') % r,
                filetypes=[('PKG files', '*.pkg'), ('All files', '*.*')])
            if p:
                app._y2jb_pkg_paths[r].set(p.replace('/', '\\'))
                app._settings['y2jb_pkg_' + r.lower()] = p
                save_settings(app._settings)
                app._y2jb_check_pkg(r)

        make_themed_button(field_row, _('Browse'),
                           command=_browse_pkg, kind='ghost',
                           font_size=9, padx=10, pady=4
                           ).pack(side='left', padx=(6, 0))

        # Readiness status line — written by _y2jb_check_pkg
        status_var = tk.StringVar(value='')
        app._y2jb_pkg_status[region] = status_var
        tk.Label(body, textvariable=status_var,
                 font=FONTS['meta'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                 anchor='w').pack(fill='x', pady=(3, 10))

        # Install button — same workflow entry point
        ibtn = make_themed_button(
            body, '\U0001f4e4  ' + _('Install %s') % region,
            command=lambda reg=region: app._y2jb_install_pkg(reg),
            kind='primary', font_size=10, padx=14, pady=8)
        ibtn.pack(fill='x')
        try:
            Tooltip(ibtn, _('Send the %s YouTube PKG to your PS5 via '
                            "etaHEN's Direct Package Installer V2. The "
                            'install will start automatically on the '
                            'PS5.') % region)
        except Exception:
            pass

    # Re-check existing paths on load (same as the legacy builder)
    for r in ('USA', 'EU', 'JP'):
        if app._y2jb_pkg_paths[r].get():
            app._y2jb_check_pkg(r)

    # ── Install progress card ──
    prog = Card(wrap, title=_('Install progress'), icon='\U0001f4e4',
                subtitle=_('Requires etaHEN with Direct Package Installer '
                           'V2 enabled.'))
    prog.pack(fill='x', pady=(12, 0))

    app._y2jb_inst_step = tk.StringVar(value='')
    app._y2jb_inst_pct  = tk.StringVar(value='')
    tk.Label(prog.body, textvariable=app._y2jb_inst_step,
             font=(FONTS['body'][0], 9, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'],
             anchor='w').pack(fill='x', pady=(0, 2))
    bar_f = tk.Frame(prog.body, bg=COLORS['bg_4'], height=10)
    bar_f.pack(fill='x')
    bar_f.pack_propagate(False)
    app._y2jb_inst_bar = tk.Canvas(bar_f, height=10, bg=COLORS['bg_4'],
                                   highlightthickness=0)
    app._y2jb_inst_bar.pack(fill='both', expand=True)
    app._y2jb_inst_bar_rect = app._y2jb_inst_bar.create_rectangle(
        0, 0, 0, 10, fill=COLORS['accent'], outline='')
    app._y2jb_inst_bar.bind('<Configure>',
        lambda e: app._y2jb_update_bar(getattr(app, '_y2jb_inst_pct_val', 0)))
    app._y2jb_inst_pct_val = 0
    tk.Label(prog.body, textvariable=app._y2jb_inst_pct,
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w').pack(fill='x', pady=(2, 0))
