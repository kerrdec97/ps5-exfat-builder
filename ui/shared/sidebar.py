"""
ui/shared/sidebar.py — left navigation rail components.

Components
----------
- NavItem  — a single clickable nav row: optional icon glyph + label,
             active/idle states, hover feedback. Fires an on_select
             callback with its key when clicked.
- Sidebar  — a vertical rail that holds grouped NavItems plus an
             optional pinned footer widget area. Tracks the active key
             and exposes `select(key)` / `on_select` for the host.

Design intent (matches the uploaded dashboard mock): grouped sections
("WORKSPACE", "MAIN TOOLS", "ADVANCED") with an accent bar on the active
item and a status panel pinned at the bottom.

Stage 2 (UI refactor): standalone, NOT wired into the app. The host app
will later create a Sidebar, add items, and point on_select at the real
`_switch_tab`. No backend calls are made here. Safe to import alone.
"""

import tkinter as tk

from tkinter_theme import (
    COLORS, FONTS, SPACING,
    get_color, get_font,
)


class NavItem(tk.Frame):
    """One row in the sidebar.

    Args:
        parent   : container widget
        key      : string id passed back through on_select
        label    : visible text
        icon     : optional leading glyph (str), e.g. an emoji
        on_select: callable(key) invoked on click — optional. If None,
                   the item is purely visual (no fake behavior).

    The item never invents functionality: with no on_select it is inert.
    """

    def __init__(self, parent, key, label, icon=None, on_select=None,
                 **kwargs):
        super().__init__(parent, bg=get_color("bg_1"),
                         highlightthickness=0, bd=0, **kwargs)
        self.key = key
        self._on_select = on_select
        self._active = False

        # accent bar on the left edge — shown only when active
        self._bar = tk.Frame(self, bg=get_color("bg_1"), width=3)
        self._bar.pack(side="left", fill="y")

        self._inner = tk.Frame(self, bg=get_color("bg_1"))
        self._inner.pack(side="left", fill="both", expand=True)

        pad = SPACING["sm"]
        if icon:
            self._icon = tk.Label(self._inner, text=icon,
                                  bg=get_color("bg_1"),
                                  fg=get_color("fg_4"),
                                  font=get_font("body"))
            self._icon.pack(side="left", padx=(SPACING["md"], SPACING["sm"]),
                            pady=pad)
        else:
            self._icon = None

        self._label = tk.Label(self._inner, text=label,
                               bg=get_color("bg_1"),
                               fg=get_color("fg_3"),
                               font=get_font("body"), anchor="w")
        self._label.pack(side="left", fill="x", expand=True,
                         padx=(0 if icon else SPACING["md"], SPACING["md"]),
                         pady=pad)

        for w in (self, self._inner, self._label,
                  *( [self._icon] if self._icon else [] )):
            w.bind("<Button-1>", self._click)
            w.bind("<Enter>", self._enter)
            w.bind("<Leave>", self._leave)
            w.configure(cursor="hand2")

    # ── state ────────────────────────────────────────────────────────
    def set_active(self, active):
        self._active = bool(active)
        self._render()

    def _render(self):
        if self._active:
            bg = get_color("bg_3")
            fg = get_color("accent_hi")
            bar = get_color("accent")
            icon_fg = get_color("accent")
        else:
            bg = get_color("bg_1")
            fg = get_color("fg_3")
            bar = get_color("bg_1")
            icon_fg = get_color("fg_4")
        self._bar.configure(bg=bar)
        for w in (self, self._inner, self._label):
            w.configure(bg=bg)
        self._label.configure(fg=fg)
        if self._icon:
            self._icon.configure(bg=bg, fg=icon_fg)

    # ── events ───────────────────────────────────────────────────────
    def _enter(self, _e=None):
        if not self._active:
            for w in (self, self._inner, self._label):
                w.configure(bg=get_color("bg_2"))
            if self._icon:
                self._icon.configure(bg=get_color("bg_2"))

    def _leave(self, _e=None):
        if not self._active:
            self._render()

    def _click(self, _e=None):
        if callable(self._on_select):
            self._on_select(self.key)


class Sidebar(tk.Frame):
    """Vertical navigation rail holding grouped NavItems.

    Usage (host app, a later stage):
        sb = Sidebar(parent, width=210)
        sb.on_select = app._switch_tab          # real callback
        sb.add_group("WORKSPACE")
        sb.add_item("dashboard", "Dashboard", icon="\u25a3")
        sb.add_item("quickstart", "Quick Start", icon="\u26a1")
        sb.add_group("MAIN TOOLS")
        sb.add_item("build", "Build Image", icon="\U0001f528")
        sb.select("dashboard")
        footer = sb.footer()                    # pinned bottom area
        ...populate footer...

    The Sidebar makes no backend calls. on_select is whatever the host
    assigns; until then, clicking items is inert.
    """

    def __init__(self, parent, width=210, **kwargs):
        super().__init__(parent, bg=get_color("bg_1"), width=width,
                         highlightthickness=0, bd=0, **kwargs)
        self.pack_propagate(False)
        self.on_select = None          # host assigns a callable(key)
        self._items = {}               # key -> NavItem
        self._active_key = None

        self._body = tk.Frame(self, bg=get_color("bg_1"))
        self._body.pack(side="top", fill="both", expand=True)

        self._footer = tk.Frame(self, bg=get_color("bg_1"))
        self._footer.pack(side="bottom", fill="x")

    # ── building ─────────────────────────────────────────────────────
    def add_group(self, title):
        """Add an uppercase section header."""
        lbl = tk.Label(self._body, text=title.upper(),
                       bg=get_color("bg_1"), fg=get_color("fg_5"),
                       font=get_font("eyebrow"), anchor="w")
        lbl.pack(fill="x", padx=SPACING["md"],
                 pady=(SPACING["lg"], SPACING["xs"]))
        return lbl

    def add_item(self, key, label, icon=None):
        """Add a NavItem. Clicking it routes through self._dispatch."""
        item = NavItem(self._body, key, label, icon=icon,
                       on_select=self._dispatch)
        item.pack(fill="x")
        self._items[key] = item
        return item

    def footer(self):
        """Return the pinned bottom frame for the host to fill
        (e.g. a system-status panel). Empty by default."""
        return self._footer

    # ── selection ────────────────────────────────────────────────────
    def _dispatch(self, key):
        self.select(key)
        if callable(self.on_select):
            self.on_select(key)

    def select(self, key):
        """Mark `key` active (visual only — does not call on_select)."""
        self._active_key = key
        for k, item in self._items.items():
            item.set_active(k == key)

    @property
    def active_key(self):
        return self._active_key


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.title("sidebar.py — preview")
    root.configure(bg=COLORS["bg_0"])
    root.geometry("260x520")

    sb = Sidebar(root, width=220)
    sb.pack(side="left", fill="y")
    sb.on_select = lambda k: print("selected:", k)

    sb.add_group("Workspace")
    sb.add_item("dashboard", "Dashboard", icon="\u25a3")
    sb.add_item("quickstart", "Quick Start", icon="\u26a1")
    sb.add_group("Main Tools")
    sb.add_item("build", "Build Image", icon="\U0001f528")
    sb.add_item("library", "Game Library", icon="\U0001f4da")
    sb.add_group("Advanced")
    sb.add_item("advanced", "Advanced Options", icon="\u2699")
    sb.select("dashboard")

    tk.Label(sb.footer(), text="System: Healthy",
             bg=COLORS["bg_1"], fg=COLORS["success_hi"],
             font=FONTS["meta"]).pack(padx=12, pady=12, anchor="w")

    root.mainloop()
