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
    """A centered empty-list placeholder.

    Usage:
        es = EmptyState(parent, icon="\U0001f4e6",
                        title="No images built yet",
                        description="Build an image to see it here.")
        es.pack(fill="both", expand=True)
        # optional real action:
        from ui.shared.toolbar import IconButton
        IconButton(es.actions, label="Build Now", variant="primary",
                   command=app._switch_tab_build).pack()

    `es.actions` is empty by default; the host fills it (or leaves it).
    """

    def __init__(self, parent, icon="\u2014", title="Nothing here yet",
                 description="", bg="bg_1", **kwargs):
        self._bg = get_color(bg)
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)

        center = tk.Frame(self, bg=self._bg)
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(center, text=icon, bg=self._bg,
                 fg=get_color("fg_5"),
                 font=(get_font("h1")[0], 34, "normal")).pack()

        tk.Label(center, text=title, bg=self._bg,
                 fg=get_color("fg_2"), font=get_font("h3")).pack(
                     pady=(SPACING["md"], 0))

        if description:
            tk.Label(center, text=description, bg=self._bg,
                     fg=get_color("fg_5"), font=get_font("body"),
                     wraplength=360, justify="center").pack(
                         pady=(SPACING["xs"], 0))

        self.actions = tk.Frame(center, bg=self._bg)
        self.actions.pack(pady=(SPACING["lg"], 0))


class SectionGroup(tk.Frame):
    """A labelled grouping container for related controls.

    Usage:
        sg = SectionGroup(parent, title="FTP Connection",
                          description="Settings for PS5 transfers.")
        sg.pack(fill="x")
        # host fills sg.body:
        SomeFieldRow(sg.body, ...).pack(fill="x")

    Renders an uppercase eyebrow title + optional description + a hair-
    line, with the content area in `.body`.
    """

    def __init__(self, parent, title="", description="",
                 bg="bg_1", **kwargs):
        self._bg = get_color(bg)
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)

        if title:
            tk.Label(self, text=title.upper(), bg=self._bg,
                     fg=get_color("accent"), font=get_font("eyebrow"),
                     anchor="w").pack(fill="x")
        if description:
            tk.Label(self, text=description, bg=self._bg,
                     fg=get_color("fg_4"), font=get_font("label"),
                     anchor="w", justify="left").pack(
                         fill="x", pady=(SPACING["xxs"], 0))

        # hairline divider
        tk.Frame(self, bg=get_color("border_2"), height=1).pack(
            fill="x", pady=(SPACING["sm"], SPACING["md"]))

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
