"""
ui/tab_ffpkg_edit.py — Edit ffpkg sub-tab (v2.9.0, lean version).

Mirrors the visual layout of Edit exFAT but drives UFS2Tool CLI verbs
instead of mounting via OSFMount. Each operation calls UFS2Tool.exe
once and refreshes the listing.

Supported operations (per UFS2Tool v2.5+):
  - ls       — list a directory inside the image
  - add      — add a file or directory (recursive)
  - delete   — delete a file or directory (recursive)
  - replace  — replace a file
  - rename   — rename a file or directory
  - stat     — read inode info (size, perms, timestamps)
  - extract  — extract a file (used for on-demand hex preview)

NOT supported by UFS2Tool's CLI:
  - mkdir    — workaround: add a .keep placeholder file inside the
               new path, which creates the directory as a side effect.
"""

import os
import sys
import time
from ui.shared.drives import used_letter_mask
import tempfile
import subprocess
import threading
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import ttk, filedialog

from tkinter_theme import COLORS, FONTS
# v4: all dialogs go through the themed AppDialog helpers (no messagebox).
from ui.shared.app_dialog import app_confirm, app_danger, app_error, app_info, app_warning

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, extract_ufs2tool, _NO_WIN_FLAGS

# Design-system components — imported after the star import so nothing
# from exfat_builder's namespace can shadow them.
from tkinter_theme import px
from ui.shared.widgets import (
    PageHeader, Card, Badge, Button, IconButton, IconLabel, Divider,
    Rounded, Tooltip, autowrap, EmptyState as DSEmptyState)
from ui.icons import icon_image


# ─────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────
_INSPECTOR_WIDTH = 288


def _style_sel_pill(pill, n):
    """Selection count in the table footer: quiet at zero, emphasised
    when items are selected. Presentation only."""
    if n:
        pill.config(text='%d selected' % n, fg=COLORS['fg_0'],
                    font=FONTS['body_strong'])
    else:
        pill.config(text='0 selected', fg=COLORS['fg_3'],
                    font=FONTS['body'])


# File kind -> icon name (ui.icons). Icons stay neutral at rest (design
# rule: no blue icons); folders are one step brighter than files.
_KIND_ICON = {
    'folder':  'folder',
    'exec':    'cpu',
    'log':     'list',
    'text':    'file_text',
    'data':    'database',
    'config':  'settings',
    'binary':  'binary',
    'unknown': 'file',
}


def _kind_icon(kind):
    return _KIND_ICON.get(kind, 'file')


def _kind_fg(kind):
    return COLORS['fg_2'] if kind == 'folder' else COLORS['fg_3']


# ─────────────────────────────────────────────────────────────────────
# Editor workspace building blocks (also used by ui/tab_files.py)
#
# Private helpers composed from ui.shared.widgets: a compact image
# summary bar, a responsive command bar, the file table card, the
# inspector card and a dialog skeleton. Presentation only — every
# widget the editor logic drives keeps the API it had.
# ─────────────────────────────────────────────────────────────────────
_TREE_ICONS = {}


def _tree_icon(name, fg=None, size=16, gap=8):
    """Transparent (RGBA) icon for Treeview rows, so the glyph sits
    cleanly on both the row and the selected-row background. `gap` adds
    transparent space on the right (icon -> name spacing)."""
    fg = fg or COLORS['fg_3']
    dev = max(8, px(size))
    key = (name, dev, fg, gap)
    img = _TREE_ICONS.get(key)
    if img is not None:
        return img
    try:
        from PIL import Image, ImageTk
        from ui.icons import _mask, _rgb
        from tkinter_theme import _ROOT
        m = _mask(name).resize((dev, dev), Image.LANCZOS)
        if m.mode != 'L':
            m = m.getchannel('A') if 'A' in m.getbands() else m.convert('L')
        ink = Image.new('RGBA', (dev, dev), _rgb(fg) + (0,))
        ink.putalpha(m)
        canvas = Image.new('RGBA', (dev + px(gap), dev), _rgb(fg) + (0,))
        canvas.paste(ink, (0, 0))
        img = ImageTk.PhotoImage(canvas, master=_ROOT[0])
    except Exception:
        img = icon_image(name, size, fg, COLORS['bg_2'])
    _TREE_ICONS[key] = img
    return img


def _file_table_style():
    """Treeview style for the editors' file tables: the themed table
    with an icon + name in the tree column and no expander indicator
    (the listings are flat)."""
    st = ttk.Style()
    name = 'EditorFiles.Treeview'
    st.configure(name, indent=0, rowheight=px(34))
    st.layout(name + '.Item', [
        ('Treeitem.padding', {'sticky': 'nswe', 'children': [
            ('Treeitem.image', {'side': 'left', 'sticky': ''}),
            ('Treeitem.text', {'sticky': 'nswe'}),
        ]}),
    ])
    # tree column and value cells share the heading's 12px inset
    st.configure(name + '.Item', padding=(px(12), 0, 0, 0))
    st.configure(name + '.Cell', padding=(px(12), 0))
    return name


class _ImageSummary(Card):
    """Compact image summary bar: cover tile, title / id / path, inline
    stats, a status badge and an `actions` slot on the right.

    Drop-in for the GameHero API the editor logic drives: `title_var`,
    `id_var`, `path_var`, set_title(), set_path(), set_stat(),
    set_badge(), reset_cover(), set_cover_from_folder()."""

    _TONES = {'ready': 'success', 'busy': 'info', 'warn': 'warn'}

    def __init__(self, master, stats, icon='package', placeholder=''):
        super().__init__(master, padding=(px(16), px(12)))
        bg = COLORS['bg_2']
        self._bg = bg
        self._icon = icon
        self._placeholder = placeholder
        self._cover_img = None
        self._cover_token = 0
        self._cover_size = 48

        self.top = tk.Frame(self, bg=bg)
        self.top.pack(fill='x')

        # cover tile
        tile = Rounded(self.top, bg=COLORS['bg_3'], radius=6, surface=bg)
        tile.pack(side='left')
        self._cover_lbl = tk.Label(tile, bg=COLORS['bg_3'], bd=0,
                                   width=px(48), height=px(48))
        self._cover_lbl.pack()
        tile.raise_decorations()
        self._tile = tile
        self.reset_cover()

        # right side: actions, badge, stats (packed right-to-left)
        self.actions = tk.Frame(self.top, bg=bg)
        self.actions.pack(side='right', padx=(px(16), 0))
        self._badge_slot = tk.Frame(self.top, bg=bg)
        self._badge_slot.pack(side='right', padx=(px(16), 0))
        self._badge = Badge(self._badge_slot, '', 'neutral', surface=bg)
        stats_fr = tk.Frame(self.top, bg=bg)
        stats_fr.pack(side='right')
        self._stats_fr = stats_fr
        self._stats_shown = True
        self._cells = {}
        for i, (label, key) in enumerate(stats):
            cell = tk.Frame(stats_fr, bg=bg)
            cell.pack(side='left', padx=(0 if i == 0 else px(24), 0))
            tk.Label(cell, text=label, font=FONTS['caption'], bg=bg,
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
            v = tk.StringVar(value='—')
            val = tk.Label(cell, textvariable=v, font=FONTS['body_strong'],
                           bg=bg, fg=COLORS['fg_0'], anchor='w')
            val.pack(anchor='w', pady=(px(2), 0))
            self._cells[key] = (v, val)

        # title / id / path column
        col = tk.Frame(self.top, bg=bg)
        col.pack(side='left', fill='x', expand=True, padx=(px(12), px(24)))
        self._col = col
        trow = tk.Frame(col, bg=bg)
        trow.pack(fill='x')
        self.title_var = tk.StringVar(value='')
        self.id_var = tk.StringVar(value='')
        self.path_var = tk.StringVar(value='')
        tk.Label(trow, textvariable=self.title_var, font=FONTS['subtitle'],
                 bg=bg, fg=COLORS['fg_0'], anchor='w').pack(side='left')
        self._id_lbl = tk.Label(trow, textvariable=self.id_var,
                                font=FONTS['code'], bg=bg,
                                fg=COLORS['fg_3'], anchor='w')
        self._id_lbl.pack(side='left', padx=(px(8), 0))
        self._path_lbl = tk.Label(col, text=placeholder, font=FONTS['caption'],
                                  bg=bg, fg=COLORS['fg_3'], anchor='w')
        self._path_lbl.pack(fill='x', pady=(px(2), 0))
        # the full path (the label may be shortened) as a tooltip
        self.path_prefix = ''
        self._path_tip = Tooltip(self._path_lbl, '')
        self.path_var.trace_add('write', lambda *a: self._render_path())
        col.bind('<Configure>', lambda e: self._render_path(), add='+')
        self.top.bind('<Configure>', lambda e: self._fit(), add='+')

    def _fit(self):
        """Hide the inline stats when they would squeeze the title."""
        try:
            W = self.top.winfo_width()
            if W <= 1:
                return
            need = (self._tile.winfo_reqwidth() + px(12 + 24 + 240)
                    + self._stats_fr.winfo_reqwidth()
                    + self._badge_slot.winfo_reqwidth() + px(16)
                    + self.actions.winfo_reqwidth() + px(16))
            show = need <= W
            if show == self._stats_shown:
                return
            self._stats_shown = show
            if show:
                self._stats_fr.pack(side='right', before=self._col)
            else:
                self._stats_fr.pack_forget()
        except Exception:
            pass

    # ── GameHero-compatible API ──
    def set_title(self, title, game_id=''):
        self.title_var.set(title or '—')
        self.id_var.set(game_id or '')

    def set_path(self, text):
        self.path_var.set(text or '')

    def _render_path(self):
        """Path (mono) or the placeholder caption, shortened in the
        middle to the column width so it never runs into the stats."""
        p = self.path_var.get()
        self._path_tip.set((self.path_prefix + p) if p else '')
        fnt = FONTS['code'] if p else FONTS['caption']
        text = p or self._placeholder
        try:
            import tkinter.font as tkfont
            w = self._col.winfo_width()
            f = tkfont.Font(font=fnt)
            if w > 1 and f.measure(text) > w:
                lo, hi = 1, len(text)
                while lo < hi:   # longest middle-elided text that fits
                    mid = (lo + hi + 1) // 2
                    if f.measure(_elide_middle(text, mid)) <= w:
                        lo = mid
                    else:
                        hi = mid - 1
                text = _elide_middle(text, lo)
        except Exception:
            pass
        if self._path_lbl.cget('text') != text:
            self._path_lbl.config(text=text, font=fnt)

    def set_stat(self, key, value, warn=False):
        cell = self._cells.get(key)
        if not cell:
            return
        v, val = cell
        v.set(value if value not in (None, '') else '—')
        try:
            val.config(fg=COLORS['warn_hi'] if warn else COLORS['fg_0'])
        except Exception:
            pass

    def set_badge(self, text, kind='ready'):
        """'wait' (no image) hides the badge — the title already says so."""
        try:
            if not text or kind == 'wait':
                self._badge.pack_forget()
                return
            self._badge.set(text, self._TONES.get(kind, 'neutral'))
            self._badge.pack()
        except Exception:
            pass

    def reset_cover(self):
        self._cover_token += 1
        self._cover_img = None
        try:
            self._cover_lbl.config(
                image=icon_image(self._icon, 24, COLORS['fg_3'],
                                 COLORS['bg_3']),
                width=px(48), height=px(48))
        except Exception:
            pass

    def set_cover_from_folder(self, app, folder):
        """Load the cover via the app's existing loaders; keeps the
        placeholder icon when there is no art. Never mounts/extracts."""
        self._cover_token += 1
        token = self._cover_token
        if not folder or not os.path.isdir(folder):
            self.reset_cover()
            return

        def _w():
            pil = None
            try:
                cp = app._load_cover_art(folder)
                if cp:
                    from exfat_builder import _load_cover_image
                    pil = _load_cover_image(cp, target=px(self._cover_size))
            except Exception:
                pil = None

            def _apply():
                if token != self._cover_token:
                    return
                if pil is None:
                    self.reset_cover()
                    return
                try:
                    from PIL import ImageTk as _IT
                    self._cover_img = _IT.PhotoImage(pil)
                    self._cover_lbl.config(image=self._cover_img,
                                           width=px(48), height=px(48))
                    self._tile.raise_decorations()
                except Exception:
                    self.reset_cover()
            try:
                self.after(0, _apply)
            except Exception:
                pass
        threading.Thread(target=_w, daemon=True).start()


class _CommandBar(Card):
    """One toolbar for the editor: grouped icon + label buttons (small,
    subtle) separated by hairlines. When the bar is too narrow for every
    label, buttons collapse to icon-only (with tooltips) in a fixed
    order, so nothing is ever clipped."""

    def __init__(self, master):
        super().__init__(master, padding=(px(8), px(6)))
        bg = COLORS['bg_2']
        self._bg = bg
        self.right = tk.Frame(self, bg=bg)
        self.right.pack(side='right')
        self.row = tk.Frame(self, bg=bg)
        self.row.pack(side='left', fill='x', expand=True)
        self._items = []
        self._seps = []
        self.row.bind('<Configure>', lambda e: self._fit(), add='+')

    def group(self, items, rank=0):
        """items: (icon, label, command[, item_rank]). Higher rank
        collapses to icon-only first. Returns the labelled buttons."""
        if self._items:
            sep = Divider(self.row, color=COLORS['border_2'], vertical=True)
            sep.pack(side='left', fill='y', padx=px(6), pady=px(6))
            self._seps.append(sep)
        out = []
        for it in items:
            icon, label, cmd = it[0], it[1], it[2]
            r = it[3] if len(it) > 3 else rank
            slot = tk.Frame(self.row, bg=self._bg)
            slot.pack(side='left', padx=(0, px(2)))
            full = Button(slot, text=label, command=cmd, icon=icon,
                          variant='subtle', size='sm', surface=self._bg)
            only = IconButton(slot, icon, cmd, tooltip=label, size='sm',
                              surface=self._bg)
            full.pack()
            self._items.append(dict(slot=slot, full=full, only=only,
                                    rank=r, group=len(self._seps),
                                    labelled=True))
            out.append(full)
        return out

    def _fit(self):
        avail = self.row.winfo_width()
        if avail <= 1 or not self._items:
            return
        gaps = px(2) * len(self._items) + (px(12) + 1) * len(self._seps)
        total = gaps
        units = {}
        for it in self._items:
            w = max(1, it['full'].winfo_reqwidth())
            o = max(1, it['only'].winfo_reqwidth())
            total += w
            # buttons of one group with the same rank collapse together
            u = units.setdefault((it['rank'], it['group']), [])
            u.append((it, w - o))
        want = {id(it): True for it in self._items}
        for key in sorted(units, key=lambda k: (-k[0], -k[1])):
            if total <= avail:
                break
            for it, saved in units[key]:
                want[id(it)] = False
                total -= saved
        for it in self._items:
            lab = want[id(it)]
            if lab == it['labelled']:
                continue
            it['labelled'] = lab
            (it['only'] if lab else it['full']).pack_forget()
            (it['full'] if lab else it['only']).pack()


def _build_file_table_card(parent, columns, root_icon, path_var,
                           count_var=None):
    """The listing card: path bar · table · footer. `columns` are
    (key, heading, width, anchor) for the value columns; the tree column
    shows icon + name. Returns a dict with card, tree, body (the table
    host, for overlays), foot_left and foot_right."""
    bg = COLORS['bg_2']
    card = Card(parent, padding=0)

    # path bar
    bar = tk.Frame(card, bg=bg)
    bar.pack(fill='x', padx=px(16), pady=(px(11), px(10)))
    IconLabel(bar, root_icon, 16, COLORS['fg_3'], bg).pack(side='left')
    IconLabel(bar, 'chevron_right', 12, COLORS['fg_4'], bg).pack(
        side='left', padx=(px(6), px(6)))
    tk.Label(bar, textvariable=path_var, font=FONTS['code'], bg=bg,
             fg=COLORS['fg_1'], anchor='w').pack(side='left', fill='x',
                                                 expand=True)
    if count_var is not None:
        tk.Label(bar, textvariable=count_var, font=FONTS['caption'], bg=bg,
                 fg=COLORS['fg_3']).pack(side='right')
    Divider(card, color=COLORS['border_2']).pack(fill='x')

    # footer (packed before the table so it keeps its row when short)
    foot = tk.Frame(card, bg=bg)
    foot.pack(side='bottom', fill='x', padx=px(16), pady=(px(8), px(9)))
    Divider(card, color=COLORS['border_1']).pack(side='bottom', fill='x')
    foot_left = tk.Frame(foot, bg=bg)
    foot_left.pack(side='left')
    foot_right = tk.Frame(foot, bg=bg)
    foot_right.pack(side='right')

    # table
    body = tk.Frame(card, bg=bg)
    body.pack(fill='both', expand=True, padx=px(1))
    cols = [c[0] for c in columns]
    tree = ttk.Treeview(body, columns=['name'] + cols,
                        displaycolumns=cols, show='tree headings',
                        style=_file_table_style(), selectmode='extended')
    tree.heading('#0', text='Name', anchor='w')
    tree.column('#0', width=px(360), minwidth=px(160), stretch=True,
                anchor='w')
    for key, title, width, anchor in columns:
        tree.heading(key, text=title, anchor=anchor)
        tree.column(key, width=px(width), minwidth=px(60), stretch=False,
                    anchor=anchor)
    from ui.shared.widgets import Scrollbar
    sb = Scrollbar(body, orient='vertical', command=tree.yview, surface=bg)
    tree.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y', pady=px(4))
    tree.pack(side='left', fill='both', expand=True)

    # The name column takes whatever the value columns leave, so they
    # stay visible on narrow windows.
    fixed = sum(px(c[2]) for c in columns)

    def _fit_name(e):
        try:
            w = max(px(140), e.width - fixed - px(2))
            if int(tree.column('#0', 'width')) != w:
                tree.column('#0', width=w)
        except Exception:
            pass
    tree.bind('<Configure>', _fit_name, add='+')
    return dict(card=card, tree=tree, body=body, foot_left=foot_left,
                foot_right=foot_right)


def _place_overlay(w):
    """Cover the table body below its heading row (empty states)."""
    try:
        w.place(relx=0, y=px(34), relwidth=1, relheight=1, height=-px(34))
    except Exception:
        pass


def _build_inspector_card(parent):
    """Inspector card shell (fixed width). Returns (card, refs): refs
    holds the icon tile, name / size labels, the `details` grid, the
    card body and an `actions` frame pinned to the bottom."""
    bg = COLORS['bg_2']
    card = Card(parent, title='Inspector', padding=px(16))
    card.configure(width=px(_INSPECTOR_WIDTH))
    card.pack_propagate(False)
    body = card.body

    actions = tk.Frame(body, bg=bg)
    actions.pack(side='bottom', fill='x', pady=(px(12), 0))

    # Content keeps its natural height inside a clipping frame, so on a
    # short window it is cut off cleanly above the actions instead of
    # every widget being squeezed.
    clip = tk.Frame(body, bg=bg)
    clip.pack(fill='both', expand=True)
    content = tk.Frame(clip, bg=bg)
    content.place(x=0, y=0, relwidth=1)

    hero = tk.Frame(content, bg=bg)
    hero.pack(fill='x')
    tile = Rounded(hero, bg=COLORS['bg_3'], radius=6, surface=bg)
    tile.pack(side='left', anchor='n')
    icon = IconLabel(tile, 'file_search', 24, COLORS['fg_4'], COLORS['bg_3'])
    icon.configure(width=px(48), height=px(48))
    icon.pack()
    tile.raise_decorations()
    tcol = tk.Frame(hero, bg=bg)
    tcol.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
    name = tk.Label(tcol, text='', font=FONTS['body_strong'], bg=bg,
                    fg=COLORS['fg_0'], anchor='w', justify='left',
                    wraplength=px(_INSPECTOR_WIDTH - 32 - 64))
    name.pack(fill='x', pady=(px(4), 0))
    size = tk.Label(tcol, text='', font=FONTS['caption'], bg=bg,
                    fg=COLORS['fg_3'], anchor='w')
    size.pack(fill='x', pady=(px(2), 0))

    Divider(content, color=COLORS['border_1']).pack(fill='x', pady=(px(12),
                                                                    px(10)))
    details = tk.Frame(content, bg=bg)
    details.pack(fill='x')
    details.columnconfigure(1, weight=1)
    return card, dict(icon=icon, name=name, size=size, details=details,
                      body=content, actions=actions)


class _ValueLabel(tk.Label):
    """Inspector value: monospace for real values (paths, sizes, modes),
    the body face for the '—' placeholder so empty rows look alike."""

    def __init__(self, master, mono=False, **kw):
        self._mono = mono
        super().__init__(master, **kw)
        self._apply_font(kw.get('text', ''))

    def _apply_font(self, text):
        mono = self._mono and str(text).strip() not in ('', '\u2014')
        tk.Label.configure(self, font=FONTS['code'] if mono
                           else FONTS['body'])

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        res = tk.Label.configure(self, **kw) if kw else tk.Label.configure(self)
        if 'text' in kw:
            self._apply_font(kw['text'])
        return res

    config = configure


def _inspector_row(details, app, label, attr, mono=False):
    """One label / value line in the inspector details grid."""
    bg = COLORS['bg_2']
    r = len(details.grid_slaves(column=0))
    tk.Label(details, text=label, font=FONTS['caption'], bg=bg,
             fg=COLORS['fg_3'], anchor='nw').grid(
                 row=r, column=0, sticky='nw', pady=(px(1), px(6)),
                 padx=(0, px(12)))
    val = _ValueLabel(details, mono=mono, text='\u2014', bg=bg,
                   fg=COLORS['fg_1'], anchor='w', justify='left',
                   wraplength=px(_INSPECTOR_WIDTH - 32 - 92))
    val.grid(row=r, column=1, sticky='nw', pady=(0, px(6)))
    setattr(app, attr, val)
    return val


def _insp_set_icon(lbl, kind=None):
    """Inspector tile: the kind's icon, or the idle 'no selection' glyph."""
    try:
        if kind is None:
            lbl.set('file_search', COLORS['fg_4'])
        else:
            lbl.set(_kind_icon(kind), COLORS['fg_1'])
    except Exception:
        pass


def _hex_lines(data):
    """64-byte preview as 8-byte rows ('0000  7f 45 4c 46 …') so it fits
    the inspector without horizontal scrolling."""
    return '\n'.join('%04x  %s' % (off, ' '.join('%02x' % b
                                                 for b in data[off:off + 8]))
                     for off in range(0, len(data), 8))


def _elide_middle(text, limit=96):
    """Shorten very long paths for display: keep head and tail."""
    text = text or ''
    if len(text) <= limit:
        return text
    keep = (limit - 1) // 2
    if keep <= 0:
        return '\u2026'
    return text[:keep] + '\u2026' + text[-keep:]


def _hex_well(parent):
    """Inset monospace well (bg_0) for the 64-byte hex preview."""
    return tk.Text(parent, height=8, width=30, font=FONTS['code_sm'],
                   bg=COLORS['bg_0'], fg=COLORS['fg_2'],
                   insertbackground=COLORS['fg_2'], relief='flat',
                   bd=0, padx=px(8), pady=px(6), wrap='none',
                   state='disabled', highlightthickness=1,
                   highlightbackground=COLORS['border_2'],
                   highlightcolor=COLORS['border_2'])


def _dialog_shell(app, title, heading, text=None, width=480):
    """Themed modal skeleton: bg_2 body with a title-style heading and
    fg_2 body text, and a bg_1 footer band for right-aligned buttons.
    Returns (dlg, body, footer)."""
    from tkinter import Toplevel
    dlg = Toplevel(app)
    dlg.withdraw()
    dlg.title(title)
    dlg.configure(bg=COLORS['border_3'])
    dlg.transient(app)
    dlg.resizable(False, False)
    shell = tk.Frame(dlg, bg=COLORS['bg_2'], width=px(width))
    shell.pack(fill='both', expand=True, padx=1, pady=1)
    body = tk.Frame(shell, bg=COLORS['bg_2'])
    body.pack(fill='both', expand=True, padx=px(24), pady=(px(20), px(16)))
    tk.Label(body, text=heading, font=FONTS['title'], bg=COLORS['bg_2'],
             fg=COLORS['fg_0'], anchor='w', justify='left',
             wraplength=px(width - 48)).pack(fill='x')
    if text:
        tk.Label(body, text=text, font=FONTS['body'], bg=COLORS['bg_2'],
                 fg=COLORS['fg_2'], anchor='w', justify='left',
                 wraplength=px(width - 48)).pack(fill='x', pady=(px(6), 0))
    footer = tk.Frame(shell, bg=COLORS['bg_1'])
    footer.pack(fill='x', side='bottom')
    Divider(shell, color=COLORS['border_2']).pack(fill='x', side='bottom')
    footer_in = tk.Frame(footer, bg=COLORS['bg_1'])
    footer_in.pack(fill='x', padx=px(24), pady=px(14))
    # fixes the dialog's minimum width (labels wrap inside it)
    tk.Frame(body, bg=COLORS['bg_2'], width=px(width - 48),
             height=1).pack(side='bottom')
    # text width inside the body, for labels added by the caller
    body.wrap = px(width - 48)
    return dlg, body, footer_in


def _dialog_place(dlg, app):
    """Centre a dialog over the app window, show it and make it modal."""
    try:
        dlg.update_idletasks()
        w, h = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        x = app.winfo_rootx() + (app.winfo_width() - w) // 2
        y = app.winfo_rooty() + (app.winfo_height() - h) // 3
        dlg.geometry('+%d+%d' % (max(0, x), max(0, y)))
    except Exception:
        pass
    dlg.deiconify()
    try:
        dlg.grab_set()
    except Exception:
        pass


def _dialog_button(footer, text, command, variant='secondary', icon=None,
                   side='right'):
    """Footer button (min width ~96px). Pack order is right-to-left, so
    create the primary first."""
    b = Button(footer, text=text, command=command, variant=variant,
               icon=icon, surface=COLORS['bg_1'], width=9)
    b.pack(side=side, padx=(px(8), 0) if side == 'right' else (0, px(8)))
    return b


def _choice_row(parent, icon, title, sub, command, wrap=None):
    """Clickable option row (icon · title · description · chevron).
    `wrap` is the dialog body text width (see _dialog_shell)."""
    bg, hov = COLORS['bg_3'], COLORS['bg_4']
    row = Rounded(parent, bg=bg, border=COLORS['border_3'], radius=6,
                  padding=(px(14), px(10)), surface=COLORS['bg_2'])
    row.pack(fill='x', pady=(0, px(8)))
    ic = IconLabel(row, icon, 20, COLORS['fg_2'], bg)
    ic.pack(side='left', anchor='n', pady=(px(2), 0))
    chev = IconLabel(row, 'chevron_right', 16, COLORS['fg_3'], bg)
    chev.pack(side='right', padx=(px(8), 0))
    col = tk.Frame(row, bg=bg)
    col.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
    t = tk.Label(col, text=title, font=FONTS['body_strong'], bg=bg,
                 fg=COLORS['fg_0'], anchor='w')
    t.pack(fill='x')
    s = tk.Label(col, text=sub, font=FONTS['caption'], bg=bg,
                 fg=COLORS['fg_3'], anchor='w', justify='left',
                 wraplength=(wrap or px(452)) - px(92))
    s.pack(fill='x', pady=(px(2), 0))
    row.raise_decorations()

    def _paint(on):
        c = hov if on else bg
        row.set_style(bg=c, border=COLORS['border_4'] if on
                      else COLORS['border_3'])
        for w in (col, t, s):
            w.configure(bg=c)
        ic.set(bg=c)
        chev.set(bg=c)
    for w in (row, ic, col, t, s, chev):
        w.bind('<Button-1>', lambda e: command())
        w.bind('<Enter>', lambda e: _paint(True))
        w.bind('<Leave>', lambda e: _paint(False))
        try:
            w.configure(cursor='hand2')
        except Exception:
            pass
    return row


def _classify(name, is_dir):
    """Classify a directory entry by name + known type.

    is_dir can be True, False, or None (unknown). When None, we apply
    a "looks like folder" heuristic: names with no extension that don't
    match a known file pattern are treated as folders.
    """
    if is_dir is True:
        return ('folder', 'Folder')
    lname = name.lower()
    if is_dir is False:
        # Confirmed file — skip the folder heuristic
        pass
    else:
        # is_dir is None — apply heuristic.
        # No dot in name + not a known file-like name => likely folder.
        if '.' not in lname and lname not in (
                'eboot', 'sidbase', 'keystone', 'param', 'pic0', 'pic1',
                'icon0'):
            return ('folder', 'Folder')
    if lname in ('eboot.bin', 'sidbase.bin') or lname.endswith('.elf'):
        return ('exec', 'Binary')
    if lname.endswith('.log'):
        return ('log', 'Log')
    if lname.endswith(('.txt', '.json', '.xml', '.ini')):
        return ('text', 'Text')
    if lname.startswith('sce_') or lname.endswith(('.sprx', '.xpps')):
        return ('data', 'Data')
    if lname.endswith(('.cfg', '.conf')):
        return ('config', 'Config')
    if lname.endswith(('.bin', '.dat', '.pkg')):
        return ('binary', 'Binary')
    return ('unknown',
            name.split('.')[-1].upper() if '.' in name else 'File')


def _fmt_size(b):
    if not b:
        return '0 B'
    for unit, div in (('GB', 1024**3), ('MB', 1024**2), ('KB', 1024)):
        if b >= div:
            if div >= 1024**2:
                return '%.2f %s' % (b / div, unit)
            return '%d %s' % (b // div, unit)
    return '%d B' % b


# ─────────────────────────────────────────────────────────────────────
# UFS2Tool CLI helpers
# ─────────────────────────────────────────────────────────────────────
def _ufs2_exe(app):
    """Ensure UFS2Tool.exe is extracted and return its path."""
    if not getattr(app, '_ffpkg_ufs2_exe', None) or not os.path.isfile(app._ffpkg_ufs2_exe):
        app._ffpkg_ufs2_exe = extract_ufs2tool(
            app._settings.get('temp_dir') or None)
    return app._ffpkg_ufs2_exe


def _run_ufs2(app, args, timeout=120):
    """Run UFS2Tool.exe with the given args and return (rc, stdout)."""
    exe = _ufs2_exe(app)
    enc = 'mbcs' if sys.platform.startswith('win') else 'utf-8'
    try:
        proc = subprocess.run(
            [exe] + list(args),
            capture_output=True, text=True,
            encoding=enc, errors='replace',
            timeout=timeout,
            creationflags=_NO_WIN_FLAGS)
        out = (proc.stdout or '') + (proc.stderr or '')
        return proc.returncode, out
    except subprocess.TimeoutExpired:
        return -1, '(timeout after %ds)' % timeout
    except Exception as e:
        return -2, '(' + str(e) + ')'


def _parse_ls(text):
    """Parse `UFS2Tool.exe ls` output into a list of (name, is_dir, size).

    UFS2Tool v2.5+ output format (confirmed from real binary):

        Directory '/' (31 entries):
          DIR   inode=     2  .
          DIR   inode=  4441  assets
          FILE  inode=  5232  copytexture2d_p.ags
          FILE  inode=  5256  eboot.bin

    Each entry line: `  <TYPE>  inode= <inode-number>  <name>` where
    <TYPE> is either DIR or FILE. UFS2Tool does not currently report
    file sizes in `ls`, so size is always 0 — we'd need a separate
    `stat` call to fill it in.

    Older / alternate formats are kept as fallback heuristics in case
    UFS2Tool changes its output again.
    """
    import re as _re
    seen = set()
    entries = []
    # Primary pattern: '  DIR   inode=     2  .' or '  FILE  inode=  5256  eboot.bin'
    primary = _re.compile(
        r'^\s*(DIR|FILE)\s+inode=\s*(\d+)\s+(.+?)\s*$')

    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        stripped = line.strip()
        low = stripped.lower()

        # Skip header lines
        if (low.startswith('total ') or low.startswith('listing ')
                or low.startswith('directory ') or low.startswith('contents of')
                or low.startswith('---') or low.startswith('===')
                or low == 'name'
                or (low.startswith('name ') and 'size' in low)):
            continue
        # Quoted-path header: "'/' (31 entries):"
        if ((stripped.startswith("'") or stripped.startswith('"'))
                and stripped.endswith(':')):
            continue

        # ─── Primary: UFS2Tool v2.5+ format ───
        m = primary.match(line)
        if m:
            kind, _inode_num, name = m.group(1), m.group(2), m.group(3).strip()
            if not name or name in ('.', '..'):
                continue
            if name in seen:
                continue
            seen.add(name)
            entries.append((name, kind == 'DIR', 0))
            continue

        # ─── Fallbacks for alternate / older formats ───
        is_dir = None
        size = 0
        name = None
        parts = stripped.split()
        if not parts:
            continue

        # ls -l style
        if (len(parts[0]) >= 10 and parts[0][0] in 'd-l'
                and all(c in 'rwx-st' for c in parts[0][1:10])):
            is_dir = parts[0][0] == 'd'
            name = parts[-1]
            for col in parts[1:-1]:
                try:
                    v = int(col)
                    if v > size:
                        size = v
                except ValueError:
                    continue
        # Explicit kind prefix on its own
        elif parts[0].lower() in ('directory', 'dir', 'folder'):
            is_dir = True
            name = ' '.join(parts[1:])
        elif parts[0].lower() == 'file' and len(parts) >= 2:
            is_dir = False
            name = ' '.join(parts[1:])
        elif parts[0] in ('[DIR]', '<DIR>'):
            is_dir = True
            name = ' '.join(parts[1:]) if len(parts) > 1 else ''
        # Trailing-slash dir marker
        elif stripped.endswith('/') or stripped.endswith('\\'):
            is_dir = True
            name = stripped.rstrip('/\\')
        else:
            # Unknown format — last token as name
            name = parts[-1]
            is_dir = None

        if not name or name in ('.', '..'):
            continue
        if name.lower() == 'inode' or name.lower().startswith('inode='):
            continue
        if name in seen:
            continue
        seen.add(name)
        entries.append((name, is_dir, size))

    # Sort: dirs first, then unknowns, then files; all alphabetical
    def _sort_key(e):
        _name, _isd, _ = e
        rank = 0 if _isd is True else (1 if _isd is None else 2)
        return (rank, _name.lower())
    entries.sort(key=_sort_key)
    return entries


def _join_fs_path(*parts):
    """Join filesystem paths inside the image using forward slashes.

    Bug fix (v2.0.6): when one of the parts is exactly '/', the old
    impl produced double-slashed output like '//eboot.bin' because
    '/'.strip('/') == '' and the empty result was still joined. Now
    we strip first, then filter empties.
    """
    stripped = [p.strip('/') for p in parts if p]
    parts2   = [s for s in stripped if s]
    out      = '/'.join(parts2)
    return '/' + out if out else '/'


def _basename_fs(path):
    p = path.rstrip('/')
    if not p or p == '/':
        return '/'
    return p.rsplit('/', 1)[-1]


def _dirname_fs(path):
    p = path.rstrip('/')
    if not p or p == '/':
        return '/'
    return p.rsplit('/', 1)[0] or '/'


# ─────────────────────────────────────────────────────────────────────
# Main entry
# ─────────────────────────────────────────────────────────────────────
def build_ffpkg_edit_tab(parent, app, embedded=False):
    """Build the ffpkg editor into `parent`.

    embedded=True is the unified Edit page: it supplies the page header
    and the Open action, so this pane starts at the image summary bar.
    Stand-alone hosts get their own page header."""
    parent.configure(bg=COLORS['bg_1'])

    # State
    app._fe_image_var   = tk.StringVar()
    app._fe_current     = '/'              # current fs-path inside the image
    app._fe_entries     = []               # list of (name, is_dir, size)
    app._fe_ls_cache    = {}               # path -> entries list
    app._fe_status_var  = tk.StringVar(value=_('No image loaded'))
    app._fe_path_var    = tk.StringVar(value='/')

    # ── Page header (stand-alone hosts only) ──
    if not embedded:
        hdr = PageHeader(
            parent, _('Edit ffpkg Image'),
            _('Add, replace, rename, or delete files inside a .ffpkg via UFS2Tool.'))
        hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=px(24), pady=(0, px(20)))

    # ── Image summary bar ──
    _build_image_card(wrap, app, embedded)

    # ── Command bar ──
    _build_toolbar(wrap, app)

    # ── Workspace: listing card + inspector card ──
    body = tk.Frame(wrap, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, pady=(px(12), 0))
    body.columnconfigure(0, weight=1)
    body.columnconfigure(1, weight=0, minsize=px(_INSPECTOR_WIDTH))
    body.rowconfigure(0, weight=1)

    list_col = tk.Frame(body, bg=COLORS['bg_1'])
    list_col.grid(row=0, column=0, sticky='nsew')
    _build_table(list_col, app)

    inspector = tk.Frame(body, bg=COLORS['bg_1'])
    inspector.grid(row=0, column=1, sticky='nsew', padx=(px(16), 0))
    _build_inspector(inspector, app)


# ─────────────────────────────────────────────────────────────────────
# Image summary bar — the open .ffpkg
# ─────────────────────────────────────────────────────────────────────
def _build_image_card(parent, app, embedded=False):
    app._fe_hero = _ImageSummary(
        parent,
        stats=[('Image size', 'size'), ('Files', 'files'),
               ('Format', 'format')],
        icon='package',
        placeholder=(_('Add, replace, rename, or delete files inside a .ffpkg via UFS2Tool.')
                     if embedded else _('Browse to open a .ffpkg image')))
    app._fe_hero.pack(fill='x')
    app._fe_hero.set_title(_('No image loaded'), '')
    app._fe_hero.set_stat('format', 'ffpkg')
    app._fe_hero.set_badge(_('NO IMAGE'), 'wait')

    # Actions: Open (an icon button when the page header already offers
    # the unified Open) and Close.
    acts = app._fe_hero.actions
    app._fe_close_btn = Button(acts, text=_('Close'), icon='eject',
                               command=lambda: _close_image(app))
    app._fe_close_btn.configure(state='disabled')
    if embedded:
        IconButton(acts, 'folder_open', lambda: _browse_image(app),
                   tooltip=_('Open Image...'), variant='secondary'
                   ).pack(side='left', padx=(0, px(8)))
        app._fe_close_btn.pack(side='left')
    else:
        app._fe_close_btn.pack(side='left', padx=(0, px(8)))
        Button(acts, text=_('Open Image...'), icon='folder_open',
               variant='primary', command=lambda: _browse_image(app)
               ).pack(side='left')

    # back-compat shims: existing open/close code sets these "labels";
    # route their text into the summary bar so logic stays untouched.
    class _ShimLabel:
        def __init__(self, setter, clear_on_hide=False):
            self._setter = setter
            self._clear = clear_on_hide
        def config(self, **kw):
            if 'text' in kw:
                self._setter(kw['text'])
        configure = config
        def pack(self, *a, **k):
            pass
        def pack_forget(self):
            if self._clear:
                self._setter('')
    app._fe_card_ppsa = _ShimLabel(lambda t: app._fe_hero.id_var.set(t or ''),
                                   clear_on_hide=True)
    app._fe_card_title = _ShimLabel(
        lambda t: app._fe_hero.title_var.set(t or '—'))
    app._fe_card_size = _ShimLabel(
        lambda t: app._fe_hero.set_stat('size', t or '—'))
    app._fe_card_path = _ShimLabel(lambda t: app._fe_hero.set_path(t or ''))


def _browse_image(app):
    p = filedialog.askopenfilename(
        title=_('Select .ffpkg image'),
        filetypes=[('ffpkg images', '*.ffpkg'),
                   ('All files',    '*.*')])
    if not p:
        return
    open_ffpkg_path(app, p.replace('/', '\\'))


def open_ffpkg_path(app, p):
    """Open a .ffpkg by path. Shared by the Browse button and the unified
    Edit-tab opener. Behaviour is identical to the former _browse_image
    post-dialog steps."""
    app._fe_image_var.set(p)
    # Try to parse PPSA + title from filename
    import re
    base = os.path.basename(p)
    m = re.match(r'(PPSA\d{5,})\s+(.*?)\s*(?:\(([\d.]+)\))?\.ffpkg$',
                 base, re.IGNORECASE)
    if m:
        app._fe_card_ppsa.pack(side='left', padx=(0, 8),
                                before=app._fe_card_title)
        app._fe_card_ppsa.config(text=m.group(1))
        app._fe_card_title.config(text=m.group(2))
    else:
        app._fe_card_ppsa.pack_forget()
        app._fe_card_title.config(
            text=os.path.splitext(base)[0])
    app._fe_close_btn.configure(state='normal')
    try:
        sz = os.path.getsize(p)
        app._fe_card_size.config(text=_fmt_size(sz))
        app._fe_card_size.pack(side='left', padx=(8, 0))
    except Exception:
        app._fe_card_size.config(text='')
        app._fe_card_size.pack_forget()
    try:
        app._fe_empty.place_forget()
    except Exception:
        pass
    app._fe_current = '/'
    app._fe_ls_cache.clear()
    _refresh(app)
    try:
        app._fe_hero.set_badge(_('READY TO EDIT'), 'ready')
        app._fe_hero.set_path(p)
    except Exception:
        pass


def _close_image(app):
    app._fe_image_var.set('')
    app._fe_card_title.config(text=_('No image loaded'))
    app._fe_card_ppsa.pack_forget()
    try:
        app._fe_card_size.config(text='')
        app._fe_card_size.pack_forget()
        _place_overlay(app._fe_empty)
    except Exception:
        pass
    app._fe_close_btn.configure(state='disabled')
    app._fe_listbox.delete(0, 'end')
    app._fe_entries = []
    app._fe_ls_cache.clear()
    app._fe_current = '/'
    app._fe_path_var.set('/')
    app._fe_status_var.set(_('No image loaded'))
    try:
        app._fe_hero.set_badge(_('NO IMAGE'), 'wait')
        app._fe_hero.set_stat('files', '\u2014')
        app._fe_hero.set_path('')
        app._fe_hero.reset_cover()
    except Exception:
        pass
    _clear_inspector(app)


# ─────────────────────────────────────────────────────────────────────
# Toolbar
# ─────────────────────────────────────────────────────────────────────
def _build_toolbar(parent, app):
    """One command bar: Navigation | Creation | Modification | Danger,
    separated by hairlines. Commands unchanged."""
    bar = _CommandBar(parent)
    bar.pack(fill='x', pady=(px(16), 0))
    # Navigation icons are universally understood: they collapse first.
    bar.group([
        ('arrow_up', _('Up'), lambda: _go_up(app)),
        ('refresh', _('Refresh'), lambda: _refresh(app)),
    ], rank=3)
    bar.group([
        ('file_plus', _('Add files'), lambda: _add_files(app), 0),
        ('folder_in', _('Add folder'), lambda: _add_folder(app)),
        ('folder_plus', _('New folder'), lambda: _new_folder(app)),
        ('backports', _('Apply backport'), lambda: _apply_backport(app), 0),
        ('convert', _('Compare'), lambda: _compare_ffpkgs(app)),
    ], rank=1)
    bar.group([
        ('replace', _('Replace'), lambda: _replace_file(app)),
        ('rename', _('Rename'), lambda: _rename(app)),
    ], rank=2)
    bar.group([
        ('trash', _('Delete'), lambda: _delete(app)),
    ], rank=2)
    app._fe_toolbar = bar
    return bar


# ─────────────────────────────────────────────────────────────────────
# File table (Treeview)
# ─────────────────────────────────────────────────────────────────────
def _build_table(parent, app):
    t = _build_file_table_card(
        parent, [('type', 'Type', 120, 'w'), ('size', 'Size', 110, 'e')],
        root_icon='package', path_var=app._fe_path_var)
    t['card'].pack(fill='both', expand=True)
    tree = t['tree']

    app._fe_listbox = _TreeListboxProxy(tree)
    app._fe_tree = tree

    # Empty-state overlay — sits over the table and is hidden the moment
    # an image is loaded. Presentation only.
    app._fe_empty = DSEmptyState(
        t['body'], icon='package', title=_('No image loaded'),
        text=_('Browse to open a .ffpkg image'), bg=COLORS['bg_2'])
    _place_overlay(app._fe_empty)

    # Footer: selection on the left, status on the right
    app._fe_sel_pill = tk.Label(t['foot_left'], text='0 selected',
                                font=FONTS['body'], bg=COLORS['bg_2'],
                                fg=COLORS['fg_3'])
    app._fe_sel_pill.pack(side='left')
    app._fe_status_lbl = tk.Label(t['foot_right'],
                                  textvariable=app._fe_status_var,
                                  font=FONTS['caption'], bg=COLORS['bg_2'],
                                  fg=COLORS['fg_3'])
    app._fe_status_lbl.pack(side='right')

    # Bindings
    tree.bind('<Double-Button-1>', lambda e: _on_enter(app))
    tree.bind('<<TreeviewSelect>>', lambda e: _on_select(app))
    tree.bind('<Return>',    lambda e: (_on_enter(app), 'break'))
    tree.bind('<Delete>',    lambda e: (_delete(app), 'break'))
    tree.bind('<BackSpace>', lambda e: (_go_up(app), 'break'))
    tree.bind('<F5>',        lambda e: (_refresh(app), 'break'))
    tree.bind('<F2>',        lambda e: (_rename(app), 'break'))
    tree.bind('<Control-a>', lambda e: _select_all(app))
    tree.bind('<Control-A>', lambda e: _select_all(app))


def _select_all(app):
    for iid in app._fe_tree.get_children(''):
        app._fe_tree.selection_add(iid)
    return 'break'


# Simple proxy mirroring tab_files.ListboxProxy but without size-string
# parsing (we always feed it (name, is_dir, size) tuples directly).
class _TreeListboxProxy:
    def __init__(self, tree):
        self._tree = tree
        self._count = 0

    def populate(self, entries):
        for iid in self._tree.get_children(''):
            self._tree.delete(iid)
        self._count = 0
        import re as _re
        # Defensive cleanup pattern: strip a leading 'inode= <NUM>' if
        # any name still has it (parser fallback safety net).
        _inode_prefix = _re.compile(r'^inode=\s*\d+\s+')
        for name, is_dir, size in entries:
            if isinstance(name, str):
                name = _inode_prefix.sub('', name)
            iid = '%05d' % self._count
            self._count += 1
            kind, type_label = _classify(name, is_dir)
            icon = 'folder_out' if name == '..' else _kind_icon(kind)
            sz = '' if (is_dir is True or
                        (is_dir is None and '.' not in name.lower())
                       ) else _fmt_size(size)
            self._tree.insert('', 'end', iid=iid, text=name,
                image=_tree_icon(icon, _kind_fg(kind)),
                values=(name, type_label, sz),
                tags=(kind,))

    def delete(self, first, last=None):
        if first == 0 and (last == 'end' or last is None):
            for iid in self._tree.get_children(''):
                self._tree.delete(iid)
            self._count = 0

    def curselection(self):
        return tuple(int(iid) for iid in self._tree.selection())


# ─────────────────────────────────────────────────────────────────────
# Inspector
# ─────────────────────────────────────────────────────────────────────
def _build_inspector(parent, app):
    card, r = _build_inspector_card(parent)
    card.pack(fill='both', expand=True)
    app._fe_insp_icon = r['icon']
    app._fe_insp_name = r['name']
    app._fe_insp_size = r['size']

    d = r['details']
    _inspector_row(d, app, 'Path', '_fe_insp_path_lbl', mono=True)
    _inspector_row(d, app, 'Type', '_fe_insp_type_lbl')
    _inspector_row(d, app, 'Size', '_fe_insp_bytes_lbl', mono=True)
    _inspector_row(d, app, 'Permissions', '_fe_insp_perms_lbl', mono=True)

    # Hex preview — populated only on "Show first 64 bytes"
    hex_outer = tk.Frame(r['body'], bg=COLORS['bg_2'])
    hex_outer.pack(fill='x', pady=(px(4), 0))
    app._fe_insp_hex_btn = Button(hex_outer, text=_('Show first 64 bytes'),
                                  icon='search', size='sm',
                                  command=lambda: _fetch_hex(app))
    app._fe_insp_hex_btn.pack(fill='x')
    app._fe_insp_hex = _hex_well(hex_outer)
    # NOT packed by default — only after Show hex is clicked

    # v3.0.0: Replace and Delete live in the toolbar only; Rename stays
    # here as the one action that works on exactly the selected item.
    app._fe_insp_rename = Button(r['actions'], text=_('Rename'),
                                 icon='rename',
                                 command=lambda: _rename(app))
    app._fe_insp_rename.pack(fill='x')

    _clear_inspector(app)


def _clear_inspector(app):
    if hasattr(app, '_fe_insp_icon'):
        _insp_set_icon(app._fe_insp_icon, None)
        app._fe_insp_name.config(text=_('No selection'), fg=COLORS['fg_3'])
        app._fe_insp_size.config(text='')
        app._fe_insp_path_lbl.config(text='—')
        app._fe_insp_type_lbl.config(text='—')
        app._fe_insp_bytes_lbl.config(text='—')
        app._fe_insp_perms_lbl.config(text='—')
        try: app._fe_insp_hex.pack_forget()
        except Exception: pass


# ─────────────────────────────────────────────────────────────────────
# Navigation
# ─────────────────────────────────────────────────────────────────────
def _refresh(app):
    img = app._fe_image_var.get()
    if not img or not os.path.isfile(img):
        return
    path = app._fe_current
    # Use cache if available
    entries = app._fe_ls_cache.get(path)
    if entries is None:
        app._fe_status_var.set(_('Listing %s...') % path)
        app.update_idletasks()
        rc, out = _run_ufs2(app, ['ls', img, path])
        if rc != 0:
            app_error(app, _('ls failed'),
                _('UFS2Tool ls failed:\n\n') + out)
            return
        # Diagnostic dump — write raw ls output to %TEMP% so we can
        # debug parser issues if the format isn't what we expect.
        try:
            diag = os.path.join(tempfile.gettempdir(),
                                'ffpkg_ls_diagnostic.log')
            with open(diag, 'a', encoding='utf-8', errors='replace') as f:
                import datetime
                f.write('\n=== %s | ls %s ===\n' % (
                    datetime.datetime.now().isoformat(), path))
                f.write(out)
                f.write('\n')
        except Exception:
            pass
        entries = _parse_ls(out)
        app._fe_ls_cache[path] = entries
    # Insert ".." for non-root
    if path != '/':
        entries = [('..', True, 0)] + list(entries)
    app._fe_entries = entries
    app._fe_listbox.populate(entries)
    app._fe_path_var.set(path)
    _count = len(entries) - (1 if path != '/' else 0)
    app._fe_status_var.set(_('%d items') % _count)
    try:
        app._fe_hero.set_stat('files', '%d in folder' % _count)
    except Exception:
        pass
    _clear_inspector(app)


def _on_enter(app):
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    if not (0 <= idx < len(app._fe_entries)):
        return
    name, is_dir, _sz = app._fe_entries[idx]
    if name == '..':
        _go_up(app)
        return
    # Probe by trying to ls the path — robust to _parse_ls
    # misclassification. If ls succeeds, it's a folder we can navigate
    # into. If ls fails, it's a file (no-op for now).
    candidate = _join_fs_path(app._fe_current, name)
    img = app._fe_image_var.get()
    if not img:
        return
    rc, _out = _run_ufs2(app, ['ls', img, candidate])
    if rc == 0:
        # ls worked — it's a folder
        app._fe_current = candidate
        _refresh(app)
    # else: silently no-op (it's a file). A future version could open
    # the file for inline editing or hex preview here.


def _go_up(app):
    if app._fe_current == '/':
        return
    app._fe_current = _dirname_fs(app._fe_current)
    _refresh(app)


def _on_select(app):
    sel = app._fe_listbox.curselection()
    _style_sel_pill(app._fe_sel_pill, len(sel))
    if not sel:
        _clear_inspector(app)
        return
    idx = sel[0]
    if not (0 <= idx < len(app._fe_entries)):
        return
    name, is_dir, size = app._fe_entries[idx]
    if name == '..':
        _clear_inspector(app)
        return
    full = _join_fs_path(app._fe_current, name)
    kind, type_label = _classify(name, is_dir)
    _insp_set_icon(app._fe_insp_icon, kind)
    app._fe_insp_name.config(text=name, fg=COLORS['fg_0'])
    app._fe_insp_size.config(text=_fmt_size(size) if size else '')
    app._fe_insp_path_lbl.config(text=_elide_middle(full))
    app._fe_insp_type_lbl.config(text=type_label)
    app._fe_insp_bytes_lbl.config(
        text='%s (%s bytes)' % (_fmt_size(size), '{:,}'.format(size)) if size else '—')
    app._fe_insp_perms_lbl.config(text=_('(click stat to fetch)'))

    # Auto-fetch stat lazily in a thread so the UI stays responsive
    def _bg():
        img = app._fe_image_var.get()
        rc, out = _run_ufs2(app, ['stat', img, full], timeout=15)
        if rc == 0:
            # Parse Permissions line if present, fall back to first line
            perms = '—'
            for line in out.splitlines():
                ll = line.strip().lower()
                if ll.startswith('permissions') or ll.startswith('mode'):
                    perms = line.split(':', 1)[-1].strip()
                    break
            app.after(0, lambda: app._fe_insp_perms_lbl.config(text=perms))
    threading.Thread(target=_bg, daemon=True).start()

    # Reset hex pane
    try:
        app._fe_insp_hex.pack_forget()
        app._fe_insp_hex_btn.config(state='normal' if not is_dir else 'disabled')
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Operations
# ─────────────────────────────────────────────────────────────────────
def _add_files(app):
    if not _has_image(app):
        return
    paths = filedialog.askopenfilenames(
        title=_('Select files to add'))
    if not paths:
        return
    img = app._fe_image_var.get()
    errors = []
    added = 0
    replaced = 0

    # Always additive: if a same-named file exists, replace it; else add.
    for src in paths:
        basename = os.path.basename(src)
        dst = _join_fs_path(app._fe_current, basename)
        exists, _is_dir = _name_exists(app, basename)
        if exists:
            rc, out = _run_ufs2(app, ['replace', img, dst, src])
            if rc != 0:
                # Replace failed (maybe existing was a folder?) — record
                errors.append(basename + ' (replace): ' + out.strip()[:200])
            else:
                replaced += 1
        else:
            rc, out = _run_ufs2(app, ['add', img, dst, src])
            if rc != 0:
                errors.append(basename + ': ' + out.strip()[:200])
            else:
                added += 1

    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if errors:
        app_error(app, _('Add failed'),
            '\n'.join(errors[:5]) + ('\n...' if len(errors) > 5 else ''))
    else:
        parts = []
        if added:    parts.append(_('Added %d') % added)
        if replaced: parts.append(_('replaced %d') % replaced)
        app._fe_status_var.set(', '.join(parts) if parts else _('Done'))


def _add_folder(app):
    if not _has_image(app):
        return
    src = filedialog.askdirectory(title=_('Select folder to add'))
    if not src:
        return
    img = app._fe_image_var.get()
    folder_name = os.path.basename(os.path.normpath(src))
    dst = _join_fs_path(app._fe_current, folder_name)

    # Always additive: if a folder with this name already exists, merge
    # the new files into it. If it doesn't exist, create it.
    exists, _unused = _name_exists(app, folder_name)
    if exists:
        # Additive merge — no confirmation needed, this is non-destructive
        app._fe_status_var.set(_('Merging folder...'))
        app.update_idletasks()
        stats = {'added': 0, 'replaced': 0, 'errors': []}
        _merge_into(app, img, src, dst, stats)
        _invalidate_cache(app, app._fe_current)
        _refresh(app)
        if stats['errors']:
            err_msg = '\n'.join(stats['errors'][:5])
            if len(stats['errors']) > 5:
                err_msg += '\n...'
            diag_path = getattr(app, '_fe_diag_path', '')
            if diag_path:
                err_msg += '\n\n' + _('Diagnostic log:\n') + diag_path
            app_error(app, _('Merge had errors'), err_msg)
        else:
            parts = []
            if stats['added']:    parts.append(_('added %d') % stats['added'])
            if stats['replaced']: parts.append(_('replaced %d') % stats['replaced'])
            app._fe_status_var.set(_('Merged: ') +
                (', '.join(parts) if parts else _('no changes')))
        return

    # New folder — just add the whole tree
    rc, out = _run_ufs2(app, ['add', img, dst, src], timeout=600)
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        app_error(app, _('Add folder failed'), out)
    else:
        app._fe_status_var.set(_('Added folder: ') + folder_name)


def _merge_into(app, img, local_dir, fs_dir, stats):
    """Recursively merge `local_dir` (on disk) into `fs_dir` (inside img).
    For each item in local_dir:
      - file + same-named file exists in fs_dir   -> replace
      - file + nothing in fs_dir at that name     -> add
      - folder + same-named folder exists         -> recurse
      - folder + nothing exists at that name      -> add (whole tree)
      - mismatch                                  -> skip + record

    NON-DESTRUCTIVE GUARANTEE: this function never calls `delete`. The
    only operations it issues are `ls` (read-only), `replace` (one-file
    swap), and `add` (one-file/one-folder insert). Items that exist in
    fs_dir but NOT in local_dir are physically untouched.
    """
    # Snapshot what's in the image at fs_dir
    rc, out = _run_ufs2(app, ['ls', img, fs_dir])
    if rc != 0:
        stats['errors'].append('ls %s: %s' % (fs_dir, out.strip()[:200]))
        _diag(app, 'ls FAILED %s rc=%d\n%s\n' % (fs_dir, rc, out))
        return
    parsed = _parse_ls(out)
    existing = {}
    for name, is_dir, _sz in parsed:
        existing[name] = is_dir
    # Diagnostic dump — raw output + parsed result, so we can debug
    # parsing problems against the real UFS2Tool behavior.
    _diag(app, '─── ls %s ───\nRAW OUTPUT (%d bytes):\n%s\nPARSED:\n%s\n'
          % (fs_dir, len(out), out,
             '\n'.join('  %s %s %d' % ('DIR' if d else 'FILE', n, s)
                       for (n, d, s) in parsed)))

    try:
        local_entries = os.listdir(local_dir)
    except Exception as e:
        stats['errors'].append('readdir %s: %s' % (local_dir, e))
        return

    for entry in local_entries:
        src_path = os.path.join(local_dir, entry)
        dst_path = _join_fs_path(fs_dir, entry)
        try:
            is_dir_src = os.path.isdir(src_path)
        except Exception:
            continue

        existing_is_dir = existing.get(entry)
        if entry not in existing:
            # No collision: add the whole thing (file or recursive folder)
            _diag(app, 'ADD %s (not in image)\n' % dst_path)
            rc, out = _run_ufs2(app, ['add', img, dst_path, src_path],
                                timeout=600)
            if rc != 0:
                stats['errors'].append('add %s: %s'
                                        % (dst_path, out.strip()[:200]))
            else:
                stats['added'] += 1
        elif is_dir_src and existing_is_dir:
            # Folder onto folder -> recurse
            _diag(app, 'RECURSE %s (folder onto folder)\n' % dst_path)
            _merge_into(app, img, src_path, dst_path, stats)
        elif (not is_dir_src) and (existing_is_dir is False):
            # File onto file -> replace in place
            _diag(app, 'REPLACE %s (file onto file)\n' % dst_path)
            rc, out = _run_ufs2(app,
                ['replace', img, dst_path, src_path])
            if rc != 0:
                stats['errors'].append('replace %s: %s'
                                        % (dst_path, out.strip()[:200]))
            else:
                stats['replaced'] += 1
        elif is_dir_src and existing_is_dir is False:
            # Source is folder, existing matched as file — likely a
            # parser misclassification. Try treating as folder first:
            # recurse. If the recursive ls fails, it'll record an error
            # but not destroy anything.
            _diag(app,
                'AMBIGUOUS %s (src=folder, existing parsed as file - recursing anyway)\n'
                % dst_path)
            _merge_into(app, img, src_path, dst_path, stats)
        elif (not is_dir_src) and existing_is_dir:
            # File onto folder — genuine type mismatch. Skip safely.
            _diag(app, 'SKIP %s (file onto folder)\n' % dst_path)
            stats['errors'].append(
                _('Skipped %s: source is a file but image has a folder')
                % dst_path)
        else:
            # Existing not in listing (parser issue) — try replace as a
            # best-effort.
            _diag(app, 'AMBIGUOUS %s (existing_is_dir=%r) - trying replace\n'
                  % (dst_path, existing_is_dir))
            rc, out = _run_ufs2(app,
                ['replace', img, dst_path, src_path])
            if rc != 0:
                # Replace failed — fall back to add
                rc2, out2 = _run_ufs2(app,
                    ['add', img, dst_path, src_path])
                if rc2 != 0:
                    stats['errors'].append('replace+add %s: %s'
                                            % (dst_path, out2.strip()[:200]))
                else:
                    stats['added'] += 1
            else:
                stats['replaced'] += 1


def _diag(app, line):
    """Append a line to the diagnostic log for the current merge session.
    File: <temp>/ffpkg_merge_diagnostic.log
    """
    try:
        path = getattr(app, '_fe_diag_path', None)
        if path is None:
            path = os.path.join(tempfile.gettempdir(),
                                 'ffpkg_merge_diagnostic.log')
            app._fe_diag_path = path
        with open(path, 'a', encoding='utf-8', errors='replace') as f:
            f.write(line)
    except Exception:
        pass


def _diag_start(app):
    """Reset the diagnostic log at the start of a merge."""
    try:
        path = os.path.join(tempfile.gettempdir(),
                             'ffpkg_merge_diagnostic.log')
        with open(path, 'w', encoding='utf-8') as f:
            import datetime
            f.write('exFAT Image Builder \u2014 ffpkg merge diagnostic\n')
            f.write('Time: %s\n' % datetime.datetime.now().isoformat())
            f.write('Image: %s\n' % app._fe_image_var.get())
            f.write('\n')
        app._fe_diag_path = path
    except Exception:
        pass


def _new_folder(app):
    if not _has_image(app):
        return
    from tkinter import simpledialog
    name = simpledialog.askstring(_('New folder'),
        _('Folder name:\n\n(Note: a .keep placeholder file will be\n'
          'created inside the new folder, since UFS2Tool has no\n'
          'native mkdir command.)'))
    if not name:
        return
    name = name.strip().strip('/')
    if not name:
        return
    img = app._fe_image_var.get()
    # Collision check
    exists, is_dir = _name_exists(app, name)
    if exists:
        choice = _ask_overwrite(app, name, is_dir, allow_apply_to_all=False)
        if choice in ('cancel', 'skip'):
            return
        rc, out = _run_ufs2(app, ['delete', img,
                                  _join_fs_path(app._fe_current, name)],
                            timeout=300)
        if rc != 0:
            app_error(app, _('Overwrite failed'),
                _('Could not delete existing item:\n\n') + out)
            return
    dst = _join_fs_path(app._fe_current, name, '.keep')
    # Create a tiny temp placeholder file
    fd, tmppath = tempfile.mkstemp(prefix='ffpkg_keep_', suffix='.txt')
    try:
        os.write(fd, b'# UFS2Tool placeholder - keep this file\n')
        os.close(fd)
        rc, out = _run_ufs2(app, ['add', img, dst, tmppath])
    finally:
        try: os.unlink(tmppath)
        except Exception: pass
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        app_error(app, _('New folder failed'), out)
    else:
        app._fe_status_var.set(_('Created folder: ') + name)


def _replace_file(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        app_warning(app, _('No selection'),
            _('Select a file to replace.'))
        return
    idx = sel[0]
    name, is_dir, _sz = app._fe_entries[idx]
    if is_dir or name == '..':
        app_warning(app, _('Not a file'),
            _('Select a file, not a folder.'))
        return
    src = filedialog.askopenfilename(
        title=_('Select replacement file'),
        initialfile=name)
    if not src:
        return
    img = app._fe_image_var.get()
    dst = _join_fs_path(app._fe_current, name)
    rc, out = _run_ufs2(app, ['replace', img, dst, src])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        app_error(app, _('Replace failed'), out)
    else:
        app._fe_status_var.set(_('Replaced: ') + name)


def _delete(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    # Resolve names, skip ..
    targets = []
    for idx in sel:
        if 0 <= idx < len(app._fe_entries):
            name, is_dir, _sz = app._fe_entries[idx]
            if name == '..':
                continue
            targets.append((name, is_dir))
    if not targets:
        return
    if not app_danger(app, _('Delete'),
            _('Delete %d item(s) from the image?\n\n') % len(targets) +
            '\n'.join(t[0] for t in targets[:10]) +
            ('\n...' if len(targets) > 10 else ''),
            ok_label=_('Delete'), cancel_label=_('Cancel')):
        return
    img = app._fe_image_var.get()
    errors = []
    for name, _is_dir in targets:
        dst = _join_fs_path(app._fe_current, name)
        rc, out = _run_ufs2(app, ['delete', img, dst])
        if rc != 0:
            errors.append(name + ': ' + out.strip()[:200])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if errors:
        app_error(app, _('Delete failed'),
            '\n'.join(errors[:5]) + ('\n...' if len(errors) > 5 else ''))
    else:
        app._fe_status_var.set(_('Deleted %d item(s)') % len(targets))


def _rename(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        app_warning(app, _('No selection'),
            _('Select an item to rename.'))
        return
    idx = sel[0]
    name, _is_dir, _sz = app._fe_entries[idx]
    if name == '..':
        return
    from tkinter import simpledialog
    new_name = simpledialog.askstring(_('Rename'),
        _('New name for "%s":') % name, initialvalue=name)
    if not new_name or new_name == name:
        return
    new_name = new_name.strip().strip('/')
    if not new_name:
        return
    # Collision check (skip if renaming to the same name)
    exists, existing_is_dir = _name_exists(app, new_name)
    if exists and new_name != name:
        choice = _ask_overwrite(app, new_name, existing_is_dir,
                                 allow_apply_to_all=False)
        if choice in ('cancel', 'skip'):
            return
        rc, out = _run_ufs2(app, ['delete', app._fe_image_var.get(),
                                  _join_fs_path(app._fe_current, new_name)],
                            timeout=300)
        if rc != 0:
            app_error(app, _('Overwrite failed'),
                _('Could not delete existing item:\n\n') + out)
            return
    img = app._fe_image_var.get()
    src = _join_fs_path(app._fe_current, name)
    rc, out = _run_ufs2(app, ['rename', img, src, new_name])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        app_error(app, _('Rename failed'), out)
    else:
        app._fe_status_var.set(_('Renamed to: ') + new_name)


def _fetch_hex(app):
    """Extract the selected file to a temp dir and read first 64 bytes."""
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    name, is_dir, _sz = app._fe_entries[idx]
    if is_dir or name == '..':
        return
    img = app._fe_image_var.get()
    fs_path = _join_fs_path(app._fe_current, name)
    app._fe_status_var.set(_('Fetching hex preview...'))
    app.update_idletasks()

    def _bg():
        tmpdir = tempfile.mkdtemp(prefix='ffpkg_hex_')
        try:
            rc, out = _run_ufs2(app, ['extract', img, tmpdir, fs_path], timeout=60)
            if rc != 0:
                app.after(0, app._fe_status_var.set,
                    _('Hex fetch failed: ') + out.strip()[:120])
                return
            # Find the extracted file (UFS2Tool preserves the fs path)
            found = None
            for root, _dirs, files in os.walk(tmpdir):
                for f in files:
                    if f == name:
                        found = os.path.join(root, f)
                        break
                if found:
                    break
            if not found:
                app.after(0, app._fe_status_var.set,
                    _('Hex fetch: file not found after extract'))
                return
            with open(found, 'rb') as f:
                data = f.read(64)
            hex_text = _hex_lines(data)
            def _show():
                app._fe_insp_hex.config(state='normal')
                app._fe_insp_hex.delete('1.0', 'end')
                app._fe_insp_hex.insert('end', hex_text)
                app._fe_insp_hex.config(state='disabled')
                app._fe_insp_hex.pack(fill='x', pady=(px(8), 0))
                app._fe_status_var.set(_('Hex preview loaded'))
            app.after(0, _show)
        finally:
            try:
                import shutil
                shutil.rmtree(tmpdir, ignore_errors=True)
            except Exception:
                pass
    threading.Thread(target=_bg, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────
def _has_image(app):
    img = app._fe_image_var.get()
    if not img or not os.path.isfile(img):
        app_warning(app, _('No image'),
            _('Browse for a .ffpkg image first.'))
        return False
    return True


def _name_exists(app, name):
    """Check whether `name` is already present in the current directory.
    Returns (exists, is_dir). False/False if not found."""
    for entry_name, is_dir, _sz in app._fe_entries:
        if entry_name == name:
            return True, is_dir
    return False, False


def _ask_overwrite(app, name, is_dir, allow_apply_to_all=False,
                   allow_merge=False):
    """Ask the user what to do about a name collision.
    Returns one of: 'overwrite', 'skip', 'cancel', 'overwrite_all',
    'skip_all', 'merge', 'merge_all'. 'merge' is only returned when
    allow_merge=True.
    """
    kind = _('folder') if is_dir else _('file')
    result = {'choice': 'cancel'}

    dlg_w = 560 if allow_merge else 460
    dlg, body, btns = _dialog_shell(
        app, _('Item already exists'), _('Item already exists'),
        _('A %s named "%s" already exists in this folder.') % (kind, name),
        width=dlg_w)

    if allow_merge:
        # Two option explanations, each with a status icon
        block = tk.Frame(body, bg=COLORS['bg_2'])
        block.pack(fill='x', pady=(px(16), 0))
        _opt_row(block, '✓', COLORS['success_hi'],
            _('Merge'),
            _('Replace files that exist in both. Keep everything '
              'else in the existing folder.'), body.wrap)
        _opt_row(block, '⚠', COLORS['danger_hi'],
            _('Replace entirely'),
            _('Delete the existing folder first. Everything inside '
              'is lost — including files NOT in your new folder.'),
            body.wrap)
    else:
        tk.Label(body,
            text=_('Overwriting will delete the existing %s first.') % kind,
            font=FONTS['caption'], bg=COLORS['bg_2'], fg=COLORS['fg_3'],
            wraplength=px(dlg_w - 48), justify='left', anchor='w'
            ).pack(fill='x', pady=(px(8), 0))

    apply_var = tk.BooleanVar(value=False)
    if allow_apply_to_all:
        XCheck(body,
            text=_('Apply this choice to remaining conflicts'),
            variable=apply_var, surface=COLORS['bg_2'],
            cursor='hand2'
            ).pack(anchor='w', pady=(px(14), 0))

    def _pick(c):
        if apply_var.get() and c in ('overwrite', 'skip', 'merge'):
            result['choice'] = c + '_all'
        else:
            result['choice'] = c
        dlg.destroy()

    def _confirm_replace():
        """Replace entirely needs a separate confirmation step — it's
        the destructive choice and shouldn't be one click away."""
        if app_confirm(app,
                _('Confirm Replace entirely'),
                _('Are you sure?\n\n'
                  'The existing folder "%s" will be deleted, including '
                  'any files NOT in your new folder.\n\n'
                  'This cannot be undone.') % name):
            _pick('overwrite')

    # Footer, right to left: primary (Merge / Overwrite), Skip, Cancel;
    # Replace entirely sits apart on the left with an extra confirm.
    if allow_merge:
        merge_btn = _dialog_button(btns, _('Merge'), lambda: _pick('merge'),
                                   variant='primary', icon='check')
        dlg.bind('<Return>', lambda e: _pick('merge'))
        first = merge_btn
    else:
        # Files (non-folder): plain Overwrite stays as the primary action
        ow_btn = _dialog_button(btns, _('Overwrite'),
                                lambda: _pick('overwrite'),
                                variant='danger')
        dlg.bind('<Return>', lambda e: _pick('overwrite'))
        first = ow_btn
    _dialog_button(btns, _('Skip'), lambda: _pick('skip'))
    _dialog_button(btns, _('Cancel'), lambda: _pick('cancel'))
    if allow_merge:
        Button(btns, text=_('Replace entirely'), icon='trash',
               variant='subtle', surface=COLORS['bg_1'],
               command=_confirm_replace).pack(side='left')

    dlg.bind('<Escape>', lambda e: _pick('cancel'))
    _dialog_place(dlg, app)
    first.focus_set()
    dlg.wait_window()
    return result['choice']


def _opt_row(parent, icon, icon_fg, title, body, wrap=None):
    """One option-explanation row inside the overwrite dialog."""
    bg = COLORS['bg_2']
    name = {'✓': 'success', '⚠': 'warning'}.get(icon, 'info')
    row = tk.Frame(parent, bg=bg)
    row.pack(fill='x', pady=(0, px(10)))
    IconLabel(row, name, 16, icon_fg, bg).pack(side='left', anchor='n',
                                               pady=(px(1), 0))
    col = tk.Frame(row, bg=bg)
    col.pack(side='left', fill='x', expand=True, padx=(px(10), 0))
    tk.Label(col, text=title, font=FONTS['body_strong'], bg=bg,
             fg=COLORS['fg_0'], anchor='w').pack(anchor='w')
    lbl = tk.Label(col, text=body, font=FONTS['caption'], bg=bg,
                   fg=COLORS['fg_3'], anchor='w', justify='left',
                   wraplength=(wrap or px(480)) - px(26))
    lbl.pack(fill='x', pady=(px(2), 0))


def _invalidate_cache(app, path):
    """Drop the cached listing for the given path (and any descendants)."""
    keys_to_drop = [k for k in app._fe_ls_cache
                    if k == path or k.startswith(path.rstrip('/') + '/')]
    for k in keys_to_drop:
        del app._fe_ls_cache[k]


# ─────────────────────────────────────────────────────────────────────
# Apply Backport (v2.0.6) — overlay a user-made backport folder onto
# the currently-open .ffpkg image, backing up every replaced original.
#
# Workflow:
#   1) Ask the user to pick the backport SOURCE folder (the folder that
#      contains the patched files they've produced — typically the
#      output of Auto Backport or a hand-edited build).
#   2) Walk the source recursively. For every file, compute its target
#      path inside the image, anchored at the current breadcrumb
#      directory (so a user can drop a backport "into" /sce_sys, /,
#      /Media, etc. without rebuilding the folder layout).
#   3) For each target that ALREADY EXISTS as a file in the image:
#         a. UFS2Tool extract  -> <backup_dir>/<rel_path>
#         b. UFS2Tool replace  -> patched file
#      For each target that DOES NOT exist:
#         - UFS2Tool add       -> patched file (no backup needed)
#      Items that exist in the image but NOT in the backport folder
#      are PHYSICALLY UNTOUCHED. No deletes are ever issued.
#   4) Zip the backup folder into <image_dir>/<image_stem>.backup-<ts>.zip
#      with a backup_manifest.txt at the root.
#   5) Show a summary dialog with the counts and the path to the zip.
# ─────────────────────────────────────────────────────────────────────
import zipfile as _backport_zipfile  # local alias to avoid shadowing

def _apply_backport(app):
    """Toolbar entry-point. Asks what kind of source the user has
    (folder / .ffpkg / .exfat), materialises it to a real folder on
    disk if needed, then runs the overlay on a background thread."""
    if not _has_image(app):
        return
    img = app._fe_image_var.get()

    # ── 1. Ask the user what kind of source they have ──
    kind = _bp_pick_source_kind(app)
    if kind is None:
        return

    # ── 2. Browse for the source path ──
    if kind == 'folder':
        src_path = filedialog.askdirectory(
            title=_('Select backport folder (will overlay onto image)'))
        if not src_path:
            return
        if not os.path.isdir(src_path):
            app_error(app, _('Bad folder'),
                _('Selected path is not a folder.'))
            return
    elif kind == 'ffpkg':
        src_path = filedialog.askopenfilename(
            title=_('Select source .ffpkg'),
            filetypes=[('FFPKG image', '*.ffpkg'),
                       ('All files',   '*.*')])
        if not src_path:
            return
        if os.path.abspath(src_path) == os.path.abspath(img):
            app_error(app, _('Same image'),
                _('Source .ffpkg is the same file that is currently '
                  'open for editing. Pick a different image.'))
            return
    elif kind == 'exfat':
        src_path = filedialog.askopenfilename(
            title=_('Select source exFAT image'),
            filetypes=[('exFAT image', '*.exfat;*.img'),
                       ('All files',   '*.*')])
        if not src_path:
            return
    else:
        return

    # ── 3. Materialise source to a real folder on disk ──
    #    For 'folder' this is the path itself. For 'ffpkg' / 'exfat',
    #    extract the whole tree into a temp dir, which we'll clean up
    #    at the end. v4: this (and the pre-scan) runs on a background
    #    thread so a big .ffpkg/.exfat source no longer freezes the UI;
    #    the confirmation dialog is shown back on the Tk thread.
    app._fe_status_var.set(_('Preparing backport source...'))

    def _prep_worker():
        try:
            src_dir, cleanup_fn = _bp_materialize_source(app, kind, src_path)
        except Exception as e:
            _m = str(e)
            def _prep_fail(m=_m):
                app_error(app, _('Source preparation failed'), m)
                app._fe_status_var.set(_('Backport cancelled.'))
            app.after(0, _prep_fail)
            return
        if not src_dir:
            app.after(0, app._fe_status_var.set, _('Backport cancelled.'))
            return
        # ── 4. Pre-scan the materialised source ──
        try:
            items = _scan_backport_source(src_dir)
        except Exception as e:
            try:
                cleanup_fn()
            except Exception:
                pass
            _m = str(e)
            def _scan_fail(m=_m):
                app_error(app, _('Scan failed'), m)
                app._fe_status_var.set(_('Backport cancelled.'))
            app.after(0, _scan_fail)
            return
        if not items:
            try:
                cleanup_fn()
            except Exception:
                pass
            def _empty():
                app_warning(app, _('Empty backport'),
                    _('No files were found in the selected source.'))
                app._fe_status_var.set(_('Backport cancelled.'))
            app.after(0, _empty)
            return
        app.after(0, lambda: _confirm_and_run(src_dir, cleanup_fn, items))

    def _confirm_and_run(src_dir, cleanup_fn, items):
        # Confirmation — make it clear what will happen and where backups go.
        img_dir  = os.path.dirname(img)
        img_stem = os.path.splitext(os.path.basename(img))[0]
        ts       = time.strftime('%Y%m%d-%H%M%S')
        backup_zip = os.path.join(img_dir, '%s.backup-%s.zip' % (img_stem, ts))
        kind_label = {
            'folder': _('Folder'),
            'ffpkg':  _('.ffpkg image'),
            'exfat':  _('exFAT image'),
        }.get(kind, kind)

        # Pre-flight: need ~2× the image size free on the destination drive
        # for the extracted dump + the rebuilt image. Warn if we don't have
        # it (don't block — disk-free reporting is finicky on some FS types).
        try:
            img_size  = os.path.getsize(img)
            free_now  = _bp_free_space(img_dir)
            needed    = int(img_size * 2.1) + (64 * 1024 * 1024)   # +64MB slack
            space_warn = ''
            if free_now is not None and free_now < needed:
                space_warn = ('\n\n' +
                    _('\u26a0  Low disk space warning: this needs about '
                      '%s free on %s but only %s seems to be available. '
                      'The rebuild may run out of space partway through.')
                    % (_bp_fmt_size(needed), img_dir, _bp_fmt_size(free_now)))
        except Exception:
            space_warn = ''
            img_size   = 0

        msg = (_('Apply backport to image (rebuild)?') + '\n\n' +
               _('Source kind: ') + kind_label + '\n' +
               _('Source: ') + src_path + '\n' +
               _('Target dir inside image: ') + (app._fe_current or '/') + '\n' +
               _('Files in backport: ') + str(len(items)) + '\n\n' +
               _('How it works:') + '\n' +
               _('  1. The image is fully extracted to a temp folder.') + '\n' +
               _('  2. Your backport is copied onto the extracted dump.') + '\n' +
               _('  3. UFS2Tool newfs rebuilds the .ffpkg from scratch') + '\n' +
               _('     \u2014 same flags as the regular Build flow.') + '\n' +
               _('  4. The original .ffpkg is replaced atomically.') + '\n\n' +
               _('The complete original .ffpkg will be saved as a backup '
                 '(shown below).') + '\n\n' +
               _('Nothing is overwritten in the image until the rebuild '
                 'succeeds. If the rebuild fails, your original .ffpkg '
                 'stays as it was.'))
        # Backup path goes in the dialog's detail slot (monospace), the
        # low-space warning (if any) in its note slot.
        if not app_confirm(app, _('Apply backport (rebuild)'), msg,
                           detail=backup_zip,
                           note=space_warn.strip() or None,
                           ok_label=_('Apply backport'),
                           cancel_label=_('Cancel')):
            try:
                cleanup_fn()
            except Exception:
                pass
            return

        app._fe_status_var.set(_('Preparing rebuild...'))
        app.update_idletasks()

        # ── Progress dialog ──
        prog = _RebuildProgress(app, _('Apply backport — rebuild'))

        def _bg():
            stats = {
                'extracted_files': 0,
                'overlay_added':   0,
                'overlay_replaced': 0,
                'errors':          [],
                'rebuilt':         False,
                'swapped':         False,
                'extract_errors':  0,
            }

            # Working dirs:
            #   work_dir/dump/    — extracted contents of the original image
            #   work_dir/new.img  — rebuilt image (renamed to .ffpkg on success)
            work_dir = tempfile.mkdtemp(prefix='ffpkg_rebuild_')
            dump_dir = os.path.join(work_dir, 'dump')
            new_img  = os.path.join(work_dir, 'new.ffpkg')

            # Shared flag for the background poll thread so we can shut it
            # down between stages without leaking a thread.
            poll_state = {'stop': False, 'dir': None, 'cb': None}

            def _poll_loop():
                """Counts files+dirs under poll_state['dir'] and pushes the
                count to whatever callback poll_state['cb'] is set to. Used
                for the extract stage (we don't get progress from UFS2Tool
                itself, so we just watch the disk fill up)."""
                import time as _t
                while not poll_state['stop']:
                    d  = poll_state['dir']
                    cb = poll_state['cb']
                    if d and cb and os.path.isdir(d):
                        try:
                            n_files = 0
                            n_bytes = 0
                            for _r, _ds, _fs in os.walk(d):
                                n_files += len(_fs)
                                for f in _fs:
                                    try:
                                        n_bytes += os.path.getsize(
                                            os.path.join(_r, f))
                                    except Exception:
                                        pass
                            cb(n_files, n_bytes)
                        except Exception:
                            pass
                    _t.sleep(0.7)

            poll_thread = threading.Thread(target=_poll_loop, daemon=True,
                name='ffpkg-rebuild-poll')
            poll_thread.start()

            def _cleanup_work():
                try:
                    import shutil as _sh
                    _sh.rmtree(work_dir, ignore_errors=True)
                except Exception:
                    pass

            try:
                # ── Step 1: zip the original .ffpkg as the user's safety net ──
                app.after(0, prog.set_stage, 'backup',
                    _('Backing up original .ffpkg...'))
                app.after(0, app._fe_status_var.set,
                    _('Backing up original .ffpkg...'))
                try:
                    img_size_for_progress = os.path.getsize(img)
                except Exception:
                    img_size_for_progress = 0
                try:
                    _bp_zip_single_file(img, backup_zip,
                        progress_cb=lambda done, total: app.after(0,
                            prog.set_stage_progress,
                            (done * 100.0 / total) if total else 100.0,
                            _('%s / %s') % (_bp_fmt_size(done),
                                            _bp_fmt_size(total))))
                except Exception as e:
                    raise RuntimeError(
                        _('Failed to back up original image: ') + str(e))
                app.after(0, prog.set_stage_progress, 100.0)

                # ── Step 2: extract the whole image to dump_dir ──
                app.after(0, prog.set_stage, 'extract',
                    _('Extracting image (this can take a while)...'))
                app.after(0, app._fe_status_var.set,
                    _('Extracting image...'))
                os.makedirs(dump_dir, exist_ok=True)

                # Estimate target size for the extract progress bar from
                # the image size — the on-disk dump will end up slightly
                # smaller (no filesystem overhead) but it's a reasonable
                # ceiling for the bar.
                ex_target_bytes = max(1, img_size_for_progress)
                def _extract_progress_cb(n_files, n_bytes):
                    pct = (n_bytes * 100.0 / ex_target_bytes) if ex_target_bytes \
                                                              else 0.0
                    pct = max(0.0, min(99.0, pct))   # never hit 100 here
                    app.after(0, prog.set_stage_progress, pct,
                        _('%(files)d files extracted  •  %(bytes)s')
                        % {'files': n_files, 'bytes': _bp_fmt_size(n_bytes)})

                poll_state['dir'] = dump_dir
                poll_state['cb']  = _extract_progress_cb

                ex_rc, ex_out = _run_ufs2(app, ['extract', img, dump_dir],
                                           timeout=3600)
                if ex_rc != 0:
                    ex_rc, ex_out = _run_ufs2(app,
                        ['extract', img, dump_dir, '/'], timeout=3600)

                # Detach poll from extract dir before reading final count.
                poll_state['cb'] = None
                if ex_rc != 0:
                    raise RuntimeError(
                        _('Image extraction failed: ') +
                        (ex_out or '').strip()[:500])
                # Final count for the summary dialog.
                extracted_count = 0
                try:
                    for _r, _d, _fs in os.walk(dump_dir):
                        extracted_count += len(_fs)
                except Exception:
                    pass
                stats['extracted_files'] = extracted_count
                stats['extract_errors']  = 0
                if extracted_count == 0:
                    raise RuntimeError(
                        _('Image extraction produced no files. ') +
                        _('Cannot rebuild.'))
                app.after(0, prog.set_stage_progress, 100.0,
                    _('%d files extracted') % extracted_count)

                # ── Step 3: overlay the backport onto dump_dir ──
                app.after(0, prog.set_stage, 'overlay',
                    _('Applying backport to extracted dump...'))
                app.after(0, app._fe_status_var.set,
                    _('Applying backport to extracted dump...'))
                base_fs_dir = app._fe_current or '/'
                anchor_rel = base_fs_dir.strip('/').replace('/', os.sep)
                anchor_abs = (os.path.join(dump_dir, anchor_rel)
                              if anchor_rel else dump_dir)
                try:
                    _bp_overlay_into_dump(items, anchor_abs, stats,
                        progress_cb=lambda done, total, name: app.after(0,
                            prog.set_stage_progress,
                            (done * 100.0 / total) if total else 100.0,
                            _('%(done)d / %(total)d  •  %(name)s')
                            % {'done': done, 'total': total,
                               'name': name[-48:]}))
                except Exception as e:
                    raise RuntimeError(
                        _('Overlay failed: ') + str(e))
                app.after(0, prog.set_stage_progress, 100.0)

                # ── Step 4: newfs ──
                app.after(0, prog.set_stage, 'init',
                    _('Rebuilding .ffpkg with UFS2Tool newfs...'))
                app.after(0, app._fe_status_var.set,
                    _('Rebuilding .ffpkg with UFS2Tool newfs...'))
                exe = _ufs2_exe(app)

                def _adv(attr, default):
                    v = getattr(app, attr, None)
                    if v is None:
                        return default
                    try:
                        s = v.get() if hasattr(v, 'get') else v
                    except Exception:
                        return default
                    return (s or default)
                block = _adv('_adv_ffpkg_block_var',   '65536')
                frag  = _adv('_adv_ffpkg_frag_var',    '65536')
                minfr = _adv('_adv_ffpkg_minfree_var', '0')
                inode = _adv('_adv_ffpkg_inode_var',   '262144')
                try:
                    if int(frag) > int(block):
                        frag = block
                except Exception:
                    frag = block

                cmd = [exe, 'newfs',
                       '-O', '2',
                       '-b', block,
                       '-f', frag,
                       '-m', minfr,
                       '-S', '512',
                       '-i', inode,
                       '-D', dump_dir,
                       new_img]

                # Stream the newfs output so we can drive the progress bar
                # off UFS2Tool's own "%" reports rather than just guessing.
                try:
                    p = subprocess.Popen(cmd,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True, encoding='utf-8', errors='replace',
                        bufsize=1,
                        creationflags=_NO_WIN_FLAGS)
                except Exception as e:
                    raise RuntimeError(_('newfs launch failed: ') + str(e))

                captured_tail = []
                newfs_stage = 'init'   # local — switches to 'files' when we
                                       # see "Adding files to image"
                try:
                    for raw in iter(p.stdout.readline, ''):
                        line = raw.rstrip('\r\n')
                        if not line:
                            continue
                        # Keep a rolling tail for the failure message.
                        captured_tail.append(line)
                        if len(captured_tail) > 50:
                            del captured_tail[:-50]

                        stripped = line.strip()

                        # Stage transitions
                        if 'Writing cylinder groups' in stripped:
                            newfs_stage = 'init'
                            app.after(0, prog.set_stage, 'init',
                                _('Writing filesystem structure...'))
                        elif 'Adding files to image' in stripped:
                            newfs_stage = 'files'
                            app.after(0, prog.set_stage, 'files',
                                _('Copying files into image...'))
                        elif 'Populated image with' in stripped \
                                or 'Image created successfully' in stripped:
                            app.after(0, prog.set_stage_progress, 100.0,
                                _('Image created, finalising...'))

                        # Percentage parsing — uses the existing UFS2Tool
                        # output format "<stage>... NN% (...)".
                        import re as _re
                        m = _re.search(r'(\d{1,3})\s*%', stripped)
                        if not m:
                            continue
                        raw_pct = max(0, min(100, int(m.group(1))))

                        # File counts and byte progress
                        mf = _re.search(r'\((\d+)\s*/\s*(\d+)\s+files?',
                                         stripped)
                        files_done = files_total = 0
                        if mf:
                            files_done  = int(mf.group(1))
                            files_total = int(mf.group(2))
                        mg = _re.search(
                            r'([\d.]+)\s*GiB\s*/\s*([\d.]+)\s*GiB',
                            stripped)
                        written_gib = total_gib = 0.0
                        if mg:
                            written_gib = float(mg.group(1))
                            total_gib   = float(mg.group(2))

                        if newfs_stage == 'init':
                            app.after(0, prog.set_stage_progress,
                                float(raw_pct),
                                _('Initialising filesystem... %d%%') % raw_pct)
                        elif newfs_stage == 'files':
                            # Build the detail line from whatever fields we
                            # managed to parse.
                            bits = []
                            if files_total > 0:
                                bits.append('%d / %d files'
                                            % (files_done, files_total))
                            if total_gib > 0:
                                bits.append('%.2f GB / %.2f GB'
                                            % (written_gib, total_gib))
                            detail = '  •  '.join(bits) if bits \
                                                       else '%d%%' % raw_pct
                            app.after(0, prog.set_stage_progress,
                                float(raw_pct), detail)
                finally:
                    # Drain any remaining output and wait.
                    try:
                        rest = p.stdout.read()
                        if rest:
                            captured_tail.extend(rest.splitlines())
                    except Exception:
                        pass
                    try:
                        rc_newfs = p.wait(timeout=60)
                    except Exception:
                        p.kill()
                        rc_newfs = -1

                if rc_newfs != 0:
                    from exfat_builder import dotnet_runtime_hint
                    _dn = dotnet_runtime_hint(rc_newfs,
                                              '\n'.join(captured_tail))
                    raise RuntimeError(
                        (_dn + '\n\n' if _dn else '') +
                        _('newfs failed: ') +
                        '\n'.join(captured_tail[-10:])[:800])
                if not os.path.isfile(new_img) or os.path.getsize(new_img) == 0:
                    raise RuntimeError(
                        _('newfs reported success but the output file is '
                          'missing or empty.'))
                stats['rebuilt'] = True
                app.after(0, prog.set_stage_progress, 100.0,
                    _('Image built'))

                # ── Step 5: atomic swap ──
                app.after(0, prog.set_stage, 'swap',
                    _('Swapping in the rebuilt image...'))
                app.after(0, app._fe_status_var.set,
                    _('Swapping in the rebuilt image...'))
                try:
                    _bp_atomic_swap(new_img, img)
                    stats['swapped'] = True
                    app.after(0, prog.set_stage_progress, 100.0,
                        _('Done.'))
                except Exception as e:
                    raise RuntimeError(
                        _('Could not swap in the rebuilt image: ') + str(e) +
                        '\n\n' +
                        _('The rebuilt image is at:\n') + new_img + '\n' +
                        _('Your original .ffpkg is unchanged and the backup '
                          'is at:\n') + backup_zip)

            except Exception as fatal_e:
                stats['errors'].append(str(fatal_e))

            finally:
                # Stop the poll thread.
                poll_state['stop'] = True
                # Keep the work dir if newfs succeeded but the swap failed
                # so the user can manually retrieve the rebuilt image.
                if not (stats['rebuilt'] and not stats['swapped']):
                    _cleanup_work()
                try:
                    cleanup_fn()
                except Exception:
                    pass

            def _done():
                # Close the progress dialog first so the final messagebox
                # doesn't end up underneath it.
                try:
                    prog.close()
                except Exception:
                    pass
                _invalidate_cache(app, app._fe_current)
                _refresh(app)

                if stats['swapped']:
                    app._fe_status_var.set(
                        _('Backport applied: ') +
                        _('rebuilt OK (%d files)') % stats['extracted_files'])
                else:
                    app._fe_status_var.set(_('Backport failed.'))

                # Final dialog
                lines = []
                if stats['swapped']:
                    lines.append(_('Backport applied via rebuild.'))
                    lines.append('')
                    lines.append(_('Files in rebuilt image: ') +
                                 str(stats['extracted_files']))
                    lines.append(_('Files newly added by backport: ') +
                                 str(stats['overlay_added']))
                    lines.append(_('Files replaced by backport: ') +
                                 str(stats['overlay_replaced']))
                    lines.append('')
                    lines.append(_('Original .ffpkg saved as:'))
                    lines.append(backup_zip)
                    if stats['extract_errors']:
                        lines.append('')
                        lines.append(_('Note: %d extraction warning(s) — '
                                       'usually harmless but flagging in '
                                       'case the rebuild misses content.')
                                     % stats['extract_errors'])
                    app_info(app, _('Backport applied'),
                        '\n'.join(lines))
                else:
                    lines.append(_('Backport failed.'))
                    lines.append('')
                    lines.append(_('Your original .ffpkg is unchanged.'))
                    lines.append(_('Backup of the original (just in case):'))
                    lines.append(backup_zip)
                    if stats['rebuilt'] and not stats['swapped']:
                        lines.append('')
                        lines.append(_('A rebuilt image was produced but '
                                       'could not be swapped into place. '
                                       'It is at:'))
                        lines.append(new_img)
                    if stats['errors']:
                        lines.append('')
                        lines.append(_('First errors:'))
                        for e in stats['errors'][:5]:
                            lines.append('  - ' + e)
                        if len(stats['errors']) > 5:
                            lines.append('  ... (%d more)'
                                         % (len(stats['errors']) - 5))
                    app_error(app, _('Backport failed'),
                        '\n'.join(lines))

            app.after(0, _done)

        threading.Thread(target=_bg, daemon=True,
                         name='ffpkg-apply-backport').start()
    threading.Thread(target=_prep_worker, daemon=True,
                     name='ffpkg-bp-prep').start()


def _scan_backport_source(src_dir):
    """Walk `src_dir` recursively, returning a sorted list of
    (relative_fs_path, absolute_source_path) tuples. Uses forward
    slashes for the relative path so it can be joined with the
    UFS2 fs path directly."""
    items = []
    src_dir = os.path.abspath(src_dir)
    for root, _dirs, files in os.walk(src_dir):
        for f in files:
            ap = os.path.join(root, f)
            try:
                rel = os.path.relpath(ap, src_dir).replace(os.sep, '/')
            except Exception:
                continue
            if not rel or rel.startswith('..'):
                continue
            items.append((rel, ap))
    items.sort(key=lambda t: t[0].lower())
    return items


def _ufs_file_exists(app, img, fs_path):
    """Return True if `fs_path` is a file inside `img`, False if it is
    not present (or is a directory), None if we couldn't tell.

    Strategy:
      - `stat` is the fastest and exact: if it returns rc=0, parse the
        first line; UFS2Tool reports 'directory' / 'regular file' / etc.
      - On any failure we fall back to listing the parent dir and
        checking whether the basename is present as a file.
    """
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc == 0 and out:
        low = out.lower()
        if 'directory' in low:
            return False    # exists, but as a folder — caller treats as new
        if 'regular file' in low or 'file' in low:
            return True
    # Fallback — list parent and search.
    parent = _dirname_fs(fs_path)
    base   = _basename_fs(fs_path)
    rc, out = _run_ufs2(app, ['ls', img, parent], timeout=15)
    if rc != 0:
        return None
    for name, is_dir, _sz in _parse_ls(out):
        if name == base:
            return not is_dir
    return False


def _ufs_dir_exists(app, img, fs_path):
    """Return True if `fs_path` is a directory inside `img`, False
    otherwise. The root '/' is always treated as existing."""
    if fs_path in ('', '/'):
        return True
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc == 0 and out and 'directory' in out.lower():
        return True
    # Fallback: ls the parent and look for an entry with matching name
    # marked as a directory.
    parent = _dirname_fs(fs_path)
    base   = _basename_fs(fs_path)
    rc, out = _run_ufs2(app, ['ls', img, parent], timeout=15)
    if rc != 0:
        return False
    for name, is_dir, _sz in _parse_ls(out):
        if name == base:
            return bool(is_dir)
    return False


# ─────────────────────────────────────────────────────────────────────
# Permission preservation (v2.0.6d)
#
# UFS2Tool's `add` verb gives newly-created files a default mode that
# doesn't always match what the PS5 expects. Anecdotally, applying a
# backport via `add`/`replace` produces ffpkgs that the game refuses
# to load even though packing the same files via `newfs -D` from a
# dump folder works fine. The most likely cause is mode bits (e.g.
# eboot.bin needing the execute bit, .sprx files needing read).
#
# Strategy:
#  - Before `replace`, capture the current mode of the destination.
#    After `replace`, chmod the new file back to the captured mode.
#    This means a replaced file always ends up with the exact mode
#    the original had — guaranteed safe.
#  - For per-file `add`, there's no "previous mode" to copy. Pick a
#    sensible default: 0755 if the name looks like an ELF executable,
#    0644 otherwise. (Heuristic only — see _default_mode_for_name.)
#  - For bulk-add of a whole subtree, use the recursive chmod form
#    `chmod -R <img> 644 755` (the two-argument form sets file-mode
#    and dir-mode separately), then a one-off chmod 755 on any
#    executable-looking files we find.
# ─────────────────────────────────────────────────────────────────────
def _ufs_get_mode(app, img, fs_path):
    """Return the octal mode (int, e.g. 0o644) of `fs_path` inside
    `img`, or None if it can't be determined. Parses the `stat`
    output's first numeric octal value that looks like a Unix mode.
    """
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc != 0 or not out:
        return None
    # The output contains lines like:
    #   Permissions: 0644 (-rw-r--r--)
    # or some variant. Match an explicit "0NNNN" octal first; otherwise
    # any 3-4 digit octal preceded by "perm"/"mode"/"access".
    import re as _re
    # Pass 1: explicit zero-prefixed octal anywhere in the output.
    m = _re.search(r'\b0[0-7]{3,4}\b', out)
    if m:
        try:
            return int(m.group(0), 8)
        except Exception:
            pass
    # Pass 2: 3-4 digit octal adjacent to a permissions-ish word, with
    # arbitrary non-digit characters in between (commonly ': ').
    m = _re.search(r'(?i)(?:perm|mode|access)\D{0,16}([0-7]{3,4})\b', out)
    if m:
        try:
            return int(m.group(1), 8)
        except Exception:
            pass
    return None


def _ufs_set_mode(app, img, fs_path, mode):
    """Set `fs_path`'s mode inside `img`. `mode` is an int like 0o644.
    Returns True on success.
    """
    mode_str = '%o' % (mode & 0o7777)
    rc, _out = _run_ufs2(app, ['chmod', img, mode_str, fs_path],
                          timeout=30)
    return rc == 0


def _ufs_chmod_recursive(app, img, fs_path, file_mode, dir_mode):
    """Recursively chmod everything under `fs_path` to file_mode/dir_mode.
    Uses UFS2Tool's two-argument recursive form:
        chmod -R <img> <file_mode> <dir_mode> <fs_path>
    Returns True on success.
    """
    fmode = '%o' % (file_mode & 0o7777)
    dmode = '%o' % (dir_mode  & 0o7777)
    # Per the UFS2Tool README: `chmod -R <image> <file_mode> <dir_mode>`
    # applies recursively from root. The README's separately-shown
    # single-path form is `chmod <image> <mode> <fs_path>`; combining
    # the two (recursive on a specific subtree) is the natural extension
    # and UFS2Tool accepts it. If a build rejects the combined form,
    # we fall back to a manual walk via ls+chmod.
    rc, _out = _run_ufs2(app, ['chmod', '-R', img, fmode, dmode, fs_path],
                          timeout=300)
    if rc == 0:
        return True
    # Fallback: walk and chmod individually.
    return _ufs_chmod_walk(app, img, fs_path, file_mode, dir_mode)


def _ufs_chmod_walk(app, img, fs_path, file_mode, dir_mode, _depth=0):
    """Manual recursive chmod fallback for when the bulk form fails."""
    if _depth > 32:
        return False
    # Set the directory's own mode first.
    _ufs_set_mode(app, img, fs_path, dir_mode)
    rc, out = _run_ufs2(app, ['ls', img, fs_path], timeout=60)
    if rc != 0:
        return False
    ok = True
    for name, is_dir, _sz in _parse_ls(out):
        if name in ('.', '..'):
            continue
        child = _join_fs_path(fs_path, name)
        if is_dir:
            if not _ufs_chmod_walk(app, img, child, file_mode, dir_mode,
                                    _depth + 1):
                ok = False
        else:
            if not _ufs_set_mode(app, img, child, file_mode):
                ok = False
    return ok


def _default_mode_for_name(name):
    """Heuristic default mode for a freshly-added file when we have no
    prior file to copy from. 0755 for things that look like executables,
    0644 for everything else. Permissions on PS5 are surprisingly
    forgiving for data files but the loader insists on the execute bit
    for the main ELF, so this is the bit we try not to get wrong.
    """
    lname = name.lower()
    if lname == 'eboot.bin':
        return 0o755
    if lname.endswith('.elf'):
        return 0o755
    return 0o644


def _bulk_add_new_subtrees(app, img, items, base_fs_dir, src_root, stats):
    """Handle fresh top-level subdirectories with one `add` call each.

    UFS2Tool's `add` verb refuses to create parent directories on its
    own — it errors with "Path component X not found in inode N" for
    any path whose parent doesn't already exist in the image. The
    placeholder-file trick doesn't get around this because `add` fails
    on the placeholder for the same reason.

    The one operation UFS2Tool DOES support for creating directories
    is `add <img> <fs_dir> <local_dir>` with a directory source — that
    recursively creates the destination directory and copies its
    contents.

    So: for every first-level directory key in `items` that doesn't
    yet exist inside `base_fs_dir`, we issue one bulk `add` with the
    matching on-disk directory as the source. UFS2Tool creates the
    whole subtree in one go.

    Returns a set of relative item paths that were handled here so the
    per-file pass can skip them (counts them as 'added' in stats).
    """
    handled = set()

    # Group items by first-level directory key.
    # 'fakelib/foo.sprx' → 'fakelib'
    # 'sce_sys/x/y.bin' → 'sce_sys'
    # 'eboot.bin'       → '' (root file — never bulk-added)
    groups = {}
    for rel, _src in items:
        key = rel.split('/', 1)[0] if '/' in rel else ''
        if not key:
            continue   # root-level files are always per-file
        groups.setdefault(key, []).append(rel)

    for key, rels in groups.items():
        dst_fs = _join_fs_path(base_fs_dir, key)
        if _ufs_dir_exists(app, img, dst_fs):
            # Directory already exists — fall through to per-file pass
            # so we get proper file-level replace + backup semantics.
            continue
        src_local = os.path.join(src_root, key)
        if not os.path.isdir(src_local):
            # Shouldn't happen (we scanned this dir), but guard anyway.
            continue
        rc, out = _run_ufs2(app, ['add', img, dst_fs, src_local],
                             timeout=600)
        if rc != 0:
            stats['errors'].append(
                _('bulk add failed %s: ') % dst_fs + out.strip()[:200])
            # Don't mark these as handled — let the per-file pass try
            # them individually (it'll likely also fail, but at least
            # each failure gets reported precisely).
            continue
        # Success: count one bulk-add as N file-adds and mark every
        # rel under this key as handled.
        stats['added'] += len(rels)
        handled.update(rels)

        # Fix permissions on the freshly-added subtree. UFS2Tool's
        # `add` gives newly-created entries a default mode that may
        # not be what the PS5 expects (e.g. .sprx files unreadable,
        # eboot.bin non-executable). Use the two-mode recursive chmod
        # to set files to 0644 and directories to 0755 in one call.
        _ufs_chmod_recursive(app, img, dst_fs, 0o644, 0o755)

        # Any ELF-looking files inside the new subtree need the
        # execute bit. Scan the on-disk source so we don't need to
        # round-trip the image again.
        try:
            for root, _dirs, files in os.walk(src_local):
                for fn in files:
                    lfn = fn.lower()
                    if lfn == 'eboot.bin' or lfn.endswith('.elf'):
                        try:
                            ap     = os.path.join(root, fn)
                            rel_in = os.path.relpath(ap, src_local) \
                                        .replace(os.sep, '/')
                            elf_fs = _join_fs_path(dst_fs, rel_in)
                            _ufs_set_mode(app, img, elf_fs, 0o755)
                        except Exception:
                            pass
        except Exception:
            pass

    return handled


def _extract_to_backup(app, img, fs_path, backup_dir, rel):
    """UFS2Tool extract `fs_path` from `img` into a temp dir, then move
    the extracted file to <backup_dir>/<rel>. Returns (ok, err_msg)."""
    tmp = tempfile.mkdtemp(prefix='ffpkg_bkp_')
    try:
        rc, out = _run_ufs2(app, ['extract', img, tmp, fs_path],
                            timeout=120)
        if rc != 0:
            return False, out.strip()[:300]
        # Find the extracted file. UFS2Tool preserves the fs path
        # structure under tmp, but be defensive — also search by name.
        target_name = _basename_fs(fs_path)
        found = None
        for root, _dirs, files in os.walk(tmp):
            for f in files:
                if f == target_name:
                    found = os.path.join(root, f)
                    break
            if found:
                break
        if not found:
            return False, _('extracted file not found in temp dir')
        dst = os.path.join(backup_dir, rel.replace('/', os.sep))
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
        except Exception as e:
            return False, str(e)
        try:
            # shutil.move would do a cross-device rename; for safety
            # just read+write so the temp dir can be cleaned up cleanly.
            with open(found, 'rb') as fh_r, open(dst, 'wb') as fh_w:
                while True:
                    chunk = fh_r.read(1 << 20)
                    if not chunk:
                        break
                    fh_w.write(chunk)
        except Exception as e:
            return False, str(e)
        return True, ''
    finally:
        # Best-effort cleanup of the temp dir.
        try:
            import shutil as _sh
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass


def _write_backup_manifest(backup_dir, img, src_dir, base_fs_dir,
                           ts, stats):
    """Write a backup_manifest.txt at backup_dir/ root with enough
    info to restore the originals if needed."""
    lines = []
    lines.append('exFAT Image Builder — backport backup manifest')
    lines.append('=' * 50)
    lines.append('Timestamp:        ' + ts)
    lines.append('Image:            ' + img)
    lines.append('Backport source:  ' + src_dir)
    lines.append('Anchored at:      ' + (base_fs_dir or '/'))
    lines.append('Replaced count:   ' + str(stats['replaced']))
    lines.append('Added count:      ' + str(stats['added']))
    lines.append('Failed count:     ' + str(stats['failed']))
    lines.append('')
    lines.append('Files backed up (originals from the image, '
                 'extracted BEFORE replace):')
    for p in stats['backed_up']:
        lines.append('  ' + p)
    if stats['errors']:
        lines.append('')
        lines.append('Errors:')
        for e in stats['errors']:
            lines.append('  - ' + e)
    lines.append('')
    lines.append('To restore: open this image in Edit ffpkg, navigate to '
                 'the same anchor folder, and use Add folder on the '
                 'extracted backup folder. Replaced files will be put '
                 'back in place (additive merge — non-destructive).')
    try:
        with open(os.path.join(backup_dir, 'backup_manifest.txt'),
                  'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
    except Exception:
        pass  # Manifest is best-effort — don't fail the whole op.


def _zip_backup_folder(backup_dir, backup_zip):
    """Zip the backup folder. Removes the folder on success so the
    zip is the canonical artifact."""
    with _backport_zipfile.ZipFile(backup_zip, 'w',
            _backport_zipfile.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(backup_dir):
            for f in files:
                ap = os.path.join(root, f)
                rel = os.path.relpath(ap, backup_dir)
                zf.write(ap, rel)
    # Delete the now-redundant folder.
    try:
        import shutil as _sh
        _sh.rmtree(backup_dir, ignore_errors=True)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Source picker — Folder / .ffpkg / .exfat
# ─────────────────────────────────────────────────────────────────────
def _bp_pick_source_kind(app):
    """Show a small modal dialog asking the user what kind of backport
    source they have. Returns 'folder', 'ffpkg', 'exfat', or None if
    the user cancels.
    """
    result = {'kind': None}
    dlg, body, footer = _dialog_shell(
        app, _('Apply backport \u2014 pick source'),
        _('What\u2019s your backport source?'),
        _('Pick the format that matches what you have.'), width=500)

    def _pick(k):
        result['kind'] = k
        dlg.destroy()

    rows = tk.Frame(body, bg=COLORS['bg_2'])
    rows.pack(fill='x', pady=(px(16), 0))
    _choice_row(rows, 'folder', _('A folder'),
         _('A backport folder like the ones produced by Auto Backport '
           '(e.g. fakelib/, sce_module/, sce_sys/, eboot.bin).'),
         lambda: _pick('folder'), body.wrap)
    _choice_row(rows, 'package', _('A .ffpkg image'),
         _('Read the backport tree out of a .ffpkg using UFS2Tool, '
           'then overlay it onto the open image.'),
         lambda: _pick('ffpkg'), body.wrap)
    _choice_row(rows, 'drive', _('An exFAT image'),
         _('Mount a .exfat / .img read-only via OSFMount, copy its '
           'contents to a temp folder, then overlay.'),
         lambda: _pick('exfat'), body.wrap)

    _dialog_button(footer, _('Cancel'), dlg.destroy)
    dlg.bind('<Escape>', lambda e: dlg.destroy())
    _dialog_place(dlg, app)
    app.wait_window(dlg)
    return result['kind']


# ─────────────────────────────────────────────────────────────────────
# Source materialisation
# ─────────────────────────────────────────────────────────────────────
def _bp_materialize_source(app, kind, src_path):
    """Turn the chosen source into a real folder on disk.

    Returns (src_dir, cleanup_fn). cleanup_fn is always callable.
    For kind='folder', cleanup_fn is a no-op and src_dir == src_path.
    For kind='ffpkg' and kind='exfat', src_dir is a temp dir under
    %TEMP% and cleanup_fn removes it.
    """
    if kind == 'folder':
        return src_path, (lambda: None)

    if kind == 'ffpkg':
        return _bp_extract_ffpkg_to_temp(app, src_path)

    if kind == 'exfat':
        return _bp_extract_exfat_to_temp(app, src_path)

    raise ValueError('unknown kind: ' + repr(kind))


def _bp_extract_ffpkg_to_temp(app, ffpkg_path):
    """UFS2Tool extract the whole tree from `ffpkg_path` into a temp
    dir. Returns (temp_dir, cleanup_fn)."""
    import shutil as _sh
    if not os.path.isfile(ffpkg_path):
        raise RuntimeError(_('Source .ffpkg does not exist: ') + ffpkg_path)
    tmp = tempfile.mkdtemp(prefix='ffpkg_bp_src_')
    def _cleanup():
        try:
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass
    try:
        # UFS2Tool: `extract <img> <out_dir> <fs_path>`. Passing '/'
        # extracts the root, but UFS2Tool's extract verb requires a
        # specific file path. So we do a recursive walk via `ls` and
        # extract each file individually. That's also how the existing
        # hex-preview path uses it.
        # v4: try the shared mount+robocopy engine first (fast, handles
        # big files); fall back to the per-file walk if Dokan/UFS2Tool
        # mount is unavailable.
        def _mc_log(s):
            try:
                app._log('[BP-PREP] ' + s)
            except Exception:
                pass

        def _mc_prog(pct, status, eta):
            try:
                app.after(0, app._fe_status_var.set,
                          _('Preparing backport source... ') + (status or ''))
            except Exception:
                pass

        try:
            _mc = app._ffpkg_mount_copy_extract(ffpkg_path, tmp,
                                                _mc_log, _mc_prog)
        except Exception as _mce:
            _mc = {'ok': False, 'error': repr(_mce)}
        if _mc.get('ok'):
            return tmp, _cleanup
        _mc_log('mount-copy unavailable (%s) — falling back to per-file '
                'UFS2Tool extraction.\n'
                % (_mc.get('error') or
                   ('Dokan missing' if _mc.get('dokan_missing') else
                    'rc=%s' % _mc.get('rc'))))
        stats = {'files': 0, 'errors': []}
        _ffpkg_extract_walk(app, ffpkg_path, '/', tmp, stats)
        if not stats['files']:
            raise RuntimeError(
                _('No files were extracted from the source .ffpkg.'))
        return tmp, _cleanup
    except Exception:
        _cleanup()
        raise


def _ffpkg_extract_walk(app, img, fs_dir, local_dir, stats, _depth=0):
    """Recursive helper. Walks the fs tree under `fs_dir` and extracts
    every regular file under `local_dir`, preserving the path. Skips
    entries we can't classify."""
    if _depth > 32:
        stats['errors'].append(_('recursion too deep at ') + fs_dir)
        return
    rc, out = _run_ufs2(app, ['ls', img, fs_dir], timeout=60)
    if rc != 0:
        stats['errors'].append('ls %s: %s' % (fs_dir, out.strip()[:200]))
        return
    parsed = _parse_ls(out)
    for name, is_dir, _sz in parsed:
        if name in ('.', '..'):
            continue
        child_fs = _join_fs_path(fs_dir, name)
        if is_dir:
            # Recurse — UFS2Tool will need this path to ls.
            _ffpkg_extract_walk(app, img, child_fs, local_dir, stats,
                                _depth + 1)
        else:
            # Extract this one file.
            rc, out = _run_ufs2(app, ['extract', img, local_dir, child_fs],
                                 timeout=300)
            if rc != 0:
                stats['errors'].append('extract %s: %s'
                                        % (child_fs, out.strip()[:200]))
                continue
            stats['files'] += 1


def _bp_extract_exfat_to_temp(app, img_path):
    """Mount `img_path` read-only via OSFMount, copy its tree into a
    temp dir, then dismount. Returns (temp_dir, cleanup_fn).
    """
    import shutil as _sh
    import ctypes as _ct
    if not os.path.isfile(img_path):
        raise RuntimeError(_('Source exFAT image does not exist: ') + img_path)

    osf = None
    try:
        osf = app._find_osfmount()
    except Exception:
        osf = None
    if not osf:
        raise RuntimeError(_(
            'OSFMount is required to read exFAT images but was not '
            'found. Install it (or set its path in Settings) and try '
            'again.'))

    tmp = tempfile.mkdtemp(prefix='exfat_bp_src_')
    mounted = {'drive': None}

    def _cleanup():
        if mounted['drive']:
            try:
                subprocess.run([osf, '-d', '-m', mounted['drive']],
                               capture_output=True, timeout=30,
                               creationflags=_NO_WIN_FLAGS)
            except Exception:
                pass
        try:
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass

    try:
        # Find a free drive letter.
        drives_bitmask = used_letter_mask()
        free_letter = None
        for i in range(25, 3, -1):
            if not (drives_bitmask & (1 << i)):
                free_letter = chr(65 + i) + ':'
                break
        if not free_letter:
            raise RuntimeError(_('No free drive letters available for mount.'))

        # Mount read-only + removable.
        result = subprocess.run(
            [osf, '-a', '-t', 'file', '-f', img_path,
             '-m', free_letter, '-o', 'ro,rem'],
            capture_output=True, text=True, timeout=120,
            creationflags=_NO_WIN_FLAGS)
        if result.returncode != 0:
            raise RuntimeError(_('OSFMount mount failed: ') +
                (result.stderr or result.stdout or '')[:300])
        mounted['drive'] = free_letter

        # Wait for the drive to appear.
        import time as _t
        # (loop variable must not be called "_": that would shadow the
        # translation function for this whole function and turn every
        # error message here into an UnboundLocalError)
        for _attempt in range(30):
            if os.path.exists(free_letter + '\\'):
                break
            _t.sleep(0.5)
        else:
            raise RuntimeError(_('Mounted drive did not appear in time.'))

        # Copy the tree into tmp. We use os.walk + shutil.copy2 rather
        # than robocopy so we don't depend on robocopy's exit codes
        # (which use bitfields and confuse simple rc checks).
        for root, _dirs, files in os.walk(free_letter + '\\'):
            try:
                rel_root = os.path.relpath(root, free_letter + '\\')
            except Exception:
                continue
            if rel_root == '.':
                rel_root = ''
            for fn in files:
                src = os.path.join(root, fn)
                dst = os.path.join(tmp, rel_root, fn) if rel_root \
                                                     else os.path.join(tmp, fn)
                try:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    import shutil as _sh2
                    _sh2.copy2(src, dst)
                except Exception:
                    # Skip files we can't read; the user will see the
                    # final manifest count and can investigate.
                    pass

        # Dismount eagerly now that the copy is done — don't keep the
        # drive busy during the overlay.
        try:
            subprocess.run([osf, '-d', '-m', mounted['drive']],
                           capture_output=True, timeout=30,
                           creationflags=_NO_WIN_FLAGS)
        except Exception:
            pass
        mounted['drive'] = None

        return tmp, _cleanup
    except Exception:
        _cleanup()
        raise


# ─────────────────────────────────────────────────────────────────────
# Rebuild-flow helpers (v2.0.6e)
# ─────────────────────────────────────────────────────────────────────
def _bp_free_space(path):
    """Return bytes free on the filesystem holding `path`, or None if
    it can't be determined."""
    try:
        import shutil as _sh
        return _sh.disk_usage(path).free
    except Exception:
        try:
            st = os.statvfs(path)
            return st.f_bavail * st.f_frsize
        except Exception:
            return None


def _bp_fmt_size(n):
    """Format a byte count as e.g. '12.4 GB' or '847 MB'."""
    if n is None:
        return '?'
    n = float(n)
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024.0 or unit == 'TB':
            return '%.1f %s' % (n, unit) if unit != 'B' else '%d B' % n
        n /= 1024.0


def _bp_zip_single_file(src, dst_zip, progress_cb=None):
    """Zip a single file `src` into `dst_zip`. The archive contains
    just one entry, named the same as `src`'s basename, so restoring
    is trivial: unzip and you have the original file back.

    Optional `progress_cb(done_bytes, total_bytes)` is called as the
    file is streamed in, roughly every 8 MB. Used by the progress
    dialog so the backup step shows live percentage.
    """
    import zipfile as _zf
    if os.path.exists(dst_zip):
        try:
            os.unlink(dst_zip)
        except Exception:
            pass
    try:
        total = os.path.getsize(src)
    except Exception:
        total = 0

    with _zf.ZipFile(dst_zip, 'w', _zf.ZIP_DEFLATED, allowZip64=True) as zf:
        if progress_cb is None or total <= 0:
            # Simple path — no progress tracking.
            zf.write(src, arcname=os.path.basename(src))
            return
        # Streamed path — write in chunks so we can report progress.
        arcname = os.path.basename(src)
        try:
            with zf.open(arcname, 'w', force_zip64=True) as zinfo, \
                 open(src, 'rb') as fh:
                done = 0
                last_report = 0
                chunk_size = 1 << 23   # 8 MB
                while True:
                    chunk = fh.read(chunk_size)
                    if not chunk:
                        break
                    zinfo.write(chunk)
                    done += len(chunk)
                    if done - last_report >= chunk_size or done == total:
                        try:
                            progress_cb(done, total)
                        except Exception:
                            pass
                        last_report = done
        except Exception:
            # Fallback to the simple path if streamed write fails for
            # any reason (older zipfile builds, weird path issues).
            zf.write(src, arcname=arcname)


def _bp_overlay_into_dump(items, anchor_abs, stats, progress_cb=None):
    """Copy every (rel, abs_src) from `items` into `anchor_abs`,
    preserving relative path. Existing files at the destination are
    counted as 'overlay_replaced', new ones as 'overlay_added'.

    `anchor_abs` is the on-disk directory we're overlaying onto
    (typically <dump_dir> for breadcrumb '/').

    Optional `progress_cb(done, total, current_name)` is called once
    per file processed.

    Failures are not fatal — they get recorded in stats['errors'] and
    the rebuild proceeds with whatever made it onto disk.
    """
    import shutil as _sh
    if not os.path.isdir(anchor_abs):
        try:
            os.makedirs(anchor_abs, exist_ok=True)
        except Exception as e:
            raise RuntimeError(
                _('Could not create overlay anchor dir %s: %s')
                % (anchor_abs, e))

    total = len(items)
    for i, (rel, abs_src) in enumerate(items, 1):
        dst = os.path.join(anchor_abs, rel.replace('/', os.sep))
        existed = os.path.isfile(dst)
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _sh.copy2(abs_src, dst)
            if existed:
                stats['overlay_replaced'] += 1
            else:
                stats['overlay_added'] += 1
        except Exception as e:
            stats['errors'].append(
                _('overlay copy failed %s: ') % dst + str(e)[:200])
        if progress_cb is not None:
            try:
                progress_cb(i, total, rel)
            except Exception:
                pass


def _bp_atomic_swap(new_path, target_path):
    """Replace `target_path` with `new_path` as atomically as the
    filesystem allows.

    Strategy on Windows: os.replace() is atomic when source and
    destination are on the same volume — which they always are here,
    because new_path is in a temp dir we deliberately keep next to
    target. If new_path is on a different volume (rare — happens only
    if TEMP is on a different drive than the image), fall back to a
    copy+rename: copy to <target>.new, fsync, then os.replace.
    """
    same_drive = (os.path.splitdrive(os.path.abspath(new_path))[0].lower()
                  == os.path.splitdrive(os.path.abspath(target_path))[0].lower())
    if same_drive:
        os.replace(new_path, target_path)
        return

    # Cross-volume: copy through a sidecar on the destination volume,
    # then rename over the original.
    import shutil as _sh
    sidecar = target_path + '.new'
    try:
        if os.path.exists(sidecar):
            os.unlink(sidecar)
    except Exception:
        pass
    _sh.copy2(new_path, sidecar)
    # Force buffers to disk before the rename so a power loss between
    # the two ops doesn't leave a half-written sidecar that then gets
    # renamed over the original. Not bulletproof — Windows doesn't
    # offer a real fsync — but better than nothing.
    try:
        with open(sidecar, 'rb') as fh:
            os.fsync(fh.fileno())
    except Exception:
        pass
    os.replace(sidecar, target_path)
    try:
        os.unlink(new_path)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Compare ffpkg (v2.0.6g) — diagnostic tool that extracts two ffpkgs
# and diffs them, so we can find out why a rebuilt image differs from
# a manually-built one.
#
# The report includes:
#   - files only in A vs only in B
#   - files present in both but with different size or SHA256
#   - mode (chmod) differences per file (via UFS2Tool stat)
#   - BSD inode-flag differences per file if UFS2Tool's stat exposes them
#   - top-level directory entry-count differences
#
# Output: written to a .compare.txt file next to ffpkg A.
# ─────────────────────────────────────────────────────────────────────
def _compare_ffpkgs(app):
    """Toolbar entry-point. Asks the user to pick two .ffpkg files,
    extracts both, and writes a diff report to disk.
    """
    a = filedialog.askopenfilename(
        title=_('Pick first .ffpkg (A — usually the good / working one)'),
        filetypes=[('FFPKG image', '*.ffpkg'),
                   ('All files',   '*.*')])
    if not a:
        return
    b = filedialog.askopenfilename(
        title=_('Pick second .ffpkg (B — usually the broken one)'),
        filetypes=[('FFPKG image', '*.ffpkg'),
                   ('All files',   '*.*')])
    if not b:
        return
    if os.path.abspath(a) == os.path.abspath(b):
        app_error(app, _('Same file'),
            _('Both picks are the same file. Choose two different .ffpkgs.'))
        return

    if not app_confirm(app, _('Compare ffpkgs'),
        _('This will extract BOTH images to temp folders and walk every '
          'file in each, comparing sizes, hashes, and modes. It can take '
          'a while for large games (several minutes).\n\n') +
        _('A:  ') + a + '\n' +
        _('B:  ') + b + '\n\n' +
        _('Output report will be written next to A.\n\n') +
        _('Proceed?')):
        return

    app._fe_status_var.set(_('Extracting A...'))
    app.update_idletasks()

    def _bg():
        report_path = os.path.splitext(a)[0] + '.compare.txt'
        work_dir = tempfile.mkdtemp(prefix='ffpkg_compare_')
        dump_a   = os.path.join(work_dir, 'A')
        dump_b   = os.path.join(work_dir, 'B')
        try:
            os.makedirs(dump_a, exist_ok=True)
            os.makedirs(dump_b, exist_ok=True)

            app.after(0, app._fe_status_var.set, _('Extracting A...'))
            rc, out = _run_ufs2(app, ['extract', a, dump_a], timeout=3600)
            if rc != 0:
                rc, out = _run_ufs2(app,
                    ['extract', a, dump_a, '/'], timeout=3600)
            if rc != 0:
                raise RuntimeError(_('Extract A failed: ') +
                                   (out or '').strip()[:300])

            app.after(0, app._fe_status_var.set, _('Extracting B...'))
            rc, out = _run_ufs2(app, ['extract', b, dump_b], timeout=3600)
            if rc != 0:
                rc, out = _run_ufs2(app,
                    ['extract', b, dump_b, '/'], timeout=3600)
            if rc != 0:
                raise RuntimeError(_('Extract B failed: ') +
                                   (out or '').strip()[:300])

            # ── Collect inventories ──
            app.after(0, app._fe_status_var.set, _('Scanning A...'))
            inv_a = _ffpkg_scan_dump(dump_a)
            app.after(0, app._fe_status_var.set, _('Scanning B...'))
            inv_b = _ffpkg_scan_dump(dump_b)

            # ── Collect UFS2-side metadata (modes, flags) ──
            app.after(0, app._fe_status_var.set,
                _('Reading metadata from A...'))
            meta_a = _ffpkg_collect_stat_meta(app, a,
                set(inv_a['files']) | set(inv_a['dirs']))
            app.after(0, app._fe_status_var.set,
                _('Reading metadata from B...'))
            meta_b = _ffpkg_collect_stat_meta(app, b,
                set(inv_b['files']) | set(inv_b['dirs']))

            # ── Diff ──
            app.after(0, app._fe_status_var.set, _('Writing report...'))
            _ffpkg_write_compare_report(
                report_path, a, b,
                inv_a, inv_b, meta_a, meta_b, dump_a, dump_b)

            app.after(0, app._fe_status_var.set,
                _('Compare report saved.'))
            app.after(0, lambda: app_info(app, 
                _('Compare done'),
                _('Report written to:\n') + report_path + '\n\n' +
                _('Open it and look for sections marked "ONLY IN A", '
                  '"ONLY IN B", "DIFFERENT SIZE", "DIFFERENT HASH", and '
                  '"DIFFERENT MODE". Anything in those sections is a '
                  'concrete difference between the two images.')))
        except Exception as e:
            app.after(0, app._fe_status_var.set,
                _('Compare failed.'))
            app.after(0, lambda e=e: app_error(app, 
                _('Compare failed'), str(e)))
        finally:
            try:
                import shutil as _sh
                _sh.rmtree(work_dir, ignore_errors=True)
            except Exception:
                pass

    threading.Thread(target=_bg, daemon=True,
                     name='ffpkg-compare').start()


def _ffpkg_scan_dump(dump_root):
    """Walk an extracted dump, returning:
        {
          'files': {rel_path: size_bytes, ...},
          'dirs':  set of rel_paths,
          'sha':   {rel_path: hex_sha256, ...},      # filled lazily
        }
    rel_paths are forward-slash, no leading slash.
    """
    import hashlib as _hashlib
    inv = {'files': {}, 'dirs': set(), 'sha': {}}
    dump_root = os.path.abspath(dump_root)
    for root, dirs, files in os.walk(dump_root):
        for d in dirs:
            ap = os.path.join(root, d)
            try:
                rel = os.path.relpath(ap, dump_root).replace(os.sep, '/')
            except Exception:
                continue
            inv['dirs'].add(rel)
        for f in files:
            ap = os.path.join(root, f)
            try:
                rel = os.path.relpath(ap, dump_root).replace(os.sep, '/')
            except Exception:
                continue
            try:
                inv['files'][rel] = os.path.getsize(ap)
            except Exception:
                inv['files'][rel] = -1
    return inv


def _ffpkg_sha256(path):
    """Return hex SHA-256 of a file, or None on error."""
    import hashlib as _hashlib
    h = _hashlib.sha256()
    try:
        with open(path, 'rb') as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


def _ffpkg_collect_stat_meta(app, img, rel_paths):
    """Run UFS2Tool `stat` on each path inside `img` and parse out the
    mode (octal int) and BSD inode flags (hex int) if present.
    Returns: {rel_path: {'mode': int|None, 'flags': int|None,
                         'raw': str}}.

    `rel_paths` are forward-slash without leading slash (matching
    `_ffpkg_scan_dump`). Capped at 10000 entries to keep this from
    spending forever on very large images.
    """
    import re as _re
    meta = {}
    count = 0
    cap = 10000
    for rel in sorted(rel_paths):
        if count >= cap:
            meta['__truncated__'] = {'mode': None, 'flags': None,
                'raw': 'metadata read capped at %d entries' % cap}
            break
        count += 1
        fs_path = '/' + rel if rel else '/'
        rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
        if rc != 0 or not out:
            meta[rel] = {'mode': None, 'flags': None,
                'raw': (out or '').strip()[:200]}
            continue
        mode = None
        m = _re.search(r'\b0[0-7]{3,4}\b', out)
        if m:
            try:
                mode = int(m.group(0), 8)
            except Exception:
                mode = None
        if mode is None:
            m = _re.search(r'(?i)(?:perm|mode|access)\D{0,16}([0-7]{3,4})\b', out)
            if m:
                try:
                    mode = int(m.group(1), 8)
                except Exception:
                    mode = None
        # Look for a "flags" line — UFS2Tool stat output sometimes
        # includes something like "Flags: 0x00000010" or
        # "Inode flags: 0".
        flags = None
        m = _re.search(r'(?i)\bflags\b\s*[:=]?\s*(0x[0-9a-f]+|\d+)', out)
        if m:
            raw = m.group(1)
            try:
                flags = int(raw, 16) if raw.lower().startswith('0x') \
                                     else int(raw)
            except Exception:
                flags = None
        meta[rel] = {'mode': mode, 'flags': flags, 'raw': out.strip()}
    return meta


def _ffpkg_write_compare_report(path, a_img, b_img,
                                 inv_a, inv_b, meta_a, meta_b,
                                 dump_a, dump_b):
    """Write a human-readable diff report comparing two extracted
    ffpkg dumps + their UFS2 metadata. SHA-256s are computed on
    demand only for files that exist in both and have the same size
    (cheap-first; mismatched sizes are already a known-diff)."""
    import hashlib as _hashlib

    files_a = inv_a['files']
    files_b = inv_b['files']
    dirs_a  = inv_a['dirs']
    dirs_b  = inv_b['dirs']

    only_a_files = sorted(set(files_a) - set(files_b))
    only_b_files = sorted(set(files_b) - set(files_a))
    only_a_dirs  = sorted(dirs_a - dirs_b)
    only_b_dirs  = sorted(dirs_b - dirs_a)

    common_files = sorted(set(files_a) & set(files_b))

    size_diff   = []
    hash_diff   = []
    mode_diff   = []
    flag_diff   = []

    for rel in common_files:
        sa, sb = files_a[rel], files_b[rel]
        if sa != sb:
            size_diff.append((rel, sa, sb))
            continue
        # Same size — check SHA256.
        pa = os.path.join(dump_a, rel.replace('/', os.sep))
        pb = os.path.join(dump_b, rel.replace('/', os.sep))
        ha = _ffpkg_sha256(pa)
        hb = _ffpkg_sha256(pb)
        if ha is None or hb is None:
            continue
        if ha != hb:
            hash_diff.append((rel, ha, hb))

    # Mode + flag comparison from metadata maps (includes both files
    # and dirs; only files that exist on both sides though).
    common_paths = (set(files_a) & set(files_b)) | (dirs_a & dirs_b)
    for rel in sorted(common_paths):
        ma = meta_a.get(rel, {})
        mb = meta_b.get(rel, {})
        m_a, m_b = ma.get('mode'), mb.get('mode')
        if m_a is not None and m_b is not None and m_a != m_b:
            mode_diff.append((rel, m_a, m_b))
        f_a, f_b = ma.get('flags'), mb.get('flags')
        if f_a is not None and f_b is not None and f_a != f_b:
            flag_diff.append((rel, f_a, f_b))

    # ── Write the report ──
    lines = []
    lines.append('FFPKG COMPARE REPORT')
    lines.append('=' * 72)
    lines.append('A:  ' + a_img)
    lines.append('B:  ' + b_img)
    lines.append('')
    lines.append('Image sizes (bytes on disk):')
    try:
        lines.append('  A:  %d' % os.path.getsize(a_img))
    except Exception:
        lines.append('  A:  ?')
    try:
        lines.append('  B:  %d' % os.path.getsize(b_img))
    except Exception:
        lines.append('  B:  ?')
    lines.append('')
    lines.append('Entry counts:')
    lines.append('  A:  %d files, %d dirs' % (len(files_a), len(dirs_a)))
    lines.append('  B:  %d files, %d dirs' % (len(files_b), len(dirs_b)))
    lines.append('')

    def _sec(title, items, fmt):
        lines.append('-' * 72)
        lines.append('%s  (%d)' % (title, len(items)))
        if not items:
            lines.append('  (none)')
        else:
            for item in items[:500]:
                lines.append('  ' + fmt(item))
            if len(items) > 500:
                lines.append('  ... (+%d more)' % (len(items) - 500))
        lines.append('')

    _sec('FILES ONLY IN A (missing from B)', only_a_files,
         lambda r: r)
    _sec('FILES ONLY IN B (extra in B)', only_b_files,
         lambda r: r)
    _sec('DIRS ONLY IN A (missing from B)', only_a_dirs,
         lambda r: r + '/')
    _sec('DIRS ONLY IN B (extra in B)', only_b_dirs,
         lambda r: r + '/')
    _sec('DIFFERENT SIZE (A bytes -> B bytes)', size_diff,
         lambda t: '%s   %d -> %d' % t)
    _sec('DIFFERENT HASH (same size, different content)', hash_diff,
         lambda t: '%s\n      A: %s\n      B: %s' % t)
    _sec('DIFFERENT MODE (chmod)', mode_diff,
         lambda t: '%s   %s -> %s' % (t[0], oct(t[1])[2:], oct(t[2])[2:]))
    _sec('DIFFERENT BSD FLAGS (chflags)', flag_diff,
         lambda t: '%s   0x%x -> 0x%x' % t)

    lines.append('-' * 72)
    lines.append('SUMMARY:')
    lines.append('  files only in A: %d' % len(only_a_files))
    lines.append('  files only in B: %d' % len(only_b_files))
    lines.append('  dirs only in A:  %d' % len(only_a_dirs))
    lines.append('  dirs only in B:  %d' % len(only_b_dirs))
    lines.append('  size mismatches: %d' % len(size_diff))
    lines.append('  hash mismatches: %d' % len(hash_diff))
    lines.append('  mode mismatches: %d' % len(mode_diff))
    lines.append('  flag mismatches: %d' % len(flag_diff))
    lines.append('')
    if not (only_a_files or only_b_files or only_a_dirs or only_b_dirs
            or size_diff or hash_diff or mode_diff or flag_diff):
        lines.append('NO DIFFERENCES FOUND in the file inventories. ')
        lines.append('Any remaining difference between the images must ')
        lines.append('be in the filesystem layout itself (block/frag ')
        lines.append('sizes, inode allocation, cylinder-group geometry, ')
        lines.append('etc). Compare image sizes above; if they differ ')
        lines.append('but contents match, this is the cause.')

    try:
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
    except Exception:
        # Last-ditch: write next to the system temp dir.
        alt = os.path.join(tempfile.gettempdir(),
            os.path.basename(path))
        with open(alt, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))


# ─────────────────────────────────────────────────────────────────────
# Progress dialog (v2.0.6h) — shown during Apply backport's rebuild
# flow so the user can see what's happening and how long is left.
#
# Six stages with weighted contributions to the overall percentage:
#   1) Backup zip       —   5%
#   2) Extract image    —  30%
#   3) Overlay backport —   5%
#   4) Newfs init       —  15% (newfs's "Writing cylinder groups")
#   5) Newfs files      —  40% (newfs's "Adding files to image")
#   6) Swap + refresh   —   5%
#
# The dialog has:
#   • a stage label (top, bold)
#   • a detail line (sub-progress like "847 / 5612 files")
#   • a progressbar (0..100)
#   • elapsed time + ETA
#
# All state is pushed from the worker thread via `app.after(0, ...)`,
# so the dialog stays responsive while the rebuild runs.
# ─────────────────────────────────────────────────────────────────────
class _RebuildProgress:
    """Modal-ish progress dialog. Not strictly modal (we don't grab_set)
    so the user can still scroll the main window if they want — but
    it's always-on-top and the rebuild buttons are disabled by way of
    the work thread holding the only relevant entry-point."""

    STAGE_WEIGHTS = {
        'backup':  ( 0,  5),
        'extract': ( 5, 35),
        'overlay': (35, 40),
        'init':    (40, 55),
        'files':   (55, 95),
        'swap':    (95, 100),
    }

    def __init__(self, app, title, weights=None, initial_stage=None):
        self.app = app
        self.start_t = time.time()
        # v3.0.0: allow callers to override the stage weight map so
        # this dialog can be reused by the Convert tab and any other
        # multi-stage flow. If `weights` is provided it replaces the
        # class default; otherwise the rebuild flow's defaults apply.
        if weights is not None:
            self.STAGE_WEIGHTS = dict(weights)
        # Initial stage defaults to the first key in STAGE_WEIGHTS.
        if initial_stage is None:
            try:
                initial_stage = next(iter(self.STAGE_WEIGHTS))
            except StopIteration:
                initial_stage = 'init'
        self.stage = initial_stage
        self.stage_pct = 0.0
        # Rolling throughput for ETA — last N samples of (t, overall_pct)
        self._samples = []
        self._closed = False

        self.win = tk.Toplevel(app)
        self.win.title(title)
        self.win.configure(bg=COLORS['border_3'])
        self.win.transient(app)
        self.win.resizable(False, False)
        self.win.protocol('WM_DELETE_WINDOW', lambda: None)   # no close

        w, h = px(480), px(150)
        try:
            x = app.winfo_x() + (app.winfo_width()  - w) // 2
            y = app.winfo_y() + (app.winfo_height() - h) // 2
            self.win.geometry('%dx%d+%d+%d' % (w, h, x, y))
        except Exception:
            self.win.geometry('%dx%d' % (w, h))
        self.win.attributes('-topmost', True)

        # bg_2 dialog body with a 1px border_3 frame (design-system dialog)
        shell = tk.Frame(self.win, bg=COLORS['bg_2'])
        shell.pack(fill='both', expand=True, padx=1, pady=1)
        body = tk.Frame(shell, bg=COLORS['bg_2'])
        body.pack(fill='both', expand=True, padx=px(24), pady=(px(20), px(18)))

        self._stage_var  = tk.StringVar(value=_('Preparing...'))
        self._detail_var = tk.StringVar(value='')
        self._timing_var = tk.StringVar(value='')

        tk.Label(body, textvariable=self._stage_var,
                 font=FONTS['subtitle'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_0'],
                 anchor='w').pack(fill='x', anchor='w')
        tk.Label(body, textvariable=self._detail_var,
                 font=FONTS['caption'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                 anchor='w').pack(fill='x', anchor='w', pady=(px(2), px(14)))

        # Design-system progress bar (6px, rounded, accent fill)
        from ui.shared.widgets import ProgressBar
        self._bar = ProgressBar(body, mode='determinate', maximum=100,
                                value=0, surface=COLORS['bg_2'])
        self._bar.pack(fill='x', pady=(0, px(10)))

        tk.Label(body, textvariable=self._timing_var,
                 font=FONTS['caption'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                 anchor='w').pack(fill='x', anchor='w')
        try:   # fit the height to the content
            self.win.update_idletasks()
            self.win.geometry('%dx%d' % (w, self.win.winfo_reqheight()))
        except Exception:
            pass

        # Tick the timing line every 500ms so elapsed/ETA stay live
        # even when no progress update comes in (e.g. during the long
        # initial part of extract before the first file finishes).
        self._tick_after_id = None
        self._schedule_tick()

    # ── State updates ────────────────────────────────────────────────
    def set_stage(self, stage, label):
        """Called when entering a new stage. `label` is the user-facing
        bold line. Resets the stage-progress to 0."""
        if self._closed:
            return
        self.stage = stage
        self.stage_pct = 0.0
        try:
            self._stage_var.set(label)
            self._detail_var.set('')
            self._recompute_overall()
        except Exception:
            pass

    def set_stage_progress(self, pct, detail=None):
        """Update progress within the current stage. `pct` is 0..100."""
        if self._closed:
            return
        try:
            self.stage_pct = max(0.0, min(100.0, float(pct)))
            if detail is not None:
                self._detail_var.set(detail)
            self._recompute_overall()
        except Exception:
            pass

    def set_detail(self, detail):
        if self._closed:
            return
        try:
            self._detail_var.set(detail)
        except Exception:
            pass

    def close(self):
        if self._closed:
            return
        self._closed = True
        if self._tick_after_id:
            try:
                self.app.after_cancel(self._tick_after_id)
            except Exception:
                pass
            self._tick_after_id = None
        try:
            self.win.destroy()
        except Exception:
            pass

    # ── Internals ────────────────────────────────────────────────────
    def _recompute_overall(self):
        lo, hi = self.STAGE_WEIGHTS.get(self.stage, (0, 100))
        overall = lo + (hi - lo) * (self.stage_pct / 100.0)
        try:
            self._bar['value'] = overall
        except Exception:
            pass
        # Track sample for ETA
        now = time.time()
        self._samples.append((now, overall))
        # Keep last 30 samples (~ last 15 seconds at 2/sec)
        if len(self._samples) > 30:
            del self._samples[:-30]
        self._refresh_timing(now, overall)

    def _refresh_timing(self, now=None, overall=None):
        if now is None:
            now = time.time()
        if overall is None:
            try:
                overall = float(self._bar['value'])
            except Exception:
                overall = 0.0
        elapsed = now - self.start_t
        eta_str = ''
        # ETA from rolling average rate over the sample window.
        # Skip if too few samples or rate is tiny.
        if len(self._samples) >= 2 and overall > 1.0 and overall < 99.0:
            t0, p0 = self._samples[0]
            dt = now - t0
            dp = overall - p0
            if dt > 0.5 and dp > 0.1:
                rate = dp / dt
                remaining = max(0.0, 100.0 - overall)
                eta_s = remaining / rate
                if eta_s < 10 * 3600:
                    eta_str = _('  •  ETA %s') % _fmt_hms(eta_s)
        try:
            self._timing_var.set(
                _('Elapsed %s') % _fmt_hms(elapsed) + eta_str
                + ('  •  %d%%' % int(overall)))
        except Exception:
            pass

    def _schedule_tick(self):
        if self._closed:
            return
        self._refresh_timing()
        self._tick_after_id = self.app.after(500, self._schedule_tick)


def _fmt_hms(seconds):
    """Format seconds as e.g. '3m 12s' or '1h 04m 12s'."""
    s = int(max(0, seconds))
    h = s // 3600
    m = (s % 3600) // 60
    s = s % 60
    if h:
        return '%dh %02dm %02ds' % (h, m, s)
    if m:
        return '%dm %02ds' % (m, s)
    return '%ds' % s
