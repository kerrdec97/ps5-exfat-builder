"""
ui/shared/dashboard.py — dashboard surface components.

Components
----------
- DashboardCard — a bordered content card with an optional title row
                  (title + optional action widget) and a `.body` frame
                  the caller fills with anything.
- StatCard      — a compact metric tile: icon + big value + caption,
                  optional status accent. For the dashboard's quick
                  stat row. Has `.set_value()` for live updates.

These mirror the cards in the uploaded dashboard mock. They render with
a clean flat/dark premium look (no gradients, per project decision).
Pull all styling from tkinter_theme tokens. No backend calls; no app
instance needed. Safe to import alone.

Stage 2 (UI refactor): standalone, not wired into the app yet.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING, CARD_STYLES,
    get_color, get_font, get_status_color,
)


class DashboardCard(tk.Frame):
    """A bordered card surface with an optional header and a body frame.

    Usage:
        card = DashboardCard(parent, title="Recent Builds")
        card.pack(fill="x")
        # fill card.body with whatever content:
        tk.Label(card.body, text="...").pack()
        # optionally drop a real button in the header action slot:
        ttk.Button(card.actions, text="View All",
                   command=app._switch_tab_history).pack(side="right")

    `variant` is a key of tkinter_theme.CARD_STYLES.
    """

    def __init__(self, parent, title=None, variant="default", **kwargs):
        spec = CARD_STYLES.get(variant, CARD_STYLES["default"])
        self._bg = get_color(spec["bg"])
        self._border = get_color(spec["border"])
        super().__init__(parent, bg=self._bg,
                         highlightbackground=self._border,
                         highlightcolor=self._border,
                         highlightthickness=1, bd=0, **kwargs)

        pad = spec["pad"]
        self.actions = None

        if title is not None:
            header = tk.Frame(self, bg=self._bg)
            header.pack(fill="x", padx=pad, pady=(pad, SPACING["sm"]))
            tk.Label(header, text=title, bg=self._bg,
                     fg=get_color("fg_0"), font=get_font("h3"),
                     anchor="w").pack(side="left")
            self.actions = tk.Frame(header, bg=self._bg)
            self.actions.pack(side="right")
            body_pady = (0, pad)
        else:
            body_pady = (pad, pad)

        self.body = tk.Frame(self, bg=self._bg)
        self.body.pack(fill="both", expand=True, padx=pad, pady=body_pady)


class StatCard(tk.Frame):
    """A compact metric tile: icon, large value, caption.

    Usage:
        sc = StatCard(parent, icon="\U0001f4be", value="12",
                      caption="Built Images", status="success")
        sc.pack(side="left")
        ...
        sc.set_value("13")           # live update

    `status` (optional, a STATUS_COLORS key) tints the icon + value.
    Purely presentational — holds no logic.
    """

    def __init__(self, parent, icon="", value="", caption="",
                 status=None, **kwargs):
        self._bg = get_color("bg_2")
        self._border = get_color("border_3")
        super().__init__(parent, bg=self._bg,
                         highlightbackground=self._border,
                         highlightcolor=self._border,
                         highlightthickness=1, bd=0, **kwargs)

        accent = (get_status_color(status, "fg") if status
                  else get_color("accent"))
        pad = SPACING["md"]

        top = tk.Frame(self, bg=self._bg)
        top.pack(fill="x", padx=pad, pady=(pad, 0))
        if icon:
            tk.Label(top, text=icon, bg=self._bg, fg=accent,
                     font=get_font("h2")).pack(side="left")

        self._value_lbl = tk.Label(self, text=str(value), bg=self._bg,
                                   fg=get_color("fg_0"),
                                   font=get_font("h1"), anchor="w")
        self._value_lbl.pack(fill="x", padx=pad, pady=(SPACING["xs"], 0))

        tk.Label(self, text=caption, bg=self._bg,
                 fg=get_color("fg_4"), font=get_font("label"),
                 anchor="w").pack(fill="x", padx=pad,
                                  pady=(0, pad))

    def set_value(self, value):
        """Update the displayed metric value."""
        self._value_lbl.configure(text=str(value))


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("dashboard.py — preview")
    root.configure(bg=COLORS["bg_1"])
    root.geometry("640x420")

    stats = tk.Frame(root, bg=COLORS["bg_1"])
    stats.pack(fill="x", padx=16, pady=16)
    StatCard(stats, icon="\U0001f4be", value="12",
             caption="Built Images", status="success").pack(
                 side="left", fill="both", expand=True, padx=4)
    StatCard(stats, icon="\U0001f528", value="3",
             caption="In Queue", status="running").pack(
                 side="left", fill="both", expand=True, padx=4)
    StatCard(stats, icon="\u26a0", value="1",
             caption="Errors", status="error").pack(
                 side="left", fill="both", expand=True, padx=4)

    card = DashboardCard(root, title="Recent Builds")
    card.pack(fill="both", expand=True, padx=16, pady=(0, 16))
    tk.Label(card.body, text="(host fills this body frame)",
             bg=card._bg, fg=COLORS["fg_4"],
             font=FONTS["body"]).pack(anchor="w")
    tk.Label(card.actions, text="[View All]", bg=card._bg,
             fg=COLORS["fg_3"]).pack(side="right")

    root.mainloop()
