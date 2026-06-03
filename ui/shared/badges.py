"""
ui/shared/badges.py — small label badges for status/metadata.

Components
----------
- Badge        — a generic small tinted pill label. Caller picks fg/bg
                 tokens, or passes a STATUS_COLORS key via `status=`.
- StatusBadge  — a Badge specialised to one of the STATUS_COLORS keys
                 (idle / running / success / warn / error / ...). Has a
                 `.set_status(name, text=None)` method for live updates.

Stage 2 (UI refactor): standalone, not wired into any tab yet. Pulls all
colors from tkinter_theme tokens — no hex literals. Safe to import on its
own; needs no app instance.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING, STATUS_COLORS,
    get_color, get_font, get_status_color, apply_status_badge_style,
)


class Badge(tk.Label):
    """A small tinted pill-style label.

    Usage:
        Badge(parent, text="3 items").pack()
        Badge(parent, text="Beta", fg="accent_hi", bg="accent_08").pack()
        Badge(parent, text="Done", status="success").pack()

    `status` (if given) overrides fg/bg using STATUS_COLORS. tk has no
    true rounded corners; padding + tint reads as a pill on the dark UI.
    """

    def __init__(self, parent, text="", status=None,
                 fg="fg_4", bg="bg_3", **kwargs):
        if status is not None and status in STATUS_COLORS:
            fg_hex = get_status_color(status, "fg")
            bg_hex = get_status_color(status, "bg")
        else:
            fg_hex = get_color(fg, "fg_4")
            bg_hex = get_color(bg, "bg_3")
        kwargs.setdefault("font", get_font("eyebrow"))
        kwargs.setdefault("padx", SPACING["sm"])
        kwargs.setdefault("pady", SPACING["xxs"])
        kwargs.setdefault("bd", 0)
        kwargs.setdefault("highlightthickness", 0)
        super().__init__(parent, text=" %s " % text,
                         fg=fg_hex, bg=bg_hex, **kwargs)

    def set_text(self, text):
        self.configure(text=" %s " % text)


class StatusBadge(Badge):
    """A Badge bound to a STATUS_COLORS status word.

    Usage:
        b = StatusBadge(parent, "running", text="Building")
        b.pack()
        ...
        b.set_status("success", text="Done")

    Default label text is the status word title-cased if none is given.
    """

    def __init__(self, parent, status="idle", text=None, **kwargs):
        self._status = status if status in STATUS_COLORS else "idle"
        label = text if text is not None else self._status.title()
        super().__init__(parent, text=label, status=self._status, **kwargs)

    def set_status(self, status, text=None):
        """Update the badge to a new status (and optionally new text)."""
        self._status = status if status in STATUS_COLORS else "idle"
        apply_status_badge_style(self, self._status)
        if text is not None:
            self.set_text(text)
        else:
            self.set_text(self._status.title())

    @property
    def status(self):
        return self._status


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("badges.py — preview")
    root.configure(bg=COLORS["bg_1"])
    root.geometry("420x180")

    row1 = tk.Frame(root, bg=COLORS["bg_1"])
    row1.pack(pady=16)
    for st in ("idle", "queued", "running", "success", "warn", "error"):
        StatusBadge(row1, st).pack(side="left", padx=4)

    row2 = tk.Frame(root, bg=COLORS["bg_1"])
    row2.pack(pady=8)
    Badge(row2, text="3 items").pack(side="left", padx=4)
    Badge(row2, text="v3.3.0", fg="accent_hi", bg="accent_08").pack(
        side="left", padx=4)

    live = StatusBadge(root, "running", text="Building…")
    live.pack(pady=12)
    root.after(1500, lambda: live.set_status("success", "Done"))

    root.mainloop()
