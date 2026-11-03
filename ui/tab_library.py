"""
ui/tab_library.py — Library › Source Dumps (game library) screen.

Design-system layout (top to bottom):

    Game Library                     [Add Folder] [Scan Folders]
    Raw game folders ready to build  [Backport All] [Build All]
    [search…………………]  [Grid | List]                      status text
    [scan progress bar — shown only while a scan runs]
    ┌ games grid / list (scrolls) ───────────────┐ ┌ rail ───────────┐
    │ cover tiles, or a table-style list         │ │ Game details     │
    │                                            │ │ Scanning folders │
    └────────────────────────────────────────────┘ └──────────────────┘

Presentation only. Every `app._lib_*` attribute the scan / filter / build
logic reads keeps its name (`_lib_search_var`, `_lib_view_mode`,
`_lib_status_var`, `_lib_progress_frame`, `_lib_prog_canvas`,
`_lib_prog_fill`, `_lib_canvas`, `_lib_grid_frame`, `_lib_folder_frame`,
`_lib_count_var`, `_lib_stat_games_var`, `_lib_stat_folders_var`,
`_lib_lastscan_var`, `_lib_grid_btn`, `_lib_list_btn`). `_lib_scan` packs
the progress frame `before=app._lib_canvas.master`, so the canvas is a
direct child of the main row, which is a sibling of the progress frame.

The render helpers at the bottom (`render_grid`, `render_list`,
`make_game_tile`, `render_folders`, `render_detail`, `empty_state`,
`show_detail_dialog`) are what the monolith's `_lib_render_*` /
`_lib_make_card` / `_lib_show_detail` methods delegate to. The tile and
search-field helpers are reused by ui/tab_images.py.
"""

import os
import time
import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

from tkinter_theme import COLORS, FONTS, px, surface_of
from ui.shared.widgets import (
    Button, IconButton, TextField, Scrollbar, Card, Rounded, Badge,
    PageHeader, Segmented, EmptyState, IconLabel, Divider, Tooltip, autowrap)
from ui.icons import icon_image

# Star-import provides legacy theme constants and i18n function.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _
from ui.shared.scroll import attach_scroll

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']

RAIL_W = 304          # right rail width (px @ 96 dpi)
TILE_MIN = 136        # smallest grid tile width
TILE_IDEAL = 176      # preferred grid tile width
GAP = 16              # gap between tiles / cards


# ═════════════════════════════════════════════════════════════════════════
# Screen
# ═════════════════════════════════════════════════════════════════════════
def build_library_tab(parent, app):
    """Build the Library › Source Dumps screen into `parent`."""
    parent.configure(bg=BG1)
    app._lib_view_mode = tk.StringVar(value='grid')
    app._lib_search_var = tk.StringVar()
    # Fed by _lib_scan_done / _lib_render_folders (guarded hasattr there).
    app._lib_count_var = tk.StringVar(value='0 games')   # legacy compat
    # Seeded from the current data so a rebuild (language change) keeps them.
    app._lib_stat_games_var = tk.StringVar(
        value=str(len(getattr(app, '_lib_games', None) or [])))
    app._lib_stat_folders_var = tk.StringVar(
        value=str(len(getattr(app, '_lib_folders', None) or [])))
    app._lib_lastscan_var = tk.StringVar(value=_('Last scan: —'))
    app._lib_status_var = tk.StringVar(
        value=_('No games scanned yet — add a folder and click Scan'))
    app._lib_tiles = {}          # folder -> restyle(selected) for the view
    app._lib_cover_cache = getattr(app, '_lib_cover_cache', {})

    body = tk.Frame(parent, bg=BG1)
    body.pack(fill='both', expand=True)

    _build_page_head(body, app)
    _build_toolbar(body, app)
    _build_scan_bar(body, app)
    _build_main(body, app)

    # Paint the scan-folder list immediately so folders can be managed
    # before the first scan (deferred one frame so every widget exists).
    try:
        parent.after(0, app._lib_render_folders)
        parent.after(0, lambda: render_detail(app, _selected_game(app)))
    except Exception:
        pass


def _build_page_head(body, app):
    hdr = PageHeader(body, _('Game Library'),
                     _('Raw game folders ready to build'))
    hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    a = hdr.actions
    # Primary rightmost; the rest secondary, 8px apart.
    Button(a, text=_('Build All'), icon='build', variant='primary',
           command=app._lib_build_all).pack(side='right')
    Button(a, text='Backport All', icon='backports',
           command=app._lib_backport_all).pack(side='right', padx=(0, px(8)))
    Button(a, text='Scan Folders', icon='refresh',
           command=app._lib_scan).pack(side='right', padx=(0, px(8)))
    Button(a, text='Add Folder', icon='folder_plus',
           command=app._lib_add_folder).pack(side='right', padx=(0, px(8)))


def _build_toolbar(body, app):
    bar = tk.Frame(body, bg=BG1)
    bar.pack(fill='x', padx=px(24), pady=(0, px(12)))

    search = search_field(bar, app._lib_search_var,
                          'Search by title or title ID', width=px(360))
    search.pack(side='left')
    app._lib_search_var.trace_add('write', lambda *a: app._lib_apply_filter())

    seg = Segmented(bar, [('grid', 'Grid'), ('list', 'List')],
                    variable=app._lib_view_mode,
                    command=lambda v: app._lib_apply_filter())
    seg.pack(side='left', padx=(px(12), 0))
    # Legacy names for the two view buttons.
    app._lib_grid_btn = seg._btns['grid']
    app._lib_list_btn = seg._btns['list']

    status_label(bar, app._lib_status_var).pack(
        side='right', fill='x', expand=True, padx=(px(16), 0))


def _build_scan_bar(body, app):
    """Scan progress strip — packed by _lib_scan, hidden by _lib_scan_done."""
    app._lib_progress_frame = tk.Frame(body, bg=BG1)
    app._lib_progress_frame.pack(fill='x')
    box = Rounded(app._lib_progress_frame, bg=BG2, border=C['border_2'],
                  radius=8, padding=(px(16), px(10)))
    box.pack(fill='x', pady=(0, px(8)))
    IconLabel(box, 'refresh', 16, C['accent_hi'], BG2).pack(side='left')
    tk.Label(box, text=_('Scanning…'), font=FONTS['body_strong'],
             bg=BG2, fg=C['fg_1']).pack(side='left', padx=(px(8), px(16)))
    # Same widget structure as before so _lib_set_scan_progress (which
    # moves the rectangle with coords(…, 0, 0, w, 6)) works unchanged.
    bar_bg = tk.Frame(box, bg=C['bg_5'], height=px(6))
    bar_bg.pack(side='left', fill='x', expand=True)
    bar_bg.pack_propagate(False)
    app._lib_prog_bar_bg = bar_bg
    app._lib_prog_canvas = tk.Canvas(bar_bg, height=px(6), bg=C['bg_5'],
                                     highlightthickness=0, bd=0)
    app._lib_prog_canvas.pack(fill='both', expand=True)
    app._lib_prog_fill = app._lib_prog_canvas.create_rectangle(
        0, 0, 0, px(6), fill=C['accent'], outline='')
    app._lib_progress_frame.pack_forget()


def _build_main(body, app):
    main = tk.Frame(body, bg=BG1)
    main.pack(fill='both', expand=True, padx=px(24), pady=(0, px(16)))

    # ── Right rail: details + scan folders (scrolls when taller) ──
    rail_outer, rail = scroll_column(main, px(RAIL_W))
    rail_outer.pack(side='right', fill='y', padx=(px(GAP), 0))

    det = Card(rail, title='Game details')
    det.pack(fill='x', pady=(0, px(GAP)))
    app._lib_detail_card = det
    app._lib_detail_body = det.body

    fold = Card(rail, title=_strip_label(_('Scanning folders:')),
                subtitle=_strip_label(
                    _('(click ✕ to stop scanning a folder)')))
    fold.pack(fill='x')
    app._lib_folder_card = fold
    stats = tk.Frame(fold.body, bg=BG2)
    stats.pack(fill='x', pady=(0, px(12)))
    stats.columnconfigure(0, weight=1, uniform='ls')
    stats.columnconfigure(1, weight=1, uniform='ls')
    for col, (cap, var) in enumerate(((_('Games'), app._lib_stat_games_var),
                                      (_('Folders'), app._lib_stat_folders_var))):
        cell = tk.Frame(stats, bg=BG2)
        cell.grid(row=0, column=col, sticky='w')
        tk.Label(cell, text=cap, font=FONTS['caption'], bg=BG2,
                 fg=C['fg_3']).pack(anchor='w')
        tk.Label(cell, textvariable=var, font=FONTS['stat'], bg=BG2,
                 fg=C['fg_0']).pack(anchor='w')
    Divider(fold.body, color=C['border_1']).pack(fill='x')
    # Named slot _lib_render_folders writes the folder rows into.
    app._lib_folder_frame = tk.Frame(fold.body, bg=BG2)
    app._lib_folder_frame.pack(fill='x')
    Divider(fold.body, color=C['border_1']).pack(fill='x')
    foot = tk.Frame(fold.body, bg=BG2)
    foot.pack(fill='x', pady=(px(10), 0))
    Button(foot, text=_('Rescan'), icon='refresh', size='sm',
           command=app._lib_scan).pack(side='right')
    tk.Label(foot, textvariable=app._lib_lastscan_var,
             font=FONTS['caption'], bg=BG2, fg=C['fg_4'],
             anchor='w').pack(side='left', fill='x', expand=True)

    # ── Games grid / list (scrollable canvas) ──
    canvas = tk.Canvas(main, bg=BG1, highlightthickness=0, bd=0)
    sb = Scrollbar(main, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    canvas.pack(side='left', fill='both', expand=True)
    app._lib_canvas = canvas
    app._lib_grid_scrollbar = sb
    app._lib_grid_frame = tk.Frame(canvas, bg=BG1)
    canvas.create_window((0, 0), window=app._lib_grid_frame,
                         anchor='nw', tags='lgf')
    _autohide_scroll(canvas, app._lib_grid_frame, sb,
                     pack=dict(side='right', fill='y', padx=(px(6), 0),
                               before=canvas))
    state = {'w': 0, 'job': None}

    def _on_canvas(e):
        # The window is one gap wider than the canvas so every grid cell can
        # carry the same trailing gap; the last one is simply clipped.
        canvas.itemconfig('lgf', width=e.width + px(GAP))
        if e.width == state['w']:
            return
        state['w'] = e.width
        if state['job']:
            try:
                canvas.after_cancel(state['job'])
            except Exception:
                pass
        state['job'] = canvas.after(60, app._lib_apply_filter)
    canvas.bind('<Configure>', _on_canvas, add='+')
    attach_scroll(canvas)


# ═════════════════════════════════════════════════════════════════════════
# Reusable helpers (also used by ui/tab_images.py)
# ═════════════════════════════════════════════════════════════════════════
_FONT_CACHE = {}


def _font(f):
    key = tuple(f) if isinstance(f, (list, tuple)) else f
    fo = _FONT_CACHE.get(key)
    if fo is None:
        fo = _FONT_CACHE[key] = tkfont.Font(font=f)
    return fo


def fit_text(text, font, width, middle=False):
    """Shorten `text` with an ellipsis so it measures <= width pixels."""
    text = text or ''
    f = _font(font)
    if width <= 0 or f.measure(text) <= width:
        return text
    ell = '…'
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if middle:
            head = text[:(mid + 1) // 2]
            tail = text[len(text) - mid // 2:] if mid // 2 else ''
            cand = head + ell + tail
        else:
            cand = text[:mid].rstrip() + ell
        if f.measure(cand) <= width:
            lo = mid
        else:
            hi = mid - 1
    if middle:
        head = text[:(lo + 1) // 2]
        tail = text[len(text) - lo // 2:] if lo // 2 else ''
        return head + ell + tail
    return text[:lo].rstrip() + ell


def grid_columns(W):
    """Columns and tile width for a grid `W` px wide: as close to the
    preferred tile width as possible, never below the minimum."""
    gap = px(GAP)
    cols = max(1, int((W + gap) / float(px(TILE_IDEAL) + gap) + 0.5))
    while cols > 1 and (W - gap * (cols - 1)) // cols < px(TILE_MIN):
        cols -= 1
    return cols, (W - gap * (cols - 1)) // cols


def status_label(parent, var):
    """Right-aligned caption status text that wraps instead of clipping."""
    bg = surface_of(parent)
    lbl = tk.Label(parent, textvariable=var, font=FONTS['caption'], bg=bg,
                   fg=C['fg_3'], anchor='e', justify='right')
    autowrap(lbl)
    return lbl


def _strip_label(text):
    """'Scanning folders:' -> 'Scanning folders'; '(hint)' -> 'hint'."""
    t = (text or '').strip()
    t = t.rstrip(':：').strip()
    if t[:1] in '(（' and t[-1:] in ')）':
        t = t[1:-1].strip()
    return t[:1].upper() + t[1:] if t else t


def search_field(parent, var, placeholder='', width=None):
    """32px rounded search field with a leading search icon and a
    placeholder. Returns the field (a TextField)."""
    sf = surface_of(parent)
    # ttk width is in average characters (~10px for the body font).
    ent = TextField(parent, textvariable=var, surface=sf,
                    width=max(10, int((width or px(320)) / px(10))))
    try:
        base = ent.cget('style')
        name = 'Search.' + base
        ttk.Style(ent).configure(name, padding=(px(22), 0, 0, 0))
        ent.configure(style=name)
    except Exception:
        pass
    IconLabel(ent, 'search', 16, C['fg_4'], C['field_bg']).place(
        x=px(10), rely=0.5, anchor='w')
    ph = tk.Label(ent, text=placeholder, bg=C['field_bg'], fg=C['fg_4'],
                  font=FONTS['body'], bd=0, padx=0, pady=0)

    def refresh(*_a):
        try:
            if not var.get() and ent.focus_get() is not ent:
                ph.place(x=px(34), rely=0.5, anchor='w')
            else:
                ph.place_forget()
        except Exception:
            pass
    ph.bind('<Button-1>', lambda e: ent.focus_set())
    ent.bind('<FocusIn>', refresh, add='+')
    ent.bind('<FocusOut>', refresh, add='+')
    var.trace_add('write', refresh)
    ent.after_idle(refresh)
    return ent


def _autohide_scroll(canvas, inner, sb, pack):
    """Keep scrollregion in sync and show `sb` only when content overflows."""
    def _update(_e=None):
        try:
            canvas.configure(scrollregion=canvas.bbox('all'))
            need = inner.winfo_reqheight() > canvas.winfo_height() + 1
            if need and not sb.winfo_ismapped():
                sb.pack(**pack)
            elif not need and sb.winfo_ismapped():
                sb.pack_forget()
                canvas.yview_moveto(0)
        except Exception:
            pass
    inner.bind('<Configure>', _update, add='+')
    canvas.bind('<Configure>', _update, add='+')


def scroll_column(parent, width):
    """A fixed-width column whose content scrolls (wheel + auto-hiding bar)
    when it is taller than the space. Returns (outer, inner)."""
    bg = surface_of(parent)
    outer = tk.Frame(parent, bg=bg, width=width)
    outer.pack_propagate(False)
    canvas = tk.Canvas(outer, bg=bg, highlightthickness=0, bd=0)
    sb = Scrollbar(outer, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    canvas.pack(side='left', fill='both', expand=True)
    inner = tk.Frame(canvas, bg=bg)
    win = canvas.create_window((0, 0), window=inner, anchor='nw')
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(win, width=e.width),
                add='+')
    _autohide_scroll(canvas, inner, sb,
                     pack=dict(side='right', fill='y', padx=(px(4), 0),
                               before=canvas))
    attach_scroll(canvas)
    return outer, inner


def _recolor(widget, bg):
    """Set the background of a widget subtree (labels, frames, badges)."""
    try:
        if isinstance(widget, Badge):
            widget._sf = bg
            tk.Canvas.configure(widget, bg=bg)
            widget._draw()
        elif isinstance(widget, (tk.Label, tk.Frame)) and not getattr(
                widget, '_keep_bg', False):
            widget.configure(bg=bg)
    except Exception:
        pass
    for ch in widget.winfo_children():
        _recolor(ch, bg)


def _bind_tree(widget, seq, fn, skip=()):
    if widget in skip:
        return
    try:
        widget.bind(seq, fn)
    except Exception:
        pass
    for ch in widget.winfo_children():
        _bind_tree(ch, seq, fn, skip)


def load_cover_async(app, label, path, height):
    """Load cover art (2:3) at `height` px into `label` without blocking the
    UI. PhotoImages are cached on the app per (path, height)."""
    if not path:
        return
    cache = getattr(app, '_lib_cover_cache', None)
    if cache is None:
        cache = app._lib_cover_cache = {}
    key = (path, int(height))
    if key in cache:
        try:
            label.configure(image=cache[key])
            label.image = cache[key]
        except Exception:
            pass
        return

    def _apply(img):
        try:
            from PIL import ImageTk
            photo = ImageTk.PhotoImage(img)
            cache[key] = photo
            if label.winfo_exists():
                label.configure(image=photo)
                label.image = photo
        except Exception:
            pass

    def _work():
        try:
            from exfat_builder import _load_cover_image
            img = _load_cover_image(path, target=int(height))
            if img is not None:
                app.after(0, _apply, img)
        except Exception:
            pass
    threading.Thread(target=_work, daemon=True).start()


def cover_box(parent, w, h, placeholder='game', bg=None):
    """Fixed-size cover well (bg_3) with a centred placeholder icon. The
    label accepts .configure(image=photo) for the real artwork."""
    bg = bg or BG3
    size = 36 if w >= px(96) else (24 if w >= px(48) else 16)
    lbl = tk.Label(parent, bg=bg, bd=0, padx=0, pady=0, width=w, height=h,
                   image=icon_image(placeholder, size, C['fg_4'], bg))
    lbl._keep_bg = True
    return lbl


class Tile(Rounded):
    """Library / image grid tile: 2:3 cover, title, mono ID, caption meta and
    a badge row. Hover lifts the border; selection draws the accent border.

    Tile(parent, width, title, game_id, meta, badges=[(text, tone)], ...)
    tile.cover_label is the async cover-art target.
    """

    def __init__(self, master, width, title, game_id='', meta='',
                 badges=(), placeholder='game', selected=False,
                 check=False):
        pad = px(8)
        super().__init__(master, bg=BG2, border=C['border_2'], radius=8,
                         padding=pad, cursor='hand2')
        self._selected = False
        self._hover = False
        self._check = check
        cw = max(px(60), width - 2 * pad)
        ch = int(round(cw * 3 / 2.0))
        self.cover_w, self.cover_h = cw, ch
        self.cover_label = cover_box(self, cw, ch, placeholder)
        self.cover_label.pack(anchor='w')
        text_w = cw - px(2)
        info = tk.Frame(self, bg=BG2)
        info.pack(fill='x', padx=px(2), pady=(px(10), px(2)))
        top = tk.Frame(info, bg=BG2)
        top.pack(fill='x')
        self._check_lbl = None
        if check:
            self._check_lbl = tk.Label(top, bg=BG2, bd=0)
            self._check_lbl.pack(side='right', anchor='n', padx=(px(6), 0))
            text_w -= px(22)
        tl = tk.Label(top, text=fit_text(title, FONTS['body_strong'], text_w),
                      font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
                      anchor='w')
        tl.pack(side='left', fill='x', expand=True)
        if fit_text(title, FONTS['body_strong'], text_w) != title:
            Tooltip(tl, title)
        if game_id:
            tk.Label(info, text=fit_text(game_id, FONTS['code'], cw),
                     font=FONTS['code'], bg=BG2, fg=C['fg_3'],
                     anchor='w').pack(fill='x', pady=(px(2), 0))
        if meta:
            tk.Label(info, text=fit_text(meta, FONTS['caption'], cw),
                     font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
                     anchor='w').pack(fill='x', pady=(px(2), 0))
        if badges:
            row = tk.Frame(info, bg=BG2)
            row.pack(fill='x', pady=(px(8), 0))
            used = 0
            for text, tone in badges:
                b = Badge(row, text, tone, surface=BG2)
                bw = int(b.cget('width'))
                if used and used + bw > cw:
                    b.destroy()
                    break
                b.pack(side='left', padx=(0, px(4)))
                used += bw + px(4)
        self.set_selected(selected)
        self._bind_hover(self)

    def _bind_hover(self, w):
        w.bind('<Enter>', lambda e: self._set_hover(True), add='+')
        w.bind('<Leave>', self._maybe_leave, add='+')
        for ch in w.winfo_children():
            self._bind_hover(ch)

    def _maybe_leave(self, e):
        try:
            under = self.winfo_containing(e.x_root, e.y_root)
        except Exception:
            under = None
        while under is not None:
            if under is self:
                return
            under = getattr(under, 'master', None)
        self._set_hover(False)

    def _set_hover(self, on):
        if on == self._hover:
            return
        self._hover = on
        self._paint()

    def _paint(self):
        if self._selected:
            border = C['accent']
        elif self._hover:
            border = C['border_4']
        else:
            border = C['border_2']
        if border != self._xb_border:
            self.set_style(border=border)
        if self._check_lbl is not None:
            self._check_lbl.configure(image=icon_image(
                'check' if self._selected else 'circle', 16,
                C['accent_hi'] if self._selected else C['fg_5'], BG2))

    def set_selected(self, on):
        self._selected = bool(on)
        self._paint()

    def bind_all_events(self, left=None, double=None, right=None, skip=()):
        if left:
            _bind_tree(self, '<Button-1>', left, skip)
        if double:
            _bind_tree(self, '<Double-Button-1>', double, skip)
        if right:
            _bind_tree(self, '<Button-3>', right, skip)


# ═════════════════════════════════════════════════════════════════════════
# Game facts (display formatting only)
# ═════════════════════════════════════════════════════════════════════════
def fmt_version(v_raw):
    """'01.004.000' -> 'v1.04' (PS5 style); other strings truncated."""
    if not v_raw:
        return ''
    try:
        parts = v_raw.split('.')
        major = int(parts[0])
        minor = int(parts[1]) if len(parts) > 1 else 0
        return 'v%d.%02d' % (major, minor)
    except (ValueError, IndexError):
        return 'v' + v_raw[:8]


def fmt_size(sz):
    if not sz or sz <= 0:
        return ''
    if sz >= 1024 ** 3:
        return '%.1f GB' % (sz / 1024 ** 3)
    return '%d MB' % (sz // 1024 ** 2)


def _sdk(game):
    try:
        return int(game.get('system_ver') or 0)
    except Exception:
        return 0


def game_badges(game, full=False):
    """Status badges for a game: build state + SDK / backport state."""
    out = []
    sdk = _sdk(game)
    if game.get('built'):
        out.append(('Built', 'success'))
    elif full:
        out.append(('Build ready', 'outline'))
    if sdk:
        if full:
            out.append(('Backport required', 'warn') if sdk >= 18
                       else ('Backport ready', 'neutral'))
        else:
            out.append(('SDK %d' % sdk, 'warn' if sdk >= 18 else 'neutral'))
    return out


def _selected_game(app):
    folder = getattr(app, '_lib_sel_folder', None)
    if not folder:
        return None
    for g in getattr(app, '_lib_games', None) or []:
        if g.get('folder') == folder:
            return g
    return None


def _select(app, game):
    """Select a game in the current view and show it in the detail panel."""
    prev = getattr(app, '_lib_sel_folder', None)
    app._lib_sel_folder = game.get('folder') if game else None
    tiles = getattr(app, '_lib_tiles', {}) or {}
    if prev in tiles:
        try:
            tiles[prev](False)
        except Exception:
            pass
    if app._lib_sel_folder in tiles:
        try:
            tiles[app._lib_sel_folder](True)
        except Exception:
            pass
    render_detail(app, game)


# ═════════════════════════════════════════════════════════════════════════
# Views — called by the monolith's _lib_render_* methods
# ═════════════════════════════════════════════════════════════════════════
def _clear(app):
    for w in app._lib_grid_frame.winfo_children():
        w.destroy()
    app._lib_tiles = {}


def usable_width(canvas, sb):
    """Width available to the grid. The space of the auto-hiding scrollbar
    is always reserved, so showing or hiding it never changes the layout."""
    try:
        w = canvas.winfo_width()
        if w <= 50:
            return px(760)
        if sb is not None and not sb.winfo_ismapped():
            w -= sb.winfo_reqwidth() + px(6)
        return w
    except Exception:
        return px(760)


def _view_width(app):
    return usable_width(getattr(app, '_lib_canvas', None),
                        getattr(app, '_lib_grid_scrollbar', None))


def empty_state(app, parent):
    """Centred empty state for the grid/list area."""
    try:
        h = max(px(300), app._lib_canvas.winfo_height() - px(4))
    except Exception:
        h = px(320)
    holder = tk.Frame(parent, bg=BG1, height=h, width=_view_width(app))
    holder.pack(fill='x')
    holder.pack_propagate(False)
    query = ''
    try:
        query = app._lib_search_var.get().strip()
    except Exception:
        pass
    if query and getattr(app, '_lib_games', None):
        es = EmptyState(holder, icon='search', title='No matches',
                        text='No games match “%s”. Try a title or '
                             'a title ID such as PPSA01325.' % query)
    elif getattr(app, '_lib_folders', None):
        es = EmptyState(holder, icon='library', title=_('No games to show'),
                        text=_('Add a folder of PS5 dumps, then Scan to '
                               'populate your library.'),
                        action=('Scan Folders', app._lib_scan, 'refresh'))
    else:
        es = EmptyState(holder, icon='library', title=_('No games to show'),
                        text=_('Add a folder of PS5 dumps, then Scan to '
                               'populate your library.'),
                        action=('Add Folder', app._lib_add_folder,
                                'folder_plus'))
    es.place(relx=0, rely=0, relwidth=1, relheight=1)


def _refresh_side(app):
    """Keep the rail current: per-folder game counts and the details card
    (hidden while the library is empty)."""
    try:
        render_folders(app)
    except Exception:
        pass
    card = getattr(app, '_lib_detail_card', None)
    if card is None:
        return
    try:
        has = bool(getattr(app, '_lib_games', None))
        if has and not card.winfo_manager():
            card.pack(fill='x', pady=(0, px(GAP)),
                      before=app._lib_folder_card)
            render_detail(app, _selected_game(app))
        elif not has and card.winfo_manager():
            card.pack_forget()
        elif has and getattr(app, '_lib_sel_folder', None) and \
                _selected_game(app) is None:
            app._lib_sel_folder = None
            render_detail(app, None)
    except Exception:
        pass


def render_grid(app, games):
    _clear(app)
    _refresh_side(app)
    frame = app._lib_grid_frame
    if not games:
        empty_state(app, frame)
        return
    W = _view_width(app)
    gap = px(GAP)
    cols, tile_w = grid_columns(W)
    try:
        old_cols = frame.grid_size()[0]
    except Exception:
        old_cols = cols
    for c in range(max(old_cols, cols)):
        if c < cols:
            frame.grid_columnconfigure(c, weight=1, uniform='libcard',
                                       minsize=tile_w + gap)
        else:
            frame.grid_columnconfigure(c, weight=0, uniform='', minsize=0)
    app._lib_sel_card = None
    app._lib_tile_w = tile_w
    for i, game in enumerate(games):
        app._lib_make_card(frame, game, i // cols, i % cols)


def make_game_tile(app, parent, game, row, col):
    tile_w = getattr(app, '_lib_tile_w', px(180))
    meta = '  ·  '.join(x for x in (fmt_version(game.get('version')),
                                         fmt_size(game.get('size'))) if x)
    folder = game.get('folder')
    tile = Tile(parent, tile_w, game.get('title') or game.get('title_id', ''),
                game_id=game.get('title_id', ''), meta=meta,
                badges=game_badges(game),
                selected=(folder == getattr(app, '_lib_sel_folder', None)))
    tile.grid(row=row, column=col, padx=(0, px(GAP)), pady=(0, px(GAP)),
              sticky='nw')
    app._lib_tiles[folder] = tile.set_selected
    if folder == getattr(app, '_lib_sel_folder', None):
        app._lib_sel_card = tile

    def _left(_e=None, g=game):
        _select(app, g)
        app._lib_sel_card = tile
    tile.bind_all_events(
        left=_left,
        double=lambda e, g=game: app._add_to_queue_from_path(g['folder']),
        right=lambda e, g=game: (_select(app, g),
                                 app._lib_context_menu(e, g)))
    load_cover_async(app, tile.cover_label, game.get('cover'), tile.cover_h)
    return tile


# List columns: (heading, minsize px, anchor)
_LIST_COLS = (('', 40, 'w'), ('Title', 0, 'w'), ('Title ID', 116, 'w'),
              ('Version', 76, 'w'), ('SDK', 96, 'w'), ('Size', 84, 'e'))


def _list_grid(frame):
    for i, (_h, mn, _a) in enumerate(_LIST_COLS):
        frame.grid_columnconfigure(i, minsize=px(mn), weight=1 if i == 1 else 0)


def render_list(app, games):
    _clear(app)
    _refresh_side(app)
    frame = app._lib_grid_frame
    for c in range(frame.grid_size()[0]):
        frame.grid_columnconfigure(c, weight=0, uniform='', minsize=0)
    if not games:
        empty_state(app, frame)
        return
    W = _view_width(app)
    table = Rounded(frame, bg=BG2, border=C['border_2'], radius=8,
                    padding=(px(4), px(4)))
    table.pack(fill='x', padx=(0, px(GAP)))
    head = tk.Frame(table, bg=BG2)
    head.pack(fill='x', padx=px(12), pady=(px(6), px(6)))
    _list_grid(head)
    for i, (h, _mn, anc) in enumerate(_LIST_COLS):
        if h:
            tk.Label(head, text=h, font=FONTS['caption_strong'], bg=BG2,
                     fg=C['fg_3'], anchor=anc).grid(
                         row=0, column=i, sticky='ew', padx=(0, px(12)))
    fixed = sum(px(m) for _h, m, _a in _LIST_COLS) + px(12) * 6 + px(24)
    title_w = max(px(120), W - px(GAP) - px(8) - fixed)
    sel = getattr(app, '_lib_sel_folder', None)
    for game in games:
        Divider(table, color=C['border_1']).pack(fill='x', padx=px(8))
        _list_row(app, table, game, title_w, game.get('folder') == sel)


def _list_row(app, table, game, title_w, selected):
    row = tk.Frame(table, bg=BG2, cursor='hand2')
    row.pack(fill='x')
    inner = tk.Frame(row, bg=BG2)
    inner.pack(fill='x', padx=px(12), pady=px(6))
    _list_grid(inner)
    thumb = cover_box(inner, px(26), px(39), 'game', bg=BG3)
    thumb.grid(row=0, column=0, sticky='w')
    tcol = tk.Frame(inner, bg=BG2)
    tcol.grid(row=0, column=1, sticky='ew', padx=(0, px(12)))
    title = game.get('title') or game.get('title_id', '')
    tk.Label(tcol, text=fit_text(title, FONTS['body_strong'], title_w),
             font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
             anchor='w').pack(fill='x')
    wrapper = os.path.basename((game.get('wrapper') or game.get('folder')
                                or '').rstrip('/\\'))
    if wrapper:
        tk.Label(tcol, text=fit_text(wrapper, FONTS['caption'], title_w),
                 font=FONTS['caption'], bg=BG2, fg=C['fg_4'],
                 anchor='w').pack(fill='x')
    tk.Label(inner, text=game.get('title_id', '') or '—',
             font=FONTS['code'], bg=BG2, fg=C['fg_2'], anchor='w').grid(
                 row=0, column=2, sticky='ew', padx=(0, px(12)))
    tk.Label(inner, text=fmt_version(game.get('version')) or '—',
             font=FONTS['body'], bg=BG2, fg=C['fg_1'], anchor='w').grid(
                 row=0, column=3, sticky='ew', padx=(0, px(12)))
    sdk_cell = tk.Frame(inner, bg=BG2)
    sdk_cell.grid(row=0, column=4, sticky='w', padx=(0, px(12)))
    sdk = _sdk(game)
    if sdk:
        Badge(sdk_cell, 'SDK %d' % sdk, 'warn' if sdk >= 18 else 'neutral',
              surface=BG2).pack(side='left')
    else:
        tk.Label(sdk_cell, text='—', font=FONTS['body'], bg=BG2,
                 fg=C['fg_4']).pack(side='left')
    tk.Label(inner, text=fmt_size(game.get('size')) or '—',
             font=FONTS['body'], bg=BG2, fg=C['fg_0'], anchor='e').grid(
                 row=0, column=5, sticky='ew', padx=(0, px(12)))

    st = {'sel': False, 'hover': False}

    def paint():
        bg = C['accent_lo'] if st['sel'] else (BG3 if st['hover'] else BG2)
        _recolor(row, bg)

    def set_sel(on):
        st['sel'] = bool(on)
        paint()

    def hover(on, e=None):
        if not on and e is not None:
            under = row.winfo_containing(e.x_root, e.y_root)
            while under is not None:
                if under is row:
                    return
                under = getattr(under, 'master', None)
        if st['hover'] != on:
            st['hover'] = on
            paint()

    app._lib_tiles[game.get('folder')] = set_sel
    _bind_tree(row, '<Enter>', lambda e: hover(True))
    _bind_tree(row, '<Leave>', lambda e: hover(False, e))
    _bind_tree(row, '<Button-1>', lambda e, g=game: _select(app, g))
    _bind_tree(row, '<Double-Button-1>',
               lambda e, g=game: app._add_to_queue_from_path(g['folder']))
    _bind_tree(row, '<Button-3>', lambda e, g=game: (
        _select(app, g), app._lib_context_menu(e, g)))
    if selected:
        set_sel(True)
    load_cover_async(app, thumb, game.get('cover'), px(39))


# ─────────────────────────────────────────────────────────────────────────
# Detail panel (right rail)
# ─────────────────────────────────────────────────────────────────────────
def _fact(parent, label, value, mono=False, wrap=False):
    row = tk.Frame(parent, bg=BG2)
    row.pack(fill='x', pady=(0, px(6)))
    tk.Label(row, text=label, font=FONTS['body'], bg=BG2, fg=C['fg_3'],
             anchor='w').pack(side='left')
    v = tk.Label(row, text=value, font=FONTS['code'] if mono else FONTS['body'],
                 bg=BG2, fg=C['fg_0'], anchor='e', justify='right')
    v.pack(side='right')
    return v


def _action_row(parent, icon, text, command, danger=False):
    """Menu-like action row (icon + label), hover bg_3, click runs command."""
    row = tk.Frame(parent, bg=BG2, cursor='hand2')
    row.pack(fill='x')
    ic = IconLabel(row, icon, 16, C['danger_hi'] if danger else C['fg_3'], BG2)
    ic.pack(side='left', padx=(px(8), px(10)), pady=px(7))
    lbl = tk.Label(row, text=text, font=FONTS['body'], bg=BG2,
                   fg=C['danger_hi'] if danger else C['fg_1'], anchor='w')
    lbl.pack(side='left', fill='x', expand=True)

    def paint(on):
        bg = BG3 if on else BG2
        row.configure(bg=bg)
        lbl.configure(bg=bg)
        ic.set(bg=bg)
    for w in (row, ic, lbl):
        w.bind('<Enter>', lambda e: paint(True))
        w.bind('<Leave>', lambda e: paint(False))
        w.bind('<Button-1>', lambda e: command())
    return row


def render_detail(app, game):
    body = getattr(app, '_lib_detail_body', None)
    if body is None:
        return
    try:
        if not body.winfo_exists():
            return
    except Exception:
        return
    for w in body.winfo_children():
        w.destroy()
    if not game:
        box = tk.Frame(body, bg=BG2)
        box.pack(fill='x', pady=(px(4), px(8)))
        tile = Rounded(box, bg=BG3, radius=8, surface=BG2)
        tile.pack()
        IconLabel(tile, 'game', 24, C['fg_3'], BG3).pack(padx=px(12),
                                                          pady=px(12))
        msg = ('Select a game to see its details and actions.'
               if getattr(app, '_lib_games', None) else
               'Games you scan appear here with their details and actions.')
        tk.Label(box, text=msg, font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
                 justify='center', wraplength=px(RAIL_W - 64)).pack(
                     pady=(px(10), 0))
        return

    inner_w = px(RAIL_W) - px(32)
    top = tk.Frame(body, bg=BG2)
    top.pack(fill='x')
    cw, ch = px(72), px(108)
    cov = cover_box(top, cw, ch, 'game')
    cov.pack(side='left', anchor='n')
    load_cover_async(app, cov, game.get('cover'), ch)
    col = tk.Frame(top, bg=BG2)
    col.pack(side='left', fill='both', expand=True, padx=(px(12), 0))
    title = game.get('title') or game.get('title_id', '')
    tk.Label(col, text=title, font=FONTS['subtitle'], bg=BG2, fg=C['fg_0'],
             anchor='w', justify='left',
             wraplength=inner_w - cw - px(12)).pack(fill='x')
    if game.get('title_id'):
        tk.Label(col, text=game['title_id'], font=FONTS['code'], bg=BG2,
                 fg=C['fg_3'], anchor='w').pack(fill='x', pady=(px(2), 0))
    brow = tk.Frame(col, bg=BG2)
    brow.pack(fill='x', pady=(px(8), 0))
    for text, tone in game_badges(game, full=True):
        Badge(brow, text, tone, surface=BG2).pack(anchor='w', pady=(0, px(4)))

    facts = tk.Frame(body, bg=BG2)
    facts.pack(fill='x', pady=(px(14), 0))
    _fact(facts, 'Version', fmt_version(game.get('version')) or '—')
    _fact(facts, 'Dump size', fmt_size(game.get('size')) or '—')
    sdk = _sdk(game)
    _fact(facts, 'SDK', ('%d.xx' % sdk) if sdk else '—')
    try:
        mod = time.strftime('%Y-%m-%d %H:%M',
                            time.localtime(os.path.getmtime(game['folder'])))
    except Exception:
        mod = '—'
    _fact(facts, 'Modified', mod)
    tk.Label(facts, text=game.get('folder', ''), font=FONTS['code'], bg=BG2,
             fg=C['fg_3'], anchor='w', justify='left',
             wraplength=inner_w).pack(fill='x', pady=(px(2), 0))

    Divider(body, color=C['border_1']).pack(fill='x', pady=(px(14), px(12)))
    btns = tk.Frame(body, bg=BG2)
    btns.pack(fill='x')
    btns.columnconfigure(0, weight=1, uniform='da')
    btns.columnconfigure(1, weight=1, uniform='da')
    Button(btns, text=_('Build'), icon='build', surface=BG2,
           command=lambda g=game: app._lib_quick_add(g)).grid(
               row=0, column=0, sticky='ew', padx=(0, px(4)))
    Button(btns, text='Backport', icon='backports', surface=BG2,
           command=lambda g=game: app._lib_auto_backport(g)).grid(
               row=0, column=1, sticky='ew', padx=(px(4), 0))
    acts = tk.Frame(body, bg=BG2)
    acts.pack(fill='x', pady=(px(8), 0))
    _action_row(acts, 'folder_open', _('Open folder'),
                lambda g=game: app._open_folder(g['folder']))
    _action_row(acts, 'info', 'Details',
                lambda g=game: app._lib_show_detail(g))
    _action_row(acts, 'package', _('Add to ffpkg Queue'),
                lambda g=game: app._lib_quick_add_ffpkg(g))
    _action_row(acts, 'trash', _('Remove from library'),
                lambda g=game: _remove(app, g), danger=True)


def _remove(app, game):
    app._lib_remove_game(game)
    if getattr(app, '_lib_sel_folder', None) == game.get('folder'):
        app._lib_sel_folder = None
        render_detail(app, None)


# ─────────────────────────────────────────────────────────────────────────
# Scan-folder rows (rail card)
# ─────────────────────────────────────────────────────────────────────────
def render_folders(app):
    frame = app._lib_folder_frame
    for w in frame.winfo_children():
        w.destroy()
    folders = list(getattr(app, '_lib_folders', None) or [])
    if hasattr(app, '_lib_stat_folders_var'):
        app._lib_stat_folders_var.set(str(len(folders)))
    if not folders:
        lbl = tk.Label(frame,
                       text=_('No scan folders added yet — click "+ Add Folder"'),
                       font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
                       anchor='w', justify='left',
                       wraplength=px(RAIL_W) - px(32))
        lbl.pack(fill='x', pady=px(12))
        return
    inner_w = px(RAIL_W) - px(32)
    games = getattr(app, '_lib_games', None) or []
    for i, folder in enumerate(folders):
        if i:
            Divider(frame, color=C['border_1']).pack(fill='x')
        count = sum(1 for g in games if g.get('folder', '').startswith(folder))
        row = tk.Frame(frame, bg=BG2)
        row.pack(fill='x', pady=px(8))
        line = tk.Frame(row, bg=BG2)
        line.pack(fill='x')
        IconLabel(line, 'folder', 16, C['fg_3'], BG2).pack(side='left')
        IconButton(line, 'close', (lambda ix=i: app._lib_remove_folder(ix)),
                   tooltip=_('Stop scanning this folder'), size='sm',
                   surface=BG2).pack(side='right')
        IconButton(line, 'folder_open', (lambda f=folder: app._open_folder(f)),
                   tooltip=_('Open folder'), size='sm',
                   surface=BG2).pack(side='right')
        name = os.path.basename(folder.rstrip('/\\')) or folder
        badge = Badge(line, ('1 game' if count == 1 else
                             '%d %s' % (count, _('games'))),
                      'neutral', surface=BG2)
        avail = inner_w - px(16 + 8) - px(64) - int(badge.cget('width')) - px(8)
        tk.Label(line, text=fit_text(name, FONTS['body_strong'], avail),
                 font=FONTS['body_strong'], bg=BG2, fg=C['fg_1'],
                 anchor='w').pack(side='left', padx=(px(8), px(8)))
        badge.pack(side='left')
        path = tk.Label(row, text=fit_text(folder, FONTS['code'],
                                           inner_w - px(24), middle=True),
                        font=FONTS['code'], bg=BG2, fg=C['fg_4'], anchor='w')
        path.pack(fill='x', padx=(px(24), 0))
        if path.cget('text') != folder:
            Tooltip(path, folder)


# ─────────────────────────────────────────────────────────────────────────
# Details dialog (context menu › Details)
# ─────────────────────────────────────────────────────────────────────────
def show_detail_dialog(app, game):
    win = tk.Toplevel(app)
    win.title(game['title'])
    win.configure(bg=BG2)
    win.resizable(False, False)
    win.transient(app)

    body = tk.Frame(win, bg=BG2)
    body.pack(fill='both', expand=True, padx=px(24), pady=(px(20), px(20)))
    tk.Label(body, text=game['title'], font=FONTS['title'], bg=BG2,
             fg=C['fg_0'], anchor='w', justify='left',
             wraplength=px(520)).pack(fill='x')
    tk.Label(body, text=game['title_id'], font=FONTS['code'], bg=BG2,
             fg=C['fg_3'], anchor='w').pack(fill='x', pady=(px(2), px(16)))

    top = tk.Frame(body, bg=BG2)
    top.pack(fill='x')
    cov = cover_box(top, px(120), px(180), 'game')
    cov.pack(side='left', anchor='n', padx=(0, px(20)))
    load_cover_async(app, cov, game.get('cover'), px(180))

    info = tk.Frame(top, bg=BG2)
    info.pack(side='left', fill='both', expand=True)

    def fact(label, value=None, var=None, mono=False):
        tk.Label(info, text=label, font=FONTS['caption'], bg=BG2,
                 fg=C['fg_3'], anchor='w').pack(fill='x')
        kw = dict(textvariable=var) if var is not None else dict(text=value)
        tk.Label(info, font=FONTS['code'] if mono else FONTS['body'], bg=BG2,
                 fg=C['fg_0'], anchor='w', justify='left',
                 wraplength=px(340), **kw).pack(fill='x', pady=(px(2), px(10)))

    if game['version']:
        fact('Version', game['version'])
    if game['size'] > 0:
        sz = ('%.2f GB' % (game['size'] / 1024**3)
              if game['size'] >= 1024**3
              else '%d MB' % (game['size'] // 1024**2))
        fact('Estimated image size', sz)

    # SDK version — read in background
    sdk_var = tk.StringVar(value='Detecting SDK...')
    fact('SDK', var=sdk_var)

    def _detect_sdk_bg():
        sdk = ExFATBuilder._read_sdk_from_folder(game['folder'])
        if sdk is not None:
            suggest = max(4, sdk - 1)
            app.after(0, sdk_var.set,
                      'Game SDK: %d.xx  —  Backport target: SDK %d' % (sdk, suggest))
        else:
            app.after(0, sdk_var.set, 'SDK: not detected')
    threading.Thread(target=_detect_sdk_bg, daemon=True).start()

    fact('Folder', game['folder'], mono=True)

    # Footer band: bg_1, right-aligned buttons, primary rightmost.
    Divider(win, color=C['border_2']).pack(fill='x')
    foot = tk.Frame(win, bg=BG1)
    foot.pack(fill='x')
    fin = tk.Frame(foot, bg=BG1)
    fin.pack(fill='x', padx=px(24), pady=px(12))
    close = Button(fin, text='Close', variant='primary', surface=BG1,
                   command=win.destroy, width=10)
    close.pack(side='right')
    Button(fin, text=_('Open folder'), icon='folder_open', surface=BG1,
           command=lambda: app._open_folder(game['folder'])).pack(
               side='right', padx=(0, px(8)))
    win.bind('<Escape>', lambda e: win.destroy())
    win.bind('<Return>', lambda e: win.destroy())
    try:
        win.update_idletasks()
        w, h = max(px(580), win.winfo_reqwidth()), win.winfo_reqheight()
        x = app.winfo_rootx() + (app.winfo_width() - w) // 2
        y = app.winfo_rooty() + (app.winfo_height() - h) // 3
        win.geometry('%dx%d+%d+%d' % (w, h, max(0, x), max(0, y)))
        close.focus_set()
    except Exception:
        pass
    return win
