"""
ui/tab_help.py — Help & Tutorial tab.

Step 13 (v2.1.5): chrome restyle against preview/help-tab-redesign.html.

The mock describes a 3-column layout (TOC sidebar + article body + aside
with quick-keys/version info). The existing implementation uses a
tkinterweb HtmlFrame to render bundled HTML — which already covers
TOC + article + asides as part of the HTML content itself.

This iteration restyles the chrome (header + toolbar) with design-system
tokens but **keeps the tkinterweb viewer untouched**. Recreating a TOC
sidebar in tk Frames would require parsing headings out of the HTML at
runtime, and would be markedly worse than what tkinterweb already does
inline.

If you want the Python-side TOC/aside experience the mock shows, that's
a follow-up turn; this turn is the visual identity refresh.

Backwards compat: every `_help_*` attribute the existing callbacks read
is preserved with the same name.
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _


def build_help_tab(parent, app):
    """Help tab — restyled chrome around the existing tkinterweb viewer."""
    parent.configure(bg=COLORS['bg_1'])

    # ── Page head ──
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 12))

    tk.Label(head, text='\u2753  ' + _('Help & Tutorial'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    app._help_status_var = tk.StringVar(value='')
    tk.Label(head, textvariable=app._help_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             padx=8, pady=2,
             highlightbackground=COLORS['border_2'],
             highlightthickness=1
             ).pack(side='left', padx=(12, 0))

    # ── Toolbar ──
    toolbar = tk.Frame(parent, bg=COLORS['bg_1'])
    toolbar.pack(fill='x', padx=24, pady=(0, 10))

    _ghost_btn(toolbar, '\U0001f4c4  ' + _('Open in browser'),
               command=app._help_open_browser
               ).pack(side='left')
    _ghost_btn(toolbar, '\u21bb  ' + _('Check for updated tutorial'),
               command=app._help_fetch_latest
               ).pack(side='left', padx=(6, 0))
    _ghost_btn(toolbar, '\u2328  ' + _('Keyboard shortcuts'),
               command=app._show_keyboard_shortcuts
               ).pack(side='left', padx=(6, 0))

    # ── Embedded HTML viewer — tkinterweb if available, fallback otherwise ──
    viewer_frame = tk.Frame(parent, bg=COLORS['bg_2'],
                            highlightbackground=COLORS['border_3'],
                            highlightthickness=1)
    viewer_frame.pack(fill='both', expand=True, padx=24, pady=(0, 14))

    app._help_html_data  = [None]   # cached HTML bytes
    app._help_viewer     = [None]   # tkinterweb widget if available

    try:
        import tkinterweb
        hv = tkinterweb.HtmlFrame(viewer_frame, messages_enabled=False)
        hv.pack(fill='both', expand=True)
        app._help_viewer[0] = hv
    except ImportError:
        # Fallback: scrollable text with key FAQ content
        app._help_text_fallback(viewer_frame)

    # Load bundled tutorial immediately, then try to fetch latest
    app.after(100, app._help_load_bundled)
    app.after(2000, app._help_fetch_latest)


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
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
