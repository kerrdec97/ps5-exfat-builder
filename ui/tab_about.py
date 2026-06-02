"""
ui/tab_about.py — About tab.

Three sections:
  1. App identity (name, version, blurb)
  2. GitHub credits — researchers and tool authors this project depends on,
     each with a clickable link to their GitHub page
  3. Supporters — donors loaded from donors.json next to exfat_builder.py
     (named supporters + an anonymous-count line so we honour people who
     prefer to stay private)

If donors.json is missing or malformed, the tab still renders.

Pattern: matches ui/tab_help.py and ui/tab_advanced.py — exposes
`build_about_tab(parent, app)` and pulls colours/fonts from tkinter_theme.
"""

import json
import os
import sys
import tkinter as tk
import webbrowser

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, APP_VERSION


# ─────────────────────────────────────────────────────────────────────────
# GitHub credit list — kept in code so contributors don't go missing if
# donors.json is deleted or corrupted. Mirrors the credits section in
# the bundled tutorial HTML.
# ─────────────────────────────────────────────────────────────────────────
_GITHUB_CREDITS = [
    {
        'emoji':  '\u26a1',  # ⚡
        'name':   'Nazky',
        'role':   'PS5 Auto Backport pipeline',
        'url':    'https://github.com/Nazky',
    },
    {
        'emoji':  '\U0001f527',  # 🔧
        'name':   'BestPig',
        'role':   'PS5 backport research & BackPork payload',
        'url':    'https://github.com/BestPig',
    },
    {
        'emoji':  '\U0001f4be',  # 💾
        'name':   'drakmor',
        'role':   'ShadowMountPlus — exFAT creation reference',
        'url':    'https://github.com/drakmor',
    },
    {
        'emoji':  '\U0001f4e6',  # 📦
        'name':   'SvenGDK',
        'role':   'UFS2Tool — the ffpkg builder backend',
        'url':    'https://github.com/SvenGDK',
    },
    {
        'emoji':  '\U0001f4cb',  # 📋
        'name':   'ps5-payload-dev',
        'role':   'klogsrv — PS5 kernel log streamer',
        'url':    'https://github.com/ps5-payload-dev',
    },
    {
        'emoji':  '\U0001f3af',  # 🎯
        'name':   'NookieAI & stonemodder',
        'role':   'Inspiration (Porkfolio)',
        'url':    None,  # no shared GitHub
    },
]


# ─────────────────────────────────────────────────────────────────────────
# Donor file loading
# ─────────────────────────────────────────────────────────────────────────
# Module-level last-resort fallback. If both external donors.json AND
# exfat_builder._DEFAULT_DONORS_JSON fail (e.g. running against an older
# main file that doesn't have the baked-in constant), this is what we
# show. Edit this and the baked-in constant together when supporters
# change so the EXE always has correct credits even without donors.json.
_FALLBACK_DONORS = {
    'thank_you_message': (
        'A heartfelt thank you to everyone who has supported this tool. '
        'Your generosity keeps the project alive and growing.'),
    'donors': ['AceInTheHole'],
    'has_anonymous': True,
}


def _donors_path():
    """Return the absolute path to donors.json next to the main script /
    frozen exe. Works in both source and PyInstaller-bundled runs."""
    if getattr(sys, 'frozen', False):
        base = os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(
            sys.modules.get('exfat_builder').__file__
            if 'exfat_builder' in sys.modules else __file__))
        if os.path.basename(base) == 'ui':
            base = os.path.dirname(base)
    return os.path.join(base, 'donors.json')


def _load_donors():
    """Load donors. Tries in priority order:

      1. External donors.json next to the script/exe (lets you edit
         supporters without rebuilding the .exe).
      2. Baked-in _DEFAULT_DONORS_JSON inside exfat_builder.py (so the
         shipped .exe always shows credits with no sidecar file).
      3. Module-level _FALLBACK_DONORS dict in this file (last resort
         if both above fail — e.g. running this tab module against an
         older main file).

    Returns:
        (thank_you_message: str,
         named_donors:      list[str],
         has_anonymous:     bool,
         source:            str)
              # 'external' | 'baked-in' | 'fallback'
    """
    path = _donors_path()

    # 1) External donors.json
    if os.path.isfile(path):
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            return _parse_donor_dict(data) + ('external',)
        except Exception:
            # Malformed JSON — fall through to baked-in
            pass

    # 2) Baked-in default from main file
    try:
        from exfat_builder import _DEFAULT_DONORS_JSON
        data = json.loads(_DEFAULT_DONORS_JSON)
        return _parse_donor_dict(data) + ('baked-in',)
    except Exception:
        # Old main file without the constant, or corrupt content — fall
        # through to the module-local fallback dict.
        pass

    # 3) Module-level fallback — guarantees credits ALWAYS render
    return _parse_donor_dict(_FALLBACK_DONORS) + ('fallback',)


def _parse_donor_dict(data):
    """Pull thank_you_message / donors / has_anonymous out of a dict.
    Returns (msg, donors_list, has_anonymous_bool)."""
    msg = data.get('thank_you_message', '') or ''
    donors = data.get('donors', []) or []
    donors = [str(d).strip() for d in donors if str(d).strip()]
    # New format: has_anonymous (bool). Tolerate old format
    # (anonymous_count: int) by treating any positive count as True.
    if 'has_anonymous' in data:
        has_anon = bool(data.get('has_anonymous'))
    else:
        try:
            has_anon = int(data.get('anonymous_count', 0) or 0) > 0
        except (TypeError, ValueError):
            has_anon = False
    return (msg, donors, has_anon)


# ─────────────────────────────────────────────────────────────────────────
# Tab builder
# ─────────────────────────────────────────────────────────────────────────
def build_about_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # ── Scrollable outer wrap (so the About content fits on smaller windows) ──
    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0,
                       highlightthickness=0)
    canvas.pack(side='left', fill='both', expand=True)
    sb = tk.Scrollbar(parent, orient='vertical', command=canvas.yview,
                       bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    sb.pack(side='right', fill='y')
    canvas.configure(yscrollcommand=sb.set)

    inner = tk.Frame(canvas, bg=COLORS['bg_1'])
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig(inner_id, width=e.width))
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    # ── Page head ──
    head = tk.Frame(inner, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 6))
    tk.Label(head, text='\u2139  ' + _('Credits'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')
    tk.Label(head,
             text='\u2014  ' + _('Contributors, supporters, and app info'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(12, 0), pady=(2, 0))

    # ── App identity card ──
    id_card = tk.Frame(inner, bg=COLORS['bg_2'],
                       highlightbackground=COLORS['border_3'],
                       highlightthickness=1)
    id_card.pack(fill='x', padx=24, pady=(8, 12))
    id_inner = tk.Frame(id_card, bg=COLORS['bg_2'])
    id_inner.pack(fill='x', padx=16, pady=12)

    tk.Label(id_inner, text='exFAT Image Builder',
             font=(FONTS['h2'][0], 16, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(id_inner, text=_('Version') + ' ' + APP_VERSION +
                       '  \u2022  ' + _('by DecKerr97'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(anchor='w', pady=(2, 0))
    tk.Label(id_inner,
             text=_('Build PS5 .exfat and .ffpkg images, manage your '
                    'library, and upload to your console.'),
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['fg_2'],
             wraplength=820, justify='left'
             ).pack(anchor='w', pady=(8, 0))

    # ── GitHub credits card ──
    cred_card = tk.Frame(inner, bg=COLORS['bg_2'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    cred_card.pack(fill='x', padx=24, pady=(0, 12))
    cred_inner = tk.Frame(cred_card, bg=COLORS['bg_2'])
    cred_inner.pack(fill='x', padx=16, pady=12)

    tk.Label(cred_inner,
             text='\U0001f31f  ' + _('Thank you to the GitHub community'),
             font=(FONTS['h2'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(cred_inner,
             text=_('This tool stands on the work of these researchers '
                    'and developers — without them, none of this would '
                    'exist. Click a name to visit their GitHub.'),
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'],
             wraplength=820, justify='left'
             ).pack(anchor='w', pady=(6, 12))

    for cred in _GITHUB_CREDITS:
        _credit_row(cred_inner, cred)

    # ── Supporters card ──
    sup_card = tk.Frame(inner, bg=COLORS['bg_2'],
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
    sup_card.pack(fill='both', expand=True, padx=24, pady=(0, 14))
    sup_inner = tk.Frame(sup_card, bg=COLORS['bg_2'])
    sup_inner.pack(fill='both', expand=True, padx=16, pady=12)

    tk.Label(sup_inner,
             text='\u2764  ' + _('Thank you to our supporters'),
             font=(FONTS['h2'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(anchor='w')

    msg, donors, has_anon, source = _load_donors()
    if not msg:
        msg = _('A heartfelt thank you to everyone who has supported '
                'this tool. Your generosity keeps the project alive.')
    tk.Label(sup_inner, text=msg,
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'],
             wraplength=820, justify='left'
             ).pack(anchor='w', pady=(6, 10))

    # Hairline divider
    tk.Frame(sup_inner, bg=COLORS['border_3'], height=1
             ).pack(fill='x', pady=(0, 10))

    # Names list
    list_frame = tk.Frame(sup_inner, bg=COLORS['bg_3'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    list_frame.pack(fill='both', expand=True)

    txt = tk.Text(list_frame,
                  font=FONTS['body'],
                  bg=COLORS['bg_3'], fg=COLORS['fg_1'],
                  relief='flat', bd=0,
                  padx=14, pady=10,
                  wrap='word',
                  cursor='arrow',
                  highlightthickness=0,
                  height=6)
    txt_sb = tk.Scrollbar(list_frame, orient='vertical',
                           command=txt.yview)
    txt.config(yscrollcommand=txt_sb.set)
    txt_sb.pack(side='right', fill='y')
    txt.pack(side='left', fill='both', expand=True)

    # Tag setup for nicer look
    txt.tag_configure('name',
                       foreground=COLORS['fg_1'],
                       font=FONTS['body'])
    txt.tag_configure('anon',
                       foreground=COLORS['fg_3'],
                       font=FONTS['body'])

    # Always render the list — _load_donors() is guaranteed to return
    # valid data (external → baked-in → module fallback). Users never
    # see "Could not load..." errors.
    any_content = False
    for name in donors:
        txt.insert('end', '  \u2022  ' + name + '\n', 'name')
        any_content = True
    if has_anon:
        txt.insert('end',
                   '  \u2022  ' + _('Anonymous supporters') + '\n',
                   'anon')
        any_content = True
    if not any_content:
        txt.insert('end',
                   _('No supporters listed yet. Be the first!')
                   + '\n')

    txt.config(state='disabled')

    # ── Footer: refresh + donate ──
    foot = tk.Frame(inner, bg=COLORS['bg_1'])
    foot.pack(fill='x', padx=24, pady=(0, 14))

    def _refresh():
        # Re-read donors.json and rebuild the tab in place
        for w in parent.winfo_children():
            w.destroy()
        build_about_tab(parent, app)

    _ghost_btn(foot, '\u21bb  ' + _('Reload donor list'),
               command=_refresh
               ).pack(side='left')

    donate_url = getattr(app, '_donate_url', None) or \
        'https://ko-fi.com/'

    def _open_donate():
        try:
            webbrowser.open(donate_url)
        except Exception:
            pass

    _ghost_btn(foot, '\u2615  ' + _('Support the tool'),
               command=_open_donate
               ).pack(side='left', padx=(6, 0))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _credit_row(parent, cred):
    """Single GitHub credit row: emoji + name (clickable) + role."""
    row = tk.Frame(parent, bg=COLORS['bg_2'])
    row.pack(fill='x', pady=(0, 6))

    # Emoji
    tk.Label(row, text=cred['emoji'],
             font=(FONTS['body'][0], 14),
             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
             width=2
             ).pack(side='left', padx=(0, 4))

    # Name — clickable if a URL is set
    name_lbl = tk.Label(row, text=cred['name'],
                         font=(FONTS['body'][0], 10, 'bold'),
                         bg=COLORS['bg_2'],
                         fg=COLORS['accent'] if cred['url']
                             else COLORS['fg_1'])
    name_lbl.pack(side='left')
    if cred['url']:
        name_lbl.config(cursor='hand2')
        def _open(_e=None, u=cred['url']):
            try:
                webbrowser.open(u)
            except Exception:
                pass
        name_lbl.bind('<Button-1>', _open)
        # Hover effect
        def _hover_in(_e=None, w=name_lbl):
            w.config(fg=COLORS.get('accent_hi', COLORS['accent']))
        def _hover_out(_e=None, w=name_lbl):
            w.config(fg=COLORS['accent'])
        name_lbl.bind('<Enter>', _hover_in)
        name_lbl.bind('<Leave>', _hover_out)

    # Role
    tk.Label(row, text='\u2014  ' + cred['role'],
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']
             ).pack(side='left', padx=(8, 0))


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
