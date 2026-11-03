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
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter_theme import COLORS, FONTS


# ─────────────────────────────────────────────────────────────────────────
# DropZone — dashed-border drop target.
# ─────────────────────────────────────────────────────────────────────────
class DropZone(tk.Frame):
    """Drop target (design system › inset panel): rounded bg_0 well with a
    hairline, an icon tile, a main line and an optional hint. Hover / drag
    shows the accent selection look. Calls `on_drop(path)` per dropped
    folder (tkinterdnd2 when available) and `on_click()` on click."""

    def __init__(self, parent, on_drop=None, on_click=None,
                 hint=None, glyph='\u2913'):
        from tkinter_theme import px, surface_of
        from ui.shared.widgets import Rounded, IconLabel
        outside = surface_of(parent)
        super().__init__(parent, bg=outside)
        self._on_drop = on_drop
        self._on_click = on_click
        self._idle_bg = COLORS['bg_0']
        self._hover_bg = COLORS['accent_08']
        self._idle_border = COLORS['border_3']
        self._hover_border = COLORS['accent']
        self._well = Rounded(self, bg=self._idle_bg, border=self._idle_border,
                             radius=8, padding=(px(20), px(18)), surface=outside)
        self._well.pack(fill='both', expand=True)
        inner = tk.Frame(self._well, bg=self._idle_bg)
        inner.pack(expand=True)
        self._inner = inner
        self._glyph_lbl = IconLabel(inner, 'folder_in', 24, COLORS['fg_3'],
                                    self._idle_bg)
        self._glyph_lbl.pack()
        self._main_lbl = tk.Label(inner, text='Drop a game folder here',
                                  font=FONTS['body_strong'], bg=self._idle_bg,
                                  fg=COLORS['fg_1'])
        self._main_lbl.pack(pady=(px(8), 0))
        if hint:
            self._hint_lbl = tk.Label(inner, text=hint, font=FONTS['caption'],
                                      bg=self._idle_bg, fg=COLORS['fg_3'])
            self._hint_lbl.pack(pady=(px(2), 0))
        else:
            self._hint_lbl = None
        for w in [self, self._well, inner, self._glyph_lbl, self._main_lbl] + (
                [self._hint_lbl] if self._hint_lbl else []):
            w.bind('<Enter>', self._on_enter, add='+')
            w.bind('<Leave>', self._on_leave, add='+')
            if on_click:
                w.bind('<Button-1>', self._on_click_evt, add='+')
                try:
                    w.config(cursor='hand2')
                except Exception:
                    pass
        try:
            self.drop_target_register('DND_Files')
            self.dnd_bind('<<DropEnter>>', self._on_dnd_enter)
            self.dnd_bind('<<DropLeave>>', self._on_leave)
            self.dnd_bind('<<Drop>>', self._on_dnd_drop)
        except Exception:
            pass

    # ── Event handlers ──
    def _set_active(self, active):
        bg = self._hover_bg if active else self._idle_bg
        self._well.set_style(bg=bg, border=self._hover_border if active
                             else self._idle_border)
        for w in (self._inner, self._main_lbl, self._hint_lbl):
            if w is not None:
                try:
                    w.configure(bg=bg)
                except Exception:
                    pass
        self._glyph_lbl.set(fg=COLORS['accent_hi'] if active else COLORS['fg_3'],
                            bg=bg)

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
        from tkinter_theme import px, surface_of
        from ui.shared.widgets import TextField, Button
        row_bg = surface_of(parent)
        super().__init__(parent, bg=row_bg)
        lbl_row = tk.Frame(self, bg=row_bg)
        lbl_row.pack(fill='x')
        tk.Label(lbl_row, text=label, font=FONTS['body'], bg=row_bg,
                 fg=COLORS['fg_2'], anchor='w').pack(side='left')
        if required:
            tk.Label(lbl_row, text=' *', font=FONTS['body'], bg=row_bg,
                     fg=COLORS['warn_hi'], anchor='w').pack(side='left')
        if hint:
            tk.Label(lbl_row, text=hint, font=FONTS['caption'], bg=row_bg,
                     fg=COLORS['fg_4'], anchor='w').pack(side='left',
                                                         padx=(px(8), 0))
        row = tk.Frame(self, bg=row_bg)
        row.pack(fill='x', pady=(px(4), 0))
        self.entry = TextField(row, textvariable=var, mono=True,
                               state=('readonly' if readonly else 'normal'))
        self.entry.pack(side='left', fill='x', expand=True)
        self.button = None
        if on_browse:
            self.button = Button(row, text='Browse\u2026', icon='folder_open',
                                 command=on_browse)
            self.button.pack(side='left', padx=(px(6), 0))


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
        from tkinter_theme import px, surface_of
        from ui.shared.widgets import Rounded
        from ui.icons import leading_icon, strip_glyphs, icon_image
        row_bg = surface_of(parent)
        super().__init__(parent, bg=row_bg)
        self._var = var
        self._on_change = on_change
        self._row_bg = row_bg
        self._icon = leading_icon(glyph or '') if glyph else None
        self._pill = Rounded(self, bg=COLORS['bg_3'], border=COLORS['border_3'],
                             radius=4, padding=(px(10), px(4)), surface=row_bg,
                             cursor='hand2')
        self._pill.pack()
        self._lbl = tk.Label(self._pill, text=strip_glyphs(text),
                             font=FONTS['body'], cursor='hand2',
                             compound='left')
        self._lbl.pack()
        for w in (self, self._pill, self._lbl):
            w.bind('<Button-1>', lambda e: self._toggle())
        self._repaint()
        try:
            var.trace_add('write', lambda *a: self._repaint())
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
        from ui.icons import icon_image
        on = bool(self._var.get())
        bg = COLORS['accent_lo'] if on else COLORS['bg_3']
        bd = COLORS['accent'] if on else COLORS['border_3']
        fg = COLORS['fg_0'] if on else COLORS['fg_2']
        self._pill.set_style(bg=bg, border=bd)
        self._lbl.configure(bg=bg, fg=fg)
        if self._icon:
            self._lbl.configure(image=icon_image(self._icon, 14,
                                                 COLORS['accent_hi'] if on else fg, bg),
                                text=' ' + self._lbl.cget('text').strip())
