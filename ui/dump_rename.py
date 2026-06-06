"""
ui/dump_rename.py — Simplified Dump Rename tab.

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
from tkinter import ttk, filedialog

from tkinter_theme import COLORS, FONTS

from exfat_builder import _, _load_cover_image


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
            app._dumps_scanning = False
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
        # Page head
        head = tk.Frame(parent, bg=COLORS['bg_1'])
        head.pack(fill='x', padx=24, pady=(14, 6))
        tk.Label(head, text='\u270f  ' + _('Dump Rename'),
                 font=(FONTS['h2'][0], 14, 'bold'),
                 bg=COLORS['bg_1'], fg=COLORS['fg_0']
                 ).pack(side='left')
        tk.Label(head,
                 text='\u2014  ' + _('Rename PS5 dump folders from their '
                                     'PPSA metadata'),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_4']
                 ).pack(side='left', padx=(12, 0), pady=(2, 0))

        # ── Stats strip (v3.6.0 pass) ──
        self._build_kpi_strip(parent)

        # ── Source folder card ──
        self._build_source_card(parent)

        # ── Naming pattern card ──
        self._build_pattern_card(parent)

        # ── Found dumps list ──
        self._build_list_card(parent)

        # ── Status + progressbar + Rename button (footer) ──
        self._build_footer(parent)

    def _build_kpi_strip(self, parent):
        """Four stat cards: Dumps Found / Matched / Unknown / Already
        Named. Fed by _refresh_kpi from the scan model — presentation
        only, derived from data the scan already produces."""
        strip = tk.Frame(parent, bg=COLORS['bg_1'])
        strip.pack(fill='x', padx=24, pady=(6, 4))
        for i in range(4):
            strip.grid_columnconfigure(i, weight=1, uniform='drkpi')

        def _cell(col, caption, sub, value_fg):
            cell = tk.Frame(strip, bg=COLORS['bg_2'],
                            highlightbackground=COLORS['border_2'],
                            highlightthickness=1)
            cell.grid(row=0, column=col, sticky='ew',
                      padx=(0 if col == 0 else 10, 0))
            tk.Label(cell, text=caption,
                     font=(FONTS['mono_sm'][0], 8, 'bold'),
                     bg=COLORS['bg_2'], fg=COLORS['fg_5']
                     ).pack(anchor='w', padx=14, pady=(10, 0))
            v = tk.Label(cell, text='0',
                         font=(FONTS['h2'][0], 18, 'bold'),
                         bg=COLORS['bg_2'], fg=value_fg)
            v.pack(anchor='w', padx=14, pady=(2, 0))
            tk.Label(cell, text=sub,
                     font=(FONTS['mono_sm'][0], 8),
                     bg=COLORS['bg_2'], fg=COLORS['fg_5']
                     ).pack(anchor='w', padx=14, pady=(0, 10))
            return v

        self._kpi_found   = _cell(0, _('Dumps Found').upper(),
                                  _('folders scanned'), COLORS['fg_0'])
        self._kpi_matched = _cell(1, _('Matched').upper(),
                                  _('will be renamed'),
                                  COLORS['success_hi'])
        self._kpi_unknown = _cell(2, _('Unknown').upper(),
                                  _('no match found'), COLORS['warn_hi'])
        self._kpi_already = _cell(3, _('Already Named').upper(),
                                  _('already clean'), COLORS['fg_4'])

    @staticmethod
    def _classify(item, m):
        """Return 'matched' | 'unknown' | 'already' for one dump."""
        try:
            full, name_var, _c, _s, _st = item
        except (ValueError, TypeError):
            return 'unknown'
        ppsa = (m or {}).get('ppsa', '') or ''
        if not ppsa:
            return 'unknown'
        orig = (m or {}).get('orig_name') or os.path.basename(full)
        proposed = name_var.get() or ''
        if proposed and proposed == orig:
            return 'already'
        return 'matched'

    def _build_source_card(self, parent):
        card = tk.Frame(parent, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        card.pack(fill='x', padx=24, pady=(8, 8))

        inner = tk.Frame(card, bg=COLORS['bg_2'])
        inner.pack(fill='x', padx=14, pady=10)

        tk.Label(inner, text=_('Source folder'),
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_1']
                 ).pack(anchor='w')
        tk.Label(inner,
                 text=_('Folder containing your PS5 dumps. Each dump is '
                        'a sub-folder. The original folders will be '
                        'renamed in place.'),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                 wraplength=720, justify='left'
                 ).pack(anchor='w', pady=(2, 8))

        row = tk.Frame(inner, bg=COLORS['bg_2'])
        row.pack(fill='x')

        tk.Entry(row, textvariable=self.app._dumps_src_var,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_3'], fg=COLORS['fg_0'],
                 insertbackground=COLORS['fg_0'],
                 relief='flat', bd=0,
                 highlightbackground=COLORS['border_2'],
                 highlightthickness=1
                 ).pack(side='left', fill='x', expand=True, ipady=4)

        _ghost_btn(row, '\U0001f4c1  ' + _('Browse'),
                   command=self._browse_src
                   ).pack(side='left', padx=(8, 0))

        _accent_btn(row, '\U0001f50d  ' + _('Scan'),
                    command=self._do_scan
                    ).pack(side='left', padx=(6, 0))

    def _build_pattern_card(self, parent):
        card = tk.Frame(parent, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        card.pack(fill='x', padx=24, pady=(0, 8))

        inner = tk.Frame(card, bg=COLORS['bg_2'])
        inner.pack(fill='x', padx=14, pady=10)

        tk.Label(inner, text=_('Naming pattern'),
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_1']
                 ).pack(anchor='w', pady=(0, 6))

        for key, label, example in _PATTERNS:
            row = tk.Frame(inner, bg=COLORS['bg_2'])
            row.pack(fill='x', pady=2)
            rb = tk.Radiobutton(row, text=' ' + _(label),
                                value=key,
                                variable=self.app._dumps_naming_preset_var,
                                font=FONTS['body'],
                                bg=COLORS['bg_2'],
                                fg=COLORS['fg_1'],
                                activebackground=COLORS['bg_2'],
                                activeforeground=COLORS['fg_0'],
                                selectcolor=COLORS['bg_3'],
                                relief='flat', bd=0,
                                highlightthickness=0,
                                anchor='w')
            rb.pack(side='left')
            tk.Label(row, text='  e.g.  ' + example,
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_4']
                     ).pack(side='left', padx=(12, 0))

    def _build_list_card(self, parent):
        card = tk.Frame(parent, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        card.pack(fill='both', expand=True, padx=24, pady=(0, 8))

        inner = tk.Frame(card, bg=COLORS['bg_2'])
        inner.pack(fill='both', expand=True, padx=14, pady=10)

        head = tk.Frame(inner, bg=COLORS['bg_2'])
        head.pack(fill='x')

        tk.Label(head, text=_('Found dumps'),
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_1']
                 ).pack(side='left')

        self._count_lbl = tk.Label(head, text='',
                                    font=FONTS['mono_sm'],
                                    bg=COLORS['bg_2'], fg=COLORS['fg_4'])
        self._count_lbl.pack(side='left', padx=(8, 0))

        _ghost_btn(head, _('Select all'),
                   command=self._select_all
                   ).pack(side='right')
        _ghost_btn(head, _('Select none'),
                   command=self._select_none
                   ).pack(side='right', padx=(0, 6))

        # Scrollable list of rows (Canvas + inner Frame)
        list_wrap = tk.Frame(inner, bg=COLORS['bg_3'],
                             highlightbackground=COLORS['border_2'],
                             highlightthickness=1)
        list_wrap.pack(fill='both', expand=True, pady=(8, 0))

        self._canvas = tk.Canvas(list_wrap, bg=COLORS['bg_3'],
                                  highlightthickness=0, bd=0)
        sb = ttk.Scrollbar(list_wrap, orient='vertical',
                           command=self._canvas.yview)
        self._canvas.configure(yscrollcommand=sb.set)

        sb.pack(side='right', fill='y')
        self._canvas.pack(side='left', fill='both', expand=True)

        self._list_inner = tk.Frame(self._canvas, bg=COLORS['bg_3'])
        self._list_window = self._canvas.create_window(
            (0, 0), window=self._list_inner, anchor='nw')

        def _on_inner_config(event):
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        self._list_inner.bind('<Configure>', _on_inner_config)

        def _on_canvas_config(event):
            self._canvas.itemconfig(self._list_window, width=event.width)
        self._canvas.bind('<Configure>', _on_canvas_config)

        # Mouse-wheel scroll over the list
        def _on_wheel(event):
            self._canvas.yview_scroll(-int(event.delta / 60), 'units')
        self._canvas.bind('<Enter>',
                          lambda e: self._canvas.bind_all(
                              '<MouseWheel>', _on_wheel))
        self._canvas.bind('<Leave>',
                          lambda e: self._canvas.unbind_all('<MouseWheel>'))

        # Empty-state placeholder
        self._empty_label = tk.Label(
            self._list_inner,
            text=_('No dumps scanned yet. Pick a source folder and click '
                   'Scan.'),
            font=FONTS['mono_sm'],
            bg=COLORS['bg_3'], fg=COLORS['fg_4'],
            padx=14, pady=24, justify='left')
        self._empty_label.pack(anchor='w')

    def _build_footer(self, parent):
        foot = tk.Frame(parent, bg=COLORS['bg_1'])
        foot.pack(fill='x', padx=24, pady=(0, 14))

        # Status line
        status_row = tk.Frame(foot, bg=COLORS['bg_1'])
        status_row.pack(fill='x', pady=(0, 6))

        tk.Label(status_row, textvariable=self.app._dumps_status_var,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_3'],
                 anchor='w', justify='left'
                 ).pack(side='left', fill='x', expand=True)

        # Progressbar — used by the rename worker to report bytes copied
        self.app._dumps_pbar = ttk.Progressbar(
            foot, mode='indeterminate', length=200)
        self.app._dumps_pbar.pack(fill='x', pady=(0, 8))

        # Big rename button — label tracks the live selection count
        self._rename_btn = _accent_btn(
            foot, '\u270f  ' + _('Rename selected'),
            command=self._do_rename, big=True)
        self._rename_btn.pack(fill='x')

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
            tk.Label(self._list_inner,
                     text=_('No dumps scanned yet. Pick a source folder '
                            'and click Scan.'),
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_3'], fg=COLORS['fg_4'],
                     padx=14, pady=24, justify='left'
                     ).pack(anchor='w')
            self._refresh_kpi()
            return

        for i, item in enumerate(items):
            try:
                full, name_var, chk_var, size_var, status_var = item
            except (ValueError, TypeError):
                continue
            m = model[i] if i < len(model) else {}
            self._row(i, full, name_var, chk_var, size_var, status_var, m)

        self._refresh_kpi()

    def _row(self, idx, full, name_var, chk_var, size_var, status_var, m):
        orig_name  = m.get('orig_name') or os.path.basename(full)
        proposed   = name_var.get() or _('(no PPSA found)')
        confidence = m.get('confidence', 'yellow')

        # Pull components for the rich title display
        ppsa     = m.get('ppsa', '') or ''
        title    = m.get('title', '') or ''
        version  = m.get('version', '') or ''

        # Confidence pip colour (matches the legacy mapping)
        pip_color = {'green':  COLORS.get('ok',   '#4caf50'),
                     'yellow': COLORS.get('warn', '#f0a93a'),
                     'red':    COLORS.get('err',  '#e0584b')
                     }.get(confidence, COLORS.get('warn', '#f0a93a'))

        # ── Row container ──
        row = tk.Frame(self._list_inner, bg=COLORS['bg_3'])
        row.pack(fill='x', padx=8, pady=4)

        # Inner panel — gives each row its own card-like surface so
        # padding around the cover/text doesn't bleed into the next row.
        card = tk.Frame(row, bg=COLORS['bg_2'],
                        highlightbackground=COLORS['border_2'],
                        highlightthickness=1)
        card.pack(fill='x')

        inner = tk.Frame(card, bg=COLORS['bg_2'])
        inner.pack(fill='x', padx=10, pady=10)

        # ── Checkbox ──
        cb = tk.Checkbutton(inner, variable=chk_var,
                            bg=COLORS['bg_2'],
                            activebackground=COLORS['bg_2'],
                            selectcolor=COLORS['bg_3'],
                            relief='flat', bd=0,
                            highlightthickness=0,
                            command=self._refresh_count)
        cb.pack(side='left', padx=(2, 10), anchor='n', pady=(36, 0))

        # ── Cover image (96 × 128) ──
        # Reserve the slot even if no cover loads, so all rows align.
        cover_lbl = tk.Label(inner,
                              bg=COLORS['bg_3'],
                              fg=COLORS['fg_4'],
                              text='\U0001f3ae',  # placeholder gamepad
                              font=(FONTS['body'][0], 22),
                              width=8,            # in chars; ~96 px
                              height=6,           # in lines; ~128 px
                              relief='flat', bd=0,
                              highlightbackground=COLORS['border_3'],
                              highlightthickness=1)
        cover_lbl.pack(side='left', padx=(0, 14))

        # If we already cached a PhotoImage for this folder, use it
        # immediately. Otherwise kick off a background load.
        cached = self._cover_cache.get(full)
        if cached is not None:
            try:
                cover_lbl.config(image=cached, text='', width=96, height=128)
                cover_lbl.image = cached
            except Exception:
                pass
        else:
            self._async_load_cover(full, cover_lbl)

        # ── Centre column: title + ppsa/version + old name ──
        text_wrap = tk.Frame(inner, bg=COLORS['bg_2'])
        text_wrap.pack(side='left', fill='x', expand=True, anchor='n')

        # Title — biggest text, top of the card
        title_text = title if title else _('(unknown title)')
        tk.Label(text_wrap, text=title_text,
                 font=(FONTS['body'][0], 12, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_0'],
                 anchor='w', justify='left'
                 ).pack(anchor='w', fill='x')

        # PPSA + version pill row
        meta_row = tk.Frame(text_wrap, bg=COLORS['bg_2'])
        meta_row.pack(anchor='w', fill='x', pady=(4, 0))

        if ppsa:
            tk.Label(meta_row, text=' ' + ppsa + ' ',
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_3'], fg=COLORS['fg_1'],
                     padx=6, pady=1
                     ).pack(side='left')
        else:
            tk.Label(meta_row, text=_('No PPSA detected'),
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_4']
                     ).pack(side='left')

        if version:
            tk.Label(meta_row, text='  v' + version,
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_3']
                     ).pack(side='left', padx=(8, 0))

        # Confidence pip + label
        conf_label = {'green':  _('Ready'),
                      'yellow': _('Needs review'),
                      'red':    _('Cannot rename')
                      }.get(confidence, _('Needs review'))
        tk.Label(meta_row, text='\u25cf  ' + conf_label,
                 fg=pip_color, bg=COLORS['bg_2'],
                 font=FONTS['mono_sm']
                 ).pack(side='left', padx=(12, 0))

        # ── Old → New names (always shown, per user preference) ──
        names_wrap = tk.Frame(text_wrap, bg=COLORS['bg_2'])
        names_wrap.pack(anchor='w', fill='x', pady=(10, 0))

        # Old name row
        old_row = tk.Frame(names_wrap, bg=COLORS['bg_2'])
        old_row.pack(anchor='w', fill='x')
        tk.Label(old_row, text=_('Old:'),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                 width=5, anchor='w'
                 ).pack(side='left')
        tk.Label(old_row, text=orig_name,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                 anchor='w', justify='left'
                 ).pack(side='left', fill='x', expand=True)

        # New name row
        new_row = tk.Frame(names_wrap, bg=COLORS['bg_2'])
        new_row.pack(anchor='w', fill='x', pady=(2, 0))
        tk.Label(new_row, text=_('New:'),
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                 width=5, anchor='w'
                 ).pack(side='left')

        # Highlight when the rename is a no-op (already matches)
        is_noop = (orig_name == proposed)
        new_color = COLORS.get('fg_4', '#888') if is_noop \
            else COLORS.get('accent', '#5b8cff')
        tk.Label(new_row, text=proposed,
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=new_color,
                 anchor='w', justify='left'
                 ).pack(side='left', fill='x', expand=True)

        if is_noop:
            tk.Label(new_row, text='  ' + _('(unchanged)'),
                     font=FONTS['mono_sm'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_4']
                     ).pack(side='left')

        # ── Right column: outcome chip + size + status ──
        right = tk.Frame(inner, bg=COLORS['bg_2'])
        right.pack(side='right', padx=(8, 4), anchor='n', pady=(2, 0))

        kind = self._classify(
            (full, name_var, chk_var, size_var, status_var), m)
        chip_map = {
            'matched': ('\u2713 ' + _('Will rename'),
                        COLORS['success_bg'], COLORS['success_hi']),
            'unknown': ('\u26a0 ' + _('No match'),
                        COLORS['warn_bg'], COLORS['warn_hi']),
            'already': ('\u2713 ' + _('Already OK'),
                        COLORS['bg_3'], COLORS['fg_4']),
        }
        c_text, c_bg, c_fg = chip_map[kind]
        tk.Label(right, text=' ' + c_text + ' ',
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=c_bg, fg=c_fg, padx=6, pady=2
                 ).pack(anchor='e', pady=(0, 6))

        tk.Label(right, textvariable=size_var,
                 font=(FONTS['body'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_1']
                 ).pack(anchor='e')
        tk.Label(right, textvariable=status_var,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4']
                 ).pack(anchor='e', pady=(2, 0))

    def _async_load_cover(self, full, label_widget):
        """Find icon0.png for the dump and load it as a 96×128 thumb on
        a worker thread. Posts the resulting PhotoImage back to the Tk
        main thread for assignment to the label widget. Cached on
        self._cover_cache for the lifetime of the tab so subsequent
        list refreshes don't re-decode the same image."""
        if full in self._covers_in_flight:
            return
        self._covers_in_flight.add(full)

        import threading

        def _worker():
            photo = None
            try:
                # Use the same discovery + decode pipeline the
                # Library and exFAT tabs use.
                cover_path = self.app._load_cover_art(full)
                if cover_path:
                    img = _load_cover_image(cover_path, target=128)
                    if img is not None:
                        try:
                            from PIL import Image, ImageTk
                            # Fit to 96x128 portrait box. _load_cover_image
                            # returns a square crop; resize keeping aspect.
                            img.thumbnail((96, 128), Image.LANCZOS)
                            photo = ImageTk.PhotoImage(img)
                        except Exception:
                            photo = None
            except Exception:
                photo = None

            def _apply():
                self._covers_in_flight.discard(full)
                if photo is None:
                    return
                # Cache so subsequent refreshes are instant
                self._cover_cache[full] = photo
                # The label may have been destroyed if the list refreshed
                # in the meantime — guard the config call.
                try:
                    if label_widget.winfo_exists():
                        label_widget.config(image=photo, text='',
                                            width=96, height=128)
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
                    text='\u2014 %d ' % selected + _('of') +
                         ' %d ' % total + _('selected'))
            btn = getattr(self, '_rename_btn', None)
            if btn is not None and btn.winfo_exists():
                if selected > 0:
                    btn.config(text='\u270f  ' + _('Rename %d folder(s)')
                               % selected)
                else:
                    btn.config(text='\u270f  ' + _('Rename selected'))
        except Exception:
            pass

    # ─────────────────────────────────────────────────────────────────
    # Stubs — kept so legacy callbacks in exfat_builder.py don't crash
    # ─────────────────────────────────────────────────────────────────
    def _refresh_kpi(self):
        """v3.6.0 pass: feed the four KPI cards from the scan model,
        then refresh the selection count (the legacy hook)."""
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


# ─────────────────────────────────────────────────────────────────────────
# Helpers — themed buttons matching tab_help.py / tab_advanced.py styling
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


def _accent_btn(parent, text, command, big=False):
    accent = COLORS.get('accent', '#5b8cff')
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0],
                           11 if big else 9,
                           'bold'),
                     bg=accent, fg='#0a0a0a',
                     activebackground=accent,
                     activeforeground='#0a0a0a',
                     relief='flat', bd=0,
                     padx=16,
                     pady=10 if big else 6,
                     cursor='hand2',
                     command=command)


# ─────────────────────────────────────────────────────────────────────────
# Legacy entry point — exfat_builder.py calls this via the shim in
# ui/tab_dump_rename.py. Kept identical to the v3 signature.
# ─────────────────────────────────────────────────────────────────────────
def build_dump_rename_tab(parent, app):
    DumpRenameTab(parent, app)
