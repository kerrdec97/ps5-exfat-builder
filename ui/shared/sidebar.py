"""
ui/shared/sidebar.py — the application's left navigation rail.

Design system › Sidebar: 232px rail on bg_0 (56px compact), grouped items
under overline labels, 36px items with a 16px icon. The selected item gets
a bg_3 fill, fg_0 semibold label, an accent_hi icon and a 3px accent pill on
its left edge; hover is a bg_2 fill. Compact mode hides labels, group
labels and the footer and shows the label as a tooltip.

Public API (unchanged, used by exfat_builder.py):
    Sidebar(parent, width=232, rail_width=56)
    .add_group(title) / .add_item(key, label, icon=None) / .footer()
    .select(key) / .active_key / .on_select = callable(key)
    .toggle() / .set_collapsed(bool, persist=False) / .collapsed
    .on_toggle = callable(collapsed)
    .set_item_label(key, text) / .set_group_labels([...])

`icon` may be an icon name from ui.icons or a legacy emoji (mapped to the
matching icon).
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS, px, SIZES, get_color, get_font
from ui.icons import icon_image, EMOJI_ICON, has_icon

C = COLORS

# Sidebar key -> icon (wins over whatever glyph the caller passes).
KEY_ICONS = {
    'home': 'home', 'unibuild': 'build', 'extract': 'extract', 'edit': 'edit',
    'library': 'library', 'ps5': 'ps5', 'convert': 'convert',
    'dumps': 'rename', 'backports': 'backports', 'advanced': 'advanced',
    'hist': 'history', 'settings': 'settings', 'about': 'credits',
    'report': 'bug', 'help': 'help',
}


def _resolve_icon(key, icon):
    if key in KEY_ICONS:
        return KEY_ICONS[key]
    if icon and has_icon(icon):
        return icon
    if icon:
        return EMOJI_ICON.get(icon.strip()[:1], 'circle')
    return None


class _Tip:
    """Tooltip used for compact-mode labels."""

    def __init__(self, widget, get_text):
        self.widget, self.get_text = widget, get_text
        self.enabled = False
        self.tw = None
        self._after = None

    def enter(self):
        if self.enabled:
            self._after = self.widget.after(450, self.show)

    def leave(self):
        if self._after:
            try:
                self.widget.after_cancel(self._after)
            except Exception:
                pass
            self._after = None
        if self.tw:
            try:
                self.tw.destroy()
            except Exception:
                pass
            self.tw = None

    def show(self):
        if self.tw or not self.enabled:
            return
        try:
            w = self.widget
            x = w.winfo_rootx() + w.winfo_width() + px(6)
            y = w.winfo_rooty() + px(6)
            tw = tk.Toplevel(w)
            tw.wm_overrideredirect(True)
            tw.configure(bg=C['border_3'])
            tk.Label(tw, text=self.get_text(), bg=C['bg_3'], fg=C['fg_1'],
                     font=FONTS['caption'], padx=px(8), pady=px(4)
                     ).pack(padx=1, pady=1)
            tw.wm_geometry('+%d+%d' % (x, y))
            self.tw = tw
        except Exception:
            self.tw = None


class NavItem(tk.Frame):
    """One 36px row in the sidebar."""

    def __init__(self, parent, key, label, icon=None, on_select=None,
                 rail_bg=None, **kwargs):
        self._rail_bg = rail_bg or C['bg_0']
        super().__init__(parent, bg=self._rail_bg, highlightthickness=0,
                         bd=0, height=px(SIZES['nav_item']), **kwargs)
        self.pack_propagate(False)
        self.key = key
        self.label = label
        self._on_select = on_select
        self._active = False
        self._hover = False
        self._compact = False
        self._icon_name = _resolve_icon(key, icon)

        from ui.shared.widgets import Rounded
        self._bgf = Rounded(self, bg=self._rail_bg, radius=4,
                            surface=self._rail_bg)
        self._bgf.place(x=0, y=0, relwidth=1, relheight=1)
        self._bar = tk.Frame(self._bgf, bg=self._rail_bg, width=px(3),
                             height=px(16))
        self._icon = tk.Label(self._bgf, bg=self._rail_bg, bd=0)
        self._label = tk.Label(self._bgf, text=label, bg=self._rail_bg,
                               fg=C['fg_2'], font=FONTS['body'], anchor='w')
        self._layout()
        self._tip = _Tip(self, lambda: self.label)
        for w in (self, self._bgf, self._icon, self._label):
            w.bind('<Button-1>', self._click)
            w.bind('<Enter>', self._enter)
            w.bind('<Leave>', self._leave)
            w.configure(cursor='hand2')
        self._render()

    def _layout(self):
        for w in (self._icon, self._label):
            w.place_forget()
        self._bar.place(x=0, rely=0.5, anchor='w')
        if self._compact:
            self._icon.place(relx=0.5, rely=0.5, anchor='center')
        else:
            self._icon.place(x=px(12), rely=0.5, anchor='w')
            self._label.place(x=px(40), rely=0.5, anchor='w')

    def set_compact(self, compact):
        compact = bool(compact)
        if compact == self._compact:
            return
        self._compact = compact
        self._tip.enabled = compact
        self._layout()

    def set_active(self, active):
        self._active = bool(active)
        self._render()

    def set_label(self, text):
        self.label = text
        try:
            self._label.configure(text=text)
        except Exception:
            pass

    def _render(self):
        if self._active:
            bg, fg, ifg, font, bar = (C['bg_3'], C['fg_0'], C['accent_hi'],
                                      FONTS['body_strong'], C['accent'])
        elif self._hover:
            bg, fg, ifg, font, bar = (C['bg_2'], C['fg_0'], C['fg_1'],
                                      FONTS['body'], C['bg_2'])
        else:
            bg, fg, ifg, font, bar = (self._rail_bg, C['fg_2'], C['fg_3'],
                                      FONTS['body'], self._rail_bg)
        self._bgf.set_style(bg=bg)
        self._bar.configure(bg=bar)
        self._label.configure(bg=bg, fg=fg, font=font)
        if self._icon_name:
            self._icon.configure(image=icon_image(self._icon_name, 16, ifg, bg),
                                 bg=bg)
        self._bar.lift()

    def _enter(self, _e=None):
        self._hover = True
        self._render()
        self._tip.enter()

    def _leave(self, _e=None):
        # Leave fires when moving between child widgets; only clear the
        # hover once the pointer has really left the row.
        try:
            x, y = self.winfo_pointerxy()
            w = self.winfo_containing(x, y)
            while w is not None:
                if w is self:
                    return
                w = w.master
        except Exception:
            pass
        self._hover = False
        self._render()
        self._tip.leave()

    def _click(self, _e=None):
        self._tip.leave()
        if callable(self._on_select):
            self._on_select(self.key)


class Sidebar(tk.Frame):
    """Vertical navigation rail holding grouped NavItems."""

    def __init__(self, parent, width=None, rail_width=None, **kwargs):
        width = px(SIZES['sidebar']) if not width or width < 220 else px(width)
        rail_width = px(SIZES['sidebar_compact'])
        super().__init__(parent, bg=C['bg_0'], width=width,
                         highlightthickness=0, bd=0, **kwargs)
        self.pack_propagate(False)
        self.on_select = None
        self.on_toggle = None
        self._items = {}
        self._active_key = None
        self._collapsed = False
        self._full_width = width
        self._rail_width = rail_width
        self._group_headers = []
        self._group_divs = []
        self._build_order = []
        # Kept for compatibility: the collapse control now lives in the app
        # header (hamburger), as in Windows 11 navigation.
        self._toggle = tk.Label(self, bg=C['bg_0'])

        self._footer = tk.Frame(self, bg=C['bg_0'])
        self._footer.pack(side='bottom', fill='x')
        self._footer_rule = tk.Frame(self, bg=C['border_2'], height=1)
        self._footer_rule.pack(side='bottom', fill='x')
        self._body = tk.Frame(self, bg=C['bg_0'])
        self._body.pack(side='top', fill='both', expand=True,
                        padx=px(8), pady=(px(8), 0))

    # ── building ─────────────────────────────────────────────────────
    def add_group(self, title):
        first = not getattr(self, '_has_groups', False)
        self._has_groups = True
        lbl = tk.Label(self._body, text=title.upper(), bg=C['bg_0'],
                       fg=C['fg_4'], font=FONTS['overline'], anchor='w')
        lbl.pack(fill='x', padx=px(12), pady=(px(14), px(4)))
        self._group_headers.append(lbl)
        self._build_order.append(lbl)
        div = tk.Frame(self._body, bg=C['border_2'], height=1)
        self._group_divs.append(div)
        self._build_order.append(div)
        return lbl

    def add_item(self, key, label, icon=None):
        item = NavItem(self._body, key, label, icon=icon,
                       on_select=self._dispatch)
        item.pack(fill='x', pady=(0, px(2)))
        self._items[key] = item
        self._build_order.append(item)
        if self._collapsed:
            item.set_compact(True)
        return item

    def footer(self):
        return self._footer

    # ── retranslation ────────────────────────────────────────────────
    def set_item_label(self, key, text):
        item = self._items.get(key)
        if item is None:
            return
        try:
            item.set_label(text)
        except Exception:
            pass

    def set_group_labels(self, titles):
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
        self._active_key = key
        for k, item in self._items.items():
            item.set_active(k == key)

    @property
    def active_key(self):
        return self._active_key

    # ── collapse / expand ────────────────────────────────────────────
    @property
    def collapsed(self):
        return self._collapsed

    def toggle(self):
        self.set_collapsed(not self._collapsed, persist=True)

    def set_collapsed(self, collapsed, persist=False):
        self._collapsed = bool(collapsed)
        self._apply_collapsed()
        if persist and callable(self.on_toggle):
            try:
                self.on_toggle(self._collapsed)
            except Exception:
                pass

    def _apply_collapsed(self):
        for w in self._body.winfo_children():
            try:
                w.pack_forget()
            except Exception:
                pass
        if self._collapsed:
            self.configure(width=self._rail_width)
            for item in self._items.values():
                item.set_compact(True)
            first = True
            for w in self._build_order:
                if w in self._group_headers:
                    continue
                if w in self._group_divs:
                    if not first:
                        w.pack(fill='x', padx=px(4), pady=px(8))
                    continue
                first = False
                w.pack(fill='x', pady=(0, px(2)))
            for w in (self._footer, self._footer_rule):
                w.pack_forget()
        else:
            self.configure(width=self._full_width)
            for item in self._items.values():
                item.set_compact(False)
            for w in self._build_order:
                if w in self._group_divs:
                    continue
                if w in self._group_headers:
                    w.pack(fill='x', padx=px(12), pady=(px(14), px(4)))
                else:
                    w.pack(fill='x', pady=(0, px(2)))
            self._body.pack_forget()
            self._footer.pack(side='bottom', fill='x')
            self._footer_rule.pack(side='bottom', fill='x')
            self._body.pack(side='top', fill='both', expand=True,
                            padx=px(8), pady=(px(8), 0))
