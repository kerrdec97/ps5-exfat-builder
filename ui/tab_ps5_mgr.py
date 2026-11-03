"""
ui/tab_ps5_mgr.py — PS5 Control Center (the "Manager" sub-tab of PS5).

Farewell Edition redesign on the design system:

    PageHeader  PS5 Control Center                       [Refresh all]
    ┌ PS5 connection ─────────────── [badge] [Test] ┐ ┌ Setup checklist ┐
    │ IP · FTP · Kernel log · Payload ports          │ │ ✓ / ⚠ rows       │
    │ ── console stats grid ──                       │ └──────────────────┘
    └────────────────────────────────────────────────┘
    PS5 tools — six cards that open the other PS5 sub-tabs
    Recent PS5 activity
    PS5 game manager — search, legend, table (rows rendered by
                       app._ps5mgr_render in the main file)

The module also hosts a small private kit (``ps5_page``, ``StatGrid``,
``StatTile``, ``VarBadge``, ``NullWidget``, ``LegacyBar``, ``SubTabs``,
``console``) shared by the other PS5 sub-tab modules (FTP, Kernel log,
Payloads, Y2JB, ShadowMount+, MicroMount). They build on
ui/shared/widgets.py and exist because those pages need legacy-compatible
variants (widgets the networking code still recolours with
``.config(fg=…, bg=…)``).

Backwards compat: every ``app._ps5mgr_*`` / ``app._ps5ov_*`` attribute the
main file reads is still created here with a compatible API.
"""

import os
import re
import tkinter as tk
from ui.shared.widgets import (
    Button as XButton, Check as XCheck, Radio as XRadio,
    TextField as XEntry, Scrollbar as XScrollbar,
)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.shared.page_head import make_themed_button
from ui.shared.scroll import attach_scroll

C = COLORS


# ═════════════════════════════════════════════════════════════════════════
# Private PS5 page kit (shared by the PS5 sub-tab modules)
# ═════════════════════════════════════════════════════════════════════════
def ps5_page(parent, scroll=True):
    """Page canvas for a PS5 sub-tab. Returns the frame to build into.

    scroll=True wraps the page in one vertical scroll region (one scrollbar
    for the whole page — lists inside simply extend the page)."""
    parent.configure(bg=C['bg_1'])
    if not scroll:
        body = tk.Frame(parent, bg=C['bg_1'])
        body.pack(fill='both', expand=True)
        return body
    canvas = tk.Canvas(parent, bg=C['bg_1'], highlightthickness=0, bd=0)
    sb = XScrollbar(parent, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)
    body = tk.Frame(canvas, bg=C['bg_1'])
    win = canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>', lambda e: canvas.configure(
        scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(win, width=e.width))
    attach_scroll(canvas)
    body._ps5_canvas = canvas
    return body


def page_header(body, title, description=None):
    """Design-system page header packed with the standard page spacing."""
    from ui.shared.widgets import PageHeader
    hdr = PageHeader(body, title, description)
    hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    return hdr


def section(body, pady=(0, 16)):
    """A full-width row inside the page gutter."""
    f = tk.Frame(body, bg=C['bg_1'])
    f.pack(fill='x', padx=px(24), pady=(px(pady[0]), px(pady[1])))
    return f


def two_columns(body, left=7, right=5, pady=(0, 16)):
    """Two-column grid row (uniform weights like the Build page)."""
    row = section(body, pady)
    row.columnconfigure(0, weight=left, uniform='ps5cols')
    row.columnconfigure(1, weight=right, uniform='ps5cols')
    l = tk.Frame(row, bg=C['bg_1'])
    l.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))
    r = tk.Frame(row, bg=C['bg_1'])
    r.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))
    return l, r


def fact(parent, label, value='—', mono=False, textvariable=None):
    """Caption label above a value (Build page 'facts'). Returns the value
    label so callers can recolour it."""
    bg = parent.cget('bg')
    box = tk.Frame(parent, bg=bg)
    tk.Label(box, text=label, font=FONTS['caption'], bg=bg, fg=C['fg_3'],
             anchor='w').pack(anchor='w', fill='x')
    kw = dict(textvariable=textvariable) if textvariable else dict(text=value)
    val = tk.Label(box, font=FONTS['code'] if mono else FONTS['body_strong'],
                   bg=bg, fg=C['fg_0'], anchor='w', **kw)
    val.pack(anchor='w', fill='x', pady=(px(2), 0))
    box.value = val
    return box


class StatGrid(tk.Frame):
    """Grid of caption/value facts. API-compatible with the legacy
    ui.shared.ps5_kit.StatStrip: set(key, value, warn=False, ok=False)."""

    def __init__(self, parent, stats, cols=None, bg=None, row_gap=12):
        bg = bg or parent.cget('bg')
        super().__init__(parent, bg=bg)
        self._cells = {}
        cols = cols or len(stats) or 1
        for c in range(cols):
            self.columnconfigure(c, weight=1, uniform='sg%d' % id(self))
        for i, (label, key) in enumerate(stats):
            r, c = divmod(i, cols)
            var = tk.StringVar(value='—')
            box = fact(self, label, textvariable=var)
            box.grid(row=r, column=c, sticky='nw',
                     padx=(0, px(16)), pady=(0 if r == 0 else px(row_gap), 0))
            self._cells[key] = (var, box.value)

    def set(self, key, value, warn=False, ok=False):
        cell = self._cells.get(key)
        if not cell:
            return
        var, lbl = cell
        var.set(value if value not in (None, '') else '—')
        # Legacy callers passed a colour name positionally ('accent',
        # 'purple', 'ok', 'warn'); only real status meanings colour a value.
        if isinstance(warn, str):
            warn, ok = warn == 'warn', warn == 'ok'
        try:
            lbl.config(fg=C['warn_hi'] if warn else
                       C['success_hi'] if ok else C['fg_0'])
        except Exception:
            pass

    def get(self, key):
        cell = self._cells.get(key)
        return cell[0].get() if cell else ''


class StatTile(tk.Frame):
    """Stat card: caption, large value (stat role) and an optional status
    icon. `set_value(v)` updates it (StatCard-compatible)."""

    def __init__(self, parent, caption, value='0', icon=None, tone=None,
                 compact=False):
        from ui.shared.widgets import Card, IconLabel
        super().__init__(parent, bg=parent.cget('bg'))
        card = Card(self, padding=16)
        card.pack(fill='both', expand=True)
        top = tk.Frame(card, bg=C['bg_2'])
        top.pack(fill='x')
        tk.Label(top, text=caption, font=FONTS['caption'], bg=C['bg_2'],
                 fg=C['fg_3'], anchor='w').pack(side='left')
        if icon:
            col = {'warn': C['warn'], 'danger': C['danger'],
                   'success': C['success']}.get(tone, C['fg_3'])
            IconLabel(top, icon, 16, col, C['bg_2']).pack(side='right')
        self._value_lbl = tk.Label(
            card, text=str(value),
            font=FONTS['body_strong'] if compact else FONTS['stat'],
            bg=C['bg_2'], fg=C['fg_0'], anchor='w', justify='left')
        self._value_lbl.pack(fill='x', pady=(px(10) if compact else px(6), 0))
        if compact:
            from ui.shared.widgets import autowrap
            autowrap(self._value_lbl)

    def set_value(self, value):
        try:
            self._value_lbl.configure(text=str(value))
        except Exception:
            pass


def _tone_for_colour(col):
    """Map a legacy status colour (hex) onto a Badge tone."""
    if not isinstance(col, str):
        return None
    h = col.lower()
    groups = {
        'success': ('success', 'success_hi', 'success_bg'),
        'warn': ('warn', 'warn_hi', 'warn_bg'),
        'danger': ('danger', 'danger_hi', 'danger_bg', 'danger_fill'),
        'info': ('accent', 'accent_hi', 'accent_lo', 'accent_08'),
    }
    for tone, keys in groups.items():
        if h in {C[k].lower() for k in keys}:
            return tone
    return 'neutral'


try:
    from ui.shared.widgets import Badge as _Badge
except Exception:   # pragma: no cover
    _Badge = tk.Label


class VarBadge(_Badge):
    """Status badge (icon + word) that stays compatible with legacy status
    labels: it follows a StringVar (`textvariable`), maps the displayed
    word through `mapper(value) -> (text, tone)` when given, and accepts
    `.config(fg=…)` from networking code, translating the colour to a tone.
    """

    def __init__(self, parent, textvariable=None, mapper=None, tone='neutral',
                 text='', **kw):
        super().__init__(parent, text=text, tone=tone, **kw)
        self._var = textvariable
        self._mapper = mapper
        if textvariable is not None:
            try:
                textvariable.trace_add('write', lambda *a: self._sync())
            except Exception:
                pass
            self._sync()

    def _sync(self):
        try:
            v = self._var.get()
        except Exception:
            return
        if self._mapper:
            try:
                txt, tone = self._mapper(v)
                self.set(txt, tone)
                return
            except Exception:
                pass
        self.set(v)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        fg = kw.pop('fg', None) or kw.pop('foreground', None)
        kw.pop('bg', None)
        kw.pop('background', None)
        kw.pop('font', None)
        tv = kw.pop('textvariable', None)
        if tv is not None:
            self._var = tv
            self._sync()
        if fg is not None and not self._mapper:
            self.set(tone=_tone_for_colour(fg))
        if kw:
            return super().configure(**kw)

    config = configure


class NullWidget:
    """Stands in for a retired decoration (pill frame, status dot) that the
    networking code still recolours. Accepts and ignores every call."""

    def __init__(self, *a, **kw):
        pass

    def configure(self, *a, **kw):
        return None

    config = configure

    def __getattr__(self, name):
        return lambda *a, **kw: None


class LegacyBar:
    """Mixin target: a RoundedBar that also accepts the Canvas calls the
    legacy progress code makes — coords(item, x0, y0, x1, y1) and
    itemconfig(item, fill=…). Create with ``legacy_bar(parent)``; the item
    id to pass is ``LegacyBar.ITEM``."""
    ITEM = 'bar'


def legacy_bar(parent, height=6):
    from ui.shared.widgets import RoundedBar

    class _Bar(RoundedBar):
        def coords(self, item, *xy):
            if len(xy) >= 4:
                w = max(1, self.winfo_width())
                self.set(max(0.0, min(1.0, (float(xy[2]) - float(xy[0])) / w)))
                return None
            return super().coords(item, *xy)

        def itemconfig(self, item, cnf=None, **kw):
            if cnf:
                kw.update(cnf)
            fill = kw.get('fill')
            if fill:
                tone = _tone_for_colour(fill)
                self.set_color(C['success'] if tone == 'success' else
                               C['danger'] if tone == 'danger' else
                               C['fg_5'] if tone == 'neutral' else C['accent'])
            return None

        itemconfigure = itemconfig

    return _Bar(parent, height=height)


class SubTabs(tk.Frame):
    """Underline tabs + content panes, API-compatible with the legacy
    SubTabBar used by Y2JB: add_tab(key, label) -> pane, show(key),
    show_first(), and a `_tabs` list of (key, button, frame)."""

    def __init__(self, parent, on_change=None):
        from ui.shared.widgets import Tabs
        super().__init__(parent, bg=parent.cget('bg'))
        self._on_change = on_change
        self.bar = Tabs(self, on_select=self._clicked)
        self.bar.pack(fill='x')
        self.content = tk.Frame(self, bg=self.cget('bg'))
        self.content.pack(fill='both', expand=True, pady=(px(16), 0))
        self._tabs = []
        self.current = None

    def add_tab(self, key, label, icon=None):
        btn = self.bar.add(key, label, icon)
        frame = tk.Frame(self.content, bg=self.cget('bg'))
        self._tabs.append((key, btn, frame))
        if self.current is None:
            self.show(key)
        return frame

    def _clicked(self, key):
        self.show(key)
        if self._on_change:
            self._on_change(key)

    def show(self, key):
        found = False
        for k, _b, f in self._tabs:
            if k == key:
                found = True
                f.pack(fill='both', expand=True)
            else:
                f.pack_forget()
        if not found:
            raise KeyError(key)
        self.current = key
        self.bar.select(key)

    def show_first(self):
        if self._tabs:
            self.show(self._tabs[0][0])


def console(parent, height=10, wrap='word'):
    """Inset console well (bg_0) with a monospace Text and scrollbar, using
    the design-system log colours. Returns (card, text)."""
    from ui.shared.widgets import Card
    well = Card(parent, tone='inset', padding=4)
    sb = XScrollbar(well, orient='vertical')
    sb.pack(side='right', fill='y', pady=px(4))
    txt = tk.Text(well, font=FONTS['code'], bg=C['bg_0'], fg=C['fg_1'],
                  insertbackground=C['fg_1'],
                  selectbackground=C['accent_lo'],
                  selectforeground=C['fg_0'], relief='flat', bd=0,
                  padx=px(10), pady=px(8), highlightthickness=0,
                  state='disabled', wrap=wrap, height=height,
                  spacing1=1, spacing3=1, yscrollcommand=sb.set)
    txt.pack(side='left', fill='both', expand=True)
    sb.configure(command=txt.yview)
    txt.tag_configure('error', foreground=C['danger_hi'])
    txt.tag_configure('warning', foreground=C['warn_hi'])
    txt.tag_configure('info', foreground=C['fg_1'])
    txt.tag_configure('success', foreground=C['success_hi'])
    txt.tag_configure('debug', foreground=C['fg_4'])
    txt.tag_configure('dim', foreground=C['fg_4'])
    txt.tag_configure('highlight', foreground=C['fg_0'],
                      background=C['warn_bg'])
    return well, txt


def search_field(parent, var, placeholder=None, width=None):
    """Search box: rounded field with a leading search icon."""
    from ui.shared.widgets import IconLabel
    bg = parent.cget('bg')
    wrap = tk.Frame(parent, bg=bg)
    kw = {'width': width} if width else {}
    e = XEntry(wrap, textvariable=var, placeholder=placeholder, **kw)
    e.pack(side='left', fill='x', expand=True)
    ic = IconLabel(e, 'search', 16, C['fg_4'], C['field_bg'])
    ic.place(relx=1.0, x=-px(12), rely=0.5, anchor='e')
    wrap.entry = e
    return wrap


def labeled_field(parent, label, var, width=None, mono=True, hint=None):
    """Label (body, fg_2) above a 32px field. Returns (frame, entry)."""
    from ui.shared.widgets import field_label
    bg = parent.cget('bg')
    f = tk.Frame(parent, bg=bg)
    field_label(f, label, hint, bg=bg).pack(fill='x')
    kw = {'width': width} if width else {}
    e = XEntry(f, textvariable=var, mono=mono, **kw)
    if width:
        e.pack(anchor='w', pady=(px(4), 0))
    else:
        e.pack(fill='x', pady=(px(4), 0))
    return f, e


class TintIcon(tk.Label):
    """A 16px icon label that accepts legacy `.configure(fg=…, bg=…)`
    recolouring (the emoji glyph labels it replaces were recoloured that
    way). `icon` may be an icon name or a legacy emoji."""

    def __init__(self, parent, icon, size=16, fg=None, bg=None, **kw):
        from ui.icons import has_icon, leading_icon
        name = icon if has_icon(icon) else (leading_icon(icon) or 'circle')
        self._ti = [name, size, fg or C['fg_3'], bg or parent.cget('bg')]
        super().__init__(parent, bd=0, padx=0, pady=0, bg=self._ti[3])
        self._draw()

    def _draw(self):
        from ui.icons import icon_image
        name, size, fg, bg = self._ti
        super().configure(image=icon_image(name, size, fg, bg), bg=bg)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        fg = kw.pop('fg', None) or kw.pop('foreground', None)
        bg = kw.pop('bg', None) or kw.pop('background', None)
        kw.pop('font', None)
        kw.pop('text', None)
        kw.pop('width', None)
        if fg or bg:
            if fg:
                self._ti[2] = fg
            if bg:
                self._ti[3] = bg
            self._draw()
        if kw:
            return super().configure(**kw)

    config = configure


def pill(parent, text, kind='advanced'):
    """Inline flag pill (danger / advanced) as a design-system Badge."""
    from ui.shared.widgets import Badge
    t = (text or '').strip()
    return Badge(parent, t[:1].upper() + t[1:].lower(),
                 'danger' if kind == 'danger' else 'outline', icon=False)


class ToolHero:
    """Page header + summary card for the automounter editors
    (ShadowMount+, MicroMount). Keeps the legacy ControlHero API those
    modules drive: set_stat(key, value, warn=, ok=), set_badge(text, kind),
    set_subtitle(text), `title_row` (status label slot), actions_row()
    (dirty-pill slot) and `actions` (page header actions, primary
    rightmost — pack with side='right')."""

    def __init__(self, parent, title, description, path, stats, cols=None):
        from ui.shared.widgets import Card, Badge
        hdr = page_header(parent, title, description)
        self.header = hdr
        self.actions = hdr.actions
        wrap = tk.Frame(parent, bg=C['bg_1'])
        wrap.pack(side='top', fill='x', padx=px(24), pady=(0, px(16)))
        self.frame = wrap
        card = Card(wrap, padding=16)
        card.pack(fill='x')
        right = tk.Frame(card, bg=C['bg_2'])
        right.pack(side='right', anchor='n', padx=(px(16), 0))
        self._badge_row = tk.Frame(right, bg=C['bg_2'])
        self._badge_row.pack(anchor='e')
        self._badge = Badge(self._badge_row, '', 'neutral')
        self._badge.pack(side='right')
        tk.Label(right, text=path, font=FONTS['code'], bg=C['bg_2'],
                 fg=C['fg_3'], anchor='e').pack(anchor='e', pady=(px(6), 0))
        self.title_row = tk.Frame(right, bg=C['bg_2'])
        self.title_row.pack(anchor='e', fill='x', pady=(px(4), 0))
        self.grid = StatGrid(card, stats, cols=cols)
        self.grid.pack(side='left', fill='x', expand=True)

    def set_stat(self, key, value, warn=False, ok=False):
        self.grid.set(key, value, warn=warn, ok=ok)

    def set_badge(self, text, kind='wait'):
        tone = {'ready': 'success', 'ok': 'success', 'warn': 'warn',
                'fail': 'danger'}.get(kind, 'neutral')
        txt = (text or '').strip()
        if txt.isupper():
            txt = txt[:1] + txt[1:].lower()
            txt = txt.replace('ps5', 'PS5')
        self._badge.set(txt, tone, icon=None if tone == 'success' else False)

    def set_subtitle(self, text):
        self.header.set(subtitle=text)

    def actions_row(self):
        return self._badge_row


def conn_mapper(value):
    """Display word + tone for the Manager connection var."""
    if value == _('CONNECTED'):
        return _('Online'), 'success'
    if value == _('OFFLINE'):
        return _('Offline'), 'warn'
    return _('Not tested'), 'neutral'


# ═════════════════════════════════════════════════════════════════════════
# Manager tab
# ═════════════════════════════════════════════════════════════════════════
def build_ps5_mgr_tab(parent, app):
    """Build the PS5 Control Center into `parent`."""
    app._ps5mgr_local = []
    app._ps5mgr_remote = []
    app._ps5mgr_merged = []
    app._ps5mgr_filter_var = tk.StringVar()

    body = ps5_page(parent)
    app._ps5mgr_page_canvas = getattr(body, '_ps5_canvas', None)

    hdr = page_header(body, _('PS5 Control Center'),
                      _('Browse and transfer files to and from your PS5.'))
    from ui.shared.widgets import Button
    Button(hdr.actions, text=_('Refresh All'), icon='refresh',
           variant='primary', command=app._ps5mgr_refresh).pack(side='right')

    left, right = two_columns(body)
    _build_connection_card(left, app)
    _build_setup_checklist(right, app)
    _build_tool_cards(body, app)
    _build_recent_activity(body, app)
    _build_game_manager(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Connection card — the anchor of the page.
#
# Backwards compat: `_ps5mgr_render` recolours `_ps5mgr_conn_pill` /
# `_conn_pill_inner` / `_conn_dot` / `_conn_lbl` and writes
# `_ps5mgr_conn_var` + `_ps5mgr_storage_var`, and feeds `_ps5mgr_hero`
# through set_stat()/set_subtitle(). All of these still exist: the status
# label is a VarBadge, the retired pill decorations are NullWidgets and the
# hero is a small adapter over this card.
# ─────────────────────────────────────────────────────────────────────────
class _HeroAdapter:
    """set_stat / set_subtitle / set_badge API of the old ControlHero,
    driving the connection card's stat grid and subtitle."""

    def __init__(self, card, grid, ip_label):
        self._card = card
        self._grid = grid
        self._ip = ip_label

    def set_stat(self, key, value, warn=False, ok=False):
        if key == 'ip':
            try:
                self._ip.config(text=value or '—',
                                fg=C['fg_0'] if value and value != '—'
                                else C['fg_4'])
            except Exception:
                pass
            return
        self._grid.set(key, value, warn=warn, ok=ok)

    def set_subtitle(self, text):
        try:
            if self._card.subtitle_label is not None:
                self._card.subtitle_label.config(text=text)
        except Exception:
            pass

    def set_title(self, text):
        try:
            self._card.title_label.config(text=text)
        except Exception:
            pass

    def set_badge(self, *a, **kw):
        pass


def _build_connection_card(parent, app):
    from ui.shared.widgets import Card, Button, Divider
    ip_now = app._ftp_ip_var.get().strip()
    card = Card(parent, title=_('PS5 Connection'),
                subtitle=(ip_now or _('No PS5 configured — set the IP in the '
                                      'FTP tab or Settings')))
    card.pack(fill='both', expand=True)

    # Status badge — driven by the legacy connection var.
    app._ps5mgr_conn_var = tk.StringVar(value=_('NOT TESTED'))
    app._ps5mgr_conn_lbl = VarBadge(card.head_actions,
                                    textvariable=app._ps5mgr_conn_var,
                                    mapper=conn_mapper)
    app._ps5mgr_conn_lbl.pack(side='left', padx=(0, px(12)))
    app._ps5mgr_conn_dot = NullWidget()
    app._ps5mgr_conn_pill = NullWidget()
    app._ps5mgr_conn_pill_inner = NullWidget()
    Button(card.head_actions, text=_('Test Connection'), icon='plug',
           command=app._ps5mgr_refresh).pack(side='left')

    # Address + service ports (read from settings — always available, with
    # the documented 2121 / 3232 / 9090 defaults).
    s = getattr(app, '_settings', {}) or {}
    ports = tk.Frame(card.body, bg=C['bg_2'])
    ports.pack(fill='x')
    for c in range(4):
        ports.columnconfigure(c, weight=1, uniform='ps5ports')
    ip_box = fact(ports, _('IP Address'), ip_now or '—', mono=True)
    ip_box.grid(row=0, column=0, sticky='nw')
    ip_lbl = ip_box.value
    ip_lbl.config(fg=C['fg_0'] if ip_now else C['fg_4'])
    fact(ports, _('FTP'), str(s.get('ftp_port', 2121)), mono=True
         ).grid(row=0, column=1, sticky='nw')
    fact(ports, _('Kernel Log'), str(s.get('klog_port', 3232)), mono=True
         ).grid(row=0, column=2, sticky='nw')
    fact(ports, _('Payload'), str(s.get('pl_port', 9090)), mono=True
         ).grid(row=0, column=3, sticky='nw')

    Divider(card.body, color=C['border_1']).pack(fill='x', pady=px(16))

    grid = StatGrid(card.body,
                    [(_('Firmware'), 'fw'), ('etaHEN', 'etahen'),
                     ('FTP', 'ftp'), (_('Payload'), 'payload'),
                     (_('Temperature'), 'temp'),
                     (_('Storage Used'), 'used'), (_('Free Space'), 'free'),
                     (_('Games on PS5'), 'games'),
                     (_('Local Images'), 'local')], cols=5)
    grid.pack(fill='x')

    hero = _HeroAdapter(card, grid, ip_lbl)
    app._ps5mgr_hero = hero
    app._ps5mgr_storage_var = tk.StringVar(value='')

    def _sync(*_a):
        try:
            state = app._ps5mgr_conn_var.get()
            ip = app._ftp_ip_var.get().strip()
            hero.set_stat('ip', ip or '—')
            hero.set_subtitle(ip or _('No PS5 configured'))
            if state == _('CONNECTED'):
                hero.set_stat('ftp', _('ONLINE') + ' · :' +
                              (app._ftp_port_var.get().strip() or '2121'),
                              ok=True)
            elif state == _('OFFLINE'):
                hero.set_stat('ftp', _('OFFLINE'), warn=True)
        except Exception:
            pass
    app._ps5mgr_conn_var.trace_add('write', _sync)

    # Payload cell mirrors the Payloads sub-tab status var once that
    # sub-tab exists (it builds lazily — retry briefly).
    def _hook_payload_status(tries=0):
        var = getattr(app, '_pl_status_var', None)
        if var is None:
            if tries < 10:
                card.after(600, lambda: _hook_payload_status(tries + 1))
            return

        def _sync_pl(*_a):
            try:
                t = var.get()
                if not t:
                    return
                if t.startswith('✓'):
                    hero.set_stat('payload', _('SENT'), ok=True)
                elif t.startswith('✗'):
                    hero.set_stat('payload', _('FAILED'), warn=True)
                elif 'Sending' in t or '%' in t:
                    hero.set_stat('payload', _('SENDING…'))
            except Exception:
                pass
        var.trace_add('write', _sync_pl)
    card.after(600, _hook_payload_status)


# ─────────────────────────────────────────────────────────────────────────
# Setup checklist — derived from existing settings + the connection var.
# ─────────────────────────────────────────────────────────────────────────
def _build_setup_checklist(parent, app):
    from ui.shared.widgets import Card, Badge, IconLabel, Divider
    card = Card(parent, title=_('Setup Checklist'))
    card.pack(fill='both', expand=True)
    progress = Badge(card.head_actions, '', 'neutral')
    progress.pack(side='right')
    app._ps5ov_check_progress = progress

    app._ps5ov_check_rows = {}

    def _row(key, done_label, pending_label, first=False):
        if not first:
            Divider(card.body, color=C['border_1']).pack(fill='x')
        r = tk.Frame(card.body, bg=C['bg_2'])
        r.pack(fill='x', pady=px(8))
        dot = IconLabel(r, 'circle', 16, C['fg_5'], C['bg_2'])
        dot.pack(side='left', padx=(0, px(10)))
        txt = tk.Label(r, text=pending_label, font=FONTS['body'],
                       bg=C['bg_2'], fg=C['fg_2'], anchor='w')
        txt.pack(side='left', fill='x', expand=True)
        app._ps5ov_check_rows[key] = {
            'dot': dot, 'txt': txt,
            'done': done_label, 'pending': pending_label, 'state': False}

    def _set(key, done):
        row = app._ps5ov_check_rows.get(key)
        if not row:
            return
        row['state'] = bool(done)
        try:
            if done:
                row['dot'].set('success', C['success'])
                row['txt'].config(text=row['done'], fg=C['fg_1'])
            else:
                row['dot'].set('warning', C['warn'])
                row['txt'].config(text=row['pending'], fg=C['fg_2'])
        except Exception:
            pass

    _row('ip', _('PS5 IP configured'), _('PS5 IP not configured'), first=True)
    _row('ftp', _('FTP reachable'), _('FTP not reachable'))
    _row('klog', _('Kernel Log configured'), _('Kernel Log not configured'))
    _row('payload', _('Payload configured'), _('Payload not configured'))
    _row('al_dir', _('Payload folder configured'),
         _('Payload folder not configured'))

    def _update_progress():
        try:
            rowmap = app._ps5ov_check_rows
            total = len(rowmap)
            done = sum(1 for r in rowmap.values() if r.get('state'))
            progress.set('%d / %d %s' % (done, total, _('complete')),
                         'success' if done == total else 'neutral',
                         icon=None if done == total else False)
        except Exception:
            pass

    def _refresh(*_a):
        s = getattr(app, '_settings', {}) or {}
        _set('ip', bool(str(s.get('ftp_ip', '')).strip()))
        _set('klog', bool(str(s.get('klog_ip', '')).strip()))
        _set('payload', bool(str(s.get('pl_ip', '')).strip()))
        _set('al_dir', bool(str(s.get('al_dir', '')).strip()))
        try:
            _set('ftp', app._ps5mgr_conn_var.get() == _('CONNECTED'))
        except Exception:
            _set('ftp', False)
        _update_progress()

    try:
        app._ps5mgr_conn_var.trace_add('write', _refresh)
    except Exception:
        pass
    _refresh()


# ─────────────────────────────────────────────────────────────────────────
# Tool cards — open the other PS5 sub-tabs through _ps5_subtab_activate.
# ─────────────────────────────────────────────────────────────────────────
def _build_tool_cards(body, app):
    from ui.shared.widgets import Card, Button, Badge, IconLabel, SectionTitle
    from ui.shared.widgets import autowrap

    def _go(key):
        try:
            getattr(app, '_ps5_subtab_activate', lambda k: None)(key)
        except Exception:
            pass

    sec = section(body)
    SectionTitle(sec, _('PS5 Tools')).pack(fill='x', pady=(0, px(12)))
    grid = tk.Frame(sec, bg=C['bg_1'])
    grid.pack(fill='x')
    cols = 3
    for c in range(cols):
        grid.columnconfigure(c, weight=1, uniform='ps5tool')

    cards = [
        ('ftp', 'wifi', _('File Transfer'),
         _('Browse and transfer files to and from your PS5.')),
        ('payloads', 'rocket', _('Send Payloads'),
         _('Send .elf / .bin payloads to your PS5.')),
        ('klog', 'klog', _('Kernel Log'),
         _('Stream the PS5 kernel log live.')),
        ('y2jb', 'youtube', 'Y2JB',
         _('YouTube to Jailbreak — install enabling PKGs.')),
        ('smp', 'zap', 'ShadowMount+',
         _('Configure and deploy the ShadowMount+ automounter.')),
        ('micromount', 'folders', 'MicroMount',
         _('Configure and deploy the MicroMount automounter.')),
    ]
    app._ps5ov_status_lbls = {}

    for i, (key, icon, title, desc) in enumerate(cards):
        r, c = divmod(i, cols)
        card = Card(grid, padding=16)
        card.grid(row=r, column=c, sticky='nsew',
                  padx=(0 if c == 0 else px(8), 0 if c == cols - 1 else px(8)),
                  pady=(0 if r == 0 else px(16), 0))
        top = tk.Frame(card, bg=C['bg_2'])
        top.pack(fill='x')
        IconLabel(top, icon, 20, C['fg_2'], C['bg_2']).pack(
            side='left', padx=(0, px(10)))
        tk.Label(top, text=title, font=FONTS['subtitle'], bg=C['bg_2'],
                 fg=C['fg_0'], anchor='w').pack(side='left')
        badge = Badge(top, '', 'neutral')
        app._ps5ov_status_lbls[key] = badge
        Button(card, text=_('Open'), icon='arrow_right', icon_right=True,
               size='sm', command=lambda k=key: _go(k)).pack(
                   side='bottom', anchor='w', pady=(px(12), 0))
        d = tk.Label(card, text=desc, font=FONTS['caption'], bg=C['bg_2'],
                     fg=C['fg_3'], anchor='nw', justify='left')
        d.pack(fill='both', expand=True, pady=(px(8), 0))
        autowrap(d)

    def _set_status(key, text, kind='muted'):
        b = app._ps5ov_status_lbls.get(key)
        if b is None:
            return
        try:
            if not text:
                b.pack_forget()
                return
            b.set(text, {'ok': 'success', 'warn': 'warn'}.get(kind, 'neutral'))
            if not b.winfo_ismapped():
                b.pack(side='right')
        except Exception:
            pass

    def _sync_ftp(*_a):
        try:
            state = app._ps5mgr_conn_var.get()
            if state == _('CONNECTED'):
                _set_status('ftp', _('Online'), 'ok')
            elif state == _('OFFLINE'):
                _set_status('ftp', _('Offline'), 'warn')
            else:
                _set_status('ftp', '', 'muted')
        except Exception:
            pass
    try:
        app._ps5mgr_conn_var.trace_add('write', _sync_ftp)
        _sync_ftp()
    except Exception:
        pass

    def _hook_payload(tries=0):
        var = getattr(app, '_pl_status_var', None)
        if var is None:
            if tries < 10:
                sec.after(600, lambda: _hook_payload(tries + 1))
            return

        def _sync_pl(*_a):
            try:
                t = var.get()
                if not t:
                    return
                if t.startswith('✓'):
                    _set_status('payloads', _('Sent'), 'ok')
                elif t.startswith('✗'):
                    _set_status('payloads', _('Failed'), 'warn')
                elif 'Sending' in t or '%' in t:
                    _set_status('payloads', _('Sending…'), 'muted')
            except Exception:
                pass
        try:
            var.trace_add('write', _sync_pl)
        except Exception:
            pass
    sec.after(600, _hook_payload)


# ─────────────────────────────────────────────────────────────────────────
# Recent activity — read-only view of app._ps5_activity_recent().
# ─────────────────────────────────────────────────────────────────────────
def _build_recent_activity(body, app):
    from ui.shared.widgets import Card, IconLabel, Divider
    sec = section(body)
    card = Card(sec, title=_('Recent PS5 Activity'))
    card.pack(fill='x')
    rows_wrap = tk.Frame(card.body, bg=C['bg_2'])
    rows_wrap.pack(fill='x')

    _ICONS = {'payload': ('rocket', C['fg_3']),
              'connection': ('wifi', C['fg_3']),
              'error': ('warning', C['warn'])}

    def _render():
        try:
            for w in rows_wrap.winfo_children():
                w.destroy()
        except Exception:
            return
        try:
            events = app._ps5_activity_recent(5)
        except Exception:
            events = []
        if not events:
            tk.Label(rows_wrap, text=_('No recent activity yet.'),
                     font=FONTS['body'], bg=C['bg_2'], fg=C['fg_3'],
                     anchor='w').pack(fill='x')
            return
        for i, ev in enumerate(events):
            if i:
                Divider(rows_wrap, color=C['border_1']).pack(fill='x')
            r = tk.Frame(rows_wrap, bg=C['bg_2'])
            r.pack(fill='x', pady=px(8))
            icon, col = _ICONS.get(ev.get('kind', ''), ('dot', C['fg_4']))
            IconLabel(r, icon, 16, col, C['bg_2']).pack(
                side='left', padx=(0, px(10)))
            tk.Label(r, text=ev.get('time', ''), font=FONTS['caption'],
                     bg=C['bg_2'], fg=C['fg_4'], anchor='e').pack(side='right')
            tk.Label(r, text=ev.get('detail', ''), font=FONTS['body'],
                     bg=C['bg_2'], fg=C['fg_1'], anchor='w').pack(
                         side='left', fill='x', expand=True)

    app._ps5ov_activity_refresh = _render
    _render()


# ─────────────────────────────────────────────────────────────────────────
# Game manager — search + legend + table. Rows are rendered into
# `app._ps5mgr_rows` by app._ps5mgr_render (main file).
# ─────────────────────────────────────────────────────────────────────────
# Column specs — (key, label, width or None=flex, anchor)
_COLS = [
    ('cover',   '',           48,   'w'),
    ('game',    _('Game'),    None, 'w'),
    ('local',   _('Local'),   90,   'e'),
    ('ps5',     _('PS5'),     90,   'e'),
    ('status',  _('Status'),  150,  'w'),
    ('path',    _('Path on PS5'), None, 'w'),
    ('actions', '',           96,   'e'),
]


def _build_game_manager(body, app):
    from ui.shared.widgets import Card, Badge, Divider, Segmented
    sec = section(body, pady=(0, 24))
    card = Card(sec, title=_('PS5 Game Manager'),
                subtitle=_('Compare local images with games on your PS5.'),
                padding=0)
    card.pack(fill='x')
    # Card(padding=0) — give the heading its own 16px inset.
    try:
        card.title_label.master.master.pack_configure(padx=px(16),
                                                      pady=(px(16), px(12)))
    except Exception:
        pass

    app._ps5mgr_status_var = tk.StringVar(
        value=_('Click Refresh All to load PS5 game list'))
    tk.Label(card.head_actions, textvariable=app._ps5mgr_status_var,
             font=FONTS['caption'], bg=C['bg_2'], fg=C['fg_3']
             ).pack(side='right', pady=(px(2), 0))

    # Toolbar: search · legend · (visual-only) filter chips
    bar = tk.Frame(card.body, bg=C['bg_2'])
    bar.pack(fill='x', padx=px(16), pady=(0, px(12)))
    sf = search_field(bar, app._ps5mgr_filter_var, width=32)
    sf.pack(side='left')

    def _on_search(*_a):
        if app._ps5mgr_merged:
            try:
                app._ps5mgr_render(app._ps5mgr_merged,
                                   len(app._ps5mgr_local),
                                   len(app._ps5mgr_remote))
            except Exception:
                pass
    app._ps5mgr_filter_var.trace_add('write', _on_search)

    # The chips were never wired to filter state; they stay a visual
    # segmented hint exactly as before (no callback).
    app._ps5mgr_chip_var = tk.StringVar(value='all')
    Segmented(bar, [('all', _('All')), ('on', _('On PS5')),
                    ('local', _('Local only')),
                    ('mismatch', _('Size mismatch'))],
              variable=app._ps5mgr_chip_var).pack(side='left', padx=(px(12), 0))

    leg = tk.Frame(card.body, bg=C['bg_2'])
    leg.pack(fill='x', padx=px(16), pady=(0, px(12)))
    for text, tone in ((_('On PS5'), 'success'),
                       (_('Built locally, not uploaded'), 'warn'),
                       (_('Not built yet'), 'neutral')):
        Badge(leg, text, tone, icon=False).pack(side='left', padx=(0, px(8)))

    Divider(card.body, color=C['border_2']).pack(fill='x')
    head = tk.Frame(card.body, bg=C['bg_2'])
    head.pack(fill='x')
    head_inner = tk.Frame(head, bg=C['bg_2'])
    head_inner.pack(fill='x', padx=px(16), pady=px(8))
    for i, (key, label, width, anchor) in enumerate(_COLS):
        if width is None:
            head_inner.grid_columnconfigure(i, weight=1, uniform='col')
        else:
            head_inner.grid_columnconfigure(i, weight=0, minsize=px(width))
        tk.Label(head_inner, text=label or '', font=FONTS['caption_strong'],
                 bg=C['bg_2'], fg=C['fg_3'], anchor=anchor
                 ).grid(row=0, column=i, sticky='ew',
                        padx=(0 if i == 0 else px(8), px(24) if key == 'ps5' else 0))
    Divider(card.body, color=C['border_2']).pack(fill='x')

    app._ps5mgr_rows = tk.Frame(card.body, bg=C['bg_2'])
    app._ps5mgr_rows.pack(fill='both', expand=True)
    _show_empty_state(app._ps5mgr_rows, app)


def _show_empty_state(parent, app=None):
    """EmptyState inside the rows frame, with a Refresh action."""
    from ui.shared.widgets import EmptyState
    action = None
    if app is not None:
        action = (_('Refresh All'), app._ps5mgr_refresh, 'refresh')
    es = EmptyState(parent, icon='game', title=_('No data yet'),
                    text=_('Refresh to scan local images and connect to '
                           'your PS5 over FTP.'),
                    action=action, bg=C['bg_2'])
    es.configure(height=px(260))
    es.pack(fill='x')
    return es


# ─────────────────────────────────────────────────────────────────────────
# Helpers exposed for _ps5mgr_render in the main file
# ─────────────────────────────────────────────────────────────────────────
def parse_meta_from_filename(name):
    """Parse a CUSA/PPSA id and version out of an .exfat filename.

    Returns (game_id, version, display_title). Examples:
      'CUSA-12345-Astros.exfat'     -> ('CUSA-12345', None,    'Astros')
      'CUSA-22871-Returnal-v2.10.exfat' -> ('CUSA-22871', '2.10', 'Returnal')
      'PPSA-01325 Bloodborne.exfat' -> ('PPSA-01325', None,    'Bloodborne')
    """
    base = name.rsplit('.', 1)[0]
    gid = None
    m = re.search(r'((?:CUSA|PPSA|PPLH)[-_ ]?\d{5})', base, re.IGNORECASE)
    if m:
        gid = m.group(1).upper().replace('_', '-').replace(' ', '-')
        # Normalize CUSA12345 → CUSA-12345
        if '-' not in gid:
            gid = gid[:4] + '-' + gid[4:]
    ver = None
    m = re.search(r'v(\d+\.\d+)', base, re.IGNORECASE)
    if m:
        ver = m.group(1)
    # Strip the gid + 'v...' from base to get a title-ish display name
    cleaned = base
    if gid:
        cleaned = re.sub(r'(?:CUSA|PPSA|PPLH)[-_ ]?\d{5}', '', cleaned,
                         flags=re.IGNORECASE)
    if ver:
        cleaned = re.sub(r'v\d+\.\d+', '', cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r'^[\s\-_.]+|[\s\-_.]+$', '', cleaned).strip()
    cleaned = re.sub(r'[\s_]+', ' ', cleaned).strip()
    return gid, ver, cleaned or base


def humanize_size(sz):
    if sz is None:
        return '—'
    if sz >= 1024**3:
        return '%.1f GB' % (sz / 1024**3)
    if sz >= 1024**2:
        return '%d MB' % (sz // 1024**2)
    return '%d KB' % (sz // 1024) if sz else '0 KB'


_GLYPH_ICON = {'↑': 'upload', '⤓': 'download', '↓': 'download',
               '\U0001f4c2': 'folder_open', '\U0001f5d1': 'trash',
               '✕': 'close', '⏸': 'pause', '▶': 'play',
               '✓': 'check'}


def _ghost_btn(parent, text, command):
    return XButton(parent, text=text, command=command, size='sm')


def _accent_btn(parent, text, command):
    return XButton(parent, text=text, command=command, variant='primary',
                   size='sm')


def _icon_btn(parent, glyph, tooltip='', command=None,
              accent=False, danger=False):
    """Row action: a small icon button with a tooltip."""
    from ui.shared.widgets import IconButton
    return IconButton(parent, _GLYPH_ICON.get(glyph, glyph), command,
                      tooltip=tooltip, size='sm')
