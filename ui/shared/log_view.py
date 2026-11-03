"""
ui/shared/log_view.py — terminal-styled console log widget.

ConsoleView — a tk.Text-based terminal console with:
  - Klog-green-on-near-black palette (matches the design system's
    `--klog-green: #00ff41` token, used exclusively by terminal-style logs)
  - Pre-configured color tags for ERR / WARN / INFO / DBG levels
  - Filter-highlight tag for live search
  - Optional word-wrap toggle, scrollbar, and selection highlight

Used by the Klog tab. Could be reused by Files (mount log) or any other
tab that needs a terminal log surface.

Step 4 (v2.0.6).
"""

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter_theme import COLORS, FONTS


# Terminal palette — pinned to design system tokens for the klog console.
# These five colors are intentional and design-system-blessed (see
# README.md "Klog console gets terminal-grade colors").
_TERM_BG     = COLORS['bg_0']        # near-black sunken console (#050505)
_TERM_GREEN  = COLORS['success_ok']  # saturated #00ff41 — klog signature
_TERM_RED    = COLORS['danger_hi']   # ERR
_TERM_AMBER  = COLORS['warn_hi']     # WARN
_TERM_DIM    = COLORS['fg_4']        # DBG / muted lines


class ConsoleView(tk.Frame):
    """Terminal-styled console log surface.

    Wraps a tk.Text widget with pre-configured tags for log levels, plus a
    scrollbar and a thin top-rule that mimics the klog mock's
    `linear-gradient(90deg, transparent, klog-green, transparent)` accent.

    The Text widget itself is exposed as `.text` for direct manipulation by
    callers (the legacy app callbacks insert/delete on it). This is a
    deliberate choice: rather than wrap every tk.Text method (`insert`,
    `delete`, `tag_add`, `search`, etc.), we expose the underlying widget
    and trust the callers. The styling is the value-add.

    Usage:
        cv = ConsoleView(parent)
        cv.pack(fill='both', expand=True)
        # Then use cv.text.insert(...), cv.text.tag_add(...), etc.
        # The 'error', 'warning', 'info', 'debug', and 'highlight' tags
        # are already configured.

    Args:
        parent: parent widget
        **kwargs: forwarded to the outer Frame (height, width, etc.)
    """

    def __init__(self, parent, **kwargs):
        from tkinter_theme import px, scrollbar_style
        from tkinter import ttk
        from ui.shared.widgets import Rounded
        outside = kwargs.pop('surface', None)
        super().__init__(parent, bg=COLORS['bg_0'], highlightthickness=0,
                         **kwargs)
        # Inset console well (design system › Card inset): bg_0, hairline,
        # monospace log text in the log colours.
        body = tk.Frame(self, bg=COLORS['bg_0'], highlightthickness=1,
                        highlightbackground=COLORS['border_2'])
        body.pack(fill='both', expand=True)
        sb = ttk.Scrollbar(body, orient='vertical',
                           style=scrollbar_style(COLORS['bg_0']))
        sb.pack(side='right', fill='y')
        self.text = tk.Text(body,
                            font=FONTS['code_sm'],
                            bg=COLORS['bg_0'], fg=COLORS['fg_2'],
                            insertbackground=COLORS['fg_1'],
                            selectbackground=COLORS['accent_lo'],
                            selectforeground=COLORS['fg_0'],
                            relief='flat', bd=0, padx=px(12), pady=px(10),
                            state='disabled', wrap='word',
                            yscrollcommand=sb.set,
                            spacing1=1, spacing3=1)
        self.text.pack(side='left', fill='both', expand=True)
        sb.config(command=self.text.yview)
        self.text.tag_configure('error',   foreground=COLORS['danger_hi'])
        self.text.tag_configure('warning', foreground=COLORS['warn_hi'])
        self.text.tag_configure('info',    foreground=COLORS['fg_1'])
        self.text.tag_configure('debug',   foreground=COLORS['fg_4'])
        self.text.tag_configure('highlight', foreground=COLORS['fg_0'],
                                background=COLORS['warn_bg'])

    # ── Convenience pass-through methods ──
    # These mirror the most-used tk.Text methods so callers can write
    # `cv.append(text, tag='info')` instead of three separate calls.
    def append(self, text, tag=None):
        """Append text to the bottom of the log."""
        self.text.config(state='normal')
        if tag:
            self.text.insert('end', text, tag)
        else:
            self.text.insert('end', text)
        self.text.config(state='disabled')

    def clear(self):
        """Erase all log content."""
        self.text.config(state='normal')
        self.text.delete('1.0', 'end')
        self.text.config(state='disabled')

    def set_word_wrap(self, on):
        """Toggle word wrap mode."""
        self.text.config(wrap=('word' if on else 'none'))
