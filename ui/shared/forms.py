"""
ui/shared/forms.py — form input widgets.

- DropZone     — large dashed-border drop target with glyph + text.
                 Calls `on_drop(path)` when a folder is dropped.
- LabeledField — a label-above-input row with an optional Browse button,
                 matching the design system's `.field` block. The input is
                 light-on-dark per the design rule (Windows-form holdover).

Both pull colors from `tkinter_theme.COLORS` and fonts from FONTS — no
hex literals. Step 3 (v2.0.5).
"""

import os
import tkinter as tk
from tkinter_theme import COLORS, FONTS


# ─────────────────────────────────────────────────────────────────────────
# DropZone — dashed-border drop target.
# ─────────────────────────────────────────────────────────────────────────
class DropZone(tk.Frame):
    """A dashed-border drop target with a glyph and instructional text.

    Matches `.drop` in exfat-tab-redesign.html. Calls `on_drop(path)` for
    each dropped folder. Falls back gracefully if tkinterdnd2 is unavailable
    (just shows the static label).

    The dashed border is approximated using a 1px-bordered Frame with a
    slightly tinted background. tkinter doesn't render true CSS dashes.

    Args:
        parent: parent widget
        on_drop: callable(path: str) -> None, fired per dropped path
        on_click: optional callable() -> None, fired when the zone is clicked
        hint: optional small hint text under the main label
        glyph: leading emoji/symbol (default '⤓' download glyph)

    Visual states:
        idle   — border_3 border, fg_5 glyph
        hover  — accent border, accent_08 background
        active — same as hover (drag in progress)
    """

    def __init__(self, parent, on_drop=None, on_click=None,
                 hint=None, glyph='\u2913'):
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
        inner.pack(fill='both', expand=True, padx=22, pady=22)

        self._glyph_lbl = tk.Label(inner, text=glyph,
                                   font=(FONTS['body'][0], 24),
                                   bg=bg, fg=COLORS['fg_5'])
        self._glyph_lbl.pack()

        self._main_lbl = tk.Label(inner,
                                  text='Drop a game folder here',
                                  font=(FONTS['body'][0], 11, 'bold'),
                                  bg=bg, fg=COLORS['fg_2'])
        self._main_lbl.pack(pady=(6, 0))

        if hint:
            self._hint_lbl = tk.Label(inner, text=hint,
                                      font=FONTS['meta'],
                                      bg=bg, fg=COLORS['fg_5'])
            self._hint_lbl.pack(pady=(2, 0))
        else:
            self._hint_lbl = None

        # ── Hover and click feedback on the whole zone ──
        for w in [self, inner, self._glyph_lbl, self._main_lbl] + (
                [self._hint_lbl] if self._hint_lbl else []):
            w.bind('<Enter>', self._on_enter, add='+')
            w.bind('<Leave>', self._on_leave, add='+')
            if on_click:
                w.bind('<Button-1>', self._on_click_evt, add='+')
                try:
                    w.config(cursor='hand2')
                except Exception:
                    pass

        # ── Drag-and-drop registration (tkinterdnd2 if available) ──
        try:
            self.drop_target_register('DND_Files')
            self.dnd_bind('<<DropEnter>>', self._on_dnd_enter)
            self.dnd_bind('<<DropLeave>>', self._on_leave)
            self.dnd_bind('<<Drop>>', self._on_dnd_drop)
        except Exception:
            # No tkinterdnd2 — fall back to "click to browse" UX. Caller can
            # bind on_click to open a folder dialog.
            pass

    # ── Event handlers ──
    def _set_active(self, active):
        if active:
            self.configure(highlightbackground=self._hover_border,
                           highlightthickness=1)
            self._main_lbl.configure(bg=self._hover_bg)
            self._glyph_lbl.configure(bg=self._hover_bg, fg=COLORS['accent'])
            for child in self.winfo_children():
                try:
                    child.configure(bg=self._hover_bg)
                except Exception:
                    pass
            if self._hint_lbl:
                self._hint_lbl.configure(bg=self._hover_bg)
        else:
            self.configure(highlightbackground=self._idle_border,
                           highlightthickness=1)
            self._main_lbl.configure(bg=self._idle_bg)
            self._glyph_lbl.configure(bg=self._idle_bg, fg=COLORS['fg_5'])
            for child in self.winfo_children():
                try:
                    child.configure(bg=self._idle_bg)
                except Exception:
                    pass
            if self._hint_lbl:
                self._hint_lbl.configure(bg=self._idle_bg)

    def _on_enter(self, _e=None):
        self._set_active(True)

    def _on_leave(self, _e=None):
        self._set_active(False)

    def _on_click_evt(self, _e=None):
        if self._on_click:
            try:
                self._on_click()
            except Exception:
                pass

    def _on_dnd_enter(self, _e=None):
        self._set_active(True)

    def _on_dnd_drop(self, event):
        self._set_active(False)
        if not self._on_drop:
            return
        try:
            paths = self.tk.splitlist(event.data)
        except Exception:
            paths = [event.data]
        for raw in paths:
            p = raw.strip('{}').strip('"').strip()
            if os.path.isdir(p):
                try:
                    self._on_drop(p)
                except Exception:
                    pass


# ─────────────────────────────────────────────────────────────────────────
# LabeledField — label + light-on-dark input + optional Browse button.
# Mirrors `.field` in the design system.
# ─────────────────────────────────────────────────────────────────────────
class LabeledField(tk.Frame):
    """A label above a text input, optional Browse button, design-system styled.

    Matches `.field` in the mock — the input is light (`field_bg` = #f0f0f0)
    on dark, with a Browse button to its right that opens a folder picker.

    Args:
        parent: parent widget
        label: visible label text
        var: tk.StringVar bound to the entry
        on_browse: callback fired when Browse is clicked
        required: if True, an amber `*` asterisk is appended to the label
        hint: optional dimmer hint text after the label, e.g. "must contain
              eboot.bin"
        readonly: if True, the entry is non-editable (still selectable)

    Attributes:
        entry: the tk.Entry widget itself
        button: the Browse tk.Button, or None if on_browse=None

    The input row uses the parent's bg color so it blends into both BG and
    Card (bg_2) backgrounds.
    """

    def __init__(self, parent, label, var, on_browse=None,
                 required=False, hint=None, readonly=False):
        try:
            row_bg = parent.cget('bg')
        except Exception:
            row_bg = COLORS['bg_2']
        super().__init__(parent, bg=row_bg)

        # ── Label row ──
        lbl_row = tk.Frame(self, bg=row_bg)
        lbl_row.pack(fill='x')

        tk.Label(lbl_row, text=label,
                 font=FONTS['label'],
                 bg=row_bg, fg=COLORS['fg_3'], anchor='w'
                 ).pack(side='left')
        if required:
            tk.Label(lbl_row, text='  *',
                     font=FONTS['label'],
                     bg=row_bg, fg=COLORS['warn'], anchor='w'
                     ).pack(side='left')
        if hint:
            tk.Label(lbl_row, text='  \u2014  ' + hint,
                     font=FONTS['meta'],
                     bg=row_bg, fg=COLORS['fg_5'], anchor='w'
                     ).pack(side='left')

        # ── Input row ──
        input_wrap = tk.Frame(self, bg=COLORS['field_bg'],
                              highlightbackground=COLORS['border_3'],
                              highlightthickness=1)
        input_wrap.pack(fill='x', pady=(6, 0))

        self.entry = tk.Entry(input_wrap, textvariable=var,
                              font=FONTS['mono_sm'],
                              bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                              insertbackground=COLORS['field_fg'],
                              selectbackground=COLORS['accent'],
                              selectforeground=COLORS['fg_0'],
                              relief='flat', bd=6,
                              state=('readonly' if readonly else 'normal'))
        self.entry.pack(side='left', fill='x', expand=True)

        self.button = None
        if on_browse:
            self.button = tk.Button(input_wrap, text='Browse',
                                    font=FONTS['button'],
                                    bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                                    activebackground=COLORS['bg_5'],
                                    activeforeground=COLORS['accent'],
                                    relief='flat', bd=0,
                                    padx=14, pady=6,
                                    cursor='hand2',
                                    command=on_browse)
            self.button.pack(side='right')


# ─────────────────────────────────────────────────────────────────────────
# SegmentedToggle — a pill-style toggle that's bound to a BooleanVar.
# Mirrors the design system's `.toolbar .seg.on` style.
# ─────────────────────────────────────────────────────────────────────────
class SegmentedToggle(tk.Frame):
    """A pill toggle that lights up accent-blue when its var is True.

    Used for the Klog tab's "Auto-scroll / Word wrap / Timestamps" toolbar
    toggles. Clicking the pill flips the BooleanVar and updates the colors
    in place. Optional callback fires on change.

    Args:
        parent: parent widget
        text: pill label
        var: tk.BooleanVar to bind. Pill state mirrors var.
        on_change: optional callable() fired after var changes
        glyph: optional small leading glyph
    """

    def __init__(self, parent, text, var, on_change=None, glyph=None):
        try:
            row_bg = parent.cget('bg')
        except Exception:
            row_bg = COLORS['bg_1']
        super().__init__(parent, bg=row_bg)
        self._var = var
        self._on_change = on_change

        # The pill body is itself a labeled Frame with a 1px border.
        self._pill_bg_off = COLORS['bg_2']
        self._pill_bg_on  = COLORS['accent_08']
        self._pill_border_off = COLORS['border_2']
        self._pill_border_on  = COLORS['accent_lo']
        self._pill_fg_off = COLORS['fg_3']
        self._pill_fg_on  = COLORS['accent']

        self._pill = tk.Frame(self,
                              highlightthickness=1)
        self._pill.pack()

        display = ((glyph + ' ') if glyph else '') + text
        self._lbl = tk.Label(self._pill, text=display,
                             font=FONTS['meta'],
                             padx=10, pady=4,
                             cursor='hand2')
        self._lbl.pack()

        # Click anywhere on the pill (or its label) toggles the var.
        for w in (self._pill, self._lbl):
            w.bind('<Button-1>', lambda e: self._toggle())

        # Initial paint
        self._repaint()
        # Reactively repaint when the var changes externally
        try:
            var.trace_add('write', lambda *a: self._repaint())
        except Exception:
            try:
                var.trace('w', lambda *a: self._repaint())
            except Exception:
                pass

    def _toggle(self):
        try:
            self._var.set(not self._var.get())
        except Exception:
            return
        if self._on_change:
            try:
                self._on_change()
            except Exception:
                pass

    def _repaint(self):
        on = bool(self._var.get())
        bg = self._pill_bg_on if on else self._pill_bg_off
        bd = self._pill_border_on if on else self._pill_border_off
        fg = self._pill_fg_on if on else self._pill_fg_off
        self._pill.configure(bg=bg, highlightbackground=bd)
        self._lbl.configure(bg=bg, fg=fg)
