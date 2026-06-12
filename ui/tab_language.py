"""
ui/tab_language.py — Language picker tab.

Step 13 (v2.1.5): refactored against preview/language-tab-redesign.html.

Layout:

    ┌─ left pane (320px) ─────┬─ right pane ─────────────────────────┐
    │ Interface language      │ ┌─ head (lang + actions) ──────────┐ │
    │ 17 languages · 16 done  │ │ 🇬🇧 English (UK) · 1,847 strings  │ │
    │ ┌─ search ─────────────┐│ │ [Import] [Export] [Apply]        │ │
    │ │ 🔍 search…           ││ ├─ body (preview + status aside) ──┤ │
    │ └────────────────────┘ │ │ ┌─ live preview ─┐ ┌─ status ──┐  │ │
    │ ┌─ list ─────────────┐ │ │ │ Build exFAT    │ │Coverage   │  │ │
    │ │ ▌🇬🇧 English (UK)  │ │ │ │ … translated   │ │Last upd   │  │ │
    │ │  🇺🇸 English (US)  │ │ │ │ ┌─ strings ──┐ │ │Contributor│  │ │
    │ │  🇫🇷 Français       │ │ │ │ │ key→value   │ │ │           │  │ │
    │ │  🇩🇪 Deutsch        │ │ │ │ └────────────┘ │ │           │  │ │
    │ │  🇪🇸 Español       │ │ │ └────────────────┘ └───────────┘  │ │
    │ │  ...               │ │ │                                    │ │
    │ │ ▌█████ 100%        │ │ │                                    │ │
    │ │ ▌███▄  88% (amber) │ │ │                                    │ │
    │ │ ▌██    41% (red)   │ │ │                                    │ │
    │ └────────────────────┘ │ │                                    │ │
    └────────────────────────┴─────────────────────────────────────┘

The active language gets a 2px accent left border + tinted background,
matching the rail-item style we built for Settings.

Backwards compat: relies on `app._lang_var` (set in App.__init__),
`set_language()` and `save_settings()` from main, `app._rebuild_all_tabs()`,
and `app._switch_tab()`. All exist and are unchanged.
"""

import json
import os
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import (
    _, set_language, save_settings, _TRANSLATIONS,
)
from ui.shared.scroll import attach_scroll


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


def build_language_tab(parent, app):
    """Build the redesigned Language tab into `parent`."""
    parent.configure(bg=COLORS['bg_1'])

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

    # ── 2-column workspace ──
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    body.grid_columnconfigure(0, weight=0, minsize=320)
    body.grid_columnconfigure(1, weight=1)
    body.grid_rowconfigure(0, weight=1)

    _build_left_pane(body, app).grid(row=0, column=0, sticky='nsew')
    tk.Frame(body, bg=COLORS['border_2'], width=1
             ).grid(row=0, column=0, sticky='nse')
    _build_right_pane(body, app).grid(row=0, column=1, sticky='nsew')


# ─────────────────────────────────────────────────────────────────────────
# Left pane — header + search + scrollable list with mini progress bars
# ─────────────────────────────────────────────────────────────────────────
def _build_left_pane(parent, app):
    pane = tk.Frame(parent, bg=COLORS['bg_0'])

    # Header
    head = tk.Frame(pane, bg=COLORS['bg_0'])
    head.pack(fill='x', padx=18, pady=(16, 6))
    tk.Label(head, text=_('Interface language'),
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_0'], fg=COLORS['fg_0'],
             anchor='w'
             ).pack(fill='x')

    n_total = len(_LANGS)
    n_full = sum(1 for c in app._lang_coverage.values() if c[2] >= 100)
    tk.Label(head,
             text=_('%d languages · %d fully translated · '
                    'community-maintained') % (n_total, n_full),
             font=FONTS['meta'],
             bg=COLORS['bg_0'], fg=COLORS['fg_5'],
             anchor='w', wraplength=280, justify='left'
             ).pack(fill='x', pady=(2, 10))

    # Search input
    search_wrap = tk.Frame(pane, bg=COLORS['bg_3'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(fill='x', padx=18, pady=(0, 10))
    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_3'], fg=COLORS['fg_5']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(search_wrap, textvariable=app._lang_search_var,
             font=FONTS['body'],
             bg=COLORS['bg_3'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0
             ).pack(fill='x', ipady=5, padx=(0, 8))

    # Scrollable list
    list_outer = tk.Frame(pane, bg=COLORS['bg_0'])
    list_outer.pack(fill='both', expand=True)

    canvas = tk.Canvas(list_outer, bg=COLORS['bg_0'], highlightthickness=0)
    sb = tk.Scrollbar(list_outer, command=canvas.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_0'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    inner = tk.Frame(canvas, bg=COLORS['bg_0'])
    win = canvas.create_window((0, 0), window=inner, anchor='nw')
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

    for code, name, native, flag in _LANGS:
        # Search filter
        if search:
            hay = (code + ' ' + name + ' ' + native).lower()
            if search not in hay:
                continue
        is_active = (code == active)
        _render_lang_row(parent_frame, app, code, name, native, flag,
                          is_active)


def _render_lang_row(parent, app, code, name, native, flag, is_active):
    """One row in the language list."""
    n_done, n_total, pct = app._lang_coverage.get(code, (0, 1, 0))

    # Active row uses accent-tinted bg + 2px left border accent strip,
    # matching the rail item style.
    row_bg = COLORS['accent_08'] if is_active else COLORS['bg_0']
    border_color = COLORS['accent'] if is_active else COLORS['bg_0']

    wrap = tk.Frame(parent, bg=row_bg)
    wrap.pack(fill='x')

    accent_strip = tk.Frame(wrap, bg=border_color, width=2)
    accent_strip.pack(side='left', fill='y')
    accent_strip.pack_propagate(False)

    inner = tk.Frame(wrap, bg=row_bg, cursor='hand2')
    inner.pack(side='left', fill='x', expand=True, padx=(8, 14), pady=8)

    # Flag + names column
    text_col = tk.Frame(inner, bg=row_bg)
    text_col.pack(side='left', fill='x', expand=True)

    name_row = tk.Frame(text_col, bg=row_bg)
    name_row.pack(fill='x')
    tk.Label(name_row, text=flag,
             font=(FONTS['body'][0], 12),
             bg=row_bg, fg=COLORS['fg_0']
             ).pack(side='left', padx=(0, 8))
    tk.Label(name_row, text=name,
             font=(FONTS['body'][0], 10, 'bold'),
             bg=row_bg, fg=COLORS['fg_0'], anchor='w'
             ).pack(side='left')
    tk.Label(text_col, text=native,
             font=FONTS['mono_sm'],
             bg=row_bg, fg=COLORS['fg_5'], anchor='w'
             ).pack(fill='x', padx=(28, 0))

    # Mini bar + percentage
    meta_col = tk.Frame(inner, bg=row_bg)
    meta_col.pack(side='right', padx=(8, 0))

    pct_color = (COLORS['success_hi'] if pct >= 95
                 else COLORS['warn_hi'] if pct >= 60
                 else COLORS['danger_hi'])
    bar_fill = (COLORS['success'] if pct >= 95
                else COLORS['warn'] if pct >= 60
                else COLORS['danger'])

    bar_track = tk.Frame(meta_col, bg=COLORS['bg_4'], width=60, height=4)
    bar_track.pack(anchor='e')
    bar_track.pack_propagate(False)
    fill_w = max(1, int(60 * pct / 100))
    fill = tk.Frame(bar_track, bg=bar_fill, width=fill_w, height=4)
    fill.pack(side='left', fill='y')

    tk.Label(meta_col, text='%d%%' % pct,
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=row_bg, fg=pct_color
             ).pack(anchor='e', pady=(2, 0))

    # Click-to-select binds — entire row (and all descendants)
    def _on_click(_e=None, c=code):
        _select_lang(app, c)
    for w in [wrap, accent_strip, inner, text_col, meta_col, bar_track, fill]:
        w.bind('<Button-1>', _on_click)
    for w in inner.winfo_children() + text_col.winfo_children() + meta_col.winfo_children():
        try:
            w.bind('<Button-1>', _on_click)
            for sub in w.winfo_children():
                sub.bind('<Button-1>', _on_click)
        except Exception:
            pass


def _select_lang(app, code):
    """Switch the preview to a different language. Doesn't apply yet —
    user clicks the Apply button to commit."""
    app._lang_preview_code.set(code)
    _render_lang_list(app)
    _refresh_preview_pane(app)


# ─────────────────────────────────────────────────────────────────────────
# Right pane — head + body (preview + status aside)
# ─────────────────────────────────────────────────────────────────────────
def _build_right_pane(parent, app):
    pane = tk.Frame(parent, bg=COLORS['bg_1'])

    # Head
    app._lang_right_head_frame = tk.Frame(pane, bg=COLORS['bg_1'])
    app._lang_right_head_frame.pack(fill='x', padx=24, pady=(16, 12))

    # Body
    body_wrap = tk.Frame(pane, bg=COLORS['bg_1'])
    body_wrap.pack(fill='both', expand=True, padx=24, pady=(0, 16))

    body_wrap.grid_columnconfigure(0, weight=1)
    body_wrap.grid_columnconfigure(1, weight=0, minsize=240)
    body_wrap.grid_rowconfigure(0, weight=1)

    app._lang_preview_frame = tk.Frame(body_wrap, bg=COLORS['bg_1'])
    app._lang_preview_frame.grid(row=0, column=0, sticky='nsew',
                                  padx=(0, 16))

    app._lang_status_frame = tk.Frame(body_wrap, bg=COLORS['bg_1'])
    app._lang_status_frame.grid(row=0, column=1, sticky='nsew')

    _refresh_preview_pane(app)
    return pane


def _refresh_preview_pane(app):
    """Populate the right-pane head, preview, and status from the
    currently selected preview language."""
    code = app._lang_preview_code.get()
    info = next((l for l in _LANGS if l[0] == code), _LANGS[0])
    # NOTE: don't unpack the first element into `_` — that name is the
    # i18n translation function from exfat_builder, and rebinding it here
    # breaks every later `_('...')` call in this function.
    _ignored_code, name, native, flag = info

    # ── Head ──
    for w in app._lang_right_head_frame.winfo_children():
        w.destroy()

    flag_lbl = tk.Label(app._lang_right_head_frame, text=flag,
                        font=(FONTS['body'][0], 26),
                        bg=COLORS['bg_1'], fg=COLORS['fg_0'])
    flag_lbl.pack(side='left', padx=(0, 14))

    title_col = tk.Frame(app._lang_right_head_frame, bg=COLORS['bg_1'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=name,
             font=(FONTS['h2'][0], 16, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')

    n_done, n_total, pct = app._lang_coverage.get(code, (0, 1, 0))
    tk.Label(title_col,
             text='%s · %s %s · %s' % (
                 native,
                 '{:,}'.format(n_done),
                 _('strings'),
                 _('community-maintained')),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(4, 0))

    # Action buttons (right-aligned)
    actions = tk.Frame(app._lang_right_head_frame, bg=COLORS['bg_1'])
    actions.pack(side='right')

    _accent_btn(actions, _('Apply'),
                command=lambda: _apply_lang(app, code)
                ).pack(side='right')
    _ghost_btn(actions, _('Export .json'),
               command=lambda: _export_json(app, code)
               ).pack(side='right', padx=(0, 6))
    _ghost_btn(actions, _('Import .json'),
               command=lambda: _import_json(app)
               ).pack(side='right', padx=(0, 6))

    # ── Preview body ──
    for w in app._lang_preview_frame.winfo_children():
        w.destroy()
    _build_preview(app._lang_preview_frame, app, code)

    # ── Status aside ──
    for w in app._lang_status_frame.winfo_children():
        w.destroy()
    _build_status_aside(app._lang_status_frame, app, code)


def _build_preview(parent, app, code):
    """Live preview of UI strings rendered in the chosen language."""
    # Preview Card 1: a fake 'Build exFAT image' card
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x', pady=(0, 12))

    head = tk.Frame(card, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=14, pady=(12, 6))
    tk.Label(head, text=_('Live preview'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left')
    tk.Label(head, text='menu \u203a build',
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='right')

    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=14, pady=(0, 12))
    tk.Label(inner, text=_t(code, 'BUILD exFAT IMAGE',
                             default='Build exFAT image'),
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(inner,
             text=_t(code, 'Drop a folder above',
                     default='Drop a folder above and choose where to save '
                             'the image. The builder will mount, format, '
                             'and copy automatically.'),
             font=FONTS['body'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'],
             anchor='w', justify='left', wraplength=400
             ).pack(fill='x', pady=(4, 8))

    btn_row = tk.Frame(inner, bg=COLORS['bg_2'])
    btn_row.pack(fill='x')
    tk.Button(btn_row,
              text=_t(code, 'Browse', default='Browse'),
              font=(FONTS['button'][0], 9),
              bg=COLORS['bg_3'], fg=COLORS['fg_2'],
              relief='flat', bd=0, padx=12, pady=6,
              cursor='hand2',
              highlightbackground=COLORS['border_3'],
              highlightthickness=1
              ).pack(side='left', padx=(0, 6))
    tk.Button(btn_row,
              text='\u25b6  ' + _t(code, 'Build All', default='Start build'),
              font=(FONTS['button'][0], 9, 'bold'),
              bg=COLORS['accent'], fg=COLORS['fg_0'],
              relief='flat', bd=0, padx=12, pady=6,
              cursor='hand2'
              ).pack(side='left')

    # Preview Card 2: sample of translation strings (key → value)
    str_card = tk.Frame(parent, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
    str_card.pack(fill='both', expand=True)

    str_head = tk.Frame(str_card, bg=COLORS['bg_2'])
    str_head.pack(fill='x', padx=14, pady=(12, 6))
    tk.Label(str_head, text=_('Strings'),
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left')
    n_done, n_total, _pct = app._lang_coverage[code]
    tk.Label(str_head,
             text='%d / %d' % (n_done, n_total),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='right')

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

    table = tk.Frame(str_card, bg=COLORS['bg_2'])
    table.pack(fill='both', expand=True, padx=14, pady=(0, 12))
    table.grid_columnconfigure(0, weight=0, minsize=160)
    table.grid_columnconfigure(1, weight=1)

    for i, key in enumerate(sample_keys):
        tk.Label(table, text=key,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['accent_hi'],
                 anchor='w'
                 ).grid(row=i, column=0, sticky='w', pady=3)
        translated = trans.get(key, key) if code != 'en' else key
        # If the translation key isn't in this language's dict, the key
        # itself is shown — call that out subtly with a warn color.
        is_missing = (code != 'en' and key not in trans)
        fg = COLORS['fg_5'] if is_missing else COLORS['fg_1']
        tk.Label(table, text=translated,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=fg,
                 anchor='w', wraplength=300, justify='left'
                 ).grid(row=i, column=1, sticky='w', pady=3, padx=(8, 0))


def _build_status_aside(parent, app, code):
    """Status aside — coverage / last-updated / contributors."""
    n_done, n_total, pct = app._lang_coverage[code]

    tk.Label(parent, text=_('Status'),
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x', pady=(0, 6))

    # Coverage card
    cov_color = (COLORS['success_hi'] if pct >= 95
                 else COLORS['warn_hi'] if pct >= 60
                 else COLORS['danger_hi'])
    _stat_card(parent, _('Coverage'), '%d%%' % pct,
               value_color=cov_color)

    # Strings translated
    _stat_card(parent, _('Strings translated'),
               '%d / %d' % (n_done, n_total))

    # Active language
    _stat_card(parent, _('Currently applied'),
               app._lang_var.get(),
               sub=_('Click Apply to switch'))

    tk.Label(parent, text=_('Contributors'),
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x', pady=(14, 6))

    contrib_card = tk.Frame(parent, bg=COLORS['bg_2'],
                            highlightbackground=COLORS['border_2'],
                            highlightthickness=1)
    contrib_card.pack(fill='x')
    inner = tk.Frame(contrib_card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=12, pady=10)
    tk.Label(inner, text='@kerrdec97',
             font=(FONTS['body'][0], 10, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(inner,
             text=_('Maintainer · %d strings') % n_done,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(2, 0))


def _stat_card(parent, label, value, sub='', value_color=None):
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x', pady=(0, 8))
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=12, pady=10)
    tk.Label(inner, text=label,
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(fill='x')
    tk.Label(inner, text=value,
             font=(FONTS['mono'][0], 13, 'bold'),
             bg=COLORS['bg_2'],
             fg=value_color or COLORS['fg_0'], anchor='w'
             ).pack(fill='x', pady=(2, 0))
    if sub:
        tk.Label(inner, text=sub,
                 font=FONTS['meta'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(fill='x', pady=(2, 0))


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
        from tkinter import messagebox
        messagebox.showinfo(
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
    from tkinter import filedialog, messagebox
    if code == 'en':
        # English is the source — there's no entry in _TRANSLATIONS.
        # Build a key-for-key dict from another language's keys so the
        # export is at least useful as a translation template.
        sample = next(iter(_TRANSLATIONS.values()), {})
        d = {k: k for k in sample.keys()}
    else:
        d = _TRANSLATIONS.get(code, {})
    if not d:
        messagebox.showinfo(_('Export'),
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
        messagebox.showinfo(_('Export'),
                            _('Saved %d strings to %s') %
                            (len(d), os.path.basename(path)))
    except Exception as e:
        messagebox.showerror(_('Export failed'), str(e))


def _import_json(app):
    """Import a translation .json. Decorative — would need a path to merge
    user-supplied strings into _TRANSLATIONS; out of scope for this
    iteration."""
    from tkinter import messagebox
    messagebox.showinfo(_('Import .json'),
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
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 9, 'bold'),
                     bg=COLORS['accent'], fg=COLORS['fg_0'],
                     activebackground=COLORS['accent_hi'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=14, pady=6,
                     cursor='hand2',
                     command=command)
