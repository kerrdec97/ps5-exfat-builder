"""
ui/tab_exfat.py — exFAT image build tab.

Step 3 (v2.0.5): refactored against preview/exfat-tab-redesign.html.

Layout (two-column grid on the page):

    [page-head: 💾  Build exFAT Image  + subtitle      ]   ← spans full width
    [info banner: Tip about Advanced settings          ]   ← spans full width

    LEFT (1.4fr)                          RIGHT (1fr)
    ┌──── Add to queue ────────────┐      ┌──── Build progress ──────┐
    │ DropZone                     │      │ [01][02][03][04] tiles   │
    │ DetectedGameStrip (hidden)   │      │ [████████████─────] 63%  │
    │ Game folder: [________][Br]  │      │ Meta row                 │
    │ + Add multiple folders...    │      └──────────────────────────┘
    │ Output dir: [_________][Br]  │      ┌──── Build queue ─────────┐
    │ Output filename preview      │      │ Header: count + actions  │
    ├──── Action bar ──────────────┤      │ qlist scrollable         │
    │ [+ Add] [▶ Build] [⏸ Pause]  │      │   1 ● Building 63%        │
    │              [⏏ Force Dis.]   │      │   2  Waiting              │
    └──────────────────────────────┘      └──────────────────────────┘

    [Status row: detected message text]  ← bottom, full width

All callbacks (`app._add_to_queue`, `app._run_queue`, etc.) are unchanged —
this rewrite is UI-only. The 18 attributes the rest of the app expects on
`app` are preserved (see `Compatibility` section at the bottom).

NOT touched: tab strip (still uses the global custom strip), output log
(still global), PS5 status bar (still global). Those will be migrated in
later steps.
"""

import os
import tkinter as tk
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

# Star-import remains for the legacy color constants (BG, ACCENT, ...) that
# show up in callbacks executed against `app`. Step 5 cleanup will remove
# this dependency once every tab is migrated.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.shared.cards import Card, StatusPill, DetectedGameStrip
from ui.shared.forms import LabeledField
from ui.shared.progress import StagedProgressBar
from ui.shared.scroll import attach_scroll


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _make_themed_button(parent, text, command, kind='primary',
                        icon=None, font_size=10, padx=14, pady=7,
                        state='normal'):
    """Build a tk.Button styled like the design-system btn-* classes.

    We use tk.Button (not ttk.Button) because the existing app standardizes
    on tk.Button across every tab and uses raw bg/fg overrides. ttk.Button
    would respect Primary.TButton from the theme, but on Windows-clam its
    visual rendering differs subtly from the tk.Button look in adjacent
    tabs. Using tk.Button + COLORS gives us pixel parity with the mock
    and consistency across the app today.

    `kind` maps to the design tokens directly:
        'primary'      — accent (purple) fill, white text. Main CTA.
        'success'      — muted teal fill, white text. Secondary
                         forward action (Build All).
        'success_done' — bright green. ONLY for "done/verified" state.
        'warn'         — warn fill, dark text.
        'danger'       — danger fill, white text.
        'ghost'        — transparent fill, fg_3 text, border outline.
    """
    schemes = {
        'primary':      (COLORS['accent'],  COLORS['fg_0'],
                         COLORS['accent_hi'], COLORS['fg_0']),
        # 'success' = muted teal — was bright mint, recoloured app-wide
        # to feel cohesive with the brand purple instead of stoplighty.
        'success':      (COLORS['teal'],    COLORS['fg_0'],
                         COLORS['teal_hi'], COLORS['fg_0']),
        'success_done': (COLORS['success'], COLORS['fg_0'],
                         COLORS['success_hi'], COLORS['fg_0']),
        'warn':         (COLORS['warn'],    '#1a0e00',
                         COLORS['warn_hi'], '#1a0e00'),
        'danger':       (COLORS['danger'],  COLORS['fg_0'],
                         COLORS['danger_hi'], COLORS['fg_0']),
        'ghost':        (COLORS['bg_2'],    COLORS['fg_3'],
                         COLORS['bg_4'],    COLORS['fg_1']),
    }
    bg, fg, abg, afg = schemes.get(kind, schemes['primary'])
    label = (icon + '  ' + text) if icon else text
    btn = tk.Button(parent, text=label,
                    font=(FONTS['button'][0], font_size, 'bold'),
                    bg=bg, fg=fg,
                    activebackground=abg, activeforeground=afg,
                    relief='flat', bd=0,
                    padx=padx, pady=pady,
                    cursor='hand2', state=state,
                    command=command)
    if kind == 'ghost':
        btn.configure(highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
    return btn


def _info_banner(parent, text, on_click=None):
    """Slim accent-tinted info bar. Mirrors `.tip` in the mock.

    A blue-tinted strip with an `i` glyph on the left, descriptive text,
    and an optional click handler that fires when the bar is clicked.
    """
    bg = COLORS['accent_08']
    border = COLORS['accent_15']
    bar = tk.Frame(parent, bg=bg,
                   highlightbackground=border, highlightthickness=1)
    inner = tk.Frame(bar, bg=bg)
    inner.pack(fill='x', padx=14, pady=10)

    # Round-ish "i" glyph in a small accent disc on the left
    ico = tk.Label(inner, text='i',
                   bg=COLORS['accent'], fg=COLORS['fg_0'],
                   font=(FONTS['body'][0], 9, 'bold'),
                   width=2, padx=2, pady=0)
    ico.pack(side='left', padx=(0, 10))

    msg = tk.Label(inner, text=text,
                   font=FONTS['body'],
                   bg=bg, fg=COLORS['accent_hi'],
                   anchor='w', justify='left', wraplength=900)
    msg.pack(side='left', fill='x', expand=True)

    if on_click:
        for w in [bar, inner, ico, msg]:
            w.bind('<Button-1>', lambda e: on_click(), add='+')
            try:
                w.config(cursor='hand2')
            except Exception:
                pass

    return bar


def _page_head(parent, badge_emoji, title_text, subtitle_text):
    """Top-of-page header with a gradient-ish badge tile + title + subtitle."""
    bg = COLORS['bg_1']
    head = tk.Frame(parent, bg=bg)

    # Badge tile (accent gradient approximation — solid accent with darker border)
    badge = tk.Label(head, text=badge_emoji,
                     bg=COLORS['accent'], fg=COLORS['fg_0'],
                     font=(FONTS['body'][0], 18, 'bold'),
                     width=3, height=2, padx=4, pady=0,
                     highlightbackground=COLORS['accent_pressed'],
                     highlightthickness=1)
    badge.pack(side='left', padx=(0, 14))

    text_col = tk.Frame(head, bg=bg)
    text_col.pack(side='left', fill='x', expand=True)
    tk.Label(text_col, text=title_text,
             font=(FONTS['h2'][0], 18, 'bold'),
             bg=bg, fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(text_col, text=subtitle_text,
             font=FONTS['meta'],
             bg=bg, fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(2, 0))

    return head


# ─────────────────────────────────────────────────────────────────────────
# Main entry — build the exFAT tab into `parent`.
# ─────────────────────────────────────────────────────────────────────────
def build_exfat_tab(parent, app):
    """Build the redesigned exFAT tab body into `parent`.

    Layout: a top-level grid with two columns. The page-head and tip banner
    span both. The input card lives in column 0, the progress + queue cards
    stack vertically in column 1. A status row pins to the bottom.
    """
    # The parent itself uses BG (bg_1). Tk's pack manager doesn't grid-flex
    # the way CSS does — but for our window sizes a grid does the right thing.
    parent.configure(bg=COLORS['bg_1'])

    # ── Outer wrapper that hosts the grid + bottom status row ──
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    # Bottom status row — packed FIRST with side='bottom' so it pins
    # regardless of how tall the grid above gets.
    status_row = tk.Frame(body, bg=COLORS['bg_1'])
    status_row.pack(side='bottom', fill='x', padx=24, pady=(4, 8))
    app.status_lbl = tk.Label(status_row, textvariable=app.status_text,
                              font=FONTS['body'],
                              bg=COLORS['bg_1'], fg=COLORS['success_hi'],
                              anchor='w')
    app.status_lbl.pack(side='left')
    tk.Frame(body, bg=COLORS['border_2'], height=1).pack(side='bottom',
                                                          fill='x')

    # ── The content grid (page-head + banner + 2-column row) ──
    grid = tk.Frame(body, bg=COLORS['bg_1'])
    grid.pack(fill='both', expand=True, padx=24, pady=(14, 6))
    grid.grid_columnconfigure(0, weight=14, minsize=420)   # left  ~58%
    grid.grid_columnconfigure(1, weight=10, minsize=320)   # right ~42%
    grid.grid_rowconfigure(2, weight=1)                    # third row stretches

    # Row 0: page-head spanning full width
    head = _page_head(grid, '\U0001f4be', 'Build exFAT Image',
                      'Pick a game folder, set the output, and queue the build.')
    head.grid(row=0, column=0, columnspan=2, sticky='ew', pady=(0, 10))

    # Row 1: tip banner spanning full width
    banner = _info_banner(grid,
        'Tip: Recommended Size, Cluster, Sector & Thread settings live in '
        'the Advanced tab. Click here to jump there.',
        on_click=lambda: app._switch_tab('advanced'))
    banner.grid(row=1, column=0, columnspan=2, sticky='ew', pady=(0, 14))

    # Row 2: left column (input card), right column (progress + queue stack)
    _build_left_column(grid, app)
    _build_right_column(grid, app)


def _build_left_column(grid, app):
    """The form column with drop zone, fields, and the action footer.

    Step 48 (v2.5.8): brought back a minimal auto-scroll wrapper inside
    the card body. The action footer is still pinned to the bottom of
    the card (handled by the Card class), but when the OUTPUT LOG
    drawer is open the form fields above it can exceed the available
    height and get clipped. The wrapper auto-hides the scrollbar when
    content fits naturally, and only shows it when squeezed. The
    drop zone / detected strip / fields all pack into the wrapper.

    Step 47 (v2.5.8): dropped the 'Add to queue' card header (the +
    icon tile + title + 'Drop a game folder, or browse below'
    subtitle). It duplicated what the drop zone immediately below
    already says.

    Step 46 (v2.5.8): drop zone is rendered inline at ~70px tall
    instead of ~200px. Tighter vertical paddings throughout.
    Order: Drop zone → Detected strip → Game folder → Output dir
    → Output filename → action footer (pinned bottom).
    """
    card = Card(grid, with_actions=True)
    card.grid(row=2, column=0, sticky='nsew', padx=(0, 9))

    # ── Auto-scroll wrapper ──
    # A Canvas + an inner Frame. Scrollbar is created but kept
    # unpacked when content fits; we add/remove it dynamically from
    # the <Configure> handler. The mouse wheel still scrolls even
    # when the scrollbar isn't visible — useful when the wrapper is
    # only barely overflowing.
    _cv = tk.Canvas(card.body, bg=COLORS['bg_2'], highlightthickness=0)
    _sb = tk.Scrollbar(card.body, command=_cv.yview,
                       bg=COLORS['bg_3'], troughcolor=COLORS['bg_2'])
    _cv.configure(yscrollcommand=_sb.set)
    _cv.pack(side='left', fill='both', expand=True)
    _scroll_inner = tk.Frame(_cv, bg=COLORS['bg_2'])
    _win = _cv.create_window((0, 0), window=_scroll_inner, anchor='nw')

    def _exf_update_scroll(_e=None):
        try:
            _cv.update_idletasks()
            bbox = _cv.bbox('all')
            if not bbox:
                return
            canvas_h = _cv.winfo_height()
            content_h = bbox[3] - bbox[1]
            if content_h <= canvas_h + 1:
                # Content fits — hide scrollbar entirely and lock
                # the scroll region to canvas height so any small
                # overflow doesn't induce a phantom scrollbar.
                _cv.configure(scrollregion=(0, 0, bbox[2], canvas_h))
                if _sb.winfo_ismapped():
                    _sb.pack_forget()
            else:
                _cv.configure(scrollregion=bbox)
                if not _sb.winfo_ismapped():
                    _sb.pack(side='right', fill='y', before=_cv)
        except Exception:
            pass

    _scroll_inner.bind('<Configure>', _exf_update_scroll)
    _cv.bind('<Configure>',
        lambda e: (_cv.itemconfig(_win, width=e.width),
                   _exf_update_scroll()))
    # Mouse wheel works inside the form regardless of scrollbar state.
    attach_scroll(_cv)

    body = _scroll_inner  # pack everything into the scrollable inner

    # ── Compact drop zone ──
    # 1-row layout: glyph + "Drop a folder here or click to browse" +
    # tiny hint. ~60-70px tall total. The bigger 200px dropzone was
    # showing the same affordance as the Browse button below, just
    # taking 3× the space.
    dropzone = _CompactDropZone(body,
                                on_drop=lambda p: _on_dropzone_drop(app, p),
                                on_click=app._browse_game)
    dropzone.pack(fill='x')

    # ── Detected game info strip slot ──
    detected_slot = tk.Frame(body, bg=COLORS['bg_2'])
    detected_slot.pack(fill='x')
    app._detected_strip = DetectedGameStrip(detected_slot)

    # Backwards-compat aliases for the legacy detection callbacks.
    app._info_frame     = app._detected_strip
    app._cover_label    = app._detected_strip.cover_label
    app._info_title_var = app._detected_strip.title_var
    app._info_id_var    = app._detected_strip.id_var
    app._info_ver_var   = app._detected_strip.ver_var
    app._info_size_var  = app._detected_strip.size_var
    app._cover_image    = None  # the existing code maintains the image ref

    # ── Game folder field ──
    fld_game = LabeledField(body,
                            label='Game root folder',
                            var=app.game_folder,
                            on_browse=app._browse_game,
                            required=True,
                            hint='must contain eboot.bin')
    fld_game.pack(fill='x', pady=(10, 0))

    # ── "+ Add multiple folders at once" link button ──
    multi_row = tk.Frame(body, bg=COLORS['bg_2'])
    multi_row.pack(fill='x', pady=(6, 0))
    multi_btn = tk.Button(multi_row,
                          text='\U0001f4c1  Add multiple folders at once',
                          font=(FONTS['body'][0], 10),
                          bg=COLORS['bg_2'], fg=COLORS['accent'],
                          activebackground=COLORS['bg_2'],
                          activeforeground=COLORS['accent_hi'],
                          relief='flat', bd=0,
                          cursor='hand2',
                          command=app._browse_multi_games)
    multi_btn.pack(side='left')

    # ── Output directory field ──
    fld_out = LabeledField(body,
                           label='Output directory',
                           var=app.output_dir,
                           on_browse=app._browse_output)
    fld_out.pack(fill='x', pady=(10, 0))

    # ── Output filename row (read-only preview + auto/no-sfo labels) ──
    fn_block = tk.Frame(body, bg=COLORS['bg_2'])
    fn_block.pack(fill='x', pady=(10, 0))

    fn_label_row = tk.Frame(fn_block, bg=COLORS['bg_2'])
    fn_label_row.pack(fill='x')
    tk.Label(fn_label_row, text='Output filename',
             font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w'
             ).pack(side='left')
    app._auto_label = tk.Label(fn_label_row,
                               text='  \u2022 auto-detected',
                               font=FONTS['meta'],
                               bg=COLORS['bg_2'], fg=COLORS['success_hi'],
                               anchor='w')
    app._auto_label.pack(side='left')
    app._auto_label.pack_forget()
    app._no_sfo_label = tk.Label(fn_label_row,
                                 text='  \u2022 no metadata found '
                                      '\u2014 will use folder name',
                                 font=FONTS['meta'],
                                 bg=COLORS['bg_2'], fg=COLORS['warn_hi'],
                                 anchor='w')
    app._no_sfo_label.pack(side='left')
    app._no_sfo_label.pack_forget()

    fn_input = tk.Frame(fn_block, bg=COLORS['bg_3'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
    fn_input.pack(fill='x', pady=(6, 0))

    app._filename_preview = tk.Label(fn_input,
                                     textvariable=app.output_name,
                                     font=FONTS['mono_sm'],
                                     bg=COLORS['bg_3'],
                                     fg=COLORS['fg_1'],
                                     anchor='w', padx=10, pady=8)
    app._filename_preview.pack(fill='x')

    # ── Action footer — primary actions left, ghost actions right ──
    _make_themed_button(card.actions, 'Add to Queue',
                        command=app._add_to_queue,
                        kind='primary', icon='\uff0b',
                        font_size=10, padx=16, pady=8
                        ).pack(side='left', padx=(0, 6))

    app.build_btn = _make_themed_button(
        card.actions, 'Build All', command=app._run_queue,
        kind='success', icon='\u25b6',
        font_size=10, padx=16, pady=8)
    app.build_btn.pack(side='left', padx=(0, 6))

    app._pause_btn = _make_themed_button(
        card.actions, 'Pause', command=app._toggle_pause_queue,
        kind='warn', icon='\u23f8',
        font_size=10, padx=14, pady=8, state='disabled')
    app._pause_btn.pack(side='left', padx=(0, 6))

    # Spacer pushes the ghost actions to the right
    tk.Frame(card.actions, bg=COLORS['bg_3']).pack(side='left',
                                                    fill='x', expand=True)

    _make_themed_button(card.actions, 'Force Dismount',
                        command=app._force_dismount_all,
                        kind='ghost', icon='\u23cf',
                        font_size=9, padx=12, pady=8
                        ).pack(side='left')


class _CompactDropZone(tk.Frame):
    """One-row drop target. Much shorter than DropZone — designed for
    use ABOVE a Browse-field row, where the two together provide both
    drag-drop and click-to-browse without taking up half the card."""

    def __init__(self, parent, on_drop=None, on_click=None):
        bg = COLORS['bg_2']
        super().__init__(parent, bg=bg,
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1)
        self._on_drop = on_drop
        self._on_click = on_click
        self._idle_bg = bg
        self._hover_bg = COLORS['accent_08']
        self._idle_border = COLORS['border_3']
        self._hover_border = COLORS['accent']

        inner = tk.Frame(self, bg=bg)
        inner.pack(fill='x', padx=14, pady=10)

        # Glyph on the left
        self._glyph_lbl = tk.Label(inner, text='\u2913',
                                   font=(FONTS['body'][0], 14),
                                   bg=bg, fg=COLORS['fg_5'])
        self._glyph_lbl.pack(side='left', padx=(0, 10))

        # Main text + hint stacked vertically, single column
        txt_col = tk.Frame(inner, bg=bg)
        txt_col.pack(side='left', fill='x', expand=True)

        self._main_lbl = tk.Label(txt_col,
                                  text='Drop a game folder here, '
                                       'or click to browse',
                                  font=(FONTS['body'][0], 10, 'bold'),
                                  bg=bg, fg=COLORS['fg_2'], anchor='w')
        self._main_lbl.pack(fill='x')

        self._hint_lbl = tk.Label(txt_col,
                                  text='must contain eboot.bin',
                                  font=FONTS['meta'],
                                  bg=bg, fg=COLORS['fg_5'], anchor='w')
        self._hint_lbl.pack(fill='x')

        # Click + hover on every child
        widgets = [self, inner, self._glyph_lbl, txt_col,
                   self._main_lbl, self._hint_lbl]
        for w in widgets:
            w.bind('<Enter>', self._on_enter, add='+')
            w.bind('<Leave>', self._on_leave, add='+')
            if on_click:
                w.bind('<Button-1>', self._on_click_evt, add='+')
                try:
                    w.config(cursor='hand2')
                except Exception:
                    pass

        # Drag-and-drop registration
        try:
            self.drop_target_register('DND_Files')
            self.dnd_bind('<<DropEnter>>', lambda e: self._set_hover(True))
            self.dnd_bind('<<DropLeave>>', lambda e: self._set_hover(False))
            self.dnd_bind('<<Drop>>', self._on_dnd_drop)
        except Exception:
            pass

    def _set_hover(self, on):
        bg = self._hover_bg if on else self._idle_bg
        border = self._hover_border if on else self._idle_border
        try:
            self.configure(bg=bg, highlightbackground=border)
            for w in self.winfo_children():
                self._recolour(w, bg)
        except Exception:
            pass

    def _recolour(self, w, bg):
        try:
            w.configure(bg=bg)
        except Exception:
            pass
        for c in w.winfo_children():
            self._recolour(c, bg)

    def _on_enter(self, _e=None): self._set_hover(True)
    def _on_leave(self, _e=None): self._set_hover(False)
    def _on_click_evt(self, _e=None):
        if self._on_click:
            self._on_click()

    def _on_dnd_drop(self, event):
        self._set_hover(False)
        try:
            paths = self.tk.splitlist(event.data)
        except Exception:
            paths = [event.data]
        for p in paths:
            p = p.strip('{}').strip('"').strip()
            if p and self._on_drop:
                self._on_drop(p)


def _build_right_column(grid, app):
    """The right column: progress card stacked above queue card.

    Step 46 (v2.5.8): the queue card now uses fill='both' expand=True so
    it absorbs any unused vertical space when the progress card is idle
    or short — eliminating the big empty gap that appeared between them
    when a build finished and the queue was empty.
    """
    col = tk.Frame(grid, bg=COLORS['bg_1'])
    col.grid(row=2, column=1, sticky='nsew', padx=(9, 0))

    # ── Build progress card ──
    # fill='x' only, NOT expand=True — this card sticks to its natural
    # content height so the queue card below can claim the remaining
    # vertical space.
    prog_card = Card(col,
                     title='Build progress',
                     subtitle='Live status appears here once a build starts.',
                     icon='\U0001f4ca')  # 📊
    prog_card.pack(fill='x', pady=(0, 10))

    # The progress stages live in the card body. We pin the body to a
    # natural-content height (no expand) so the card doesn't stretch into
    # empty space when idle.
    app._build_stages = StagedProgressBar(prog_card.body,
                                          stages=['Mount', 'Format',
                                                  'Copy files', 'Verify'],
                                          bg=COLORS['bg_2'])
    app._build_stages.pack(fill='x')

    # Backwards-compat aliases for the legacy callbacks. The new widget
    # owns its own state, but the existing `_update_bar_visual`,
    # `_on_bar_resize`, and `_activate_dot` callbacks reference these names.
    # Since the brief forbids changing callbacks, we expose the underlying
    # canvas + rect, plus a `_stage_dots` list-shaped shim that the
    # one-line `_activate_dot` can iterate without crashing.
    app._bar_canvas = app._build_stages.bar_canvas
    app._bar_rect   = app._build_stages.bar_rect
    # _stage_dots is used as a list of (dot, label) for itemconfig/.config
    # by the legacy _activate_dot. We provide an empty list so legacy
    # iterations are no-ops — the new StagedProgressBar handles state via
    # its own set_active(idx)/set_complete(idx). The main file's
    # _activate_dot will be updated to call the new widget.
    app._stage_dots = []

    # Step + percentage row goes inside the progress card, BELOW the stages.
    meta_row = tk.Frame(prog_card.body, bg=COLORS['bg_2'])
    meta_row.pack(fill='x', pady=(8, 0))
    tk.Label(meta_row, textvariable=app._step_label_var,
             font=FONTS['body_b'],
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(side='left')
    tk.Label(meta_row, textvariable=app._pct_var,
             font=(FONTS['mono'][0], 10, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['accent'], anchor='e'
             ).pack(side='right')

    eta_row = tk.Frame(prog_card.body, bg=COLORS['bg_2'])
    eta_row.pack(fill='x', pady=(2, 0))
    tk.Label(eta_row, textvariable=app._eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
             ).pack(side='left')
    tk.Label(eta_row, textvariable=app._queue_eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['accent_hi'], anchor='e'
             ).pack(side='right')

    # ── Build queue card ──
    q_card = Card(col, bg=COLORS['bg_2'])
    q_card.pack(fill='both', expand=True)

    # Custom queue header — a count pill + ghost action buttons. We don't
    # use Card's built-in title because the layout differs (count pill, no
    # icon tile, action buttons inline).
    q_head = tk.Frame(q_card.body, bg=COLORS['bg_2'])
    q_head.pack(fill='x')

    head_left = tk.Frame(q_head, bg=COLORS['bg_2'])
    head_left.pack(side='left')
    tk.Label(head_left, text='Build queue',
             font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(side='left')
    app._queue_count_var = tk.StringVar(value='0 items')
    tk.Label(head_left, textvariable=app._queue_count_var,
             font=(FONTS['body'][0], 9, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
             padx=8, pady=2
             ).pack(side='left', padx=(10, 0))

    head_right = tk.Frame(q_head, bg=COLORS['bg_2'])
    head_right.pack(side='right')
    _qbtn(head_right, '\U0001f4c2  Load',
          command=app._queue_load).pack(side='left', padx=(0, 4))
    _qbtn(head_right, '\U0001f4be  Save',
          command=app._queue_save).pack(side='left', padx=(0, 4))
    _qbtn(head_right, '\U0001f5d1  Clear', danger=True,
          command=app._clear_queue).pack(side='left')

    # Drop hint sits inside the queue card, just above the list. Same DnD
    # logic as the legacy code — when tkinterdnd2 is unavailable we just
    # show a less-encouraging hint.
    dnd_hint = tk.Label(q_card.body,
                        text='\U0001f3ae  Drop game folders here to add to queue',
                        font=FONTS['meta'],
                        bg=COLORS['bg_2'], fg=COLORS['fg_5'])
    dnd_hint.pack(pady=(10, 8))

    # The actual scrollable queue list area. SURFACE2 with a 1px hairline
    # border, just like the queue card. Items are appended by external code
    # via app._queue_frame.
    q_outer = tk.Frame(q_card.body, bg=COLORS['bg_3'],
                       highlightbackground=COLORS['border_2'],
                       highlightthickness=1)
    q_outer.pack(fill='both', expand=True)

    canvas = tk.Canvas(q_outer, bg=COLORS['bg_3'],
                       highlightthickness=0, height=200)
    sb = tk.Scrollbar(q_outer, orient='vertical', command=canvas.yview,
                      bg=COLORS['bg_4'], troughcolor=COLORS['bg_2'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._queue_canvas = canvas
    app._queue_frame = tk.Frame(canvas, bg=COLORS['bg_3'])
    canvas.create_window((0, 0), window=app._queue_frame,
                         anchor='nw', tags='qf')
    app._queue_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig('qf', width=e.width))

    # ── Drag & drop for game folders onto the queue ──
    # Same handler the legacy code used (`app._on_queue_drop`), so any code
    # that maintains it stays correct. Try/except because tkinterdnd2 may
    # not be installed.
    def _drop_to_queue(event):
        try:
            paths = app.tk.splitlist(event.data)
            for raw in paths:
                p = raw.strip('{}').strip('"')
                if os.path.isdir(p):
                    app._game_var.set(p)
                    app._on_game_browse_done(p)
                    app._add_to_queue()
                    dnd_hint.config(fg=COLORS['success_hi'],
                                    text='\u2713  Added to queue')
                    app.after(2000, lambda: dnd_hint.config(
                        fg=COLORS['fg_5'],
                        text='\U0001f3ae  Drop game folders here to add to queue'))
        except Exception as e:
            app._log('[DROP] Error: %s\n' % e)

    try:
        app.drop_target_register('DND_Files')
        app.dnd_bind('<<Drop>>', _drop_to_queue)
    except Exception:
        dnd_hint.config(text='\u26a0  Install tkinterdnd2 to enable drag & drop')
    try:
        canvas.drop_target_register('DND_Files')
        canvas.dnd_bind('<<Drop>>', getattr(app, '_on_queue_drop',
                                            _drop_to_queue))
    except Exception:
        pass


def _qbtn(parent, text, command, danger=False):
    """Small ghost-style button for the queue card header.

    Matches `.qbtn` in the mock: bg_3 fill, fg_3 text, border_2 hairline,
    optional danger hover state for Clear.
    """
    bg = COLORS['bg_3']
    fg = COLORS['fg_3']
    abg = COLORS['bg_4']
    afg = COLORS['danger_hi'] if danger else COLORS['fg_1']
    return tk.Button(parent, text=text,
                     font=FONTS['meta'],
                     bg=bg, fg=fg,
                     activebackground=abg, activeforeground=afg,
                     relief='flat', bd=0,
                     padx=10, pady=5,
                     cursor='hand2',
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1,
                     command=command)


def _on_dropzone_drop(app, path):
    """Called when a folder is dropped into the DropZone widget.

    Just sets the game folder and triggers the existing detection logic.
    No queue auto-add — the user clicks "+ Add to Queue" explicitly.
    """
    if not os.path.isdir(path):
        return
    try:
        app._game_var.set(path)
        app._on_game_browse_done(path)
    except Exception as e:
        try:
            app._log('[DROP] Error: %s\n' % e)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────────
# Compatibility — attributes preserved on `app` for the rest of the app.
# ─────────────────────────────────────────────────────────────────────────
# After build_exfat_tab() returns, every `app.*` attribute the rest of the
# codebase expects is in place. Specifically:
#
#   app._info_frame        — alias for the DetectedGameStrip; supports
#                            pack(fill='x', pady=...) and pack_forget()
#   app._cover_label       — the strip's cover thumbnail
#   app._info_title_var    — strip's title StringVar
#   app._info_id_var       — strip's ID StringVar
#   app._info_ver_var      — strip's version StringVar
#   app._info_size_var     — strip's size StringVar
#   app._cover_image       — set to None initially; the existing code
#                            assigns the PhotoImage and reuses this slot
#   app._auto_label        — "● auto-detected" label, hidden initially
#   app._no_sfo_label      — "● no metadata found" label, hidden initially
#   app._filename_preview  — read-only Label showing the resolved filename
#   app._build_stages      — the new StagedProgressBar (replaces dots)
#   app._bar_canvas        — Canvas the legacy `_update_bar_visual` writes to
#   app._bar_rect          — the rect ID inside that canvas
#   app._stage_dots        — empty list (legacy iteration is now a no-op;
#                            real state is in app._build_stages)
#   app._queue_count_var   — header count StringVar
#   app._queue_canvas      — scroll canvas for queue rows
#   app._queue_frame       — frame inside the canvas where rows are appended
#   app.build_btn          — primary "Build All" button
#   app._pause_btn         — pause toggle button (starts disabled)
#   app.status_lbl         — bottom status line
#
# Eighteen attributes total — exactly the set the rest of the app reads
# from. The widget classes all support the same `.config(...)` calls the
# old plain widgets supported, so no main-file edits are needed beyond
# the one-line `_activate_dot` migration (see the diff for that).
