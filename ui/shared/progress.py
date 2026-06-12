"""
ui/shared/progress.py — staged build progress widget.

StagedProgressBar — the four-tile + bar + meta progress widget from the
exFAT tab redesign mock. Mirrors `.stages`, `.pbar`, and `.progress-meta`
in exfat-tab-redesign.html.

Visual layout (top to bottom):
    [01 Mount ●] [02 Format ●] [03 Copy ●] [04 Verify ●]
    [══════════════════════════════════════════════════]   ← progress bar
    17.1 GB of 27.1 GB · 63%             ETA 4m 12s · 38 MB/s

Stage tile states:
    pending — bg_3, border_2, muted dot
    active  — accent border + accent_15 fill, pulsing accent dot, accent_hi name
    done    — success border + success_bg fill, success dot, success_hi name
    error   — danger border + danger_bg fill, danger dot, danger_hi name

The widget owns its state. Public API:
    set_active(idx)        — mark stage idx active (others before = done,
                             after = pending). Use this for normal progression.
    set_complete(idx)      — mark stage idx as done (others unchanged)
    set_pending(idx)       — reset stage idx to pending
    set_error(idx)         — mark stage idx as failed
    set_progress(pct)      — update the bar (0-100)
    set_meta(left, right)  — update the meta row labels
    reset()                — all stages → pending, bar → 0
    bar_canvas             — exposed for legacy `_update_bar_visual` callers
    bar_rect               — exposed for legacy callers

The `bar_canvas` and `bar_rect` are kept as public attributes so existing
callbacks in exfat_builder.py (`_update_bar_visual`, `_on_bar_resize`)
continue to work unchanged. Eventually those callbacks should call
`set_progress()` directly; preserved for now per the brief's "don't change
callbacks" rule.

Step 3 (v2.0.5).
"""

import tkinter as tk
from tkinter_theme import COLORS, FONTS


class StagedProgressBar(tk.Frame):
    """Four-stage progress widget with a bar and meta row."""

    # State color schemes — (border, fill, dot, name_fg, num_fg)
    _PENDING = (COLORS['border_2'], COLORS['bg_3'],
                COLORS['fg_6'],     COLORS['fg_2'], COLORS['fg_5'])
    _ACTIVE  = (COLORS['accent'],   COLORS['accent_15'],
                COLORS['accent'],   COLORS['accent_hi'], COLORS['accent'])
    _DONE    = (COLORS['success'],  COLORS['success_bg'],
                COLORS['success'],  COLORS['success_hi'], COLORS['success_hi'])
    _ERROR   = (COLORS['danger'],   COLORS['danger_bg'],
                COLORS['danger'],   COLORS['danger_hi'], COLORS['danger_hi'])

    def __init__(self, parent, stages, **kwargs):
        bg = kwargs.pop('bg', COLORS['bg_2'])
        super().__init__(parent, bg=bg, **kwargs)
        self._bg = bg
        self._stages = list(stages)
        self._tiles = []        # list of dicts: {frame, num, name, dot}
        self._bar_width = 0
        self._current_pct = 0

        # ── Stage tile row ──
        # Use a grid so tiles share width equally regardless of label length.
        tiles_row = tk.Frame(self, bg=bg)
        tiles_row.pack(fill='x', pady=(0, 12))
        for i, name in enumerate(self._stages):
            tiles_row.grid_columnconfigure(i, weight=1, uniform='stages')
            tile = self._make_tile(tiles_row, i, name)
            tile['frame'].grid(row=0, column=i, sticky='nsew',
                               padx=(0 if i == 0 else 4, 0))
            self._tiles.append(tile)

        # ── Progress bar ──
        # Canvas-based so we can do a simple gradient fill (left=accent,
        # right=success). tkinter doesn't render true CSS gradients; we fake
        # it with two overlaid rectangles whose widths track progress.
        bar_outer = tk.Frame(self, bg=COLORS['bg_4'], height=10,
                             highlightthickness=0)
        bar_outer.pack(fill='x')
        bar_outer.pack_propagate(False)
        self.bar_canvas = tk.Canvas(bar_outer, height=10,
                                    bg=COLORS['bg_4'],
                                    highlightthickness=0, bd=0)
        self.bar_canvas.pack(fill='both', expand=True)
        # Two rectangles — one for the accent gradient body, one for the
        # success-tinted leading edge that grows as progress nears 100%.
        # Simple approximation of the design's accent→success gradient.
        self.bar_rect = self.bar_canvas.create_rectangle(
            0, 0, 0, 10, fill=COLORS['accent'], outline='')
        self.bar_canvas.bind('<Configure>', self._on_bar_resize)

        # ── Meta row ──
        meta_row = tk.Frame(self, bg=bg)
        meta_row.pack(fill='x', pady=(8, 0))
        self._meta_left_var  = tk.StringVar(value='')
        self._meta_right_var = tk.StringVar(value='')
        tk.Label(meta_row, textvariable=self._meta_left_var,
                 font=FONTS['mono_sm'],
                 bg=bg, fg=COLORS['fg_4'], anchor='w'
                 ).pack(side='left')
        tk.Label(meta_row, textvariable=self._meta_right_var,
                 font=FONTS['mono_sm'],
                 bg=bg, fg=COLORS['accent'], anchor='e'
                 ).pack(side='right')

    # ── Tile factory ──
    def _make_tile(self, parent, idx, name):
        # Wrap each tile in a 1px-bordered Frame for the colored border state.
        border, fill, dot, name_fg, num_fg = self._PENDING
        frame = tk.Frame(parent, bg=fill,
                         highlightbackground=border,
                         highlightthickness=1)
        # Padding inside
        inner = tk.Frame(frame, bg=fill)
        inner.pack(fill='both', expand=True, padx=10, pady=8)

        top_row = tk.Frame(inner, bg=fill)
        top_row.pack(fill='x')
        num_lbl = tk.Label(top_row,
                           text=('%02d' % (idx + 1)),
                           font=(FONTS['eyebrow'][0], 8, 'bold'),
                           bg=fill, fg=num_fg)
        num_lbl.pack(side='left')
        # Dot indicator on the right of the top row
        dot_lbl = tk.Label(top_row, text='\u25cf',
                           font=(FONTS['body'][0], 11),
                           bg=fill, fg=dot)
        dot_lbl.pack(side='right')

        name_lbl = tk.Label(inner, text=name,
                            font=(FONTS['body'][0], 10, 'bold'),
                            bg=fill, fg=name_fg, anchor='w')
        name_lbl.pack(fill='x', pady=(4, 0))

        return {
            'frame': frame, 'inner': inner, 'top_row': top_row,
            'num': num_lbl, 'name': name_lbl, 'dot': dot_lbl,
        }

    # ── State setters ──
    def _apply_state(self, idx, scheme):
        if not (0 <= idx < len(self._tiles)):
            return
        border, fill, dot, name_fg, num_fg = scheme
        t = self._tiles[idx]
        t['frame'].configure(bg=fill, highlightbackground=border)
        t['inner'].configure(bg=fill)
        t['top_row'].configure(bg=fill)
        t['num'].configure(bg=fill, fg=num_fg)
        t['name'].configure(bg=fill, fg=name_fg)
        t['dot'].configure(bg=fill, fg=dot)

    def set_pending(self, idx):
        self._apply_state(idx, self._PENDING)

    def set_active(self, idx):
        # Steps before idx → done, idx → active, after → pending.
        for i in range(len(self._tiles)):
            if i < idx:
                self._apply_state(i, self._DONE)
            elif i == idx:
                self._apply_state(i, self._ACTIVE)
            else:
                self._apply_state(i, self._PENDING)

    def set_complete(self, idx):
        self._apply_state(idx, self._DONE)

    def set_error(self, idx):
        self._apply_state(idx, self._ERROR)

    def reset(self):
        for i in range(len(self._tiles)):
            self._apply_state(i, self._PENDING)
        self.set_progress(0)
        self._meta_left_var.set('')
        self._meta_right_var.set('')

    # ── Bar control ──
    def _on_bar_resize(self, event):
        self._bar_width = event.width
        self._redraw_bar()

    def _redraw_bar(self):
        fill_w = int(self._bar_width * self._current_pct / 100)
        self.bar_canvas.coords(self.bar_rect, 0, 0, fill_w, 10)
        # Switch fill to success when complete, otherwise stay accent.
        # (The mock uses an accent→success gradient; tk's flat fill is the
        # closest non-image approximation without going full-canvas-stripes.)
        self.bar_canvas.itemconfig(
            self.bar_rect,
            fill=COLORS['success'] if self._current_pct >= 100
            else COLORS['accent'])

    def set_progress(self, pct):
        self._current_pct = max(0, min(100, pct))
        self._redraw_bar()

    # ── Meta row ──
    def set_meta(self, left=None, right=None):
        if left is not None:
            self._meta_left_var.set(left)
        if right is not None:
            self._meta_right_var.set(right)
