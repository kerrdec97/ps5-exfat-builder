"""
ui/tab_history.py — Build History page (design-system layout).

    PageHeader  "Build History"                 [Refresh] [Export CSV] [Clear history]
    Stat cards  Total builds · Success rate · Failures · Total built
    Table card  ┌ search field · All | Successful | Failed ·········· count · since ┐
                │ Game | Type | Output | Size | Duration | When | Status | actions │
                │ rows (cover thumb, title, ID · version, badges, row actions)    │
                └──────────────────────────────────────────────────────────────────┘

Backwards compat: every `app._hist_*` attribute and history callback the
rest of the app reads is preserved (`_hist_search_var`, `_hist_show_success`,
`_hist_show_failed`, `_hist_list_frame`, `app._history_refresh(list_frame)`,
`app._history_clear`, `app._history_view_log`). Reading/filtering the JSON
still happens in `exfat_builder._history_refresh`; this module only builds
widgets. `render_history_rows` draws the rows that callback hands it.
"""

import os
import json
import datetime
import threading
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import (
    PageHeader, Card, Badge, IconButton, Segmented, EmptyState, Divider,
    Tooltip, Button,
)
import tkinter.font as tkfont

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _
from ui.shared.scroll import attach_scroll
from ui.shared.app_dialog import app_info, app_error

C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']


def build_history_tab(parent, app):
    """Build the History page into `parent`."""
    parent.configure(bg=BG1)

    body = tk.Frame(parent, bg=BG1)
    body.pack(fill='both', expand=True)

    _build_page_head(body, app)
    _build_stats(body, app)
    list_frame = _build_table(body, app)

    # Initial render — defer slightly so the canvas has a real width
    app.after(100, lambda: app._history_refresh(list_frame))


# ─────────────────────────────────────────────────────────────────────────
# Page head — title + description, actions on the right
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    head = PageHeader(body, _('Build History'),
                      _('Every image you have built on this PC, newest first.'))
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    Button(head.actions, text=_('Clear history'), icon='trash',
           command=app._history_clear).pack(side='right')
    Button(head.actions, text=_('Export CSV'), icon='download',
           command=lambda: _export_csv(app)
           ).pack(side='right', padx=(0, px(8)))
    Button(head.actions, text=_('Refresh'), icon='refresh',
           command=lambda: app._history_refresh(app._hist_list_frame)
           ).pack(side='right', padx=(0, px(8)))


def _compute_since_text(app):
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
        if not history:
            return _('no entries yet')
        oldest = history[-1].get('time', '')
        # Pull just the date portion if format is YYYY-MM-DD HH:MM:SS
        date_part = oldest.split(' ')[0] if oldest else ''
        return _('since %s') % date_part if date_part else _('no date')
    except Exception:
        return _('no entries yet')


# ─────────────────────────────────────────────────────────────────────────
# Stat cards
# ─────────────────────────────────────────────────────────────────────────
def _build_stats(body, app):
    """Four stat cards: Total / Success rate / Failures / Total built.
    The value labels are kept on `app._hist_stat_labels` so every list
    refresh can update them."""
    grid = tk.Frame(body, bg=BG1)
    grid.pack(fill='x', padx=px(24), pady=(0, px(16)))
    for i in range(4):
        grid.grid_columnconfigure(i, weight=1, uniform='stat')

    labels = {}
    for col, key, label in ((0, 'total', _('Total builds')),
                            (1, 'rate', _('Success rate')),
                            (2, 'failed', _('Failures')),
                            (3, 'size', _('Total built'))):
        labels[key] = _stat_card(grid, col, label)
    app._hist_stat_labels = labels
    _update_stats(app)


def _stat_card(grid, col, label):
    """Single stat card — overline label, stat value, caption."""
    card = Card(grid, padding=16)
    card.grid(row=0, column=col, sticky='nsew',
              padx=(0 if col == 0 else px(8), 0 if col == 3 else px(8)))
    tk.Label(card, text=label.upper(), font=FONTS['overline'],
             bg=BG2, fg=C['fg_4'], anchor='w').pack(fill='x')
    val = tk.Label(card, text='—', font=FONTS['stat'], bg=BG2,
                   fg=C['fg_0'], anchor='w')
    val.pack(fill='x', pady=(px(6), px(2)))
    sub = tk.Label(card, text='', font=FONTS['caption'], bg=BG2,
                   fg=C['fg_3'], anchor='w')
    sub.pack(fill='x')
    return val, sub


def _update_stats(app):
    labels = getattr(app, '_hist_stat_labels', None)
    if not labels:
        return
    try:
        if not labels['total'][0].winfo_exists():
            return
    except Exception:
        return
    stats = _compute_stats(app)
    t = stats['total']

    def put(key, value, sub):
        v, s = labels[key]
        v.config(text=value)
        s.config(text=sub)

    put('total', str(t), _('last: %s') % stats['last_when'])
    put('rate', ('%d%%' % stats['success_pct']) if t else '—',
        ('%d / %d %s' % (stats['success_count'], t, _('succeeded')))
        if t else _('no builds yet'))
    put('failed', str(stats['failed_count']),
        _('out of %d builds') % t if t else _('no builds yet'))
    put('size', stats['total_size_str'],
        '%d %s' % (stats['outputs_on_disk'], _('outputs on disk')))


def _compute_stats(app):
    """Aggregate stats from the history JSON."""
    out = {'total': 0, 'success_count': 0, 'failed_count': 0,
           'success_pct': 0, 'last_when': '—', 'total_size_str': '—',
           'outputs_on_disk': 0}
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
    except Exception:
        history = []

    out['total'] = len(history)
    if not history:
        return out

    out['success_count'] = sum(1 for h in history if h.get('success', True))
    out['failed_count']  = out['total'] - out['success_count']
    out['success_pct']   = int(out['success_count'] / out['total'] * 100)
    out['last_when']     = _humanize_when(history[0].get('time', ''))

    total_bytes = 0
    on_disk = 0
    for h in history:
        op = h.get('output', '')
        if op and os.path.isfile(op):
            try:
                total_bytes += os.path.getsize(op)
                on_disk += 1
            except Exception:
                pass
    out['outputs_on_disk'] = on_disk

    if total_bytes >= 1024**4:
        out['total_size_str'] = '%.1f TB' % (total_bytes / 1024**4)
    elif total_bytes >= 1024**3:
        out['total_size_str'] = '%.1f GB' % (total_bytes / 1024**3)
    elif total_bytes > 0:
        out['total_size_str'] = '%d MB' % (total_bytes // 1024**2)
    return out


# ─────────────────────────────────────────────────────────────────────────
# Table card — toolbar, column header, scrollable rows
# ─────────────────────────────────────────────────────────────────────────
# Column spec (key, label, fixed width or None = flexible). Order matters —
# the header and every row share it.
_COLS = [
    ('game',     _('Game'),     None),     # flex
    ('type',     _('Type'),     90),
    ('output',   _('Output'),   None),     # flex
    ('size',     _('Size'),     90),       # right-aligned
    ('duration', _('Duration'), 80),       # right-aligned
    ('when',     _('When'),     130),
    ('status',   _('Status'),   120),
    ('actions',  '',            90),
]
_FLEX = {'game': 5, 'output': 4}
_RIGHT = ('size', 'duration')
_CELL_PADX = 12
_COLW = {'type': 96, 'size': 84, 'duration': 76, 'when': 120,
         'status': 112, 'actions': 112}


def _measure_columns():
    """Size the fixed columns from the real fonts and (translated) strings
    so no cell ever asks for more than its column — otherwise grid widens
    that column in one row only and the columns stop lining up."""
    try:
        f_cap = tkfont.Font(font=FONTS['caption_strong'])
        f_code = tkfont.Font(font=FONTS['code'])
        f_body = tkfont.Font(font=FONTS['body'])
    except Exception:
        return
    pad = 2 * px(_CELL_PADX)
    head = {k: f_cap.measure(_(lbl)) if lbl else 0 for k, lbl, _w in _COLS}

    def badge(text, icon=True):
        return px(16) + f_cap.measure(text) + (px(16) if icon else 0)

    _COLW['type'] = max(head['type'], max(badge(t, False) for t in
                        ('exFAT', 'FFPKG', 'Backport'))) + pad
    _COLW['size'] = max(head['size'], f_code.measure('999.9 GB')) + pad
    _COLW['duration'] = max(head['duration'], f_code.measure('00:00')) + pad
    _COLW['when'] = max(head['when'], f_body.measure(_('Yesterday %s') % '00:00'),
                        f_body.measure('2026-10-22'),
                        f_body.measure('59m ' + _('ago'))) + pad
    _COLW['status'] = max(head['status'], badge(_('Successful')),
                          badge(_('Failed'))) + pad
    _COLW['actions'] = 3 * px(34) + px(8)


_NARROW_PX = 1000          # below this table width the Duration column
_FLEX_NARROW = {'game': 6, 'output': 3}   # folds away and Game gets more


def _grid_columns(frame, narrow=False):
    flex = _FLEX_NARROW if narrow else _FLEX
    for i, (key, _label, _w) in enumerate(_COLS):
        if key in flex:
            frame.grid_columnconfigure(i, weight=flex[key], uniform='hflex',
                                       minsize=0)
        elif narrow and key == 'duration':
            frame.grid_columnconfigure(i, weight=0, minsize=0)
        else:
            frame.grid_columnconfigure(i, weight=0, minsize=_COLW[key])


def _build_table(body, app):
    """Build the table card and return the list frame that
    `_history_refresh` populates."""
    app._hist_search_var = tk.StringVar()
    app._hist_show_success = tk.BooleanVar(value=True)
    app._hist_show_failed  = tk.BooleanVar(value=True)
    app._hist_filter_var   = tk.StringVar(value='all')

    card = Card(body, padding=0)
    card.pack(fill='both', expand=True, padx=px(24), pady=(0, px(24)))

    # ── Toolbar: search · status filter ········· count · since ──
    bar = tk.Frame(card, bg=BG2)
    bar.pack(fill='x', padx=px(16), pady=(px(12), px(12)))

    search = XEntry(bar, textvariable=app._hist_search_var, width=34,
                    placeholder=_('Search title, ID or path'))
    search.pack(side='left')
    _sync_placeholder(search, app._hist_search_var)

    def _set_filter(v):
        app._hist_show_success.set(v in ('all', 'ok'))
        app._hist_show_failed.set(v in ('all', 'fail'))
        app._history_refresh(app._hist_list_frame)

    Segmented(bar, [('all', _('All')), ('ok', _('Successful')),
                    ('fail', _('Failed'))],
              variable=app._hist_filter_var, command=_set_filter
              ).pack(side='left', padx=(px(12), 0))

    tk.Label(bar, text=_compute_since_text(app), font=FONTS['caption'],
             bg=BG2, fg=C['fg_4']).pack(side='right')
    app._hist_count_lbl = tk.Label(bar, text='', font=FONTS['caption'],
                                   bg=BG2, fg=C['fg_3'])
    app._hist_count_lbl.pack(side='right', padx=(0, px(12)))

    # Trigger filter on every keystroke
    app._hist_search_var.trace_add('write',
        lambda *a: app._history_refresh(app._hist_list_frame))

    Divider(card, color=C['border_2']).pack(fill='x')

    # ── Body grid: header row + canvas, scrollbar beside the canvas ──
    tbl = tk.Frame(card, bg=BG2)
    tbl.pack(fill='both', expand=True, pady=(0, px(4)))
    tbl.grid_columnconfigure(0, weight=1)
    tbl.grid_rowconfigure(2, weight=1)

    _measure_columns()
    app._hist_narrow = False
    head = tk.Frame(tbl, bg=BG2)
    head.grid(row=0, column=0, sticky='ew', padx=(px(4), 0))
    _grid_columns(head)
    head_labels = {}
    for i, (key, label, _w) in enumerate(_COLS):
        anchor = 'e' if key in _RIGHT else 'w'
        lbl = tk.Label(head, text=_(label) if label else '',
                       font=FONTS['caption_strong'], bg=BG2, fg=C['fg_2'],
                       anchor=anchor, width=1)
        lbl.grid(row=0, column=i, sticky='ew', padx=px(_CELL_PADX),
                 pady=(px(8), px(8)))
        head_labels[key] = lbl

    def _on_resize(e):
        narrow = e.width < px(_NARROW_PX)
        if narrow == app._hist_narrow:
            return
        app._hist_narrow = narrow
        _grid_columns(head, narrow)
        if narrow:
            head_labels['duration'].grid_remove()
        else:
            head_labels['duration'].grid()
        try:
            app._history_refresh(app._hist_list_frame)
        except Exception:
            pass
    tbl.bind('<Configure>', _on_resize, add='+')
    Divider(tbl, color=C['border_2']).grid(row=1, column=0, columnspan=2,
                                            sticky='ew')

    canvas = tk.Canvas(tbl, bg=BG2, highlightthickness=0, bd=0)
    sb = XScrollbar(tbl, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    canvas.grid(row=2, column=0, sticky='nsew', padx=(px(4), 0))
    sb.grid(row=2, column=1, sticky='ns', padx=(0, px(4)))

    list_frame = tk.Frame(canvas, bg=BG2)
    win = canvas.create_window((0, 0), window=list_frame, anchor='nw')
    list_frame.bind('<Configure>',
        lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>',
        lambda e: canvas.itemconfig(win, width=e.width))
    attach_scroll(canvas)

    card.raise_decorations()

    # Stash on app so refresh callback can find it
    app._hist_list_frame = list_frame
    app._hist_canvas = canvas
    return list_frame


# ─────────────────────────────────────────────────────────────────────────
# Row rendering — called by exfat_builder._history_refresh
# ─────────────────────────────────────────────────────────────────────────
def _elide(label, text, side='right'):
    """Keep `text` in `label`, shortened with an ellipsis to the label's
    current width (recomputed on resize). side='left' keeps the end."""
    fnt = tkfont.Font(font=label.cget('font'))

    def fit(_e=None):
        try:
            w = label.winfo_width() - 2
        except Exception:
            return
        if w <= 4:
            return
        t = text
        if fnt.measure(t) > w:
            ell = '…'
            lo, hi = 0, len(t)
            while lo < hi:
                mid = (lo + hi + 1) // 2
                cand = (ell + t[-mid:]) if side == 'left' else (t[:mid] + ell)
                if fnt.measure(cand) <= w:
                    lo = mid
                else:
                    hi = mid - 1
            t = (ell + text[-lo:]) if side == 'left' else (text[:lo] + ell)
        if label.cget('text') != t:
            label.config(text=t)
    label.config(text=text)
    label.bind('<Configure>', fit, add='+')
    return label


def _sync_placeholder(field, var):
    """Re-check a TextField placeholder after programmatic var.set():
    the field's own trace runs before the entry has the new text."""
    def _later(*_a):
        try:
            field.after_idle(lambda: field.winfo_exists() and
                             field.event_generate('<FocusOut>'))
        except Exception:
            pass
    try:
        var.trace_add('write', _later)
    except Exception:
        pass
    _later()


def _size_text(out_path):
    if out_path and os.path.isfile(out_path):
        try:
            sz = os.path.getsize(out_path)
            if sz >= 1024**3:
                return '%.1f GB' % (sz / 1024**3)
            return '%d MB' % (sz // 1024**2)
        except Exception:
            return '—'
    return '—'


_KIND_LABEL = {'exfat': 'exFAT', 'ffpkg': 'FFPKG', 'backport': 'Backport',
               'unknown': '—'}


def render_history_rows(app, list_frame, history, filtered):
    """Draw `filtered` (a list of history dicts) into `list_frame`.
    `history` is the unfiltered list (used for the count and the empty
    state wording)."""
    for w in list_frame.winfo_children():
        w.destroy()

    _update_stats(app)
    try:
        if len(filtered) == len(history):
            txt = (_('%d builds') % len(history)) if history else ''
        else:
            txt = _('%d of %d builds') % (len(filtered), len(history))
        app._hist_count_lbl.config(text=txt)
    except Exception:
        pass

    if not filtered:
        try:
            h = max(px(260), app._hist_canvas.winfo_height() - px(8))
        except Exception:
            h = px(300)
        if not history:
            es = EmptyState(list_frame, icon='history',
                            title=_('No builds yet'),
                            text=_('Finished builds appear here with their '
                                   'output, size and result.'),
                            action=(_('Go to Build'),
                                    lambda: app._switch_tab('unibuild'),
                                    'build'),
                            bg=BG2)
        else:
            es = EmptyState(list_frame, icon='search',
                            title=_('No matching builds'),
                            text=_('Try a different search or status filter.'),
                            bg=BG2)
        es.configure(height=h)
        es.pack(fill='x')
        return

    for i, entry in enumerate(filtered):
        if i:
            Divider(list_frame, color=C['border_1']).pack(fill='x',
                                                          padx=px(8))
        _history_row(app, list_frame, entry)


def _history_row(app, list_frame, entry):
    ok = entry.get('success', True)
    kind = _detect_kind(entry)
    folder = entry.get('folder', '')
    out_path = entry.get('output', '')
    output_dir = os.path.dirname(out_path) if out_path else folder
    has_out_dir = bool(output_dir) and os.path.isdir(output_dir)

    narrow = bool(getattr(app, '_hist_narrow', False))
    row = tk.Frame(list_frame, bg=BG2)
    row.pack(fill='x')
    _grid_columns(row, narrow)
    pad = px(_CELL_PADX)

    # ── Game: cover thumb + title + ID · version ──
    game = tk.Frame(row, bg=BG2)
    game.grid(row=0, column=0, sticky='ew', padx=pad, pady=px(8))
    tw, th = px(26), px(38)
    from ui.icons import icon_image
    ph = icon_image('game', 16, C['fg_4'], BG3)
    thumb = tk.Label(game, image=ph, bg=BG3, bd=0, width=tw, height=th)
    thumb.image = ph
    thumb.pack(side='left', padx=(0, px(12)))
    if folder and os.path.isdir(folder):
        _load_thumb(app, folder, thumb, th)
    txt = tk.Frame(game, bg=BG2)
    txt.pack(side='left', fill='x', expand=True)
    _elide(tk.Label(txt, font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
                    anchor='w', width=1),
           entry.get('title', '') or _('(no title)')).pack(fill='x')
    meta = ' · '.join(filter(None, [
        entry.get('title_id', ''),
        ('v' + entry['version']) if entry.get('version') else '',
    ]))
    if meta:
        _elide(tk.Label(txt, font=FONTS['code'], bg=BG2, fg=C['fg_3'],
                        anchor='w', width=1), meta).pack(fill='x')

    # ── Type ──
    tcell = tk.Frame(row, bg=BG2)
    tcell.grid(row=0, column=1, sticky='w', padx=pad)
    Badge(tcell, _KIND_LABEL.get(kind, '—'), 'outline', surface=BG2).pack(
        anchor='w')

    # ── Output path (elided from the left so the file name stays) ──
    out_lbl = tk.Label(row, font=FONTS['code'], bg=BG2,
                       fg=C['fg_3'] if out_path else C['fg_5'],
                       anchor='w', width=1,
                       cursor='hand2' if has_out_dir else '')
    _elide(out_lbl, out_path or _('No output'), side='left')
    out_lbl.grid(row=0, column=2, sticky='ew', padx=pad)
    if out_path:
        Tooltip(out_lbl, out_path)
    if out_path and has_out_dir:
        out_lbl.bind('<Button-1>', lambda e, p=output_dir: _open_dir(p))

    # ── Size / duration (right-aligned figures) ──
    tk.Label(row, text=_size_text(out_path), font=FONTS['code'], bg=BG2,
             fg=C['fg_1'], anchor='e', width=1
             ).grid(row=0, column=3, sticky='ew', padx=pad)
    if not narrow:
        tk.Label(row, text='—', font=FONTS['code'], bg=BG2, fg=C['fg_4'],
                 anchor='e', width=1
                 ).grid(row=0, column=4, sticky='ew', padx=pad)

    # ── When ──
    when = tk.Label(row, text=_humanize_when(entry.get('time', '')),
                    font=FONTS['body'], bg=BG2, fg=C['fg_2'], anchor='w',
                    width=1)
    when.grid(row=0, column=5, sticky='ew', padx=pad)
    if entry.get('time'):
        Tooltip(when, entry.get('time'))

    # ── Status ──
    scell = tk.Frame(row, bg=BG2)
    scell.grid(row=0, column=6, sticky='w', padx=pad)
    if ok:
        Badge(scell, _('Successful'), 'success', surface=BG2).pack(anchor='w')
    else:
        Badge(scell, _('Failed'), 'danger', surface=BG2).pack(anchor='w')

    # ── Row actions ──
    acts = tk.Frame(row, bg=BG2)
    acts.grid(row=0, column=7, sticky='e', padx=(0, px(8)))
    if folder and os.path.isdir(folder):
        IconButton(acts, 'repeat',
                   lambda f=folder: app._add_to_queue_from_path(f),
                   tooltip=_('Re-queue'), size='sm', surface=BG2
                   ).pack(side='right')
    ttl_for_log = entry.get('title') or entry.get('title_id') or ''
    IconButton(acts, 'file_text',
               lambda t=ttl_for_log, o=output_dir: app._history_view_log(t, o),
               tooltip=_('View log'), size='sm', surface=BG2
               ).pack(side='right')
    if has_out_dir:
        IconButton(acts, 'folder_open', lambda p=output_dir: _open_dir(p),
                   tooltip=_('Open folder'), size='sm', surface=BG2
                   ).pack(side='right')
        # Double-click anywhere on the row opens the build folder
        for w in (row, game, txt, when, tcell, scell):
            w.bind('<Double-Button-1>', lambda e, p=output_dir: _open_dir(p))


def _open_dir(path):
    try:
        os.startfile(path)
    except Exception:
        pass


def _load_thumb(app, folder, lbl, height):
    """Load the game's cover into the row thumbnail on a worker thread."""
    def work():
        try:
            from exfat_builder import _load_cover_image
            cp = app._load_cover_art(folder)
            if not cp:
                return
            img = _load_cover_image(cp, target=height)
            if img is None:
                return
            from PIL import ImageTk

            def apply():
                try:
                    if lbl.winfo_exists():
                        photo = ImageTk.PhotoImage(img)
                        lbl.config(image=photo)
                        lbl.image = photo
                except Exception:
                    pass
            app.after(0, apply)
        except Exception:
            pass
    threading.Thread(target=work, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────────
# Helpers shared with the main file
# ─────────────────────────────────────────────────────────────────────────
def _humanize_when(time_str):
    """Convert 'YYYY-MM-DD HH:MM:SS' to '12m ago' / '2h ago' / 'Yesterday 22:14' / '3d ago'."""
    if not time_str:
        return '—'
    try:
        dt = datetime.datetime.strptime(time_str, '%Y-%m-%d %H:%M:%S')
    except Exception:
        return time_str
    now = datetime.datetime.now()
    delta = now - dt
    secs = delta.total_seconds()
    if secs < 60:
        return _('just now')
    if secs < 3600:
        return '%dm %s' % (int(secs // 60), _('ago'))
    if secs < 86400 and dt.date() == now.date():
        return '%dh %s' % (int(secs // 3600), _('ago'))
    yesterday = now.date() - datetime.timedelta(days=1)
    if dt.date() == yesterday:
        return _('Yesterday %s') % dt.strftime('%H:%M')
    days = int(secs // 86400)
    if days < 7:
        return '%dd %s' % (days, _('ago'))
    return dt.strftime('%Y-%m-%d')


def _detect_kind(entry):
    """Determine build type from output filename / folder.
    Returns one of: 'exfat', 'ffpkg', 'backport', 'unknown'."""
    out = (entry.get('output', '') or '').lower()
    folder = (entry.get('folder', '') or '').lower()
    if out.endswith('.ffpkg'):
        return 'ffpkg'
    if out.endswith('.exfat'):
        # Distinguish backport from regular exFAT by folder hint
        if 'backport' in out or 'backport' in folder:
            return 'backport'
        return 'exfat'
    return 'unknown'


def _export_csv(app):
    """Export history to CSV via file dialog."""
    from tkinter import filedialog
    try:
        with open(app._history_path(), 'r') as f:
            history = json.load(f)
    except Exception:
        history = []
    if not history:
        app_info(app, _('Export CSV'),
                            _('No history to export.'))
        return
    path = filedialog.asksaveasfilename(
        defaultextension='.csv',
        filetypes=[('CSV files', '*.csv')],
        initialfile='build_history.csv')
    if not path:
        return
    try:
        import csv
        with open(path, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            w.writerow(['Time', 'Title', 'Title ID', 'Version',
                        'Type', 'Success', 'Output', 'Folder'])
            for h in history:
                w.writerow([h.get('time', ''),
                            h.get('title', ''),
                            h.get('title_id', ''),
                            h.get('version', ''),
                            _detect_kind(h),
                            'yes' if h.get('success', True) else 'no',
                            h.get('output', ''),
                            h.get('folder', '')])
        app_info(app, _('Export CSV'),
                            _('Saved %d entries to %s') %
                            (len(history), os.path.basename(path)))
    except Exception as e:
        app_error(app, _('Export CSV'), str(e))


def _icon_btn(parent, glyph, tooltip='', command=None, accent=False, danger=False):
    """Legacy helper (kept for any external caller): a small subtle icon
    button. Glyphs map to icons via the Button component."""
    return XButton(parent, text=glyph, command=command, variant='subtle',
                   size='sm', tooltip=tooltip or None)
