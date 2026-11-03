"""
ui/tab_images.py — Library › Built Images (My Images) screen.

Design-system layout, matching Library › Source Dumps:

    My Images                         [Add folder] [Re-scan] [Send selected to PS5]
    Finished images ready to use
    [search…………]  [All | FW 4.51+ | Backport]                    status text
    ┌ image tiles (scroll) ─────────────────────┐ ┌ rail ────────────┐
    │ cover, name, title ID, version · size,    │ │ Selection         │
    │ format / backport badges, select check    │ │ Image folders     │
    └───────────────────────────────────────────┘ └───────────────────┘

Presentation only. Every `_img_*` attribute the monolith's callbacks read
keeps its name (`_img_scan_dirs`, `_img_entries`, `_img_selected_idx`,
`_img_search_var`, `_img_filter_chip`, `_img_status_var`, `_img_sel_var`,
`_img_stats_var`, `_img_dirs_frame`, `_img_batch_btn`, `_img_grid_frame`,
`_img_grid_canvas`, `_img_grid_window`, `_img_listbox`). `_img_listbox` is
a hidden Listbox kept so legacy callbacks can read the selection through
`curselection()`; the visible tile selection is mirrored into it.
"""

import os
import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.widgets import (
    Button, IconButton, Scrollbar, Card, PageHeader, Segmented, EmptyState,
    IconLabel, Divider, Tooltip, Badge)
from ui.shared.scroll import attach_scroll
from ui.tab_library import (
    Tile, search_field, scroll_column, fit_text, load_cover_async,
    _autohide_scroll, grid_columns, status_label, usable_width, RAIL_W, GAP)

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']


def build_images_tab(parent, app):
    parent.configure(bg=BG1)

    app._img_scan_dirs = app._settings.get('img_scan_dirs', [])
    app._img_entries   = []
    # Selection — set of indices into _img_entries
    app._img_selected_idx = set()
    app._img_search_var = tk.StringVar()
    app._img_filter_chip = tk.StringVar(value='all')
    app._img_status_var = tk.StringVar(
        value=_('Add a folder containing PS5 images (.exfat, .ffpkg, '
                '.ffpfs, .ffpfsc) and click Re-scan'))
    app._img_sel_var = tk.StringVar(value='')
    app._img_stats_var = tk.StringVar(value='')
    app._img_tiles = {}

    _build_toolbar(parent, app)      # page header + actions
    _build_filterbar(parent, app)    # search + filter + status
    _build_main(parent, app)         # grid + rail (selection, folders)

    # Hidden listbox kept for legacy callback compat
    # (`_img_listbox.curselection()`); mirrored on every selection change.
    app._img_listbox = tk.Listbox(parent, exportselection=False)
    app._img_listbox.configure(selectmode='extended')

    # Render initial scan-dirs + status
    app._img_render_dirs()
    if app._img_scan_dirs:
        app.after(50, app._img_scan)


# ─────────────────────────────────────────────────────────────────────────
# Page header — title + description + actions (primary rightmost)
# ─────────────────────────────────────────────────────────────────────────
def _build_toolbar(parent, app):
    hdr = PageHeader(parent, _('My Images'), _('Finished images ready to use'))
    hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    a = hdr.actions
    app._img_batch_btn = Button(a, text=_('Send selected to PS5'), icon='send',
                                variant='primary', command=app._img_batch_upload)
    app._img_batch_btn.pack(side='right')
    Button(a, text=_('Re-scan'), icon='refresh',
           command=app._img_scan).pack(side='right', padx=(0, px(8)))
    Button(a, text=_('Add folder'), icon='folder_plus',
           command=app._img_add_dir).pack(side='right', padx=(0, px(8)))


# ─────────────────────────────────────────────────────────────────────────
# Filter bar — search + filter toggle + status
# ─────────────────────────────────────────────────────────────────────────
def _build_filterbar(parent, app):
    bar = tk.Frame(parent, bg=BG1)
    bar.pack(fill='x', padx=px(24), pady=(0, px(12)))

    search_field(bar, app._img_search_var, 'Search by name or title ID',
                 width=px(320)).pack(side='left')
    app._img_search_var.trace_add('write', lambda *a: _re_render_grid(app))

    seg = Segmented(bar, [('all', _('All')), ('fw451', _('FW 4.51+')),
                          ('backport', _('Backport'))],
                    variable=app._img_filter_chip,
                    command=lambda k: _re_render_grid(app))
    seg.pack(side='left', padx=(px(12), 0))
    # Legacy: key -> widgets of each filter option.
    app._img_chip_widgets = {k: (b, b, b) for k, b in seg._btns.items()}

    status_label(bar, app._img_status_var).pack(
        side='right', fill='x', expand=True, padx=(px(16), 0))


def _select_chip(app, key, re_render=True):
    """Set the filter (the Segmented toggle repaints from the variable)."""
    app._img_filter_chip.set(key)
    if re_render:
        _re_render_grid(app)


# ─────────────────────────────────────────────────────────────────────────
# Main row — tile grid (scrollable) + rail
# ─────────────────────────────────────────────────────────────────────────
def _build_main(parent, app):
    main = tk.Frame(parent, bg=BG1)
    main.pack(fill='both', expand=True, padx=px(24), pady=(0, px(16)))

    rail_outer, rail = scroll_column(main, px(RAIL_W))
    rail_outer.pack(side='right', fill='y', padx=(px(GAP), 0))

    # Selection summary
    sel = Card(rail, title='Selection')
    sel.pack(fill='x')
    sel_val = tk.Label(sel.body, font=FONTS['body_strong'], bg=BG2,
                       fg=C['fg_0'], anchor='w')
    sel_val.pack(fill='x')
    tk.Label(sel.body,
             text='Click images to select them for Send selected to PS5. '
                  'Right-click an image for more actions; double-click '
                  'opens its folder.',
             font=FONTS['caption'], bg=BG2, fg=C['fg_3'], anchor='w',
             justify='left', wraplength=px(RAIL_W) - px(32)).pack(
                 fill='x', pady=(px(4), 0))

    def _sel_text(*_a):
        v = app._img_sel_var.get()
        sel_val.configure(text=v or 'No images selected',
                          fg=C['fg_0'] if v else C['fg_3'])
    app._img_sel_var.trace_add('write', _sel_text)
    _sel_text()

    # Scan folders
    fold = Card(rail, title='Image folders')
    fold.pack(fill='x', pady=(px(GAP), 0))
    stats = tk.Label(fold.body, textvariable=app._img_stats_var,
                     font=FONTS['caption'], bg=BG2, fg=C['fg_3'], anchor='w')
    stats_div = Divider(fold.body, color=C['border_1'])
    app._img_dirs_frame = tk.Frame(fold.body, bg=BG2)
    app._img_dirs_frame.pack(fill='x')

    def _stats_vis(*_a):
        # The totals line (and its divider) only shows once images exist.
        try:
            if app._img_stats_var.get():
                if not stats.winfo_manager():
                    stats.pack(fill='x', pady=(0, px(8)),
                               before=app._img_dirs_frame)
                    stats_div.pack(fill='x', before=app._img_dirs_frame)
            elif stats.winfo_manager():
                stats.pack_forget()
                stats_div.pack_forget()
        except Exception:
            pass
    app._img_stats_var.trace_add('write', _stats_vis)

    # Tile grid
    canvas = tk.Canvas(main, bg=BG1, highlightthickness=0, bd=0)
    sb = Scrollbar(main, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    canvas.pack(side='left', fill='both', expand=True)
    app._img_grid_frame = tk.Frame(canvas, bg=BG1)
    win = canvas.create_window((0, 0), window=app._img_grid_frame, anchor='nw')
    app._img_grid_canvas = canvas
    app._img_grid_window = win
    app._img_grid_sb = sb
    _autohide_scroll(canvas, app._img_grid_frame, sb,
                     pack=dict(side='right', fill='y', padx=(px(6), 0),
                               before=canvas))
    state = {'w': 0, 'job': None}

    def _on_canvas_resize(e):
        canvas.itemconfig(win, width=e.width + px(GAP))
        if e.width == state['w']:
            return
        state['w'] = e.width
        if state['job']:
            try:
                canvas.after_cancel(state['job'])
            except Exception:
                pass
        state['job'] = canvas.after(60, lambda: _re_render_grid(app))
    canvas.bind('<Configure>', _on_canvas_resize, add='+')
    attach_scroll(canvas)


def render_dirs(app):
    """Scan-folder rows in the rail card (called by _img_render_dirs)."""
    frame = app._img_dirs_frame
    for w in frame.winfo_children():
        w.destroy()
    inner_w = px(RAIL_W) - px(32)
    if not app._img_scan_dirs:
        tk.Label(frame,
                 text='No image folders yet. Use Add folder to pick a folder '
                      'with .exfat, .ffpkg, .ffpfs or .ffpfsc images.',
                 font=FONTS['caption'], bg=BG2, fg=C['fg_3'], anchor='w',
                 justify='left', wraplength=inner_w).pack(fill='x',
                                                          pady=(0, px(4)))
        return
    for i, d in enumerate(app._img_scan_dirs):
        if i:
            Divider(frame, color=C['border_1']).pack(fill='x')
        row = tk.Frame(frame, bg=BG2)
        row.pack(fill='x', pady=px(8))
        line = tk.Frame(row, bg=BG2)
        line.pack(fill='x')
        IconLabel(line, 'folder', 16, C['fg_3'], BG2).pack(side='left')
        IconButton(line, 'close', (lambda dd=d: app._img_remove_dir(dd)),
                   tooltip='Remove folder', size='sm',
                   surface=BG2).pack(side='right')
        IconButton(line, 'folder_open', (lambda dd=d: app._open_folder(dd)),
                   tooltip=_('Open folder'), size='sm',
                   surface=BG2).pack(side='right')
        name = os.path.basename(d.rstrip('/\\')) or d
        count = sum(1 for e in app._img_entries
                    if os.path.dirname(e.get('path', '')) == d)
        badge = Badge(line, '%d image%s' % (count, '' if count == 1 else 's'),
                      'neutral', surface=BG2)
        avail = inner_w - px(24) - px(64) - int(badge.cget('width')) - px(8)
        tk.Label(line, text=fit_text(name, FONTS['body_strong'], avail),
                 font=FONTS['body_strong'], bg=BG2, fg=C['fg_1'],
                 anchor='w').pack(side='left', padx=(px(8), px(8)))
        badge.pack(side='left')
        path = tk.Label(row, text=fit_text(d, FONTS['code'], inner_w - px(24),
                                           middle=True),
                        font=FONTS['code'], bg=BG2, fg=C['fg_4'], anchor='w')
        path.pack(fill='x', padx=(px(24), 0))
        if path.cget('text') != d:
            Tooltip(path, d)


# ─────────────────────────────────────────────────────────────────────────
# Grid rendering
# ─────────────────────────────────────────────────────────────────────────
def _re_render_grid(app):
    """Re-render the tile grid from current entries + filter + search."""
    if not hasattr(app, '_img_grid_frame'):
        return
    frame = app._img_grid_frame
    for w in frame.winfo_children():
        w.destroy()
    app._img_tiles = {}

    # Apply search + chip filter
    search = ''
    try:
        search = app._img_search_var.get().strip().lower()
    except Exception:
        pass
    chip = app._img_filter_chip.get()

    visible = []
    for i, e in enumerate(app._img_entries):
        meta = _parse_meta(e['name'])
        if search:
            hay = (e['name'] + ' ' + (meta.get('cusa') or '') +
                   ' ' + meta.get('title', '')).lower()
            if search not in hay:
                continue
        if chip == 'backport' and not meta.get('is_backport'):
            continue
        # 'fw451' — FW is not known from the filename alone; no-op filter.
        visible.append((i, e, meta))

    # Folder rows show per-folder counts; keep them current.
    try:
        render_dirs(app)
    except Exception:
        pass

    if not visible:
        try:
            h = max(px(300), app._img_grid_canvas.winfo_height() - px(4))
            w = max(px(300), app._img_grid_canvas.winfo_width())
        except Exception:
            h, w = px(320), px(600)
        holder = tk.Frame(frame, bg=BG1, height=h, width=w)
        holder.pack(fill='x')
        holder.pack_propagate(False)
        if not app._img_entries:
            es = EmptyState(holder, icon='image', title=_('No Images Found'),
                            text=_('Build an image to see it here.'),
                            action=(_('Add folder'), app._img_add_dir,
                                    'folder_plus'))
        else:
            es = EmptyState(holder, icon='search', title=_('No matches'),
                            text=_('No images match the current '
                                   'search or filter.'))
        es.place(relx=0, rely=0, relwidth=1, relheight=1)
        return

    W = usable_width(app._img_grid_canvas, getattr(app, '_img_grid_sb', None))
    gap = px(GAP)
    cols, tile_w = grid_columns(W)
    try:
        old_cols = frame.grid_size()[0]
    except Exception:
        old_cols = cols
    for c in range(max(old_cols, cols)):
        if c < cols:
            frame.grid_columnconfigure(c, weight=1, uniform='imgcards',
                                       minsize=tile_w + gap)
        else:
            frame.grid_columnconfigure(c, weight=0, uniform='', minsize=0)

    for grid_i, (idx, entry, meta) in enumerate(visible):
        _make_image_card(app, entry, meta, idx, grid_i // cols,
                         grid_i % cols, tile_w)


def _make_image_card(app, entry, meta, idx, row, col, tile_w=None):
    """Build one tile for an image file."""
    tile_w = tile_w or px(180)
    sz = entry.get('size', 0)
    size_text = None
    if sz > 0:
        size_text = ('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))
    ext = entry['name'].rsplit('.', 1)[-1].lower()
    fmt_label = {'exfat': 'exFAT', 'ffpkg': 'ffpkg',
                 'ffpfs': 'PFS', 'ffpfsc': 'PFS'}.get(ext, ext.upper())
    badges = [(fmt_label, 'outline')]
    if meta.get('is_backport'):
        badges.append(('Backport', 'neutral'))
    line = '  ·  '.join(x for x in (meta.get('version_str'), size_text) if x)

    tile = Tile(app._img_grid_frame, tile_w,
                meta.get('title') or entry['name'],
                game_id=meta.get('cusa') or '', meta=line, badges=badges,
                placeholder='image', selected=(idx in app._img_selected_idx),
                check=True)
    tile.grid(row=row, column=col, padx=(0, px(GAP)), pady=(0, px(GAP)),
              sticky='nw')
    app._img_tiles[idx] = tile
    if entry.get('date'):
        Tooltip(tile.cover_label, '%s\n%s' % (entry['name'], entry['date']))

    # Tag the tile and all descendants with _img_idx so the right-click
    # handler in the main file can recover which entry was hit.
    def _tag_idx(widget, i=idx):
        widget._img_idx = i
        for child in widget.winfo_children():
            _tag_idx(child, i)
    _tag_idx(tile)

    # Cover art from a matching library game (title IDs compared without
    # hyphens: the filename gives "PPSA-05344", param.sfo "PPSA05344").
    cusa_norm = (meta.get('cusa') or '').upper().replace('-', '')
    if cusa_norm:
        try:
            for g in getattr(app, '_lib_games', []) or []:
                lib_cusa = g.get('title_id', '').upper().replace('-', '')
                if lib_cusa == cusa_norm and g.get('cover'):
                    load_cover_async(app, tile.cover_label, g['cover'],
                                     tile.cover_h)
                    break
        except Exception:
            pass

    def _on_left(_e=None, i=idx):
        _toggle_select(app, i)

    def _on_double(_e=None, e=entry):
        try:
            os.startfile(os.path.dirname(e['path']))
        except Exception:
            pass

    def _on_right(e=None, idx=idx):
        # Sync the hidden listbox first so the legacy callback sees the
        # right selection, then call the legacy handler.
        _sync_hidden_listbox(app)
        try:
            app._img_context_menu(e)
        except Exception:
            pass

    tile.bind_all_events(left=_on_left, double=_on_double, right=_on_right)


def _toggle_select(app, idx):
    """Add/remove idx from the selection and restyle its tile in place."""
    if idx in app._img_selected_idx:
        app._img_selected_idx.remove(idx)
    else:
        app._img_selected_idx.add(idx)
    _sync_hidden_listbox(app)
    _update_selection_status(app)
    tile = getattr(app, '_img_tiles', {}).get(idx)
    if tile is not None:
        try:
            tile.set_selected(idx in app._img_selected_idx)
            return
        except Exception:
            pass
    _re_render_grid(app)


def _sync_hidden_listbox(app):
    """Mirror the tile selection into the hidden _img_listbox so legacy
    callbacks can read it via `curselection()`."""
    lb = app._img_listbox
    lb.delete(0, 'end')
    for e in app._img_entries:
        lb.insert('end', e['name'])
    lb.selection_clear(0, 'end')
    for idx in sorted(app._img_selected_idx):
        if 0 <= idx < len(app._img_entries):
            lb.selection_set(idx)


def _update_selection_status(app):
    """Update the selection summary."""
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
