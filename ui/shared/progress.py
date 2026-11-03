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


class _BarCanvasShim:
    """Legacy code drew on `bar_canvas` with coords()/itemconfig(); this
    translates those calls onto the RoundedBar."""

    def __init__(self, bar):
        self._bar = bar

    def coords(self, _item, *xy):
        if len(xy) == 1 and isinstance(xy[0], (list, tuple)):
            xy = xy[0]
        if len(xy) >= 4:
            w = max(1, self._bar.winfo_width())
            x0, x1 = float(xy[0]), float(xy[2])
            self._bar.set(max(0.0, (x1 - x0) / w), relx=max(0.0, x0 / w))
        return []

    def itemconfig(self, _item, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        fill = kw.get('fill')
        if fill:
            lo = str(fill).lower()
            if lo in (COLORS['success'].lower(), COLORS['success_hi'].lower()):
                self._bar.set_color(COLORS['success'])
            elif lo in (COLORS['danger'].lower(), COLORS['danger_hi'].lower()):
                self._bar.set_color(COLORS['danger'])
            else:
                self._bar.set_color(COLORS['accent'])

    itemconfigure = itemconfig

    def bind(self, seq=None, func=None, add=None):
        return self._bar.bind(seq, func, '+')

    def __getattr__(self, name):
        return getattr(self._bar, name)


class StagedProgressBar(tk.Frame):
    """Staged progress (design system › ProgressBar): a stepper row (step
    icon + name; done = success, active = accent, error = danger), a 6px
    rounded bar and a meta row. API unchanged: set_pending, set_active,
    set_complete, set_error, reset, set_progress, set_meta."""

    def __init__(self, parent, stages, **kwargs):
        from tkinter_theme import px
        from ui.shared.widgets import RoundedBar, StepIcon
        bg = kwargs.pop('bg', COLORS['bg_2'])
        super().__init__(parent, bg=bg, **kwargs)
        self._bg = bg
        self._stages = list(stages)
        self._tiles = []
        self._current_pct = 0
        row = tk.Frame(self, bg=bg)
        row.pack(fill='x', pady=(0, px(12)))
        for i, name in enumerate(self._stages):
            row.grid_columnconfigure(i, weight=1, uniform='stages')
            cell = tk.Frame(row, bg=bg)
            cell.grid(row=0, column=i, sticky='w')
            icon = StepIcon(cell, '\u23f3', size=16, bg=bg)
            icon.pack(side='left')
            lbl = tk.Label(cell, text=name, font=FONTS['caption'], bg=bg,
                           fg=COLORS['fg_4'])
            lbl.pack(side='left', padx=(px(6), 0))
            self._tiles.append({'frame': cell, 'icon': icon, 'name': lbl})
        self._bar = RoundedBar(self, height=6, surface=bg)
        self._bar.pack(fill='x')
        # Legacy hooks: callers drew on these directly.
        self.bar_canvas = _BarCanvasShim(self._bar)
        self.bar_rect = 'bar'
        meta_row = tk.Frame(self, bg=bg)
        meta_row.pack(fill='x', pady=(px(6), 0))
        self._meta_left_var = tk.StringVar(value='')
        self._meta_right_var = tk.StringVar(value='')
        tk.Label(meta_row, textvariable=self._meta_left_var,
                 font=FONTS['caption'], bg=bg, fg=COLORS['fg_3'],
                 anchor='w').pack(side='left')
        tk.Label(meta_row, textvariable=self._meta_right_var,
                 font=FONTS['caption'], bg=bg, fg=COLORS['fg_2'],
                 anchor='e').pack(side='right')

    _GLYPH = {'pending': ('\u23f3', 'fg_4'), 'active': ('\u25b6', 'fg_0'),
              'done': ('\u2714', 'fg_2'), 'error': ('\u2715', 'danger_hi')}

    def _apply_state(self, idx, state):
        if not (0 <= idx < len(self._tiles)):
            return
        g, fg = self._GLYPH[state]
        t = self._tiles[idx]
        t['icon'].config(text=g)
        t['name'].configure(fg=COLORS[fg],
                            font=FONTS['caption_strong'] if state == 'active'
                            else FONTS['caption'])

    def set_pending(self, idx):
        self._apply_state(idx, 'pending')

    def set_active(self, idx):
        for i in range(len(self._tiles)):
            self._apply_state(i, 'done' if i < idx else
                              ('active' if i == idx else 'pending'))

    def set_complete(self, idx):
        self._apply_state(idx, 'done')

    def set_error(self, idx):
        self._apply_state(idx, 'error')
        self._bar.set_color(COLORS['danger'])

    def reset(self):
        for i in range(len(self._tiles)):
            self._apply_state(i, 'pending')
        self._bar.set_color(COLORS['accent'])
        self.set_progress(0)
        self._meta_left_var.set('')
        self._meta_right_var.set('')

    def set_progress(self, pct):
        self._current_pct = max(0, min(100, pct))
        self._bar.set_color(COLORS['success'] if self._current_pct >= 100
                            else COLORS['accent'])
        self._bar.set(self._current_pct / 100.0)

    def set_meta(self, left=None, right=None):
        if left is not None:
            self._meta_left_var.set(left)
        if right is not None:
            self._meta_right_var.set(right)
