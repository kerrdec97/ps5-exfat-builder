"""
ui/dump_rename.py — Simplified Dump Rename tab.

Layout (design system): page header; Source folder card (path, Browse,
Scan) beside the Naming pattern card; then one preview table card —
inline metrics, Game / Rename preview / Size / Status columns with
confidence badges, and a pinned footer (status, progress, Rename).

Original flow notes:

Replaces the v3 inspector (3-pane list/detail/bulk + KPI cards + Move-to
+ candidates + sanitize toggles) with a streamlined single-screen flow:

    1. Pick a source folder
    2. Click Scan to find dumps
    3. Choose ONE of three naming patterns (PPSA only / + Title / + Title + Version)
    4. Click Rename selected

That's it. All the tabs in the legacy v3 (move destination, search,
duplicates pane, candidate suggestions, case toggles, sanitize toggles,
replace-spaces toggle, etc.) are gone. The naming engine in
exfat_builder.py is unchanged — sanitization is left ON by default
because that's what 99% of users want, and the casing is preserved as-is.

Backend invariants preserved (so the existing scan / rename workers in
exfat_builder.py still work without code changes):

  - app._dumps_items: list of 5-tuples [(full, name_var, chk_var,
                                          size_var, status_var)]
  - app._dumps_model: list of dicts (one per dump)
  - app._dumps_iid_to_idx: dict (kept for compat; no longer populated
                                 in a meaningful way)
  - app._dumps_src_var, app._dumps_dst_var (empty = in-place rename),
    app._dumps_status_var, app._dumps_naming_preset_var,
    app._dumps_case_var, app._dumps_sanitize_var,
    app._dumps_replace_spaces_var, app._dumps_uppercase_id_var
  - app._dumps_pbar: a real ttk.Progressbar widget (worker calls
                     .start/.stop/.configure on it)
  - app._dumps_tab: the tab instance (with stub _refresh_kpi /
                    _show_detail methods so legacy callbacks no-op safely)

Module-level `_CONF_TO_GROUP` is preserved because `_dumps_add_card` in
the main file imports it at the top of the function.
"""

import os
import tkinter as tk
import tkinter.font as tkfont
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import (
    PageHeader, Card, Badge, Button, Divider, EmptyState, IconLabel,
    ProgressBar, Tooltip,
)
from tkinter import ttk, filedialog

from tkinter_theme import COLORS, FONTS, px
from ui.icons import icon_image
from ui.shared.scroll import attach_scroll
from ui.tab_history import _elide, _sync_placeholder

from exfat_builder import _, _load_cover_image

BG1, BG2, BG3 = COLORS['bg_1'], COLORS['bg_2'], COLORS['bg_3']


class _ScanBar(ProgressBar):
    """Indeterminate scan bar that is only visible while it runs. The scan
    worker drives it with .configure(mode=…) / .start() / .stop()."""

    def __init__(self, master=None, pack_kw=None, **kw):
        super().__init__(master, **kw)
        self._pack_kw = pack_kw or {}

    def start(self, interval=None):
        try:
            self.pack(**self._pack_kw)
        except Exception:
            pass
        return super().start(interval) if interval else super().start()

    def stop(self):
        res = super().stop()
        try:
            self.pack_forget()
        except Exception:
            pass
        return res




# ─────────────────────────────────────────────────────────────────────────
# Compat: imported by exfat_builder._dumps_add_card. Kept so the legacy
# call site continues to work even though we no longer use Treeview groups.
# ─────────────────────────────────────────────────────────────────────────
_CONF_TO_GROUP = {
    'green':  'ready',
    'yellow': 'review',
    'red':    'failed',
}


# ─────────────────────────────────────────────────────────────────────────
# Naming-pattern presets shown to the user (label, internal key, example)
# ─────────────────────────────────────────────────────────────────────────
_PATTERNS = [
    ('ppsa',         'PPSA only',                'PPSA01234'),
    ('ppsa_title',   'PPSA + Title',             'PPSA01234 Spider-Man'),
    ('ppsa_title_v', 'PPSA + Title + Version',   'PPSA01234 Spider-Man (1.05)'),
]


# ─────────────────────────────────────────────────────────────────────────
# Public API
# ─────────────────────────────────────────────────────────────────────────
class DumpRenameTab:
    """Simplified Dump Rename tab.

    Public class kept for parity with the v3 brief, but most of its
    surface (`_refresh_kpi`, `_show_detail`) is now no-op stubs that
    only exist so legacy callbacks in exfat_builder.py don't blow up.
    """

    def __init__(self, parent, app):
        self.app = app
        app._dumps_tab = self

        parent.configure(bg=COLORS['bg_1'])

        # ── State the existing scan/rename workers expect ──
        app._dumps_items     = []
        app._dumps_model     = []
        app._dumps_iid_to_idx = {}
        app._dumps_scanning  = False
        app._dumps_selected_iid = None

        app._dumps_src_var    = tk.StringVar(
            value=app._settings.get('dumps_last_src', ''))
        app._dumps_dst_var    = tk.StringVar()   # empty = in-place rename
        app._dumps_status_var = tk.StringVar(value=_('Ready.'))

        # The naming preset — the ONLY user-facing knob now
        app._dumps_naming_preset_var = tk.StringVar(value='ppsa_title_v')

        # Cover-image cache. Keys: full folder path. Values: ImageTk
        # PhotoImage. Held here so Tk doesn't garbage-collect them.
        self._cover_cache = {}
        # Folders currently being loaded — prevents double-loading the
        # same cover on every list refresh.
        self._covers_in_flight = set()

        # Sensible fixed defaults for the engine. These vars are kept
        # because _dumps_regenerate_names reads them; we just don't
        # expose them in the UI any more.
        app._dumps_case_var           = tk.StringVar(value='keep')
        app._dumps_sanitize_var       = tk.BooleanVar(value=True)
        app._dumps_replace_spaces_var = tk.BooleanVar(value=False)
        app._dumps_uppercase_id_var   = tk.BooleanVar(value=True)

        # Legacy template var — kept as an alias some older callbacks read
        app._dumps_template_var = tk.StringVar(
            value='{PPSA} {TITLE} ({VERSION})')

        # Regenerate names whenever the preset changes
        def _regen(*_a):
            try:
                app._dumps_regenerate_names()
            except Exception:
                pass
            self._refresh_list()
        app._dumps_naming_preset_var.trace_add('write', _regen)

        # Build the widget tree
        self._build(parent)

        # Legacy compat: the main app's `_dumps_scan()` calls
        # `for w in self._dumps_list_frame.winfo_children(): w.destroy()`
        # at the start of every scan. We provide a hidden Frame so that
        # call is a harmless no-op. (Our real list lives in
        # self._list_inner, refreshed from _dumps_items via _refresh_list.)
        if not hasattr(app, '_dumps_list_frame'):
            app._dumps_list_frame = tk.Frame(parent)
            # Don't pack it — invisible.

        # Legacy compat: the scan worker schedules
        # `self.after(0, self._dumps_scan_done, found)` when finished, but
        # no implementation exists in exfat_builder.py. Provide a safe one
        # that stops the progressbar, updates the status line, and
        # re-renders our list.
        def _scan_done(found):
            try:
                app._dumps_pbar.stop()
            except Exception:
                pass
            if found:
                app._dumps_status_var.set(
                    _('%d dump(s) found.') % found)
            else:
                app._dumps_status_var.set(_('No dumps found.'))
            self._refresh_list()
            # Trigger a name regen so initial rows show the right
            # proposed names per the active preset.
            try:
                app._dumps_regenerate_names()
            except Exception:
                pass
            self._refresh_list()
        app._dumps_scan_done = _scan_done

    # ─────────────────────────────────────────────────────────────────
    # UI
    # ─────────────────────────────────────────────────────────────────
    def _build(self, parent):
        # Page header
        head = PageHeader(parent, _('Dump Rename'),
                          _('Rename PS5 dump folders from their '
                            'PPSA metadata'))
        head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

        # ── Source folder + naming pattern side by side ──
        top = tk.Frame(parent, bg=BG1)
        top.pack(fill='x', padx=px(24), pady=(0, px(16)))
        top.grid_columnconfigure(0, weight=1, uniform='drtop')
        top.grid_columnconfigure(1, weight=1, uniform='drtop')
        self._build_source_card(top)
        self._build_pattern_card(top)

        # ── Preview table card (footer with the Rename action pinned) ──
        self._build_list_card(parent)

    def _build_kpi_strip(self, parent):
        """Four inline metrics: Dumps Found / Matched / Unknown / Already
        Named. Fed by _refresh_kpi from the scan model — presentation
        only, derived from data the scan already produces."""
        strip = tk.Frame(parent, bg=BG2)
        strip.pack(side='left', padx=(px(16), 0))

        def _cell(caption, sub, first=False):
            cell = tk.Frame(strip, bg=BG2)
            cell.pack(side='left', padx=(0 if first else px(16), 0))
            v = tk.Label(cell, text='0', font=FONTS['body_strong'], bg=BG2,
                         fg=COLORS['fg_0'])
            v.pack(side='left')
            lbl = tk.Label(cell, text=caption, font=FONTS['caption'], bg=BG2,
                           fg=COLORS['fg_3'])
            lbl.pack(side='left', padx=(px(4), 0))
            Tooltip(lbl, sub)
            Tooltip(v, sub)
            return v

        self._kpi_found   = _cell(_('Dumps Found'), _('folders scanned'),
                                  first=True)
        self._kpi_matched = _cell(_('Matched'), _('will be renamed'))
        self._kpi_unknown = _cell(_('Unknown'), _('no match found'))
        self._kpi_already = _cell(_('Already Named'), _('already clean'))

    @staticmethod
    def _ppsa(item, m):
        """Title ID of a dump: the scan model stores it as `title_id`."""
        m = m or {}
        return m.get('ppsa') or m.get('title_id') or ''

    @staticmethod
    def _classify(item, m):
        """Return 'matched' | 'unknown' | 'already' for one dump."""
        try:
            full, name_var, _c, _s, _st = item
        except (ValueError, TypeError):
            return 'unknown'
        ppsa = DumpRenameTab._ppsa(item, m)
        if not ppsa:
            return 'unknown'
        orig = (m or {}).get('orig_name') or os.path.basename(full)
        proposed = name_var.get() or ''
        if proposed and proposed == orig:
            return 'already'
        return 'matched'

    @staticmethod
    def _confidence(m):
        """green / yellow / red. Uses the model's own value when present,
        else derives it from what the scan found (ID + title = green,
        ID only = yellow, nothing = red)."""
        m = m or {}
        conf = m.get('confidence')
        if conf in ('green', 'yellow', 'red'):
            return conf
        if m.get('ppsa') or m.get('title_id'):
            return 'green' if m.get('title') else 'yellow'
        return 'red'

    def _build_source_card(self, parent):
        card = Card(parent, title=_('Source folder'),
                    subtitle=_('Folder containing your PS5 dumps. Each dump is '
                               'a sub-folder. The original folders will be '
                               'renamed in place.'))
        card.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))

        row = tk.Frame(card.body, bg=BG2)
        row.pack(fill='x', side='bottom')

        src = XEntry(row, textvariable=self.app._dumps_src_var, mono=True,
                     placeholder=_('Choose folder containing PS5 dumps'))
        src.pack(side='left', fill='x', expand=True)
        _sync_placeholder(src, self.app._dumps_src_var)
        Button(row, text=_('Browse') + '…', icon='folder_open',
               command=self._browse_src, surface=BG2
               ).pack(side='left', padx=(px(6), 0))
        Button(row, text=_('Scan'), icon='search', command=self._do_scan,
               surface=BG2).pack(side='left', padx=(px(6), 0))

    def _build_pattern_card(self, parent):
        card = Card(parent, title=_('Naming pattern'))
        card.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))

        grid = tk.Frame(card.body, bg=BG2)
        grid.pack(fill='x')
        grid.grid_columnconfigure(1, weight=1)
        for r, (key, label, example) in enumerate(_PATTERNS):
            rb = XRadio(grid, text=_(label), value=key,
                        variable=self.app._dumps_naming_preset_var)
            rb.grid(row=r, column=0, sticky='w', pady=px(1))
            tk.Label(grid, text=example, font=FONTS['code'], bg=BG2,
                     fg=COLORS['fg_4'], anchor='w'
                     ).grid(row=r, column=1, sticky='w', padx=(px(16), 0))

    # Table columns: (key, header, fixed width or None = flexible)
    _COLS = [('chk', '', 40), ('dump', 'Dump', None),
             ('rename', 'Rename preview', None), ('size', 'Size', 96),
             ('status', 'Status', 150)]
    _FLEX = {'dump': 4, 'rename': 5}

    def _grid_cols(self, frame):
        for i, (key, _l, w) in enumerate(self._COLS):
            if w is None:
                frame.grid_columnconfigure(i, weight=self._FLEX[key],
                                           uniform='drflex', minsize=0)
            else:
                frame.grid_columnconfigure(i, weight=0,
                                           minsize=self._colw.get(key, px(w)))

    def _measure_cols(self):
        """Fixed column widths from the real fonts/strings, so a wide
        badge in one row can never shift that row's columns."""
        self._colw = {}
        try:
            f_cap = tkfont.Font(font=FONTS['caption_strong'])
            f_code = tkfont.Font(font=FONTS['code'])
        except Exception:
            return
        pad = 2 * px(12)
        badge = max(f_cap.measure(t) for t in
                    (_('Ready'), _('Needs review'), _('Cannot rename')))
        self._colw['status'] = max(px(150), badge + px(32) + pad)
        self._colw['size'] = max(px(96), f_code.measure('999.99 GB') + pad)
        self._colw['chk'] = px(40)

    def _build_list_card(self, parent):
        card = Card(parent, padding=0)
        card.pack(fill='both', expand=True, padx=px(24), pady=(0, px(24)))
        self._list_card = card

        head = tk.Frame(card, bg=BG2)
        head.pack(fill='x', padx=px(16), pady=(px(12), px(12)))

        tk.Label(head, text=_('Found dumps'), font=FONTS['subtitle'],
                 bg=BG2, fg=COLORS['fg_0']).pack(side='left')

        self._count_lbl = tk.Label(head, text='', font=FONTS['caption'],
                                   bg=BG2, fg=COLORS['fg_3'])
        self._count_lbl.pack(side='left', padx=(px(8), 0))

        Button(head, text=_('Select all'), command=self._select_all,
               size='sm', surface=BG2).pack(side='right')
        Button(head, text=_('Select none'), command=self._select_none,
               size='sm', surface=BG2).pack(side='right', padx=(0, px(6)))

        # Inline metrics (Found / Matched / Unknown / Already named)
        self._build_kpi_strip(head)

        # ── Footer: status · progress ······ Rename selected ──
        # Packed side='bottom' BEFORE the list so the Rename button is
        # always visible: the list shrinks instead of pushing it away.
        foot = tk.Frame(card, bg=BG2)
        foot.pack(side='bottom', fill='x', padx=px(16), pady=(px(12), px(12)))
        Divider(card, color=COLORS['border_2']).pack(side='bottom', fill='x')

        self._rename_btn = Button(foot, text=_('Rename selected'),
                                  icon='rename', variant='primary',
                                  command=self._do_rename, surface=BG2)
        self._rename_btn.pack(side='right')

        tk.Label(foot, textvariable=self.app._dumps_status_var,
                 font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'],
                 anchor='w', justify='left'
                 ).pack(side='left')

        # Progressbar — the scan worker starts/stops it (indeterminate)
        self.app._dumps_pbar = _ScanBar(
            foot, mode='indeterminate', length=px(160), surface=BG2,
            pack_kw=dict(side='left', padx=(px(16), 0)))

        Divider(card, color=COLORS['border_2']).pack(fill='x')

        # ── Column header + scrollable rows ──
        self._measure_cols()
        tbl = tk.Frame(card, bg=BG2)
        tbl.pack(fill='both', expand=True)
        tbl.grid_columnconfigure(0, weight=1)
        tbl.grid_rowconfigure(2, weight=1)

        colhead = tk.Frame(tbl, bg=BG2)
        colhead.grid(row=0, column=0, sticky='ew', padx=(px(4), 0))
        self._grid_cols(colhead)
        hdr_txt = {'dump': _('Game'), 'rename': 'Rename preview',
                   'size': _('Size'), 'status': _('Status')}
        for i, (key, _l, _w) in enumerate(self._COLS):
            tk.Label(colhead, text=hdr_txt.get(key, ''),
                     font=FONTS['caption_strong'], bg=BG2, fg=COLORS['fg_2'],
                     anchor='e' if key == 'size' else 'w', width=1
                     ).grid(row=0, column=i, sticky='ew', padx=px(12),
                            pady=px(8))
        Divider(tbl, color=COLORS['border_2']).grid(row=1, column=0,
                                                     columnspan=2, sticky='ew')

        self._canvas = tk.Canvas(tbl, bg=BG2, highlightthickness=0, bd=0,
                                 height=px(120))  # request small; expand grows it
        sb = XScrollbar(tbl, orient='vertical', command=self._canvas.yview)
        self._canvas.configure(yscrollcommand=sb.set)
        self._canvas.grid(row=2, column=0, sticky='nsew', padx=(px(4), 0))
        sb.grid(row=2, column=1, sticky='ns', padx=(0, px(4)))

        self._list_inner = tk.Frame(self._canvas, bg=BG2)
        self._list_window = self._canvas.create_window(
            (0, 0), window=self._list_inner, anchor='nw')

        def _on_inner_config(event):
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        self._list_inner.bind('<Configure>', _on_inner_config)

        def _on_canvas_config(event):
            self._canvas.itemconfig(self._list_window, width=event.width)
        self._canvas.bind('<Configure>', _on_canvas_config)
        attach_scroll(self._canvas)

        card.raise_decorations()

        # Empty-state placeholder
        self._empty_label = self._empty_state()

    def _empty_state(self):
        try:
            h = max(px(200), self._canvas.winfo_height() - px(8))
        except Exception:
            h = px(240)
        es = EmptyState(self._list_inner, icon='folders',
                        title=_('No dumps scanned yet'),
                        text=_('Pick a source folder and click Scan.'),
                        bg=BG2)
        es.configure(height=h)
        es.pack(fill='x')
        return es

    def _build_footer(self, parent):
        """Kept for compatibility — the footer now lives inside the list
        card (see _build_list_card)."""
        return None

    # ─────────────────────────────────────────────────────────────────
    # Actions
    # ─────────────────────────────────────────────────────────────────
    def _browse_src(self):
        folder = filedialog.askdirectory(
            title=_('Choose folder containing PS5 dumps'),
            initialdir=self.app._dumps_src_var.get() or os.path.expanduser('~'))
        if folder:
            self.app._dumps_src_var.set(folder)

    def _do_scan(self):
        # Clear any existing rows from a previous scan, then call the
        # main app's scanner. The scanner calls back into _dumps_add_card
        # which we no-op-stub on the Treeview side.
        self._clear_list()
        try:
            self.app._dumps_scan()
        except Exception as e:
            self.app._dumps_status_var.set(
                _('Scan failed:') + ' ' + str(e))
            return
        # The scanner runs in a thread; it'll call _dumps_add_card
        # repeatedly. We schedule a refresh shortly after to render
        # whatever's been collected. Subsequent finds will piggyback on
        # the trace-driven _refresh_list in _regen, but the scan itself
        # doesn't change the preset var, so we poll for ~2s.
        self._poll_refresh(0)

    def _poll_refresh(self, ticks):
        # Re-render the list while scanning, up to ~6 seconds.
        self._refresh_list()
        if getattr(self.app, '_dumps_scanning', False) and ticks < 60:
            self.app.after(100, lambda: self._poll_refresh(ticks + 1))
        else:
            # Final render once scanning has stopped
            self.app.after(50, self._refresh_list)

    def _do_rename(self):
        # _dumps_apply reads the selected items off self._dumps_items
        # and runs the rename on a worker thread.
        try:
            self.app._dumps_apply()
        except Exception as e:
            self.app._dumps_status_var.set(
                _('Rename failed:') + ' ' + str(e))

    def _select_all(self):
        for full, nv, chk, sv, st in self.app._dumps_items:
            try:
                chk.set(True)
            except Exception:
                pass
        self._refresh_count()

    def _select_none(self):
        for full, nv, chk, sv, st in self.app._dumps_items:
            try:
                chk.set(False)
            except Exception:
                pass
        self._refresh_count()

    # ─────────────────────────────────────────────────────────────────
    # List rendering
    # ─────────────────────────────────────────────────────────────────
    def _clear_list(self):
        for w in self._list_inner.winfo_children():
            w.destroy()
        self._row_widgets = []

    def _refresh_list(self):
        # Wipe the inner frame and rebuild from current state.
        # Cheap: there's typically 1-200 rows.
        for w in self._list_inner.winfo_children():
            w.destroy()
        self._row_widgets = []

        items = list(self.app._dumps_items)
        model = list(self.app._dumps_model)

        if not items:
            self._empty_state()
            self._refresh_kpi()
            return

        for i, item in enumerate(items):
            try:
                full, name_var, chk_var, size_var, status_var = item
            except (ValueError, TypeError):
                continue
            m = model[i] if i < len(model) else {}
            if i:
                Divider(self._list_inner, color=COLORS['border_1']).pack(
                    fill='x', padx=px(8))
            self._row(i, full, name_var, chk_var, size_var, status_var, m)

        self._refresh_kpi()

    def _row(self, idx, full, name_var, chk_var, size_var, status_var, m):
        orig_name  = m.get('orig_name') or os.path.basename(full)
        proposed   = name_var.get() or ''
        confidence = self._confidence(m)

        ppsa     = self._ppsa(None, m)
        title    = m.get('title', '') or ''
        version  = m.get('version', '') or ''

        row = tk.Frame(self._list_inner, bg=BG2)
        row.pack(fill='x')
        self._grid_cols(row)
        pad = px(12)

        # ── Checkbox ──
        cb = XCheck(row, variable=chk_var, command=self._refresh_count)
        cb.grid(row=0, column=0, sticky='w', padx=(pad, 0))

        # ── Dump: cover thumb + title + PPSA · version ──
        dump = tk.Frame(row, bg=BG2)
        dump.grid(row=0, column=1, sticky='ew', padx=pad, pady=px(8))
        tw, th = px(26), px(38)
        ph = icon_image('game', 16, COLORS['fg_4'], BG3)
        cover_lbl = tk.Label(dump, image=ph, bg=BG3, bd=0, width=tw,
                             height=th)
        cover_lbl.image = ph
        cover_lbl.pack(side='left', padx=(0, px(12)))
        cached = self._cover_cache.get(full)
        if cached is not None:
            try:
                cover_lbl.config(image=cached)
                cover_lbl.image = cached
            except Exception:
                pass
        else:
            self._async_load_cover(full, cover_lbl)

        txt = tk.Frame(dump, bg=BG2)
        txt.pack(side='left', fill='x', expand=True)
        _elide(tk.Label(txt, font=FONTS['body_strong'], bg=BG2,
                        fg=COLORS['fg_0'] if title else COLORS['fg_3'],
                        anchor='w', width=1),
               title if title else _('(unknown title)')).pack(fill='x')
        if ppsa:
            meta = ppsa + (('  ·  v' + version) if version else '')
            _elide(tk.Label(txt, font=FONTS['code'], bg=BG2,
                            fg=COLORS['fg_3'], anchor='w', width=1),
                   meta).pack(fill='x')
        else:
            tk.Label(txt, text=_('No PPSA detected'), font=FONTS['caption'],
                     bg=BG2, fg=COLORS['fg_4'], anchor='w', width=1
                     ).pack(fill='x')

        # ── Rename preview: current name → proposed name ──
        ren = tk.Frame(row, bg=BG2)
        ren.grid(row=0, column=2, sticky='ew', padx=pad)
        ren.grid_columnconfigure(1, weight=1)
        IconLabel(ren, 'folder', 12, COLORS['fg_5'], BG2).grid(
            row=0, column=0, sticky='w', padx=(0, px(8)))
        old_l = tk.Label(ren, font=FONTS['code'], bg=BG2, fg=COLORS['fg_4'],
                         anchor='w', width=1)
        old_l.grid(row=0, column=1, sticky='ew')
        _elide(old_l, orig_name, side='left')
        Tooltip(old_l, full)

        is_noop = bool(proposed) and (orig_name == proposed)
        IconLabel(ren, 'arrow_right', 12,
                  COLORS['fg_3'] if proposed and not is_noop else COLORS['fg_5'],
                  BG2).grid(row=1, column=0, sticky='w', padx=(0, px(8)),
                            pady=(px(2), 0))
        if not proposed:
            new_text, new_fg, new_font = (_('(no PPSA found)'), COLORS['fg_5'],
                                          FONTS['caption'])
        elif is_noop:
            new_text, new_fg, new_font = (_('(unchanged)'), COLORS['fg_4'],
                                          FONTS['caption'])
        else:
            new_text, new_fg, new_font = proposed, COLORS['fg_0'], FONTS['code']
        new_l = tk.Label(ren, font=new_font, bg=BG2, fg=new_fg, anchor='w',
                         width=1)
        new_l.grid(row=1, column=1, sticky='ew', pady=(px(2), 0))
        _elide(new_l, new_text)
        if proposed and not is_noop:
            Tooltip(new_l, proposed)

        # ── Size ──
        tk.Label(row, textvariable=size_var, font=FONTS['code'], bg=BG2,
                 fg=COLORS['fg_1'], anchor='e', width=1
                 ).grid(row=0, column=3, sticky='ew', padx=pad)

        # ── Status: confidence badge + live row status ──
        st = tk.Frame(row, bg=BG2)
        st.grid(row=0, column=4, sticky='w', padx=pad)
        conf_label, tone = {'green':  (_('Ready'), 'success'),
                            'yellow': (_('Needs review'), 'warn'),
                            'red':    (_('Cannot rename'), 'danger')
                            }.get(confidence, (_('Needs review'), 'warn'))
        Badge(st, conf_label, tone, surface=BG2).pack(anchor='w')
        kind = self._classify(
            (full, name_var, chk_var, size_var, status_var), m)
        outcome = {'matched': _('Will rename'), 'unknown': _('No match'),
                   'already': _('Already OK')}[kind]
        sv = tk.Label(st, font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'],
                      anchor='w')
        sv.pack(anchor='w', pady=(px(2), 0))

        # The scan's own statuses repeat the badge; show the outcome
        # instead until the rename worker reports something new.
        regen = (_('Ready'), _('Already named'), _('No PPSA found'))

        def _sync_status(*_a, lbl=sv, var=status_var, fallback=outcome):
            try:
                if lbl.winfo_exists():
                    v = var.get()
                    lbl.config(text=fallback if (not v or v in regen) else v)
            except Exception:
                pass
        _sync_status()
        try:
            tid = status_var.trace_add('write', _sync_status)
            sv.bind('<Destroy>', lambda e, v=status_var, t=tid:
                    _untrace(v, t), add='+')
        except Exception:
            pass

    def _async_load_cover(self, full, label_widget):
        """Find icon0.png for the dump and load it as a small row thumb on
        a worker thread. Posts the resulting PhotoImage back to the Tk
        main thread for assignment to the label widget. Cached on
        self._cover_cache for the lifetime of the tab so subsequent
        list refreshes don't re-decode the same image."""
        if full in self._covers_in_flight:
            return
        self._covers_in_flight.add(full)

        import threading
        th = px(38)

        def _worker():
            img = None
            try:
                # Use the same discovery + decode pipeline the
                # Library and exFAT tabs use.
                cover_path = self.app._load_cover_art(full)
                if cover_path:
                    img = _load_cover_image(cover_path, target=th)
            except Exception:
                img = None

            def _apply():
                self._covers_in_flight.discard(full)
                if img is None:
                    return
                try:
                    from PIL import ImageTk
                    photo = ImageTk.PhotoImage(img)
                except Exception:
                    return
                # Cache so subsequent refreshes are instant
                self._cover_cache[full] = photo
                # The label may have been destroyed if the list refreshed
                # in the meantime — guard the config call.
                try:
                    if label_widget.winfo_exists():
                        label_widget.config(image=photo)
                        label_widget.image = photo
                except Exception:
                    pass

            try:
                self.app.after(0, _apply)
            except Exception:
                pass

        threading.Thread(target=_worker, daemon=True).start()

    def _refresh_count(self):
        try:
            total    = len(self.app._dumps_items)
            selected = sum(1 for _f, _n, c, _s, _st
                           in self.app._dumps_items if c.get())
            if total == 0:
                self._count_lbl.config(text='')
            else:
                self._count_lbl.config(
                    text='%d ' % selected + _('of') +
                         ' %d ' % total + _('selected'))
            btn = getattr(self, '_rename_btn', None)
            if btn is not None and btn.winfo_exists():
                if selected > 0:
                    btn.config(text=_('Rename %d folder(s)') % selected)
                else:
                    btn.config(text=_('Rename selected'))
        except Exception:
            pass

    # ─────────────────────────────────────────────────────────────────
    # Stubs — kept so legacy callbacks in exfat_builder.py don't crash
    # ─────────────────────────────────────────────────────────────────
    def _refresh_kpi(self):
        """Feed the four inline metrics from the scan model, then refresh
        the selection count (the legacy hook)."""
        try:
            items = list(self.app._dumps_items)
            model = list(self.app._dumps_model)
            counts = {'matched': 0, 'unknown': 0, 'already': 0}
            for i, item in enumerate(items):
                m = model[i] if i < len(model) else {}
                counts[self._classify(item, m)] += 1
            if getattr(self, '_kpi_found', None) is not None \
                    and self._kpi_found.winfo_exists():
                self._kpi_found.config(text=str(len(items)))
                self._kpi_matched.config(text=str(counts['matched']))
                self._kpi_unknown.config(text=str(counts['unknown']))
                self._kpi_already.config(text=str(counts['already']))
        except Exception:
            pass
        self._refresh_count()

    def _show_detail(self, idx):
        # Old inspector showed a per-dump detail pane. Now the row IS
        # the detail (old → new is right there). Re-render the list so
        # the row text reflects the new proposed name.
        self._refresh_list()


def _untrace(var, tid):
    try:
        var.trace_remove('write', tid)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Helpers — design-system buttons (kept for any external caller)
# ─────────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
    return Button(parent, text=text, command=command)


def _accent_btn(parent, text, command, big=False):
    return Button(parent, text=text, command=command, variant='primary')


# ─────────────────────────────────────────────────────────────────────────
# Legacy entry point — exfat_builder.py calls this via the shim in
# ui/tab_dump_rename.py. Kept identical to the v3 signature.
# ─────────────────────────────────────────────────────────────────────────
def build_dump_rename_tab(parent, app):
    DumpRenameTab(parent, app)
