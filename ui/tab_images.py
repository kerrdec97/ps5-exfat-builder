"""
ui/tab_images.py — My Images tab.

Step 15 (v2.1.7): refactored against preview/images-tab-redesign.html.

Layout:

    ┌─ toolbar (page-head) ───────────────────────────────────────────┐
    │  My Images                                                      │
    │  82 images · 4.21 TB total · D:/ free 412 GB                    │
    │              [↻ Re-scan] [📂 Open folder] [🎮 Send selected]   │
    ├─ scan-dirs strip ───────────────────────────────────────────────┤
    │  D:/PS5/Images  ✕      D:/Backports  ✕    [+ Add folder]       │
    ├─ filterbar ─────────────────────────────────────────────────────┤
    │  🔍 search    [All 82] [FW 4.51+ 61] [Backport 14] [Update 23]  │
    ├─ card grid ─────────────────────────────────────────────────────┤
    │  🎮Returnal       🎮Last of Us II  🎮Elden Ring  🎮GoW Ragnarok │
    │  CUSA-22871 v2.10  CUSA-03173 …    CUSA-26879 …  CUSA-31000 …   │
    │  ...                                                            │
    └─────────────────────────────────────────────────────────────────┘

Replaces the legacy single-column Listbox with a multi-column card grid
using the shared `GameCard` widget (same component used in Library).

Backwards compat: every `_img_*` attribute the existing 8+ callbacks read
is preserved with the same name. The `_img_listbox` attribute now points
to a hidden Listbox kept around solely so legacy callbacks
(`_img_apply_filter`, `_img_update_sel`, `_img_context_menu`) can read
selection indices via `curselection()`. Selection is mirrored from the
visible card grid into the hidden listbox on every click.
"""

import os
import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.cards import GameCard


def build_images_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    app._img_scan_dirs = app._settings.get('img_scan_dirs', [])
    app._img_entries   = []
    # Selection — set of indices into _img_entries (post-filter)
    app._img_selected_idx = set()
    app._img_search_var = tk.StringVar()
    app._img_filter_chip = tk.StringVar(value='all')
    app._img_status_var = tk.StringVar(
        value=_('Add a folder containing .exfat files and click Re-scan'))
    app._img_sel_var = tk.StringVar(value='')

    # ── Toolbar ──
    _build_toolbar(parent, app)

    # ── Scan-dirs strip ──
    app._img_dirs_frame = tk.Frame(parent, bg=COLORS['bg_2'],
                                    highlightbackground=COLORS['border_2'],
                                    highlightthickness=1)
    app._img_dirs_frame.pack(fill='x', padx=24, pady=(0, 8))

    # ── Filter bar ──
    _build_filterbar(parent, app)

    # ── Action bar ──
    _build_action_bar(parent, app)

    # ── Card grid ──
    _build_grid(parent, app)

    # Hidden listbox kept for legacy callback compat
    # (`_img_listbox.curselection()` is read by `_img_update_sel`,
    # `_img_context_menu`, etc.). We mirror the card-grid selection into
    # it on every card click.
    app._img_listbox = tk.Listbox(parent, exportselection=False)
    # Don't pack — hidden. selectmode='extended' to allow index-based
    # multi-select via .selection_set().
    app._img_listbox.configure(selectmode='extended')

    # Render initial scan-dirs + status
    app._img_render_dirs()
    if app._img_scan_dirs:
        app.after(50, app._img_scan)


# ─────────────────────────────────────────────────────────────────────────
# Toolbar — title + stats + bulk actions
# ─────────────────────────────────────────────────────────────────────────
def _build_toolbar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_1'])
    bar.pack(fill='x', padx=24, pady=(14, 8))

    title_col = tk.Frame(bar, bg=COLORS['bg_1'])
    title_col.pack(side='left', fill='x', expand=True)

    tk.Label(title_col, text=_('My Images'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(anchor='w')

    # Stats row (under title)
    app._img_stats_var = tk.StringVar(value='')
    tk.Label(title_col, textvariable=app._img_stats_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(anchor='w', pady=(2, 0))

    # Right-aligned actions
    _ghost_btn(bar, '\u21bb  ' + _('Re-scan'),
               command=app._img_scan
               ).pack(side='right', padx=(6, 0))
    _ghost_btn(bar, '+  ' + _('Add folder'),
               command=app._img_add_dir
               ).pack(side='right')
    app._img_batch_btn = _accent_btn(bar,
        '\U0001f3ae  ' + _('Send selected to PS5'),
        command=app._img_batch_upload)
    app._img_batch_btn.pack(side='right', padx=(0, 12))


# ─────────────────────────────────────────────────────────────────────────
# Filter bar — search + chips
# ─────────────────────────────────────────────────────────────────────────
def _build_filterbar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_1'])
    bar.pack(fill='x', padx=24, pady=(0, 10))

    # Search input
    search_wrap = tk.Frame(bar, bg=COLORS['bg_0'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(side='left')
    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 4))
    tk.Entry(search_wrap, textvariable=app._img_search_var,
             font=FONTS['body'],
             bg=COLORS['bg_0'], fg=COLORS['fg_1'],
             insertbackground=COLORS['accent'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=0,
             width=32
             ).pack(side='left', ipady=5, padx=(0, 8))

    # Re-render grid live on search change
    app._img_search_var.trace_add(
        'write', lambda *a: _re_render_grid(app))

    # Chip filters
    chips_frame = tk.Frame(bar, bg=COLORS['bg_1'])
    chips_frame.pack(side='left', padx=(10, 0))

    app._img_chip_widgets = {}

    def _chip(key, label):
        chip = tk.Frame(chips_frame, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1, cursor='hand2')
        chip.pack(side='left', padx=2)
        inner = tk.Frame(chip, bg=COLORS['bg_3'])
        inner.pack(padx=10, pady=4)
        lbl = tk.Label(inner, text=label,
                       font=FONTS['meta'],
                       bg=COLORS['bg_3'], fg=COLORS['fg_3'])
        lbl.pack()
        for w in [chip, inner, lbl]:
            w.bind('<Button-1>',
                   lambda e, k=key: _select_chip(app, k))
        app._img_chip_widgets[key] = (chip, inner, lbl)

    _chip('all',      _('All'))
    _chip('fw451',    _('FW 4.51+'))
    _chip('backport', _('Backport'))

    # Apply default chip styling
    _select_chip(app, 'all', re_render=False)


def _select_chip(app, key, re_render=True):
    """Update chip visuals + filter state."""
    app._img_filter_chip.set(key)
    for k, (chip, inner, lbl) in app._img_chip_widgets.items():
        if k == key:
            chip.configure(bg=COLORS['accent_08'],
                           highlightbackground=COLORS['accent'])
            inner.configure(bg=COLORS['accent_08'])
            lbl.configure(bg=COLORS['accent_08'], fg=COLORS['accent_hi'])
        else:
            chip.configure(bg=COLORS['bg_3'],
                           highlightbackground=COLORS['border_3'])
            inner.configure(bg=COLORS['bg_3'])
            lbl.configure(bg=COLORS['bg_3'], fg=COLORS['fg_3'])
    if re_render:
        _re_render_grid(app)


# ─────────────────────────────────────────────────────────────────────────
# Action bar — selection info + status
# ─────────────────────────────────────────────────────────────────────────
def _build_action_bar(parent, app):
    bar = tk.Frame(parent, bg=COLORS['bg_2'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    bar.pack(fill='x', padx=24, pady=(0, 8))
    inner = tk.Frame(bar, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=12, pady=8)

    tk.Label(inner, textvariable=app._img_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left')

    tk.Label(inner, textvariable=app._img_sel_var,
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['accent_hi']
             ).pack(side='right')


# ─────────────────────────────────────────────────────────────────────────
# Card grid (scrollable)
# ─────────────────────────────────────────────────────────────────────────
def _build_grid(parent, app):
    outer = tk.Frame(parent, bg=COLORS['bg_1'])
    outer.pack(fill='both', expand=True, padx=24, pady=(0, 12))

    canvas = tk.Canvas(outer, bg=COLORS['bg_1'], highlightthickness=0)
    sb = tk.Scrollbar(outer, command=canvas.yview,
                      bg=COLORS['bg_3'], troughcolor=COLORS['bg_1'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._img_grid_frame = tk.Frame(canvas, bg=COLORS['bg_1'])
    win = canvas.create_window((0, 0), window=app._img_grid_frame,
                                anchor='nw')
    app._img_grid_canvas = canvas
    app._img_grid_window = win
    app._img_grid_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    # Re-flow grid columns on canvas resize
    def _on_canvas_resize(e):
        canvas.itemconfig(win, width=e.width)
        _re_render_grid(app)
    canvas.bind('<Configure>', _on_canvas_resize)
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))


def _re_render_grid(app):
    """Re-render the card grid from current entries + filter + search."""
    if not hasattr(app, '_img_grid_frame'):
        return
    for w in app._img_grid_frame.winfo_children():
        w.destroy()

    # Apply search + chip filter
    search = ''
    try:
        search = app._img_search_var.get().strip().lower()
    except Exception:
        pass
    chip = app._img_filter_chip.get()

    visible = []
    for i, e in enumerate(app._img_entries):
        # Parse meta from filename for filter checks
        meta = _parse_meta(e['name'])

        if search:
            hay = (e['name'] + ' ' + (meta.get('cusa') or '') +
                   ' ' + meta.get('title', '')).lower()
            if search not in hay:
                continue

        if chip == 'backport' and not meta.get('is_backport'):
            continue
        # 'fw451' chip — skip filtering (we don't know FW from filename
        # alone). Treat as a no-op visual filter for this iteration.

        visible.append((i, e, meta))

    if not visible:
        empty = tk.Frame(app._img_grid_frame, bg=COLORS['bg_1'])
        empty.pack(fill='both', expand=True, pady=40)
        tk.Label(empty, text='\U0001f4be',
                 font=(FONTS['body'][0], 32),
                 bg=COLORS['bg_1'], fg=COLORS['fg_6']
                 ).pack()
        msg = (_('No images found') if not app._img_entries
               else _('No matches for current filter'))
        tk.Label(empty, text=msg,
                 font=FONTS['body'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_5']
                 ).pack(pady=(8, 0))
        return

    # Compute column count from canvas width
    try:
        canvas_w = app._img_grid_canvas.winfo_width()
    except Exception:
        canvas_w = 700
    if canvas_w < 50:
        canvas_w = 700
    card_w = 160
    cols = max(1, canvas_w // (card_w + 12))

    for grid_i, (idx, entry, meta) in enumerate(visible):
        row = grid_i // cols
        col = grid_i % cols
        _make_image_card(app, entry, meta, idx, row, col)

    for c in range(cols):
        app._img_grid_frame.grid_columnconfigure(c, weight=1)


def _make_image_card(app, entry, meta, idx, row, col):
    """Build one GameCard for an .exfat image."""
    # Format size
    size_text = None
    sz = entry.get('size', 0)
    if sz > 0:
        size_text = ('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))

    status = 'backport' if meta.get('is_backport') else None

    card = GameCard(
        app._img_grid_frame,
        title=meta.get('title') or entry['name'],
        game_id=meta.get('cusa') or '',
        version=meta.get('version_str'),
        size_text=size_text,
        status=status,
        cover_size=148,
    )
    card.grid(row=row, column=col, padx=6, pady=6, sticky='nsew')
    # Tag the card frame and all descendants with _img_idx so the
    # right-click handler in main file can recover which entry was hit.
    def _tag_idx(widget, i=idx):
        widget._img_idx = i
        for child in widget.winfo_children():
            _tag_idx(child, i)
    _tag_idx(card)

    # Async cover-art lookup. The .exfat file itself doesn't carry cover
    # art, but if the user has scanned a games library, we can find a
    # matching CUSA entry there and reuse its `cover` field.
    #
    # NB: my parsed CUSA is "PPSA-05344" (hyphenated), but the library's
    # `title_id` field comes from param.sfo as "PPSA05344" (no hyphen).
    # Normalize both sides by stripping hyphens before comparing.
    cusa_raw = (meta.get('cusa') or '').upper()
    cusa_norm = cusa_raw.replace('-', '')
    if cusa_norm:
        try:
            for g in getattr(app, '_lib_games', []) or []:
                lib_cusa = g.get('title_id', '').upper().replace('-', '')
                if lib_cusa == cusa_norm and g.get('cover'):
                    cover_path = g['cover']
                    def _load(p=cover_path, lbl=card.cover_label):
                        try:
                            from exfat_builder import _load_cover_image
                            img = _load_cover_image(p, target=148)
                            if img is None:
                                return
                            from PIL import ImageTk as _PITk
                            photo = _PITk.PhotoImage(img)
                            lbl.config(image=photo, text='')
                            lbl.image = photo
                        except Exception:
                            pass
                    app.after(20, _load)
                    break
        except Exception:
            pass

    # Selection visual — accent border ring
    is_selected = (idx in app._img_selected_idx)
    if is_selected:
        card.configure(highlightbackground=COLORS['accent'],
                       highlightthickness=2)

    # Click bindings — left = toggle select, right = context menu, double = open
    def _on_left(_e=None, i=idx):
        _toggle_select(app, i)
    def _on_double(_e=None, e=entry):
        try:
            os.startfile(os.path.dirname(e['path']))
        except Exception:
            pass
    def _on_right(e=None, idx=idx):
        # Sync the hidden listbox first so legacy callback sees the right
        # selection, then call the legacy handler.
        _sync_hidden_listbox(app)
        try:
            app._img_context_menu(e)
        except Exception:
            pass

    # GameCard binds clicks on itself + descendants in __init__; we
    # need to override after construction.
    def _bind_recursive(widget, handler, sequence):
        widget.bind(sequence, handler)
        for child in widget.winfo_children():
            _bind_recursive(child, handler, sequence)

    _bind_recursive(card, _on_left, '<Button-1>')
    _bind_recursive(card, _on_right, '<Button-3>')
    _bind_recursive(card, _on_double, '<Double-Button-1>')


def _toggle_select(app, idx):
    """Add/remove idx from selection set + re-render the affected card."""
    if idx in app._img_selected_idx:
        app._img_selected_idx.remove(idx)
    else:
        app._img_selected_idx.add(idx)
    _sync_hidden_listbox(app)
    _update_selection_status(app)
    _re_render_grid(app)


def _sync_hidden_listbox(app):
    """Mirror the card-grid selection into the hidden _img_listbox so
    legacy callbacks can read it via `curselection()`."""
    lb = app._img_listbox
    # Reset the listbox content to match _img_entries order
    lb.delete(0, 'end')
    for e in app._img_entries:
        lb.insert('end', e['name'])
    lb.selection_clear(0, 'end')
    for idx in sorted(app._img_selected_idx):
        if 0 <= idx < len(app._img_entries):
            lb.selection_set(idx)


def _update_selection_status(app):
    """Update the right-aligned selection summary."""
    n = len(app._img_selected_idx)
    if n == 0:
        app._img_sel_var.set('')
        return
    total = sum(app._img_entries[i].get('size', 0)
                for i in app._img_selected_idx
                if 0 <= i < len(app._img_entries))
    app._img_sel_var.set(
        '%d %s · %.2f GB' % (n, _('selected'), total / 1024**3))


# ─────────────────────────────────────────────────────────────────────────
# Filename meta parser
# ─────────────────────────────────────────────────────────────────────────
def _parse_meta(name):
    """Parse CUSA / version / display title from a filename."""
    base = name.rsplit('.', 1)[0]
    out = {'cusa': None, 'version': None, 'version_str': None,
           'title': None, 'is_backport': False}

    if '-bp' in base.lower() or '_bp' in base.lower():
        out['is_backport'] = True

    m = re.search(r'((?:CUSA|PPSA|PPLH)[-_ ]?\d{5})', base, re.IGNORECASE)
    if m:
        cusa = m.group(1).upper().replace('_', '-').replace(' ', '-')
        if '-' not in cusa:
            cusa = cusa[:4] + '-' + cusa[4:]
        out['cusa'] = cusa

    m = re.search(r'v(\d+\.\d+)', base, re.IGNORECASE)
    if m:
        out['version'] = m.group(1)
        out['version_str'] = 'v' + m.group(1)

    cleaned = base
    if out['cusa']:
        cleaned = re.sub(r'(?:CUSA|PPSA|PPLH)[-_ ]?\d{5}', '', cleaned,
                         flags=re.IGNORECASE)
    if out['version']:
        cleaned = re.sub(r'v\d+\.\d+', '', cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r'-bp\b', '', cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r'^[\s\-_.]+|[\s\-_.]+$', '', cleaned).strip()
    cleaned = re.sub(r'[\s_]+', ' ', cleaned).strip()
    out['title'] = cleaned or base

    return out


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
