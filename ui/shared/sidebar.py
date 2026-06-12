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
        self.label = label             # kept for compact-mode tooltips (5B)
        self._on_select = on_select
        self._active = False
        self._compact = False

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

    # ── compact (collapsed-rail) mode ─────────────────────────────────
    def set_compact(self, compact):
        """Compact mode (Phase 5A): hide the text label and center the
        icon so the item reads as an icon-only rail entry. Expanding
        restores the label. Purely visual — never affects on_select."""
        compact = bool(compact)
        if compact == self._compact:
            return
        self._compact = compact
        if compact:
            # Hide the label; center the icon within the narrow rail.
            try:
                self._label.pack_forget()
            except Exception:
                pass
            if self._icon:
                self._icon.pack_configure(
                    padx=SPACING["sm"], pady=SPACING["sm"])
        else:
            # Restore icon padding, then re-show the label after it.
            if self._icon:
                self._icon.pack_configure(
                    padx=(SPACING["md"], SPACING["sm"]), pady=SPACING["sm"])
            try:
                self._label.pack(side="left", fill="x", expand=True,
                                 padx=(0 if self._icon else SPACING["md"],
                                       SPACING["md"]),
                                 pady=SPACING["sm"])
            except Exception:
                pass

    # ── state ────────────────────────────────────────────────────────
    def set_active(self, active):
        self._active = bool(active)
        self._render()

    def set_label(self, text):
        """Update the visible label text in place (issue #35 — language
        retranslation). Safe in compact mode: the pack-forgotten Label
        keeps the new text and shows it when the rail expands. Also
        refreshes the compact-mode tooltip source (self.label)."""
        self.label = text
        try:
            self._label.configure(text=text)
        except Exception:
            pass

    def _render(self):
        if self._active:
            bg = get_color("accent_08")       # theme 'active' fill convention
            fg = get_color("accent_hi")
            bar = get_color("accent")
            icon_fg = get_color("accent")
            font = get_font("body_b")
        else:
            bg = get_color("bg_1")
            fg = get_color("fg_3")
            bar = get_color("bg_1")
            icon_fg = get_color("fg_4")
            font = get_font("body")
        self._bar.configure(bg=bar)
        for w in (self, self._inner, self._label):
            w.configure(bg=bg)
        self._label.configure(fg=fg, font=font)
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

    def __init__(self, parent, width=210, rail_width=60, **kwargs):
        super().__init__(parent, bg=get_color("bg_1"), width=width,
                         highlightthickness=0, bd=0, **kwargs)
        self.pack_propagate(False)
        self.on_select = None          # host assigns a callable(key)
        self.on_toggle = None          # host assigns a callable(collapsed)
        self._items = {}               # key -> NavItem
        self._active_key = None
        # Phase 5A — collapsible icon-only rail.
        self._collapsed = False
        self._full_width = width       # expanded width (default 210)
        self._rail_width = rail_width  # collapsed width (icon + accent room)
        self._group_headers = []       # uppercase section labels (hidden compact)
        self._group_divs = []          # hairline dividers (hidden compact)
        self._build_order = []         # _body children in creation order

        # Toggle control pinned at the very top, above the groups.
        self._toggle = tk.Label(self, text="\u2039\u2039",   # ‹‹ expanded
                                bg=get_color("bg_1"), fg=get_color("fg_5"),
                                font=get_font("body"), anchor="e",
                                cursor="hand2")
        self._toggle.pack(side="top", fill="x",
                          padx=SPACING["md"], pady=(SPACING["sm"], 0))
        self._toggle.bind("<Button-1>", lambda _e: self.toggle())
        self._toggle.bind("<Enter>",
                          lambda _e: self._toggle.configure(fg=get_color("fg_3")))
        self._toggle.bind("<Leave>",
                          lambda _e: self._toggle.configure(fg=get_color("fg_5")))

        self._body = tk.Frame(self, bg=get_color("bg_1"))
        self._body.pack(side="top", fill="both", expand=True)

        self._footer = tk.Frame(self, bg=get_color("bg_1"))
        self._footer.pack(side="bottom", fill="x")

    # ── building ─────────────────────────────────────────────────────
    def add_group(self, title):
        """Add an uppercase section header. Non-first groups get extra
        breathing room and a hairline divider (presentation only)."""
        first = not getattr(self, "_has_groups", False)
        self._has_groups = True
        if not first:
            div = tk.Frame(self._body, bg=get_color("border_2"), height=1)
            div.pack(fill="x", padx=SPACING["md"], pady=(SPACING["lg"], 0))
            self._group_divs.append(div)
            self._build_order.append(div)
        lbl = tk.Label(self._body, text=title.upper(),
                       bg=get_color("bg_1"), fg=get_color("fg_5"),
                       font=get_font("eyebrow"), anchor="w")
        lbl.pack(fill="x", padx=SPACING["md"],
                 pady=((SPACING["lg"] if first else SPACING["sm"]),
                       SPACING["xs"]))
        self._group_headers.append(lbl)
        self._build_order.append(lbl)
        return lbl

    def add_item(self, key, label, icon=None):
        """Add a NavItem. Clicking it routes through self._dispatch."""
        item = NavItem(self._body, key, label, icon=icon,
                       on_select=self._dispatch)
        item.pack(fill="x")
        self._items[key] = item
        self._build_order.append(item)
        # If the rail is already collapsed (e.g. items added after a
        # restore), keep the new item consistent.
        if self._collapsed:
            item.set_compact(True)
        return item

    def footer(self):
        """Return the pinned bottom frame for the host to fill
        (e.g. a system-status panel). Empty by default."""
        return self._footer

    # ── retranslation (issue #35) ─────────────────────────────────────
    def set_item_label(self, key, text):
        """Update one NavItem's visible text in place. The sidebar is
        never rebuilt for a language change, so routing, the active
        highlight and the collapsed state are untouched. Unknown keys
        are a no-op (presentation only — never raises)."""
        item = self._items.get(key)
        if item is None:
            return
        try:
            item.set_label(text)
        except Exception:
            pass

    def set_group_labels(self, titles):
        """Replace the group header texts in creation order. Shorter or
        longer lists are zipped safely; extra entries on either side are
        ignored (presentation only — never raises)."""
        for lbl, title in zip(self._group_headers, titles):
            try:
                lbl.configure(text=str(title).upper())
            except Exception:
                pass

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

    # ── collapse / expand (Phase 5A) ──────────────────────────────────
    @property
    def collapsed(self):
        return self._collapsed

    def toggle(self):
        """User-driven collapse/expand; persists via on_toggle."""
        self.set_collapsed(not self._collapsed, persist=True)

    def set_collapsed(self, collapsed, persist=False):
        """Switch the rail between full (labelled) and narrow (icon-only).
        persist=False is used to restore the saved state at startup
        without re-firing on_toggle."""
        collapsed = bool(collapsed)
        self._collapsed = collapsed
        self._apply_collapsed()
        if persist and callable(self.on_toggle):
            try:
                self.on_toggle(collapsed)
            except Exception:
                pass

    def _apply_collapsed(self):
        """Apply the current collapsed state: width, item labels, group
        headers/dividers, footer visibility, toggle glyph. Visual only."""
        if self._collapsed:
            self.configure(width=self._rail_width)
            for item in self._items.values():
                item.set_compact(True)
            # Hide group headers + dividers (grouping implied by gaps).
            for w in self._group_headers + self._group_divs:
                try:
                    w.pack_forget()
                except Exception:
                    pass
            # Hide footer contents (keep the frame for layout stability).
            try:
                self._footer.pack_forget()
            except Exception:
                pass
            self._toggle.configure(text="\u203a\u203a", anchor="center")  # ››
        else:
            self.configure(width=self._full_width)
            for item in self._items.values():
                item.set_compact(False)
            # Re-show headers/dividers in original order is non-trivial via
            # pack; simplest robust approach is to re-pack each in the order
            # they were created. They were interleaved with items, but pack
            # order within _body is preserved by re-packing headers/divs
            # relative to existing children using their original sequence.
            self._repack_groups_expanded()
            try:
                self._footer.pack(side="bottom", fill="x")
            except Exception:
                pass
            self._toggle.configure(text="\u2039\u2039", anchor="e")  # ‹‹

    def _repack_groups_expanded(self):
        """Re-show group headers and dividers when expanding. Because pack
        appends to the end, we rebuild the _body child order: walk the
        recorded creation sequence and re-pack each header/divider before
        the items that follow it. Simpler and reliable: re-pack every
        header/div, then re-pack all items after — but that reorders. To
        avoid reordering, we instead never destroyed them and only
        pack_forget()'d; re-packing in original creation order relative to
        the items requires the items still be packed. We achieve correct
        order by re-packing headers/divs and items together in the stored
        build order."""
        # Rebuild full body order from the original build sequence.
        for w in self._body.winfo_children():
            try:
                w.pack_forget()
            except Exception:
                pass
        for w in self._build_order:
            try:
                if w in self._group_divs:
                    w.pack(fill="x", padx=SPACING["md"],
                           pady=(SPACING["lg"], 0))
                elif w in self._group_headers:
                    first = (w is self._group_headers[0])
                    w.pack(fill="x", padx=SPACING["md"],
                           pady=((SPACING["lg"] if first else SPACING["sm"]),
                                 SPACING["xs"]))
                else:  # NavItem
                    w.pack(fill="x")
            except Exception:
                pass


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
