"""
ui/shared/empty_state.py — placeholder and section-grouping components.

Components
----------
- EmptyState   — a centered "nothing here yet" placeholder: large glyph,
                 a title line, a muted description, and an optional
                 action-button slot. For empty lists/grids.
- SectionGroup — a labelled container for grouping related controls
                 (e.g. a Settings section). Uppercase eyebrow title +
                 optional description + a `.body` frame.

Both are pure presentation: no backend calls, no app instance required.

Stage 2 (UI refactor): standalone, not wired into the app yet.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING,
    get_color, get_font,
)


class EmptyState(tk.Frame):
    """Centered empty-list placeholder (design system › Table empty state):
    a 56px rounded tile with the icon, a subtitle-style title, one line of
    description and an `actions` frame the host fills.

    `icon` may be an icon name from ui.icons or a legacy emoji.
    """

    def __init__(self, parent, icon="\u2014", title="Nothing here yet",
                 description="", bg="bg_1", **kwargs):
        from tkinter_theme import px
        from ui.shared.widgets import Rounded, IconLabel
        from ui.icons import leading_icon, has_icon
        self._bg = get_color(bg) if not str(bg).startswith('#') else bg
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)
        center = tk.Frame(self, bg=self._bg)
        center.place(relx=0.5, rely=0.5, anchor="center")
        name = icon if has_icon(str(icon)) else (leading_icon(str(icon)) or "box")
        tile = Rounded(center, bg=COLORS["bg_3"], radius=8, surface=self._bg)
        tile.pack()
        IconLabel(tile, name, 24, COLORS["fg_3"], COLORS["bg_3"]).pack(
            padx=px(16), pady=px(16))
        tk.Label(center, text=title, bg=self._bg, fg=COLORS["fg_0"],
                 font=FONTS["subtitle"]).pack(pady=(px(14), 0))
        if description:
            tk.Label(center, text=description, bg=self._bg,
                     fg=COLORS["fg_3"], font=FONTS["body"],
                     wraplength=px(420), justify="center").pack(
                         pady=(px(4), 0))
        self.actions = tk.Frame(center, bg=self._bg)
        self.actions.pack(pady=(px(14), 0))


class SectionGroup(tk.Frame):
    """A labelled grouping: overline title, optional caption, `.body`."""

    def __init__(self, parent, title="", description="",
                 bg="bg_1", **kwargs):
        from tkinter_theme import px
        self._bg = get_color(bg) if not str(bg).startswith('#') else bg
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)
        if title:
            tk.Label(self, text=title.upper(), bg=self._bg,
                     fg=COLORS["fg_4"], font=FONTS["overline"],
                     anchor="w").pack(fill="x")
        if description:
            tk.Label(self, text=description, bg=self._bg,
                     fg=COLORS["fg_3"], font=FONTS["caption"],
                     anchor="w", justify="left").pack(
                         fill="x", pady=(px(2), 0))
        tk.Frame(self, bg=COLORS["border_2"], height=1).pack(
            fill="x", pady=(px(8), px(12)))
        self.body = tk.Frame(self, bg=self._bg)
        self.body.pack(fill="both", expand=True)


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("empty_state.py — preview")
    root.configure(bg=COLORS["bg_1"])
    root.geometry("560x440")

    sg = SectionGroup(root, title="Library",
                      description="Folders scanned for game dumps.")
    sg.pack(fill="x", padx=20, pady=20)
    tk.Label(sg.body, text="(host fills this body frame)",
             bg=COLORS["bg_1"], fg=COLORS["fg_4"],
             font=FONTS["body"]).pack(anchor="w")

    es = EmptyState(root, icon="\U0001f4da",
                    title="No folders added yet",
                    description="Add a folder to scan for game dumps.")
    es.pack(fill="both", expand=True, padx=20, pady=(0, 20))
    tk.Label(es.actions, text="[Add Folder]", bg=COLORS["bg_1"],
             fg=COLORS["accent_hi"]).pack()

    root.mainloop()
