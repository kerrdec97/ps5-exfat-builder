"""
ui/shared/toolbar.py — action-row components.

Components
----------
- IconButton — a compact button: optional leading glyph + label. Styled
               by variant via the Stage 1 BUTTON_STYLES tokens. It is a
               plain tk.Button so hover/disabled colours are fully
               controllable. Accepts a `command` callback OR can be
               created disabled with a "coming later" hint.
- Toolbar    — a horizontal strip that lays out IconButtons (or any
               widgets) with consistent spacing and an optional title.

NO-FAKE-FUNCTIONALITY RULE
--------------------------
IconButton never invents behavior:
  * if `command` is given, it calls exactly that callback;
  * if `command` is None, the button renders DISABLED. Pass
    `coming_later=True` to also show a muted "coming later" styling so
    it is visibly a placeholder rather than a broken control.

Stage 2 (UI refactor): standalone, not wired into the app yet.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING, BUTTON_STYLES,
    get_color, get_font,
)

# variant -> (bg token, fg token, hover-bg token)
_VARIANT_COLORS = {
    "default":   ("bg_4",    "fg_1",      "bg_5"),
    "primary":   ("accent",  "fg_0",      "accent_hi"),
    "secondary": ("bg_4",    "accent",    "accent_08"),
    "ghost":     ("bg_1",    "fg_3",      "bg_3"),
    "warn":      ("warn",    "bg_0",      "warn_hi"),
    "danger":    ("danger",  "fg_0",      "danger_hi"),
    "success":   ("success", "fg_0",      "success_hi"),
    "backport":  ("purple",  "fg_0",      "purple_hi"),
    "purple":    ("purple",  "fg_0",      "purple_hi"),
}


class IconButton(tk.Button):
    """A compact icon+label button.

    Args:
        parent       : container
        label        : button text
        icon         : optional leading glyph
        variant      : a key of _VARIANT_COLORS / BUTTON_STYLES
        command      : real callback. If None -> button is disabled.
        coming_later : if True and command is None, the button shows a
                       "(coming later)" style so it reads as a planned
                       placeholder, not a bug.

    The button performs only the callback it was given. It never calls
    anything implicitly.
    """

    def __init__(self, parent, label="", icon=None, variant="default",
                 command=None, coming_later=False, **kwargs):
        self._variant = variant if variant in _VARIANT_COLORS else "default"
        bg_t, fg_t, hov_t = _VARIANT_COLORS[self._variant]

        text = ("%s  %s" % (icon, label)) if icon else label
        self._disabled_placeholder = (command is None)

        if self._disabled_placeholder:
            # visibly inert — no callback to fake
            bg_hex = get_color("bg_2")
            fg_hex = get_color("fg_6")
            if coming_later:
                text = "%s  ·  coming later" % text
            state = "disabled"
        else:
            bg_hex = get_color(bg_t)
            fg_hex = get_color(fg_t)
            state = "normal"

        kwargs.setdefault("font", get_font("button"))
        kwargs.setdefault("relief", "flat")
        kwargs.setdefault("bd", 0)
        kwargs.setdefault("highlightthickness", 0)
        kwargs.setdefault("padx", SPACING["md"])
        kwargs.setdefault("pady", SPACING["sm"])
        kwargs.setdefault("cursor",
                          "arrow" if self._disabled_placeholder else "hand2")

        super().__init__(parent, text=text, command=command,
                         bg=bg_hex, fg=fg_hex,
                         activebackground=get_color(hov_t),
                         activeforeground=fg_hex,
                         disabledforeground=get_color("fg_6"),
                         state=state, **kwargs)

        self._bg = bg_hex
        self._hover = get_color(hov_t)
        if not self._disabled_placeholder:
            self.bind("<Enter>", self._enter)
            self.bind("<Leave>", self._leave)

    def _enter(self, _e=None):
        self.configure(bg=self._hover)

    def _leave(self, _e=None):
        self.configure(bg=self._bg)

    @property
    def is_placeholder(self):
        """True if this button has no real command (planned/not wired)."""
        return self._disabled_placeholder


class Toolbar(tk.Frame):
    """A horizontal action strip.

    Usage:
        tb = Toolbar(parent, title="Queue actions")
        tb.pack(fill="x")
        tb.add(IconButton(tb.items, label="Clear", icon="\U0001f5d1",
                          variant="ghost", command=app._clear_queue))
        # or add any widget:
        tb.add(some_widget, side="right")

    Toolbar is just layout — it holds widgets the host supplies.
    """

    def __init__(self, parent, title=None, bg="bg_1", **kwargs):
        self._bg = get_color(bg)
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)

        if title:
            tk.Label(self, text=title, bg=self._bg,
                     fg=get_color("fg_4"), font=get_font("label")).pack(
                         side="left", padx=(0, SPACING["md"]))

        self.items = tk.Frame(self, bg=self._bg)
        self.items.pack(side="left", fill="x", expand=True)

    def add(self, widget, side="left", pad=None):
        """Pack a widget into the toolbar. Returns the widget."""
        if pad is None:
            pad = SPACING["xs"]
        widget.pack(side=side, padx=pad, pady=SPACING["xxs"])
        return widget


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("toolbar.py — preview")
    root.configure(bg=COLORS["bg_1"])
    root.geometry("620x180")

    tb = Toolbar(root, title="Queue")
    tb.pack(fill="x", padx=16, pady=20)
    tb.add(IconButton(tb.items, label="Add", icon="\u2795",
                      variant="primary",
                      command=lambda: print("add clicked")))
    tb.add(IconButton(tb.items, label="Build All", icon="\U0001f528",
                      variant="success",
                      command=lambda: print("build all clicked")))
    tb.add(IconButton(tb.items, label="Clear", icon="\U0001f5d1",
                      variant="ghost",
                      command=lambda: print("clear clicked")))

    tb2 = Toolbar(root, title="Planned")
    tb2.pack(fill="x", padx=16)
    # no command -> visibly disabled placeholder, no fake behavior
    tb2.add(IconButton(tb2.items, label="Cloud Sync", icon="\u2601",
                       variant="secondary", command=None,
                       coming_later=True))

    root.mainloop()
