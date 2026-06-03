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

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, SubTabBar

from ui.shared.log_view import ConsoleView


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
    # These render into the panes the SubTabBar created. The internal
    # widget styling there is legacy — a follow-up turn could rewrite
    # each builder. For now the chrome is consistent.
    app._y2jb_build_connection(make_scroll(conn_pane))
    app._y2jb_build_install(make_scroll(inst_pane))
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
