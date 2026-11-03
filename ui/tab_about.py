"""
ui/tab_about.py — Credits page (Farewell Edition).

    PageHeader "Credits"                       [Report Issue] [Support the tool]
    Farewell card   product name, version, final-release note, project links
    ┌ Contributors (GitHub links) ─────┐ ┌ Supporters (stats + names) ──────┐
    │ Acknowledgements                 │ │ Licences (bundled components)    │
    └──────────────────────────────────┘ └──────────────────────────────────┘

Supporters are loaded from donors.json next to exfat_builder.py (named
supporters + an anonymous line so we honour people who prefer to stay
private). If donors.json is missing or malformed, the page still renders.

Pattern: matches ui/tab_help.py and ui/tab_advanced.py — exposes
`build_about_tab(parent, app)` and pulls colours/fonts from tkinter_theme.
"""

import json
import os
import sys
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
import webbrowser
from ui.shared.widgets import (
    PageHeader, Card, Badge, Button, IconButton, IconLabel, Divider, Tooltip,
    Rounded, autowrap,
)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, APP_VERSION
from ui.shared.scroll import attach_scroll


# ─────────────────────────────────────────────────────────────────────────
# GitHub credit list — kept in code so contributors don't go missing if
# donors.json is deleted or corrupted. Mirrors the credits section in
# the bundled tutorial HTML.
# ─────────────────────────────────────────────────────────────────────────
_GITHUB_CREDITS = [
    {
        'icon':   'zap',
        'name':   'Nazky',
        'role':   'PS5 Auto Backport pipeline',
        'url':    'https://github.com/Nazky',
    },
    {
        'icon':   'wrench',
        'name':   'BestPig',
        'role':   'PS5 backport research & BackPork payload',
        'url':    'https://github.com/BestPig',
    },
    {
        'icon':   'drive',
        'name':   'drakmor',
        'role':   'ShadowMountPlus — exFAT creation reference',
        'url':    'https://github.com/drakmor',
    },
    {
        'icon':   'package',
        'name':   'SvenGDK',
        'role':   'UFS2Tool — the ffpkg builder backend',
        'url':    'https://github.com/SvenGDK',
    },
    {
        'icon':   'klog',
        'name':   'ps5-payload-dev',
        'role':   'klogsrv — PS5 kernel log streamer',
        'url':    'https://github.com/ps5-payload-dev',
    },
    {
        'icon':   'target',
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
    'donors': ['AceInTheHole', 'gas17', 'Helio Rogerio Silva Lopes', 'BigBoss83', 'Long Ho'],
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


_GH_RELEASES_API = ('https://api.github.com/repos/'
                    'kerrdec97/ps5-exfat-builder/releases?per_page=100')


def _fetch_github_downloads(on_done):
    """Sum asset download counts across all GitHub releases of
    kerrdec97/ps5-exfat-builder (same numbers as the github-release-
    stats page). Runs in a daemon thread; calls on_done(total or None)
    from the worker — the caller marshals to the UI thread. Fully
    guarded: offline / rate-limited / parse errors all yield None."""
    import threading

    def _worker():
        total = None
        try:
            import urllib.request
            req = urllib.request.Request(
                _GH_RELEASES_API,
                headers={'User-Agent': 'exFAT-Image-Builder',
                         'Accept': 'application/vnd.github+json'})
            with urllib.request.urlopen(req, timeout=8) as resp:
                releases = json.loads(resp.read().decode('utf-8'))
            total = 0
            for rel in releases or []:
                for asset in rel.get('assets', []) or []:
                    try:
                        total += int(asset.get('download_count', 0) or 0)
                    except (TypeError, ValueError):
                        pass
        except Exception:
            total = None
        try:
            on_done(total)
        except Exception:
            pass

    threading.Thread(target=_worker, daemon=True).start()


def _load_donor_extras():
    """Optional stats keys from donors.json (total_supporters,
    kofi_total, total_downloads). Lenient: any missing key returns
    None. Lets Dec surface Ko-fi totals by editing the sidecar file —
    no code change, no behavior change when absent."""
    try:
        path = _donors_path()
        if os.path.isfile(path):
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
        else:
            from exfat_builder import _DEFAULT_DONORS_JSON
            data = json.loads(_DEFAULT_DONORS_JSON)
        return {
            'total_supporters': data.get('total_supporters'),
            'total_downloads': data.get('total_downloads'),
        }
    except Exception:
        return {}


# ─────────────────────────────────────────────────────────────────────────
# Acknowledgements and licences — display data mirroring
# THIRD_PARTY_NOTICES.md (only entries whose licence was verified there).
# ─────────────────────────────────────────────────────────────────────────
_ACKNOWLEDGEMENTS = [
    ('PSBrew (Renan)', 'Original MkPFS project and PFS format research'),
    ('john-tornblom', 'PS5 payload SDK, the basis of the fake-signing tools'),
    ('idlesauce', 'Original SDK version patcher'),
    ('PSDevWiki and the PS5 community', 'PFS, PKG and FPKG references'),
    ('Translators and testers', 'Seventeen interface languages and every bug report'),
]

_LICENCES = [
    ('Lazy_MkPFS', 'Nazky, from PSBrew/MkPFS', 'GPL-3.0'),
    ('UFS2Tool', 'SvenGDK', 'BSD-2-Clause'),
    ('make_fself / decrypt_fself', 'john-tornblom, modified by Nazky', 'GPL-3.0'),
    ('ShadowMountPlus', 'drakmor', 'GPL-3.0'),
    ('ftpsrv and klogsrv', 'ps5-payload-dev', 'GPL-3.0'),
    ('Lucide icons', 'Lucide contributors', 'ISC'),
]

_REPO_URL = 'https://github.com/kerrdec97/ps5-exfat-builder'
_NOTICES_URL = _REPO_URL + '/blob/main/THIRD_PARTY_NOTICES.md'

BG1, BG2, BG3 = COLORS['bg_1'], COLORS['bg_2'], COLORS['bg_3']


def _open_url(url):
    try:
        webbrowser.open(url)
    except Exception:
        pass


def _open_notices():
    """Open the bundled THIRD_PARTY_NOTICES.md, else the copy on GitHub."""
    path = None
    try:
        from exfat_builder import resource_path
        path = resource_path('THIRD_PARTY_NOTICES.md')
    except Exception:
        path = None
    if path and os.path.isfile(path):
        try:
            if os.name == 'nt':
                os.startfile(path)  # noqa
                return
            _open_url('file:///' + path.replace('\\', '/'))
            return
        except Exception:
            pass
    _open_url(_NOTICES_URL)


# ─────────────────────────────────────────────────────────────────────────
# Tab builder
# ─────────────────────────────────────────────────────────────────────────
def build_about_tab(parent, app):
    parent.configure(bg=BG1)

    # ── Scrollable page ──
    canvas = tk.Canvas(parent, bg=BG1, bd=0, highlightthickness=0)
    canvas.pack(side='left', fill='both', expand=True)
    sb = XScrollbar(parent, orient='vertical', command=canvas.yview)
    sb.pack(side='right', fill='y')
    canvas.configure(yscrollcommand=sb.set)

    inner = tk.Frame(canvas, bg=BG1)
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig(inner_id, width=e.width))
    attach_scroll(canvas)

    donate_url = getattr(app, '_donate_url', None) \
        or globals().get('KOFI_URL') \
        or 'https://ko-fi.com/deckerr9746220'

    def _open_donate():
        try:
            webbrowser.open(donate_url)
        except Exception:
            pass

    def _open_report():
        try:
            app._show_report_issue()
        except Exception:
            pass

    def _refresh():
        # Re-read donors.json and rebuild the tab in place
        for w in parent.winfo_children():
            w.destroy()
        build_about_tab(parent, app)

    # ── Page header ──
    head = PageHeader(inner, _('Credits'),
                      _('Contributors, supporters, and app info'))
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    Button(head.actions, text=_('Support the tool'), icon='heart',
           variant='primary', command=_open_donate).pack(side='right')
    Button(head.actions, text=_('Report Issue'), icon='bug',
           command=_open_report).pack(side='right', padx=(0, px(8)))

    # ── Farewell card ──
    _build_farewell_card(inner, app)

    # ── Two columns ──
    cols = tk.Frame(inner, bg=BG1)
    cols.pack(fill='x', padx=px(24), pady=(0, px(24)))
    cols.grid_columnconfigure(0, weight=7, uniform='credits')
    cols.grid_columnconfigure(1, weight=5, uniform='credits')
    left = tk.Frame(cols, bg=BG1)
    left.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))
    right = tk.Frame(cols, bg=BG1)
    right.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))

    _build_contributors_card(left)
    _build_ack_card(left)
    _build_supporters_card(right, parent, _refresh)
    _build_licences_card(right)


# ─────────────────────────────────────────────────────────────────────────
# Sections
# ─────────────────────────────────────────────────────────────────────────
def _build_farewell_card(inner, app):
    card = Card(inner, padding=24)
    card.pack(fill='x', padx=px(24), pady=(0, px(16)))

    top = tk.Frame(card, bg=BG2)
    top.pack(fill='x')
    tk.Label(top, text='exFAT Image Builder', font=FONTS['title'],
             bg=BG2, fg=COLORS['fg_0']).pack(side='left')
    tk.Label(top, text='Farewell Edition', font=FONTS['subtitle'],
             bg=BG2, fg=COLORS['fg_3']).pack(side='left', padx=(px(10), 0),
                                             pady=(px(3), 0))
    Badge(top, 'v' + APP_VERSION, 'outline', mono=True,
          surface=BG2).pack(side='left', padx=(px(12), 0), pady=(px(3), 0))
    Badge(top, 'Final release', 'neutral', icon='heart',
          surface=BG2).pack(side='left', padx=(px(6), 0), pady=(px(3), 0))

    tk.Label(card, text=_('Version') + ' ' + APP_VERSION +
                       '  ·  ' + _('by DecKerr97'),
             font=FONTS['caption'], bg=BG2, fg=COLORS['fg_4'],
             anchor='w').pack(fill='x', pady=(px(4), 0))

    text_col = tk.Frame(card, bg=BG2)
    text_col.pack(fill='x', pady=(px(14), 0))
    blurb = tk.Label(text_col,
             text=_('Build PS5 .exfat and .ffpkg images, manage your '
                    'library, and upload to your console.'),
             font=FONTS['body'], bg=BG2, fg=COLORS['fg_1'],
             anchor='w', justify='left', wraplength=px(720))
    blurb.pack(anchor='w')
    tk.Label(text_col,
             text=('This is the last version of exFAT Image Builder. It '
                   'exists because of the people on this page — the '
                   'researchers whose work it builds on, everyone who '
                   'reported a bug or translated a screen, and the '
                   'supporters who kept it going. Thank you.'),
             font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'],
             anchor='w', justify='left', wraplength=px(720)
             ).pack(anchor='w', pady=(px(6), 0))

    links = tk.Frame(card, bg=BG2)
    links.pack(fill='x', pady=(px(16), 0))

    def _whats_new():
        try:
            from ui.release_notes import show_release_modal
            show_release_modal(app, force=True)
        except Exception:
            pass

    def _changelog():
        try:
            from ui.release_notes import _open_changelog
            _open_changelog(app)
        except Exception:
            _open_url(_REPO_URL + '/releases')

    for i, (label, icon, cmd) in enumerate((
            ('GitHub repository', 'github', lambda: _open_url(_REPO_URL)),
            ("What's new", 'sparkles', _whats_new),
            ('Changelog', 'file_text', _changelog),
            ('Third-party notices', 'shield', _open_notices))):
        Button(links, text=label, icon=icon, command=cmd, size='sm',
               surface=BG2).pack(side='left', padx=(0 if i == 0 else px(8), 0))


def _build_contributors_card(left):
    card = Card(left, title=_('Contributors'),
                subtitle=_('This tool stands on the work of these researchers '
                           'and developers — without them, none of this would '
                           'exist. Click a name to visit their GitHub.'))
    card.pack(fill='x')
    for i, cred in enumerate(_GITHUB_CREDITS):
        if i:
            Divider(card.body, color=COLORS['border_1']).pack(fill='x')
        _credit_row(card.body, cred)


def _build_ack_card(left):
    card = Card(left, title='Acknowledgements',
                subtitle='Research, references and help this project '
                         'learned from. No code from these sources is '
                         'bundled unless it is listed under Licences.')
    card.pack(fill='x', pady=(px(16), 0))
    grid = tk.Frame(card.body, bg=BG2)
    grid.pack(fill='x')
    grid.grid_columnconfigure(1, weight=1)
    for r, (name, what) in enumerate(_ACKNOWLEDGEMENTS):
        tk.Label(grid, text=name, font=FONTS['body_strong'], bg=BG2,
                 fg=COLORS['fg_0'], anchor='w'
                 ).grid(row=r, column=0, sticky='nw', pady=px(5),
                        padx=(0, px(16)))
        w = tk.Label(grid, text=what, font=FONTS['body'], bg=BG2,
                     fg=COLORS['fg_2'], anchor='w', justify='left')
        w.grid(row=r, column=1, sticky='ew', pady=px(5))
        autowrap(w)


def _build_supporters_card(right, parent, _refresh):
    card = Card(right, title=_('Supporters'))
    card.pack(fill='x')
    IconButton(card.head_actions, 'refresh', _refresh,
               tooltip=_('Reload donor list'), surface=BG2).pack()

    msg, donors, has_anon, source = _load_donors()
    extras = _load_donor_extras()

    # Stats: supporter total + GitHub release downloads (live from the
    # GitHub API; donors.json value as the fallback while it loads / when
    # offline).
    n_named = len(donors)
    total_sup = extras.get('total_supporters')
    if total_sup in (None, ''):
        total_sup = n_named + (1 if has_anon else 0)
    strip = tk.Frame(card.body, bg=BG2)
    strip.pack(fill='x')
    for i in range(2):
        strip.grid_columnconfigure(i, weight=1, uniform='sup')

    def _sup_cell(col, value, caption):
        cell = Rounded(strip, bg=BG3, border=COLORS['border_2'], radius=8,
                       padding=(px(14), px(10)), surface=BG2)
        cell.grid(row=0, column=col, sticky='nsew',
                  padx=(0 if col == 0 else px(6), 0 if col else px(6)))
        val_lbl = tk.Label(cell, text=str(value), font=FONTS['stat'],
                           bg=BG3, fg=COLORS['fg_0'], anchor='w')
        val_lbl.pack(fill='x')
        cap = tk.Label(cell, text=caption, font=FONTS['overline'], bg=BG3,
                       fg=COLORS['fg_4'], anchor='w', justify='left')
        cap.pack(fill='x', pady=(px(2), 0))
        autowrap(cap)
        return val_lbl

    _sup_cell(0, total_sup, _('Total supporters').upper())
    dl_lbl = _sup_cell(1, extras.get('total_downloads') or '—',
                       _('Total downloads · GitHub').upper())

    # Worker → UI handoff via a result box polled on the Tk thread
    # (widget.after from a worker thread isn't reliable).
    _dl_box = {'total': False}          # False = pending, None = failed

    def _poll_downloads(ticks=0):
        try:
            if not dl_lbl.winfo_exists():
                return
            total = _dl_box['total']
            if total is False:
                if ticks < 40:          # ~12 s budget
                    parent.after(300,
                                 lambda: _poll_downloads(ticks + 1))
                return
            if total is not None:
                dl_lbl.config(text='{:,}'.format(total))
        except Exception:
            pass

    _fetch_github_downloads(
        lambda total: _dl_box.__setitem__('total', total))
    parent.after(300, _poll_downloads)

    if not msg:
        msg = _('A heartfelt thank you to everyone who has supported '
                'this tool. Your generosity keeps the project alive.')
    m = tk.Label(card.body, text=msg, font=FONTS['body'], bg=BG2,
                 fg=COLORS['fg_2'], anchor='w', justify='left')
    m.pack(fill='x', pady=(px(14), px(12)))
    autowrap(m)

    Divider(card.body, color=COLORS['border_1']).pack(fill='x',
                                                     pady=(0, px(10)))

    # Names — two columns, heart marks
    names = tk.Frame(card.body, bg=BG2)
    names.pack(fill='x')
    names.grid_columnconfigure(0, weight=1, uniform='names')
    names.grid_columnconfigure(1, weight=1, uniform='names')
    entries = [(n, COLORS['fg_1']) for n in donors]
    if has_anon:
        entries.append((_('Anonymous supporters'), COLORS['fg_3']))
    if not entries:
        tk.Label(names, text=_('No supporters listed yet. Be the first!'),
                 font=FONTS['body'], bg=BG2, fg=COLORS['fg_3'],
                 anchor='w').grid(row=0, column=0, columnspan=2, sticky='w')
    for i, (name, fg) in enumerate(entries):
        cell = tk.Frame(names, bg=BG2)
        cell.grid(row=i // 2, column=i % 2, sticky='ew', pady=px(4))
        IconLabel(cell, 'heart', 12, COLORS['fg_4'], BG2).pack(
            side='left', padx=(0, px(8)))
        lbl = tk.Label(cell, text=name, font=FONTS['body'], bg=BG2, fg=fg,
                       anchor='w', justify='left')
        lbl.pack(side='left', fill='x', expand=True)


def _build_licences_card(right):
    card = Card(right, title='Licences',
                subtitle='Open-source components bundled with or sent by '
                         'the app. Full texts ship in the LICENSES folder.')
    card.pack(fill='x', pady=(px(16), 0))
    for i, (comp, author, lic) in enumerate(_LICENCES):
        if i:
            Divider(card.body, color=COLORS['border_1']).pack(fill='x')
        row = tk.Frame(card.body, bg=BG2)
        row.pack(fill='x', pady=px(8))
        Badge(row, lic, 'neutral', surface=BG2).pack(side='right')
        col = tk.Frame(row, bg=BG2)
        col.pack(side='left', fill='x', expand=True)
        tk.Label(col, text=comp, font=FONTS['body_strong'], bg=BG2,
                 fg=COLORS['fg_0'], anchor='w').pack(fill='x')
        tk.Label(col, text=author, font=FONTS['caption'], bg=BG2,
                 fg=COLORS['fg_3'], anchor='w').pack(fill='x')
    Divider(card.body, color=COLORS['border_1']).pack(fill='x',
                                                     pady=(px(4), px(12)))
    Button(card.body, text='View third-party notices', icon='file_text',
           command=_open_notices, surface=BG2).pack(anchor='w')


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _credit_row(parent, cred):
    """Contributor row: icon tile · name (link) · role · external link."""
    row = tk.Frame(parent, bg=BG2)
    row.pack(fill='x', pady=px(8))

    tile = Rounded(row, bg=BG3, radius=8, surface=BG2)
    tile.pack(side='left', padx=(0, px(12)))
    IconLabel(tile, cred.get('icon') or 'users', 16, COLORS['fg_2'],
              BG3).pack(padx=px(8), pady=px(8))

    url = cred.get('url')
    if url:
        IconButton(row, 'external', lambda u=url: _open_url(u),
                   tooltip=url.replace('https://', ''), size='sm',
                   surface=BG2).pack(side='right')

    col = tk.Frame(row, bg=BG2)
    col.pack(side='left', fill='x', expand=True)
    name_lbl = tk.Label(col, text=cred['name'], font=FONTS['body_strong'],
                        bg=BG2,
                        fg=COLORS['accent_hi'] if url else COLORS['fg_0'],
                        anchor='w')
    name_lbl.pack(fill='x')
    if url:
        name_lbl.config(cursor='hand2')
        name_lbl.bind('<Button-1>', lambda _e, u=url: _open_url(u))
        f_norm = (FONTS['body_strong'][0], FONTS['body_strong'][1])
        name_lbl.bind('<Enter>', lambda _e, w=name_lbl:
                      w.config(font=f_norm + ('underline',)))
        name_lbl.bind('<Leave>', lambda _e, w=name_lbl:
                      w.config(font=FONTS['body_strong']))
    tk.Label(col, text=cred['role'], font=FONTS['caption'], bg=BG2,
             fg=COLORS['fg_3'], anchor='w').pack(fill='x', pady=(px(1), 0))
