"""
ui/shared/header.py — top-of-window and top-of-page headers.

Components
----------
- AppHeader  — the global app header strip: brand glyph + title +
               version badge + subtitle on the left, and a right-side
               slot for action widgets (notifications, settings, user).
- PageHeader — a lighter per-page header: title + optional subtitle on
               the left, optional action-widget slot on the right.

Both expose an `actions` frame so the host can pack real buttons into
it later. Neither makes a backend call or needs the app instance.

Stage 2 (UI refactor): standalone, not wired into the app yet.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING,
    get_color, get_font,
)
from ui.shared.badges import Badge


class AppHeader(tk.Frame):
    """Global application header strip.

    Usage (later stage):
        hdr = AppHeader(root, title="exFAT Image Builder",
                        version="3.3.0",
                        subtitle="Build PS5 exFAT game images")
        hdr.pack(fill="x")
        # host packs real controls into hdr.actions:
        ttk.Button(hdr.actions, text="Settings",
                   command=app._switch_tab_settings).pack(side="right")

    `hdr.actions` is an empty right-aligned frame. The header invents no
    behavior of its own.
    """

    def __init__(self, parent, title="", version="", subtitle="",
                 icon="\U0001f3ae", **kwargs):
        super().__init__(parent, bg=get_color("bg_2"),
                         highlightthickness=0, bd=0, **kwargs)

        pad = SPACING["lg"]

        # left: brand + title block
        left = tk.Frame(self, bg=get_color("bg_2"))
        left.pack(side="left", padx=pad, pady=pad)

        tk.Label(left, text=icon, bg=get_color("bg_2"),
                 fg=get_color("accent"),
                 font=(get_font("h1")[0], 20, "bold")).pack(side="left",
                                                            padx=(0, SPACING["md"]))

        titles = tk.Frame(left, bg=get_color("bg_2"))
        titles.pack(side="left")

        title_row = tk.Frame(titles, bg=get_color("bg_2"))
        title_row.pack(anchor="w")
        tk.Label(title_row, text=title, bg=get_color("bg_2"),
                 fg=get_color("fg_0"), font=get_font("h2")).pack(side="left")
        if version:
            Badge(title_row, text="v%s" % version,
                  fg="accent_hi", bg="accent_08").pack(
                      side="left", padx=(SPACING["sm"], 0))

        if subtitle:
            tk.Label(titles, text=subtitle, bg=get_color("bg_2"),
                     fg=get_color("fg_4"),
                     font=get_font("meta")).pack(anchor="w",
                                                 pady=(SPACING["xxs"], 0))

        # right: action slot for the host to fill
        self.actions = tk.Frame(self, bg=get_color("bg_2"))
        self.actions.pack(side="right", padx=pad, pady=pad)


class PageHeader(tk.Frame):
    """Per-page header: title + optional subtitle + action slot.

    Usage:
        ph = PageHeader(page, title="Build Queue",
                        subtitle="3 items waiting")
        ph.pack(fill="x")
        # host adds real buttons:
        ttk.Button(ph.actions, text="Clear",
                   command=app._clear_queue).pack(side="right")
    """

    def __init__(self, parent, title="", subtitle="",
                 bg="bg_1", **kwargs):
        bg_hex = get_color(bg)
        super().__init__(parent, bg=bg_hex,
                         highlightthickness=0, bd=0, **kwargs)

        left = tk.Frame(self, bg=bg_hex)
        left.pack(side="left", fill="x", expand=True)

        tk.Label(left, text=title, bg=bg_hex,
                 fg=get_color("fg_0"),
                 font=get_font("h2"), anchor="w").pack(anchor="w")
        if subtitle:
            tk.Label(left, text=subtitle, bg=bg_hex,
                     fg=get_color("fg_4"),
                     font=get_font("label"), anchor="w").pack(
                         anchor="w", pady=(SPACING["xxs"], 0))

        self.actions = tk.Frame(self, bg=bg_hex)
        self.actions.pack(side="right")

    def set_subtitle(self, text):
        # convenience for live counts; only touches this widget
        for child in self.winfo_children():
            pass  # subtitle is internal; recreate not needed for Stage 2


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("header.py — preview")
    root.configure(bg=COLORS["bg_1"])
    root.geometry("720x260")

    hdr = AppHeader(root, title="exFAT Image Builder", version="3.3.0",
                    subtitle="Build PS5 exFAT game images — auto-detected")
    hdr.pack(fill="x")
    tk.Label(hdr.actions, text="\u2699", bg=COLORS["bg_2"],
             fg=COLORS["fg_3"], font=FONTS["h3"]).pack(side="right",
                                                       padx=6)

    body = tk.Frame(root, bg=COLORS["bg_1"])
    body.pack(fill="both", expand=True, padx=20, pady=20)

    ph = PageHeader(body, title="Build Queue", subtitle="3 items waiting")
    ph.pack(fill="x")
    tk.Label(ph.actions, text="[Clear]", bg=COLORS["bg_1"],
             fg=COLORS["fg_3"]).pack(side="right")

    root.mainloop()
