"""
ui/tab_help.py — Help & Tutorial page (Settings › Help).

    PageHeader "Help & Tutorial"   [Keyboard shortcuts] [Check for updates] [Open in browser]
    status line (bundled / updated tutorial)
    ┌ On this page ┐ ┌ article (sections, questions, answers; ≤ 720px) ┐
    │ section links│ │                                                  │
    └──────────────┘ └──────────────────────────────────────────────────┘

The article is rendered by tkinterweb when it is installed (the bundled
tutorial HTML); otherwise `app._help_text_fallback(viewer_frame)` draws the
built-in documentation with `build_help_article` below.

Backwards compat: every `_help_*` attribute the existing callbacks read
is preserved with the same name (`_help_status_var`, `_help_html_data`,
`_help_viewer`).
"""

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import (PageHeader, Card, Button, IconLabel, InfoBar,
                               Divider, Rounded, autowrap)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _
from ui.shared.scroll import attach_scroll

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']
_ARTICLE_MAX = 720


def build_help_tab(parent, app):
    """Help page — header, status line and the documentation viewer."""
    parent.configure(bg=BG1)

    # ── Page head ──
    head = PageHeader(parent, _('Help & Tutorial'),
                      'Common questions about building, OSFMount and '
                      'your PS5.')
    head.pack(fill='x', padx=px(24), pady=(px(20), px(12)))

    Button(head.actions, text=_('Open in browser'), icon='external',
           variant='primary', command=app._help_open_browser
           ).pack(side='right')
    Button(head.actions, text=_('Check for updated tutorial'), icon='refresh',
           command=app._help_fetch_latest
           ).pack(side='right', padx=(0, px(8)))
    Button(head.actions, text=_('Keyboard shortcuts'), icon='keyboard',
           command=app._show_keyboard_shortcuts
           ).pack(side='right', padx=(0, px(8)))

    # ── Status line (set by the tutorial loader) ──
    app._help_status_var = tk.StringVar(value='')
    st = tk.Frame(parent, bg=BG1)
    st.pack(fill='x', padx=px(24), pady=(0, px(12)))
    IconLabel(st, 'info', 12, C['fg_4'], BG1).pack(side='left',
                                                   padx=(0, px(6)))
    tk.Label(st, textvariable=app._help_status_var, font=FONTS['caption'],
             bg=BG1, fg=C['fg_3'], anchor='w').pack(side='left')

    # ── Documentation viewer — tkinterweb if available, fallback otherwise ──
    viewer_frame = tk.Frame(parent, bg=BG1)
    viewer_frame.pack(fill='both', expand=True, padx=px(24), pady=(0, px(24)))

    app._help_html_data  = [None]   # cached HTML bytes
    app._help_viewer     = [None]   # tkinterweb widget if available

    try:
        import tkinterweb
        card = Card(viewer_frame, padding=2)
        card.pack(fill='both', expand=True)
        hv = tkinterweb.HtmlFrame(card, messages_enabled=False)
        hv.pack(fill='both', expand=True)
        app._help_viewer[0] = hv
    except ImportError:
        # Fallback: built-in documentation (sections, questions, answers)
        app._help_text_fallback(viewer_frame)

    # Load bundled tutorial immediately, then try to fetch latest
    app.after(100, app._help_load_bundled)
    app.after(2000, app._help_fetch_latest)


# ─────────────────────────────────────────────────────────────────────────
# Built-in documentation renderer (used by app._help_text_fallback)
# ─────────────────────────────────────────────────────────────────────────
def build_help_article(parent, app, meta, sections, note=None):
    """Draw a two-column documentation layout into `parent`.

    meta      one line under the article title (version / author)
    sections  [(title, [(question, answer), ...]), ...]
    note      optional info-bar text shown above the first section

    Answers whose lines start with "1." "2." … render as a numbered list.
    """
    parent.configure(bg=BG1)
    parent.grid_columnconfigure(1, weight=1)
    parent.grid_rowconfigure(0, weight=1)

    # ── Left: "On this page" ──
    toc = Card(parent, padding=16)
    toc.grid(row=0, column=0, sticky='nw', padx=(0, px(16)))
    tk.Label(toc, text='ON THIS PAGE', font=FONTS['overline'], bg=BG2,
             fg=C['fg_4'], anchor='w').pack(fill='x', pady=(0, px(8)))

    # ── Right: scrollable article card ──
    card = Card(parent, padding=0)
    card.grid(row=0, column=1, sticky='nsew')
    canvas = tk.Canvas(card, bg=BG2, highlightthickness=0, bd=0)
    sb = XScrollbar(card, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y', padx=(0, px(4)), pady=px(6))
    canvas.pack(side='left', fill='both', expand=True, padx=(px(2), 0),
                pady=px(2))
    body = tk.Frame(canvas, bg=BG2)
    win = canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>',
              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(win, width=e.width))
    attach_scroll(canvas)
    card.raise_decorations()

    wrap = px(_ARTICLE_MAX)
    col = tk.Frame(body, bg=BG2)
    col.pack(fill='x', padx=px(32), pady=(px(28), px(32)))

    # Keep a comfortable line length: the article column never grows
    # wider than _ARTICLE_MAX, whatever the window width.
    def _limit(e):
        right = max(px(32), e.width - px(32) - wrap)
        try:
            if col.pack_info().get('padx') != (px(32), right):
                col.pack_configure(padx=(px(32), right))
        except Exception:
            pass
    body.bind('<Configure>', _limit, add='+')

    tk.Label(col, text='Frequently asked questions', font=FONTS['title'],
             bg=BG2, fg=C['fg_0'], anchor='w').pack(fill='x')
    tk.Label(col, text=meta, font=FONTS['caption'], bg=BG2, fg=C['fg_4'],
             anchor='w').pack(fill='x', pady=(px(4), 0))
    if note:
        nb = InfoBar(col, kind='info', text=note)
        nb.pack(anchor='w', fill='x', pady=(px(16), 0))

    anchors = []
    for s_idx, (title, qas) in enumerate(sections):
        sec = tk.Frame(col, bg=BG2)
        sec.pack(fill='x', pady=(px(32), 0))
        anchors.append((title, sec))
        tk.Label(sec, text=title, font=FONTS['subtitle'], bg=BG2,
                 fg=C['fg_0'], anchor='w').pack(fill='x')
        Divider(sec, color=C['border_2']).pack(fill='x', pady=(px(8), 0))
        for question, answer in qas:
            q = tk.Frame(sec, bg=BG2)
            q.pack(fill='x', pady=(px(16), 0))
            autowrap(tk.Label(q, text=question, font=FONTS['body_strong'],
                              bg=BG2, fg=C['fg_0'], anchor='w',
                              justify='left')).pack(fill='x')
            _answer(q, answer, wrap)

    # Populate the table of contents now the anchors exist
    def _jump(target):
        try:
            body.update_idletasks()
            total = max(1, body.winfo_height())
            y = target.winfo_y() + target.master.winfo_y() - px(20)
            canvas.yview_moveto(max(0.0, y / total))
        except Exception:
            pass

    for title, sec in anchors:
        lnk = tk.Label(toc, text=title, font=FONTS['body'], bg=BG2,
                       fg=C['fg_2'], anchor='w', cursor='hand2')
        lnk.pack(fill='x', pady=px(4))
        lnk.bind('<Button-1>', lambda e, t=sec: _jump(t))
        lnk.bind('<Enter>', lambda e, w=lnk: w.config(fg=C['accent_hi']))
        lnk.bind('<Leave>', lambda e, w=lnk: w.config(fg=C['fg_2']))
    return canvas


def _answer(parent, answer, wrap):
    lines = [ln for ln in answer.split('\n') if ln.strip()]
    numbered = [ln for ln in lines if len(ln) > 2 and ln[0].isdigit()
                and ln[1] == '.']
    if lines and len(numbered) == len(lines):
        lst = tk.Frame(parent, bg=BG2)
        lst.pack(fill='x', pady=(px(6), 0))
        lst.grid_columnconfigure(1, weight=1)
        for r, ln in enumerate(lines):
            num, txt = ln.split('.', 1)
            tk.Label(lst, text=num + '.', font=FONTS['body'], bg=BG2,
                     fg=C['fg_4'], anchor='ne', width=2
                     ).grid(row=r, column=0, sticky='ne', padx=(0, px(8)),
                            pady=px(2))
            autowrap(tk.Label(lst, text=txt.strip(), font=FONTS['body'],
                              bg=BG2, fg=C['fg_2'], anchor='w',
                              justify='left')
                     ).grid(row=r, column=1, sticky='ew', pady=px(2))
        return
    autowrap(tk.Label(parent, text=answer, font=FONTS['body'], bg=BG2,
                      fg=C['fg_2'], anchor='w', justify='left')
             ).pack(fill='x', pady=(px(4), 0))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
    return Button(parent, text=text, command=command)
