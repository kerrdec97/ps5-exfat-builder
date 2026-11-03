"""
ui/tab_language.py — Language picker (Settings › Language).

Design-system pass (Farewell Edition):

    Interface language                      [Import] [Export] [Apply]
    17 languages · N fully translated · community-maintained
    ┌─ languages ──────────┐ ┌─ selected language ───────────────────┐
    │ [search…]            │ │ [DE] German  Deutsch · 106 strings      │
    │ GB English (UK) 100% │ │ Coverage ▬▬▬▬  Strings  Applied  Contr. │
    │ US English (US) 100% │ └─────────────────────────────────────────┘
    │ FR French        92% │ ┌─ Live preview ──────┐ ┌─ Strings ───────┐
    │ ▌DE German       92% │ │ Build exFAT image   │ │ key → value     │
    │ …                    │ │ [Browse] [Build]    │ │ …               │
    └──────────────────────┘ └─────────────────────┘ └─────────────────┘

Flags are shown as small region-code tiles (emoji flags render as boxes on
many Windows builds). Picking a row previews it; Apply commits it.

Backwards compat: relies on `app._lang_var` (set in App.__init__),
`set_language()` and `save_settings()` from main, `app._rebuild_all_tabs()`,
and `app._switch_tab()`. All exist and are unchanged.
"""

import json
import os
import tkinter as tk
from ui.shared.widgets import (
    Button as XButton, TextField as XEntry, Scrollbar as XScrollbar,
    Card, Rounded, Badge, PageHeader, Divider, IconLabel, RoundedBar,
    autowrap)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import (
    _, set_language, save_settings, _TRANSLATIONS,
)
from ui.shared.scroll import attach_scroll
from ui.shared.app_dialog import app_error, app_info

C = COLORS


# Master list of supported language codes with native + English name.
# Order roughly mirrors the mock and the existing tab.
_LANGS = [
    ('en',    'English (UK)',         'English',       '\U0001f1ec\U0001f1e7'),  # 🇬🇧
    ('en_us', 'English (US)',         'English',       '\U0001f1fa\U0001f1f8'),  # 🇺🇸
    ('fr',    'French',               'Français',      '\U0001f1eb\U0001f1f7'),
    ('de',    'German',               'Deutsch',       '\U0001f1e9\U0001f1ea'),
    ('es',    'Spanish',              'Español',       '\U0001f1ea\U0001f1f8'),
    ('it',    'Italian',              'Italiano',      '\U0001f1ee\U0001f1f9'),
    ('pt',    'Portuguese',           'Português',     '\U0001f1f5\U0001f1f9'),
    ('nl',    'Dutch',                'Nederlands',    '\U0001f1f3\U0001f1f1'),
    ('pl',    'Polish',               'Polski',        '\U0001f1f5\U0001f1f1'),
    ('ru',    'Russian',              'Русский',       '\U0001f1f7\U0001f1fa'),
    ('tr',    'Turkish',              'Türkçe',        '\U0001f1f9\U0001f1f7'),
    ('ja',    'Japanese',             '日本語',         '\U0001f1ef\U0001f1f5'),
    ('zh',    'Chinese (Simplified)', '简体中文',       '\U0001f1e8\U0001f1f3'),
    ('ko',    'Korean',               '한국어',         '\U0001f1f0\U0001f1f7'),
    ('ar',    'Arabic',               'العربية',        '\U0001f1f8\U0001f1e6'),
    ('th',    'Thai',                 'ภาษาไทย',        '\U0001f1f9\U0001f1ed'),
    ('vi',    'Vietnamese',           'Tiếng Việt',    '\U0001f1fb\U0001f1f3'),
    ('id',    'Indonesian',           'Bahasa Indonesia', '\U0001f1ee\U0001f1e9'),
]


def _region(flag):
    """'🇩🇪' -> 'DE' (regional-indicator pair to its two letters)."""
    try:
        return ''.join(chr(ord(ch) - 0x1F1E6 + ord('A')) for ch in flag
                       if 0x1F1E6 <= ord(ch) <= 0x1F1FF) or '··'
    except Exception:
        return '··'


def _code_tile(parent, flag, bg, big=False):
    """Small rounded region-code tile standing in for an emoji flag."""
    tile = Rounded(parent, bg=C['bg_4'] if big else C['bg_3'],
                   border=C['border_3'], radius=4,
                   padding=(px(10), px(8)) if big else (px(5), px(2)),
                   surface=bg)
    tk.Label(tile, text=_region(flag),
             font=FONTS['subtitle'] if big else FONTS['caption_strong'],
             bg=C['bg_4'] if big else C['bg_3'], fg=C['fg_1'],
             width=2 if not big else 3).pack()
    return tile


def build_language_tab(parent, app):
    """Build the Language tab into `parent`."""
    parent.configure(bg=C['bg_1'])

    # Compute coverage so the bars + status panel can show real numbers.
    # English is the source of truth — its key set is the maximum.
    base_keys = _english_string_count()
    coverage = {}
    for code, _name, _native, _flag in _LANGS:
        if code == 'en' or code == 'en_us':
            coverage[code] = (base_keys, base_keys, 100)
        else:
            d = _TRANSLATIONS.get(code, {})
            n = len(d)
            pct = int(round(n / base_keys * 100)) if base_keys else 0
            coverage[code] = (n, base_keys, pct)
    app._lang_coverage = coverage

    # State for the right-pane preview
    app._lang_search_var = tk.StringVar()
    # The "currently shown" language in the right pane (for preview);
    # starts at the saved active language.
    app._lang_preview_code = tk.StringVar(value=app._lang_var.get())

    # ── Page header with the page actions (Apply is the primary) ──
    n_total = len(_LANGS)
    n_full = sum(1 for c in coverage.values() if c[2] >= 100)
    head = PageHeader(parent, _('Interface language'),
                      _('%d languages · %d fully translated · '
                        'community-maintained') % (n_total, n_full),
                      bg=C['bg_1'])
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    XButton(head.actions, text=_('Apply'), variant='primary', icon='check',
            command=lambda: _apply_lang(app, app._lang_preview_code.get())
            ).pack(side='right')
    XButton(head.actions, text=_('Export .json'), icon='download',
            command=lambda: _export_json(app, app._lang_preview_code.get())
            ).pack(side='right', padx=(0, px(6)))
    XButton(head.actions, text=_('Import .json'), icon='upload',
            command=lambda: _import_json(app)
            ).pack(side='right', padx=(0, px(6)))

    # ── 2-column workspace ──
    body = tk.Frame(parent, bg=C['bg_1'])
    body.pack(fill='both', expand=True, padx=px(24), pady=(0, px(20)))
    body.grid_columnconfigure(0, weight=0, minsize=px(300))
    body.grid_columnconfigure(1, weight=1)
    body.grid_rowconfigure(0, weight=1)

    _build_left_pane(body, app).grid(row=0, column=0, sticky='nsew',
                                     padx=(0, px(16)))
    _build_right_pane(body, app).grid(row=0, column=1, sticky='nsew')


# ─────────────────────────────────────────────────────────────────────────
# Left pane — search + scrollable language list
# ─────────────────────────────────────────────────────────────────────────
def _build_left_pane(parent, app):
    pane = Card(parent, padding=0)
    pane.configure(width=px(300))

    search_row = tk.Frame(pane, bg=C['bg_2'])
    search_row.pack(fill='x', padx=px(12), pady=(px(12), px(10)))
    ent = XEntry(search_row, textvariable=app._lang_search_var,
                 placeholder=_('Search languages'))
    ent.pack(side='left', fill='x', expand=True)
    IconLabel(search_row, 'search', 16, C['fg_3'], C['bg_2']).pack(
        side='right', padx=(px(8), px(2)))
    Divider(pane, color=C['border_2']).pack(fill='x')

    # Scrollable list
    list_outer = tk.Frame(pane, bg=C['bg_2'])
    list_outer.pack(fill='both', expand=True, padx=(0, px(4)),
                    pady=(px(4), px(8)))

    canvas = tk.Canvas(list_outer, bg=C['bg_2'], highlightthickness=0,
                       bd=0, width=px(280))
    sb = XScrollbar(list_outer, command=canvas.yview, surface=C['bg_2'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    inner = tk.Frame(canvas, bg=C['bg_2'])
    win = canvas.create_window((0, 0), window=inner, anchor='nw',
                               width=px(280))
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(win, width=e.width))
    attach_scroll(canvas)

    app._lang_list_inner = inner
    _render_lang_list(app)

    # Re-render on search change
    app._lang_search_var.trace_add('write', lambda *a: _render_lang_list(app))

    return pane


def _render_lang_list(app):
    """Populate the scrollable list with one row per language."""
    parent_frame = app._lang_list_inner
    for w in parent_frame.winfo_children():
        w.destroy()

    search = app._lang_search_var.get().strip().lower()
    active = app._lang_preview_code.get()

    shown = 0
    for code, name, native, flag in _LANGS:
        # Search filter
        if search:
            hay = (code + ' ' + name + ' ' + native).lower()
            if search not in hay:
                continue
        is_active = (code == active)
        _render_lang_row(parent_frame, app, code, name, native, flag,
                         is_active)
        shown += 1
    if not shown:
        tk.Label(parent_frame, text=_('No matching language'),
                 font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3']
                 ).pack(anchor='w', padx=px(16), pady=px(12))


def _render_lang_row(parent, app, code, name, native, flag, is_active):
    """One row in the language list (selected: accent_lo + accent pill)."""
    n_done, n_total, pct = app._lang_coverage.get(code, (0, 1, 0))
    applied = (code == app._lang_var.get())
    row_bg = C['accent_lo'] if is_active else C['bg_2']

    wrap = tk.Frame(parent, bg=C['bg_2'])
    wrap.pack(fill='x', padx=px(6), pady=(0, px(2)))
    row = Rounded(wrap, bg=row_bg, radius=4, padding=(px(10), px(7)),
                  surface=C['bg_2'])
    row.pack(fill='x')
    row.configure(cursor='hand2')
    pill = tk.Frame(row, bg=C['accent'] if is_active else row_bg,
                    width=px(3), height=px(18))
    pill.place(x=-px(10) + 1, rely=0.5, anchor='w')

    tile = _code_tile(row, flag, row_bg)
    tile.pack(side='left', padx=(px(2), px(10)))

    text_col = tk.Frame(row, bg=row_bg)
    text_col.pack(side='left', fill='x', expand=True)
    name_row = tk.Frame(text_col, bg=row_bg)
    name_row.pack(fill='x')
    tk.Label(name_row, text=name,
             font=FONTS['body_strong'] if is_active else FONTS['body'],
             bg=row_bg, fg=C['fg_0'], anchor='w').pack(side='left')
    if applied:
        IconLabel(name_row, 'check', 12, C['success'], row_bg).pack(
            side='left', padx=(px(6), 0))
    tk.Label(text_col, text=native, font=FONTS['caption'],
             bg=row_bg, fg=C['fg_3'], anchor='w').pack(fill='x')

    # Coverage: percentage over a mini bar
    meta_col = tk.Frame(row, bg=row_bg)
    meta_col.pack(side='right', padx=(px(8), 0))
    tk.Label(meta_col, text='%d%%' % pct, font=FONTS['caption'],
             bg=row_bg, fg=C['fg_2'], anchor='e').pack(anchor='e')
    bar = RoundedBar(meta_col, height=4,
                     color=C['success'] if pct >= 100 else C['accent'],
                     track=C['bg_5'], surface=row_bg)
    bar.configure(width=px(44))
    bar.pack(anchor='e', pady=(px(3), 0))
    bar.set(pct / 100.0)

    # Hover (idle rows only)
    def _hover(on, _r=row):
        if is_active:
            return
        bgc = C['bg_3'] if on else C['bg_2']
        try:
            _paint(bgc)
        except Exception:
            pass

    def _paint(bgc):
        row.set_style(bg=bgc)
        pill.configure(bg=bgc)
        for w in (text_col, name_row, meta_col):
            w.configure(bg=bgc)
        for w in name_row.winfo_children() + text_col.winfo_children() \
                + meta_col.winfo_children():
            if isinstance(w, RoundedBar):
                w._sf = bgc
                w.configure(bg=bgc)
                w._redraw()
            elif isinstance(w, IconLabel):
                w.set(bg=bgc)
            elif isinstance(w, tk.Label):
                w.configure(bg=bgc)

    # Click-to-select binds — entire row (and all descendants)
    def _on_click(_e=None, c=code):
        _select_lang(app, c)

    def _bind(w):
        w.bind('<Button-1>', _on_click)
        w.bind('<Enter>', lambda e: _hover(True), add='+')
        w.bind('<Leave>', lambda e: _hover(False), add='+')
        for ch in w.winfo_children():
            _bind(ch)
    _bind(row)


def _select_lang(app, code):
    """Switch the preview to a different language. Doesn't apply yet —
    user clicks the Apply button to commit."""
    app._lang_preview_code.set(code)
    _render_lang_list(app)
    _refresh_preview_pane(app)


# ─────────────────────────────────────────────────────────────────────────
# Right pane — selected-language card + preview / strings cards
# ─────────────────────────────────────────────────────────────────────────
def _build_right_pane(parent, app):
    pane = tk.Frame(parent, bg=C['bg_1'])

    # Head card (name, native, coverage stats) — rebuilt per selection
    app._lang_right_head_frame = tk.Frame(pane, bg=C['bg_1'])
    app._lang_right_head_frame.pack(fill='x', pady=(0, px(16)))

    body_wrap = tk.Frame(pane, bg=C['bg_1'])
    body_wrap.pack(fill='both', expand=True)
    body_wrap.grid_columnconfigure(0, weight=1, uniform='lgr')
    body_wrap.grid_columnconfigure(1, weight=1, uniform='lgr')
    body_wrap.grid_rowconfigure(0, weight=1)

    app._lang_preview_frame = tk.Frame(body_wrap, bg=C['bg_1'])
    app._lang_preview_frame.grid(row=0, column=0, sticky='nsew',
                                 padx=(0, px(8)))

    app._lang_status_frame = tk.Frame(body_wrap, bg=C['bg_1'])
    app._lang_status_frame.grid(row=0, column=1, sticky='nsew',
                                padx=(px(8), 0))

    _refresh_preview_pane(app)
    return pane


def _refresh_preview_pane(app):
    """Populate the right-pane head, preview, and strings from the
    currently selected preview language."""
    code = app._lang_preview_code.get()
    info = next((l for l in _LANGS if l[0] == code), _LANGS[0])
    # NOTE: don't unpack the first element into `_` — that name is the
    # i18n translation function from exfat_builder, and rebinding it here
    # breaks every later `_('...')` call in this function.
    _ignored_code, name, native, flag = info

    # ── Head card ──
    for w in app._lang_right_head_frame.winfo_children():
        w.destroy()
    _build_head_card(app._lang_right_head_frame, app, code, name, native,
                     flag)

    # ── Preview body ──
    for w in app._lang_preview_frame.winfo_children():
        w.destroy()
    _build_preview(app._lang_preview_frame, app, code)

    # ── Strings sample ──
    for w in app._lang_status_frame.winfo_children():
        w.destroy()
    _build_status_aside(app._lang_status_frame, app, code)


def _build_head_card(parent, app, code, name, native, flag):
    n_done, n_total, pct = app._lang_coverage.get(code, (0, 1, 0))
    card = Card(parent, padding=16)
    card.pack(fill='x')
    top = tk.Frame(card, bg=C['bg_2'])
    top.pack(fill='x')
    _code_tile(top, flag, C['bg_2'], big=True).pack(side='left',
                                                   padx=(0, px(14)))
    col = tk.Frame(top, bg=C['bg_2'])
    col.pack(side='left', fill='x', expand=True)
    tk.Label(col, text=name, font=FONTS['title'], bg=C['bg_2'],
             fg=C['fg_0'], anchor='w').pack(fill='x')
    tk.Label(col,
             text='%s · %s %s · %s' % (
                 native,
                 '{:,}'.format(n_done),
                 _('strings'),
                 _('community-maintained')),
             font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3'],
             anchor='w').pack(fill='x', pady=(px(2), 0))
    if code == app._lang_var.get():
        Badge(top, _('Currently applied'), 'success').pack(side='right',
                                                           anchor='n')
    else:
        Badge(top, _('Click Apply to switch'), 'info',
              icon='info').pack(side='right', anchor='n')

    Divider(card, color=C['border_1']).pack(fill='x', pady=(px(14), px(12)))
    tk.Label(card, text=_('Status').upper(), font=FONTS['overline'],
             bg=C['bg_2'], fg=C['fg_4'], anchor='w').pack(fill='x',
                                                          pady=(0, px(8)))
    _build_status_stats(card, app, code)


def _build_preview(parent, app, code):
    """Live preview of UI strings rendered in the chosen language."""
    card = Card(parent, title=_('Live preview'))
    card.pack(fill='both', expand=True)

    well = Card(card.body, tone='inset', padding=14)
    well.pack(fill='x')
    t1 = tk.Label(well, text=_t(code, 'BUILD exFAT IMAGE',
                                default='Build exFAT image'),
                  font=FONTS['body_strong'], bg=C['bg_0'], fg=C['fg_0'],
                  anchor='w', justify='left')
    t1.pack(fill='x')
    autowrap(t1)
    t2 = tk.Label(well,
                  text=_t(code, 'Drop a folder above',
                          default='Drop a folder above and choose where to '
                                  'save the image. The builder will mount, '
                                  'format, and copy automatically.'),
                  font=FONTS['caption'], bg=C['bg_0'], fg=C['fg_3'],
                  anchor='w', justify='left')
    t2.pack(fill='x', pady=(px(4), px(10)))
    autowrap(t2)

    btn_row = tk.Frame(well, bg=C['bg_0'])
    btn_row.pack(fill='x')
    XButton(btn_row, text=_t(code, 'Browse', default='Browse'),
            icon='folder_open', size='sm', surface=C['bg_0']
            ).pack(side='left', padx=(0, px(6)))
    XButton(btn_row, text=_t(code, 'Build All', default='Start build'),
            icon='play', size='sm', surface=C['bg_0']).pack(side='left')


def _build_status_aside(parent, app, code):
    """Sample of translation strings (key → value)."""
    n_done, n_total, _pct = app._lang_coverage[code]
    card = Card(parent, title=_('Strings'))
    card.pack(fill='both', expand=True)
    tk.Label(card.head_actions, text='%d / %d' % (n_done, n_total),
             font=FONTS['code'], bg=C['bg_2'], fg=C['fg_3']).pack()

    # Sample 8 string entries
    trans = _TRANSLATIONS.get(code, {}) if code != 'en' else {}
    sample_keys = [
        'Build All',
        '+ Add to Queue',
        'OUTPUT LOG',
        'Browse',
        'Queue is empty — add a game folder above',
        'All done!',
        'Failed',
        'Settings',
    ]

    table = tk.Frame(card.body, bg=C['bg_2'])
    table.pack(fill='both', expand=True)
    table.grid_columnconfigure(0, weight=1, uniform='strs')
    table.grid_columnconfigure(1, weight=1, uniform='strs')

    r = 0
    for i, key in enumerate(sample_keys):
        if i:
            Divider(table, color=C['border_1']).grid(
                row=r, column=0, columnspan=2, sticky='ew')
            r += 1
        k = tk.Label(table, text=key, font=FONTS['caption'], bg=C['bg_2'],
                     fg=C['fg_3'], anchor='w', justify='left')
        k.grid(row=r, column=0, sticky='ew', pady=px(5), padx=(0, px(8)))
        autowrap(k)
        translated = trans.get(key, key) if code != 'en' else key
        # If the translation key isn't in this language's dict, the key
        # itself is shown — call that out subtly in a dimmer colour.
        is_missing = (code != 'en' and key not in trans)
        v = tk.Label(table, text=translated, font=FONTS['body'],
                     bg=C['bg_2'], fg=C['fg_4'] if is_missing else C['fg_1'],
                     anchor='w', justify='left')
        v.grid(row=r, column=1, sticky='ew', pady=px(5))
        autowrap(v)
        r += 1


def _build_status_stats(parent, app, code):
    """Status row — coverage / strings translated / applied / contributors."""
    n_done, n_total, pct = app._lang_coverage[code]
    grid = tk.Frame(parent, bg=C['bg_2'])
    grid.pack(fill='x')
    for i in range(4):
        grid.grid_columnconfigure(i, weight=1, uniform='lst')

    def _cell(col, label, value, sub=None, mono=False):
        cell = tk.Frame(grid, bg=C['bg_2'])
        cell.grid(row=0, column=col, sticky='nw',
                  padx=(0 if col == 0 else px(16), 0))
        tk.Label(cell, text=label, font=FONTS['caption'], bg=C['bg_2'],
                 fg=C['fg_3'], anchor='w').pack(fill='x')
        tk.Label(cell, text=value,
                 font=FONTS['code'] if mono else FONTS['body_strong'],
                 bg=C['bg_2'], fg=C['fg_0'], anchor='w'
                 ).pack(fill='x', pady=(px(4), 0))
        if sub:
            tk.Label(cell, text=sub, font=FONTS['caption'], bg=C['bg_2'],
                     fg=C['fg_4'], anchor='w').pack(fill='x', pady=(px(2), 0))
        return cell

    cov = _cell(0, _('Coverage'), '%d%%' % pct)
    bar = RoundedBar(cov, height=4,
                     color=C['success'] if pct >= 100 else C['accent'],
                     track=C['bg_5'], surface=C['bg_2'])
    bar.configure(width=px(120))
    bar.pack(anchor='w', pady=(px(6), 0))
    bar.set(pct / 100.0)
    _cell(1, _('Strings translated'), '%d / %d' % (n_done, n_total))
    _applied = app._lang_var.get()
    _cell(2, _('Currently applied'),
          next((l[1] for l in _LANGS if l[0] == _applied), _applied),
          sub=_applied)
    _cell(3, _('Contributors'), '@kerrdec97',
          sub=_('Maintainer · %d strings') % n_done)


def _stat_card(parent, label, value, sub='', value_color=None):
    """Kept for compatibility: a small caption/value block."""
    box = tk.Frame(parent, bg=C['bg_2'])
    box.pack(fill='x', pady=(0, px(8)))
    tk.Label(box, text=label, font=FONTS['caption'], bg=C['bg_2'],
             fg=C['fg_3'], anchor='w').pack(fill='x')
    tk.Label(box, text=value, font=FONTS['body_strong'], bg=C['bg_2'],
             fg=value_color or C['fg_0'], anchor='w').pack(fill='x')
    if sub:
        tk.Label(box, text=sub, font=FONTS['caption'], bg=C['bg_2'],
                 fg=C['fg_4'], anchor='w').pack(fill='x')


# ─────────────────────────────────────────────────────────────────────────
# Apply / Import / Export
# ─────────────────────────────────────────────────────────────────────────
def _apply_lang(app, code):
    """Commit the previewed language choice — same logic as the legacy
    button: set var, save settings, rebuild all tabs."""
    # v3.6.4 (issue #35) guard: _rebuild_all_tabs now rebuilds EVERY
    # registered tab (including the unified Build tab), which would
    # destroy the live progress/queue widgets under a running build.
    # Language can wait; a build can't be rebuilt mid-flight.
    if getattr(app, '_building', False) \
            or getattr(app, '_unified_active', False) \
            or getattr(app, '_ffpkg_building', False):
        app_info(app, 
            _('Build in progress'),
            _('Finish or cancel the running build before changing the '
              'language, then click Apply again.'))
        return
    app._lang_var.set(code)
    set_language(code)
    app._settings['language'] = code
    save_settings(app._settings)
    app._rebuild_all_tabs()
    app._switch_tab('language')


def _export_json(app, code):
    """Export the current language dict to a JSON file."""
    from tkinter import filedialog
    if code == 'en':
        # English is the source — there's no entry in _TRANSLATIONS.
        # Build a key-for-key dict from another language's keys so the
        # export is at least useful as a translation template.
        sample = next(iter(_TRANSLATIONS.values()), {})
        d = {k: k for k in sample.keys()}
    else:
        d = _TRANSLATIONS.get(code, {})
    if not d:
        app_info(app, _('Export'),
                            _('No translation entries for this language.'))
        return
    path = filedialog.asksaveasfilename(
        defaultextension='.json',
        filetypes=[('JSON files', '*.json')],
        initialfile='%s.json' % code)
    if not path:
        return
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(d, f, indent=2, ensure_ascii=False, sort_keys=True)
        app_info(app, _('Export'),
                            _('Saved %d strings to %s') %
                            (len(d), os.path.basename(path)))
    except Exception as e:
        app_error(app, _('Export failed'), str(e))


def _import_json(app):
    """Import a translation .json. Decorative — would need a path to merge
    user-supplied strings into _TRANSLATIONS; out of scope for this
    iteration."""
    app_info(app, _('Import .json'),
                        _('Translation imports from JSON files are not yet '
                          'wired up. The Export button works — you can use '
                          'it to bootstrap a translation file, then send '
                          'it to @kerrdec97 for inclusion in the next '
                          'release.'))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _t(code, key, default=None):
    """Translate `key` into `code`. Falls back to `default` then `key`."""
    if code == 'en':
        return default if default is not None else key
    d = _TRANSLATIONS.get(code, {})
    return d.get(key, default if default is not None else key)


def _english_string_count():
    """Approximate the source string count (English baseline) by taking
    the largest translation dict — a good proxy for the source key set."""
    if not _TRANSLATIONS:
        return 1
    return max(len(d) for d in _TRANSLATIONS.values())


def _ghost_btn(parent, text, command):
    return XButton(parent, text=text, command=command)


def _accent_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='primary')
