"""
ui/shared/widgets.py — the design-system component library.

Every screen builds from these. They follow the published design system
"PS5 exFAT Image Builder — Farewell Edition" (graphite surfaces, one blue
accent, 4px control radius, 8px containers, 32px controls) and are drop-in
friendly: legacy tk keyword arguments (bg=, fg=, relief=, padx= …) are
accepted and translated or ignored, so existing call sites can switch to
them without re-plumbing callbacks.

Components
----------
Button, IconButton        rounded ttk buttons (primary/secondary/subtle/danger)
TextField, Dropdown       rounded ttk entry / read-only combobox
Check, Radio              ttk check / radio with drawn indicators
Switch                    on/off toggle for instant settings
Badge                     pill status badge: icon + word
Card                      8px rounded container (a tk.Frame — pack into it)
InfoBar                   inline notification bar (info/success/warn/danger)
PageHeader                page title + description + actions slot
SectionTitle              card / section heading with optional caption
Tabs, Segmented           underline tabs and segmented toggle
EmptyState                icon + title + text + optional action
IconLabel                 a single icon as a label
Divider                   1px hairline
Tooltip                   hover tooltip
Toasts                    bottom-right transient notifications
"""
import tkinter as tk
from tkinter import ttk
import tkinter.font as tkfont

from tkinter_theme import (
    COLORS, FONTS, px, surface_of, button_style, entry_style, check_style,
    progress_style, rounded_image, BUTTON_VARIANTS, BUTTON_ALIASES, _c,
)
from ui.icons import icon_image, leading_icon, strip_glyphs, has_icon

C = COLORS

# tk keyword arguments that ttk widgets do not understand.
_TK_ONLY = {
    'bg', 'background', 'fg', 'foreground', 'activebackground',
    'activeforeground', 'relief', 'bd', 'borderwidth', 'padx', 'pady',
    'highlightthickness', 'highlightbackground', 'highlightcolor',
    'disabledforeground', 'font', 'anchor', 'justify', 'wraplength',
    'selectcolor', 'indicatoron', 'overrelief', 'insertbackground',
    'selectbackground', 'selectforeground', 'disabledbackground',
    'readonlybackground', 'insertwidth', 'offrelief', 'height',
    'compound_pad',
}


def _filter(kw):
    return {k: v for k, v in kw.items() if k not in _TK_ONLY}


# ─────────────────────────────────────────────────────────────────────
# Tooltip
# ─────────────────────────────────────────────────────────────────────
class Tooltip:
    """Small delayed tooltip (bg_3 card, caption text)."""

    def __init__(self, widget, text, delay=500, wraplength=320):
        self.widget, self.text, self.delay = widget, text, delay
        self.wraplength = wraplength
        self._after = None
        self._tip = None
        widget.bind('<Enter>', self._schedule, add='+')
        widget.bind('<Leave>', self._hide, add='+')
        widget.bind('<ButtonPress>', self._hide, add='+')

    def set(self, text):
        self.text = text

    def _schedule(self, _e=None):
        self._cancel()
        if self.text:
            self._after = self.widget.after(self.delay, self._show)

    def _cancel(self):
        if self._after:
            try:
                self.widget.after_cancel(self._after)
            except Exception:
                pass
            self._after = None

    def _show(self):
        if self._tip or not self.text:
            return
        try:
            x = self.widget.winfo_rootx() + px(8)
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + px(6)
            tw = tk.Toplevel(self.widget)
            tw.wm_overrideredirect(True)
            tw.configure(bg=C['border_3'])
            tk.Label(tw, text=self.text, bg=C['bg_3'], fg=C['fg_1'],
                     font=FONTS['caption'], justify='left',
                     wraplength=px(self.wraplength), padx=px(8), pady=px(5)
                     ).pack(padx=1, pady=1)
            tw.wm_geometry('+%d+%d' % (x, y))
            self._tip = tw
        except Exception:
            self._tip = None

    def _hide(self, _e=None):
        self._cancel()
        if self._tip:
            try:
                self._tip.destroy()
            except Exception:
                pass
            self._tip = None


# ─────────────────────────────────────────────────────────────────────
# Buttons
# ─────────────────────────────────────────────────────────────────────
_SELECTED_HEX = None


def _variant_for_colour(hexcol, current, surface=None):
    """Map a legacy bg= colour onto a button variant."""
    if not isinstance(hexcol, str):
        return current
    h = hexcol.lower()
    if surface and h == str(surface).lower():
        return 'subtle'
    accentish = {C[k].lower() for k in ('accent', 'accent_hi', 'accent_hover',
                                        'accent_pressed', 'success', 'success_hi')}
    dangerish = {C[k].lower() for k in ('danger', 'danger_hi', 'danger_fill',
                                        'danger_fill_hover')}
    if h in accentish:
        return 'primary'
    if h in dangerish:
        return 'danger'
    if h in (C['bg_5'].lower(),):
        return 'selected'
    return current if current in ('subtle', 'link') else 'secondary'


class Button(ttk.Button):
    """Design-system button.

    Button(parent, text, command, variant='secondary', size='md', icon=None)

    variant: primary | secondary | subtle | danger | selected | link
             (legacy names ghost/success/warn/backport are mapped)
    size:    md (32px) | sm (28px) | lg (36px)
    icon:    icon name from ui.icons (a leading emoji in `text` is
             converted to the matching icon automatically)

    The widget carries a 3px transparent margin for the keyboard focus
    ring, so the visible button is 32px inside a 38px widget.
    """

    def __init__(self, master=None, text='', command=None, variant='secondary',
                 size='md', icon=None, icon_right=False, surface=None,
                 tooltip=None, square=False, **kw):
        self._xb_variant = BUTTON_ALIASES.get(variant, variant)
        self._xb_size = size
        self._xb_surface = surface or surface_of(master)
        self._xb_icon_right = icon_right
        self._xb_square = square or (icon and not text)
        legacy_bg = kw.pop('bg', None) or kw.pop('background', None)
        legacy_fg = kw.get('fg') or kw.get('foreground')
        if legacy_bg is not None and variant == 'secondary':
            self._xb_variant = _variant_for_colour(legacy_bg, self._xb_variant,
                                                   self._xb_surface)
            if (self._xb_variant == 'subtle' and isinstance(legacy_fg, str)
                    and legacy_fg.lower() in (C['accent'].lower(),
                                              C['accent_hi'].lower())):
                self._xb_variant = 'link'
        # Compact legacy buttons (small pady / small font) become size 'sm'.
        if size == 'md':
            lp = kw.get('pady')
            lf = kw.get('font')
            try:
                if isinstance(lp, int) and lp <= 3:
                    size = 'sm'
                elif isinstance(lf, (tuple, list)) and len(lf) > 1 and int(lf[1]) <= 8:
                    size = 'sm'
            except Exception:
                pass
            self._xb_size = size
        self._xb_had_icon = False
        if icon is None and text:
            icon = leading_icon(text)
        if text:
            text = strip_glyphs(text)
        self._xb_icon = icon
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, text=text, command=command,
                         style=self._style_name(), **kw)
        self._xb_apply_icon()
        if not self._xb_square:
            self.configure(takefocus=1)
        self._tip = Tooltip(self, tooltip) if tooltip else None

    # -- internals --
    def _style_name(self):
        return button_style(self._xb_variant, self._xb_size, self._xb_surface,
                            square=bool(self._xb_square))

    def _xb_apply_icon(self):
        name = self._xb_icon
        if not name or not has_icon(name):
            if self._xb_had_icon:
                super().configure(image='', compound='none')
                self._xb_had_icon = False
            return
        self._xb_had_icon = True
        v = BUTTON_VARIANTS.get(self._xb_variant, BUTTON_VARIANTS['secondary'])
        fill, _b, hfill, _hb, pfill, fg, _f = v
        sf = self._xb_surface
        fgc = C[fg]
        if self._xb_variant in ('secondary', 'subtle') and self._xb_square:
            fgc = C['fg_2']
        dis_fill = sf if self._xb_variant in ('subtle', 'link') else (
            C['bg_2'] if sf != C['bg_2'] else C['bg_3'])
        sz = 16
        normal = icon_image(name, sz, fgc, _c(fill, sf))
        spec = [normal,
                'disabled', icon_image(name, sz, C['fg_5'], dis_fill),
                'pressed', icon_image(name, sz, fgc, _c(pfill, sf)),
                'active', icon_image(name, sz, C['fg_0'] if self._xb_square
                                     and fg == 'fg_1' else fgc, _c(hfill, sf))]
        compound = 'image' if not self.cget('text') else (
            'right' if self._xb_icon_right else 'left')
        super().configure(image=spec, compound=compound)

    # -- legacy-compatible configure --
    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if not kw:
            return super().configure()
        restyle = False
        if 'variant' in kw:
            self._xb_variant = BUTTON_ALIASES.get(kw.pop('variant'), 'secondary')
            restyle = True
        for k in ('bg', 'background'):
            if k in kw:
                nv = _variant_for_colour(kw.pop(k), self._xb_variant,
                                         self._xb_surface)
                if nv != self._xb_variant:
                    self._xb_variant = nv
                    restyle = True
        if 'icon' in kw:
            self._xb_icon = kw.pop('icon')
            restyle = True
        if 'text' in kw and isinstance(kw['text'], str):
            ic = leading_icon(kw['text'])
            if ic and ic != self._xb_icon:
                self._xb_icon = ic
                restyle = True
            kw['text'] = strip_glyphs(kw['text'])
        kw = _filter(kw)
        if restyle:
            kw['style'] = self._style_name()
        res = super().configure(**kw) if kw else None
        if restyle or 'text' in kw:
            self._xb_apply_icon()
        return res

    config = configure

    def cget(self, key):
        if key in ('bg', 'background'):
            v = BUTTON_VARIANTS.get(self._xb_variant)
            return _c(v[0], self._xb_surface) if v else self._xb_surface
        if key in ('fg', 'foreground'):
            v = BUTTON_VARIANTS.get(self._xb_variant)
            return C[v[5]] if v else C['fg_1']
        if key in _TK_ONLY:
            return ''
        return super().cget(key)

    def __getitem__(self, key):
        return self.cget(key)

    def __setitem__(self, key, value):
        self.configure(**{key: value})

    def set_variant(self, variant):
        self.configure(variant=variant)

    def flash(self):
        pass

    def invoke_if_enabled(self):
        if str(self.cget('state')) != 'disabled':
            self.invoke()


class IconButton(Button):
    """Square icon-only button (subtle by default) with a tooltip."""

    def __init__(self, master=None, icon='more', command=None, tooltip=None,
                 variant='subtle', size='md', **kw):
        kw.pop('text', None)
        super().__init__(master, text='', command=command, variant=variant,
                         size=size, icon=icon, tooltip=tooltip, square=True,
                         **kw)


# ─────────────────────────────────────────────────────────────────────
# Fields
# ─────────────────────────────────────────────────────────────────────
class TextField(ttk.Entry):
    """Rounded 32px text field. Accepts legacy tk.Entry kwargs."""

    def __init__(self, master=None, surface=None, mono=False, placeholder=None,
                 **kw):
        self._xb_surface = surface or surface_of(master)
        f = kw.pop('font', None)
        if not mono and isinstance(f, (tuple, list)) and f:
            fam = str(f[0])
            if fam in (FONTS['code'][0], 'Consolas', 'Cascadia Mono',
                       'Courier New', 'Cascadia Code'):
                mono = True
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, style=entry_style(self._xb_surface, 'TEntry'),
                         font=FONTS['code'] if mono else FONTS['body'], **kw)
        self._ph = None
        if placeholder:
            self._install_placeholder(placeholder)

    def _install_placeholder(self, text):
        lbl = tk.Label(self, text=text, bg=C['field_bg'], fg=C['fg_4'],
                       font=FONTS['body'], bd=0, padx=0, pady=0)
        self._ph = lbl

        def refresh(*_):
            try:
                empty = not self.get() and self.focus_get() is not self
                if empty and str(self.cget('state')) != 'disabled':
                    lbl.place(x=px(12), rely=0.5, anchor='w')
                else:
                    lbl.place_forget()
            except Exception:
                pass
        lbl.bind('<Button-1>', lambda e: self.focus_set())
        self.bind('<FocusIn>', refresh, add='+')
        self.bind('<FocusOut>', refresh, add='+')
        self.bind('<KeyRelease>', refresh, add='+')
        try:
            var = self.cget('textvariable')
            if var:
                self.tk.call('trace', 'add', 'variable', var, 'write',
                             self.register(lambda *a: refresh()))
        except Exception:
            pass
        self.after_idle(refresh)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        bg = kw.get('bg') or kw.get('background')
        if isinstance(bg, str):
            bad = bg.lower() in (C['danger_bg'].lower(), C['danger'].lower(),
                                 C['danger_hi'].lower())
            try:
                self.state(['invalid'] if bad else ['!invalid'])
            except Exception:
                pass
        kw = _filter(kw)
        return super().configure(**kw) if kw else super().configure()

    config = configure

    def cget(self, key):
        if key in _TK_ONLY:
            return ''
        return super().cget(key)

    # Legacy layouts padded tk.Entry with ipady; the field is already 32px.
    def pack_configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        kw.pop('ipady', None)
        return super().pack_configure(**kw)

    pack = pack_configure

    def grid_configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        kw.pop('ipady', None)
        return super().grid_configure(**kw)

    grid = grid_configure


class Scrollbar(ttk.Scrollbar):
    """Thin rounded scrollbar matched to the surface it sits on.
    Accepts legacy tk.Scrollbar kwargs."""

    def __init__(self, master=None, orient='vertical', surface=None, **kw):
        from tkinter_theme import scrollbar_style
        sf = surface or kw.get('troughcolor') or surface_of(master)
        for k in ('troughcolor', 'width', 'activebackground', 'elementborderwidth',
                  'jump', 'repeatdelay', 'repeatinterval', 'activerelief'):
            kw.pop(k, None)
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, orient=orient,
                         style=scrollbar_style(sf, orient), **kw)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        for k in ('troughcolor', 'width', 'activebackground', 'elementborderwidth'):
            kw.pop(k, None)
        kw = _filter(kw)
        return super().configure(**kw) if kw else super().configure()

    config = configure


class Dropdown(ttk.Combobox):
    """Rounded read-only (by default) dropdown."""

    def __init__(self, master=None, surface=None, editable=False, **kw):
        self._xb_surface = surface or surface_of(master)
        kw = _filter(kw)
        kw.pop('style', None)
        kw.setdefault('state', 'normal' if editable else 'readonly')
        super().__init__(master, style=entry_style(self._xb_surface, 'TCombobox'),
                         font=FONTS['body'], **kw)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        kw = _filter(kw)
        return super().configure(**kw) if kw else super().configure()

    config = configure


class Spin(ttk.Spinbox):
    def __init__(self, master=None, surface=None, **kw):
        self._xb_surface = surface or surface_of(master)
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, style=entry_style(self._xb_surface, 'TSpinbox'),
                         font=FONTS['body'], **kw)


# ─────────────────────────────────────────────────────────────────────
# Check / Radio
# ─────────────────────────────────────────────────────────────────────
class Check(ttk.Checkbutton):
    """Checkbox with drawn 18px indicator. Accepts legacy tk kwargs and
    offers tk.Checkbutton's select()/deselect()/toggle()."""

    def __init__(self, master=None, surface=None, **kw):
        self._xb_surface = surface or surface_of(master)
        if 'text' in kw:
            kw['text'] = strip_glyphs(kw['text'])
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, style=check_style(self._xb_surface), **kw)

    def _var(self):
        name = self.cget('variable')
        return str(name) if name else None

    def select(self):
        v = self._var()
        if v:
            self.setvar(v, self.cget('onvalue'))

    def deselect(self):
        v = self._var()
        if v:
            self.setvar(v, self.cget('offvalue'))

    def toggle(self):
        self.invoke()

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if 'text' in kw:
            kw['text'] = strip_glyphs(kw['text'])
        kw = _filter(kw)
        return super().configure(**kw) if kw else super().configure()

    config = configure

    def cget(self, key):
        if key in _TK_ONLY:
            return ''
        return super().cget(key)


class Radio(ttk.Radiobutton):
    def __init__(self, master=None, surface=None, **kw):
        self._xb_surface = surface or surface_of(master)
        if 'text' in kw:
            kw['text'] = strip_glyphs(kw['text'])
        kw = _filter(kw)
        kw.pop('style', None)
        super().__init__(master, style=check_style(self._xb_surface, radio=True),
                         **kw)

    def select(self):
        name = self.cget('variable')
        if name:
            self.setvar(str(name), self.cget('value'))

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if 'text' in kw:
            kw['text'] = strip_glyphs(kw['text'])
        kw = _filter(kw)
        return super().configure(**kw) if kw else super().configure()

    config = configure

    def cget(self, key):
        if key in _TK_ONLY:
            return ''
        return super().cget(key)


# ─────────────────────────────────────────────────────────────────────
# Switch
# ─────────────────────────────────────────────────────────────────────
def _switch_images(sf):
    from PIL import Image, ImageDraw, ImageTk
    from tkinter_theme import _IMAGES, _rgb, _ROOT
    W, H = px(40), px(20)
    m = px(3)
    out = {}
    specs = {
        'off':       ('field_bg', 'border_control', 'fg_3', False),
        'off_hover': ('bg_3',     'fg_4',           'fg_2', False),
        'on':        ('accent',   'accent',         'on_accent', True),
        'on_hover':  ('accent_hover', 'accent_hover', 'on_accent', True),
        'dis':       ('bg_2' if sf != C['bg_2'] else 'bg_1', 'border_3', 'fg_5', False),
        'dis_on':    ('bg_5',     'bg_5',           'fg_5', True),
    }
    for nm, (fl, bd, th, on) in specs.items():
        for focus in (False, True):
            k = ('sw', sf, nm, focus, W)
            if k in _IMAGES:
                out[(nm, focus)] = _IMAGES[k]
                continue
            sc = 4
            TW, TH = (W + 2 * m) * sc, (H + 2 * m) * sc
            im = Image.new('RGB', (TW, TH), _rgb(sf))
            d = ImageDraw.Draw(im)
            if focus:
                d.rounded_rectangle((0, 0, TW - 1, TH - 1), TH // 2,
                                    fill=_rgb(C['focus']))
                i = px(2) * sc
                d.rounded_rectangle((i, i, TW - 1 - i, TH - 1 - i),
                                    TH // 2 - i, fill=_rgb(sf))
            M = m * sc
            box = (M, M, TW - 1 - M, TH - 1 - M)
            d.rounded_rectangle(box, (H * sc) // 2, fill=_rgb(C[bd]))
            b = px(1) * sc
            d.rounded_rectangle((box[0] + b, box[1] + b, box[2] - b, box[3] - b),
                                (H * sc) // 2 - b, fill=_rgb(C[fl]))
            r = (px(7) if on else px(6)) * sc
            cy = TH / 2
            cx = (box[2] - px(10) * sc) if on else (box[0] + px(10) * sc)
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=_rgb(C[th]))
            img = ImageTk.PhotoImage(im.resize((W + 2 * m, H + 2 * m),
                                               Image.LANCZOS), master=_ROOT[0])
            _IMAGES[k] = img
            out[(nm, focus)] = img
    return out


class Switch(tk.Frame):
    """On/off switch: 40x20 track, On/Off text. For settings that apply
    immediately. `variable` may be a BooleanVar/IntVar/StringVar with
    onvalue/offvalue; `command` runs after every toggle."""

    def __init__(self, master=None, variable=None, command=None, text=None,
                 onvalue=True, offvalue=False, show_state=True, surface=None,
                 state='normal', **kw):
        self._sf = surface or kw.pop('bg', None) or surface_of(master)
        super().__init__(master, bg=self._sf)
        self.var = variable if variable is not None else tk.BooleanVar(value=False)
        self.onvalue, self.offvalue = onvalue, offvalue
        self.command = command
        self._state = state
        self._hover = False
        self._img = _switch_images(self._sf)
        self._track = tk.Label(self, bg=self._sf, bd=0, takefocus=1,
                               highlightthickness=0)
        self._track.pack(side='left')
        self._lbl = None
        if show_state:
            self._lbl = tk.Label(self, bg=self._sf, fg=C['fg_3'],
                                 font=FONTS['body'], width=3, anchor='w')
            self._lbl.pack(side='left', padx=(px(6), 0))
        if text:
            tk.Label(self, text=text, bg=self._sf, fg=C['fg_1'],
                     font=FONTS['body']).pack(side='left', padx=(px(8), 0))
        for w in (self, self._track) + ((self._lbl,) if self._lbl else ()):
            w.bind('<Button-1>', self._click)
            w.bind('<Enter>', lambda e: self._set_hover(True))
            w.bind('<Leave>', lambda e: self._set_hover(False))
        self._track.bind('<space>', self._click)
        self._track.bind('<FocusIn>', lambda e: self._draw())
        self._track.bind('<FocusOut>', lambda e: self._draw())
        try:
            self.var.trace_add('write', lambda *a: self._draw())
        except Exception:
            pass
        self._draw()

    def is_on(self):
        try:
            v = self.var.get()
        except Exception:
            return False
        if isinstance(self.onvalue, bool):
            return bool(v)
        return v == self.onvalue

    def _click(self, _e=None):
        if self._state == 'disabled':
            return
        self.var.set(self.offvalue if self.is_on() else self.onvalue)
        if self.command:
            self.command()
        return 'break'

    def _set_hover(self, h):
        self._hover = h
        self._draw()

    def _draw(self):
        on = self.is_on()
        try:
            focus = self.focus_get() is self._track
        except Exception:
            focus = False
        if self._state == 'disabled':
            nm = 'dis_on' if on else 'dis'
        else:
            nm = ('on' if on else 'off') + ('_hover' if self._hover else '')
        self._track.configure(image=self._img[(nm, focus)])
        if self._lbl is not None:
            self._lbl.configure(text='On' if on else 'Off',
                                fg=C['fg_5'] if self._state == 'disabled' else C['fg_3'])

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if 'state' in kw:
            self._state = kw.pop('state')
            self._draw()
        if 'command' in kw:
            self.command = kw.pop('command')
        if kw:
            return super().configure(**kw)

    config = configure


# ─────────────────────────────────────────────────────────────────────
# Badge
# ─────────────────────────────────────────────────────────────────────
BADGE_TONES = {
    # tone: (bg, fg, default icon)
    'neutral': ('bg_3', 'fg_2', None),
    'info':    ('accent_lo', 'accent_hi', None),
    'success': ('success_bg', 'success_hi', 'check'),
    'warn':    ('warn_bg', 'warn_hi', 'warning'),
    'danger':  ('danger_bg', 'danger_hi', 'error'),
    'outline': (None, 'fg_3', None),
}
TONE_ALIASES = {
    'queued': 'neutral', 'idle': 'neutral', 'waiting': 'neutral',
    'skipped': 'neutral', 'running': 'info', 'active': 'info',
    'working': 'info', 'progress': 'info', 'done': 'success', 'ok': 'success',
    'complete': 'success', 'warning': 'warn', 'check': 'warn',
    'failed': 'danger', 'fail': 'danger', 'error': 'danger',
}


class Badge(tk.Canvas):
    """Pill badge: optional 12px icon + word, 20px tall.

    Badge(parent, text='Verified', tone='success')
    badge.set('Failed', tone='danger')
    """

    def __init__(self, master=None, text='', tone='neutral', icon=None,
                 mono=False, surface=None, **kw):
        self._sf = surface or kw.pop('bg', None) or surface_of(master)
        super().__init__(master, bg=self._sf, highlightthickness=0, bd=0,
                         height=px(20), width=px(40))
        self._mono = mono
        Badge.set(self, text, tone, icon)

    def set(self, text=None, tone=None, icon='__keep__'):
        if text is not None:
            self._text = strip_glyphs(text)
        if tone is not None:
            self._tone = TONE_ALIASES.get(tone, tone)
            if self._tone not in BADGE_TONES:
                self._tone = 'neutral'
        if icon != '__keep__':
            self._icon = icon
        self._draw()

    def _draw(self):
        self.delete('all')
        bgk, fgk, default_icon = BADGE_TONES[self._tone]
        ic = self._icon if self._icon is not None else default_icon
        if self._icon is False:
            ic = None
        fnt = FONTS['code'] if self._mono else FONTS['caption_strong']
        f = tkfont.Font(font=fnt)
        tw = f.measure(self._text) if self._text else 0
        pad = px(8)
        isz = px(12)
        gap = px(4) if (ic and self._text) else 0
        w = pad * 2 + tw + (isz + gap if ic else 0)
        if not self._text and ic:
            w = px(20)
        h = px(20)
        fill = C[bgk] if bgk else self._sf
        border = C['border_3'] if bgk is None else None
        img = rounded_image(w, h, h // 2, fill, self._sf, border=border, bw=1)
        self._bgimg = img
        self.configure(width=w, height=h)
        self.create_image(0, 0, image=img, anchor='nw')
        x = pad if (self._text or not ic) else (w - isz) // 2
        if ic:
            ii = icon_image(ic, 12, C[fgk], fill)
            self._icimg = ii
            self.create_image(x, h // 2, image=ii, anchor='w')
            x += isz + gap
        if self._text:
            self.create_text(x, h // 2, text=self._text, anchor='w',
                             fill=C[fgk], font=fnt)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        txt = kw.pop('text', None)
        if txt is not None:
            self.set(txt)
        for k in ('fg', 'foreground', 'font', 'padx', 'pady'):
            kw.pop(k, None)
        if 'bg' in kw:
            self._sf = kw['bg']
        if kw:
            return super().configure(**kw)

    config = configure


# ─────────────────────────────────────────────────────────────────────
# Containers
# ─────────────────────────────────────────────────────────────────────
def _corner_images(radius, fill, border, outside):
    """Four anti-aliased corner images for a rounded rectangle."""
    from PIL import Image, ImageDraw, ImageTk
    from tkinter_theme import _IMAGES, _rgb, _ROOT
    key = ('corners', radius, fill, border, outside)
    if key in _IMAGES:
        return _IMAGES[key]
    sc = 4
    r = radius
    S = r * 2 * sc
    im = Image.new('RGB', (S, S), _rgb(outside))
    d = ImageDraw.Draw(im)
    if border:
        d.ellipse((0, 0, S - 1, S - 1), fill=_rgb(border))
        b = sc
        d.ellipse((b, b, S - 1 - b, S - 1 - b), fill=_rgb(fill))
    else:
        d.ellipse((0, 0, S - 1, S - 1), fill=_rgb(fill))
    full = im.resize((2 * r, 2 * r), Image.LANCZOS)
    out = {
        'nw': full.crop((0, 0, r, r)), 'ne': full.crop((r, 0, 2 * r, r)),
        'sw': full.crop((0, r, r, 2 * r)), 'se': full.crop((r, r, 2 * r, 2 * r)),
    }
    imgs = {k: ImageTk.PhotoImage(v, master=_ROOT[0]) for k, v in out.items()}
    _IMAGES[key] = imgs
    return imgs


class Rounded(tk.Frame):
    """A tk.Frame with anti-aliased rounded corners and an optional 1px
    border. Children pack/grid into it directly; keep `padding` >= radius/3
    so content stays clear of the corners."""

    def __init__(self, master=None, bg=None, border=None, radius=8,
                 padding=0, surface=None, **kw):
        bg = bg or C['bg_2']
        self._xb_bg = bg
        self._xb_border = border
        self._xb_outside = surface or surface_of(master)
        self._xb_radius = px(radius)
        if isinstance(padding, (tuple, list)):
            padx, pady = padding[0], padding[1] if len(padding) > 1 else padding[0]
        else:
            padx = pady = padding
        padx, pady = int(padx), int(pady)
        # The placer measures from inside a frame's internal padding, so the
        # decorations are offset back out by (padx, pady).
        self._xb_pad = (padx, pady)
        kw.pop('highlightthickness', None)
        kw.pop('highlightbackground', None)
        super().__init__(master, bg=bg, padx=padx, pady=pady,
                         highlightthickness=0, bd=0, **kw)
        self._xb_deco = []
        self._xb_draw()

    def _xb_draw(self):
        for w in self._xb_deco:
            try:
                w.destroy()
            except Exception:
                pass
        self._xb_deco = []
        r = self._xb_radius
        if r <= 0 and not self._xb_border:
            return
        ox, oy = self._xb_pad
        if r > 0:
            imgs = _corner_images(r, self._xb_bg, self._xb_border, self._xb_outside)
            self._xb_imgs = imgs
            for anchor, (rx, ry) in (('nw', (0, 0)), ('ne', (1, 0)),
                                     ('sw', (0, 1)), ('se', (1, 1))):
                lbl = tk.Label(self, image=imgs[anchor], bd=0, padx=0, pady=0,
                               highlightthickness=0, bg=self._xb_outside)
                lbl.place(relx=rx, rely=ry, anchor=anchor,
                          x=ox if rx else -ox, y=oy if ry else -oy)
                self._xb_deco.append(lbl)
        if self._xb_border:
            b = self._xb_border
            for spec in (dict(x=r - ox, y=-oy, relwidth=1, width=2 * ox - 2 * r, height=1),
                         dict(x=r - ox, rely=1, y=oy - 1, relwidth=1, width=2 * ox - 2 * r, height=1),
                         dict(x=-ox, y=r - oy, relheight=1, height=2 * oy - 2 * r, width=1),
                         dict(relx=1, x=ox - 1, y=r - oy, relheight=1, height=2 * oy - 2 * r, width=1)):
                ln = tk.Frame(self, bg=b, bd=0, highlightthickness=0)
                ln.place(**spec)
                self._xb_deco.append(ln)

    def set_style(self, bg=None, border='__keep__'):
        """Change fill and/or border (e.g. hover / selected states)."""
        if bg:
            self._xb_bg = bg
            super().configure(bg=bg)
        if border != '__keep__':
            self._xb_border = border
        self._xb_draw()

    def raise_decorations(self):
        for w in self._xb_deco:
            try:
                w.lift()
            except Exception:
                pass

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        hb = kw.pop('highlightbackground', None)
        kw.pop('highlightthickness', None)
        kw.pop('highlightcolor', None)
        newbg = kw.get('bg') or kw.get('background')
        res = super().configure(**kw) if kw else super().configure()
        if newbg or hb:
            if newbg:
                self._xb_bg = newbg
            if hb:
                self._xb_border = hb
            self._xb_draw()
        return res

    config = configure


class Card(Rounded):
    """8px rounded card (bg_2, border_2 hairline). Pack content into it.

    Card(parent, padding=16)                         — plain card
    Card(parent, title='Output', subtitle='…')       — with heading; the
        heading row exposes `card.head_actions` for right-aligned controls
        and content goes into `card.body` (== the card itself for plain
        cards, so `card.body` always works).
    tone='inset' gives the bg_0 well; tone='raised' a bg_3 card;
    selected=True the accent selection look.
    """

    TONES = {
        'default': ('bg_2', 'border_2'),
        'inset':   ('bg_0', 'border_2'),
        'raised':  ('bg_3', 'border_3'),
        'canvas':  ('bg_1', 'border_2'),
        'selected': ('accent_08', 'accent'),
        'info':    ('accent_lo', 'border_2'),
    }

    def __init__(self, master=None, title=None, subtitle=None, icon=None,
                 padding=16, tone='default', actions=False, **kw):
        bgk, bdk = self.TONES.get(tone, self.TONES['default'])
        bg = kw.pop('bg', None) or C[bgk]
        border = kw.pop('border', C[bdk])
        super().__init__(master, bg=bg, border=border, radius=8,
                         padding=padding, **kw)
        self.body = self
        self.head_actions = None
        self.actions = None
        if title is not None:
            head = tk.Frame(self, bg=bg)
            head.pack(fill='x', pady=(0, px(12)))
            if icon:
                tile = tk.Label(head, image=icon_image(icon, 16, C['accent_hi'], C['accent_lo']),
                                bg=C['accent_lo'], width=px(32), height=px(32))
                tile.pack(side='left', padx=(0, px(12)))
            col = tk.Frame(head, bg=bg)
            col.pack(side='left', fill='x', expand=True)
            self.title_label = tk.Label(col, text=strip_glyphs(title), bg=bg,
                                        fg=C['fg_0'], font=FONTS['subtitle'],
                                        anchor='w', justify='left')
            self.title_label.pack(fill='x')
            self.subtitle_label = None
            if subtitle:
                self.subtitle_label = tk.Label(
                    col, text=subtitle, bg=bg, fg=C['fg_3'],
                    font=FONTS['caption'], anchor='w', justify='left')
                self.subtitle_label.pack(fill='x', pady=(px(2), 0))
                _autowrap(self.subtitle_label)
            self.head_actions = tk.Frame(head, bg=bg)
            self.head_actions.pack(side='right', anchor='n')
            self.body = tk.Frame(self, bg=bg)
            self.body.pack(fill='both', expand=True)
        if actions:
            self.actions = tk.Frame(self, bg=bg)
            self.actions.pack(side='bottom', fill='x', pady=(px(12), 0))
            Divider(self, color=C['border_1']).pack(side='bottom', fill='x',
                                                    pady=(px(12), 0))


def _autowrap(label, margin=0):
    """Re-wrap a label to its width as it resizes."""
    def _on(e):
        w = max(px(80), e.width - margin)
        try:
            if int(label.cget('wraplength') or 0) != w:
                label.configure(wraplength=w)
        except Exception:
            pass
    label.bind('<Configure>', _on, add='+')
    return label


autowrap = _autowrap


class Divider(tk.Frame):
    def __init__(self, master=None, color=None, vertical=False, **kw):
        super().__init__(master, bg=color or C['border_2'],
                         width=1 if vertical else None,
                         height=None if vertical else 1, bd=0,
                         highlightthickness=0, **kw)


class IconLabel(tk.Label):
    def __init__(self, master=None, icon='circle', size=16, fg=None, bg=None,
                 **kw):
        bg = bg or surface_of(master)
        self._xb = (icon, size, fg or C['fg_3'], bg)
        super().__init__(master, image=icon_image(icon, size, fg or C['fg_3'], bg),
                         bg=bg, bd=0, padx=0, pady=0, **kw)

    def set(self, icon=None, fg=None, bg=None):
        i, s, f, b = self._xb
        i, f, b = icon or i, fg or f, bg or b
        self._xb = (i, s, f, b)
        self.configure(image=icon_image(i, s, f, b), bg=b)


# ─────────────────────────────────────────────────────────────────────
# InfoBar
# ─────────────────────────────────────────────────────────────────────
INFO_TONES = {
    'info':    ('accent_lo',  'accent_hi', 'info'),
    'success': ('success_bg', 'success',   'success'),
    'warn':    ('warn_bg',    'warn',      'warning'),
    'danger':  ('danger_bg',  'danger',    'error'),
    'neutral': ('bg_2',       'fg_3',      'info'),
}


class InfoBar(Rounded):
    """Inline notification: tinted bar, status icon, bold lead-in title,
    message, optional action button(s) and dismiss.

    InfoBar(parent, kind='warn', title='OSFMount not detected',
            text='Required for building and verifying exFAT images.',
            action=('Download OSFMount', callback), dismiss=callable|None)
    `bar.actions` is a frame for more buttons.
    """

    def __init__(self, master=None, kind='info', text='', title=None,
                 action=None, dismiss=None, **kw):
        kind = {'warning': 'warn', 'error': 'danger', 'ok': 'success'}.get(kind, kind)
        bgk, ick, icn = INFO_TONES.get(kind, INFO_TONES['info'])
        bg = C[bgk]
        super().__init__(master, bg=bg, border=C['border_2'], radius=8,
                         padding=(px(16), px(10)), **kw)
        self._bg = bg
        IconLabel(self, icn, 16, C[ick], bg).pack(side='left', anchor='n',
                                                  pady=(px(1), 0))
        self.actions = tk.Frame(self, bg=bg)
        self.actions.pack(side='right', anchor='center', padx=(px(12), 0))
        txt = tk.Frame(self, bg=bg)
        txt.pack(side='left', fill='x', expand=True, padx=(px(12), 0))
        self.title_label = None
        if title:
            self.title_label = tk.Label(txt, text=title, bg=bg, fg=C['fg_0'],
                                        font=FONTS['body_strong'], anchor='w',
                                        justify='left')
            self.title_label.pack(fill='x')
        self.text_label = tk.Label(txt, text=text, bg=bg, fg=C['fg_1'],
                                   font=FONTS['body'], anchor='w',
                                   justify='left')
        if text:
            self.text_label.pack(fill='x')
            _autowrap(self.text_label)
        if action:
            lbl, cmd = action
            Button(self.actions, text=lbl, command=cmd, size='sm',
                   surface=bg).pack(side='left')
        if dismiss:
            def _d():
                try:
                    dismiss()
                finally:
                    self.destroy()
            IconButton(self.actions, 'close', _d, tooltip='Dismiss', size='sm',
                       surface=bg).pack(side='left', padx=(px(4), 0))

    def set_text(self, text):
        self.text_label.configure(text=text)
        if text and not self.text_label.winfo_ismapped():
            self.text_label.pack(fill='x')


# ─────────────────────────────────────────────────────────────────────
# Headers
# ─────────────────────────────────────────────────────────────────────
class PageHeader(tk.Frame):
    """Page title (title style) + one-line description + actions slot.

    PageHeader(parent, 'Build', 'Pack a game dump into an image.')
    header.actions — pack buttons with side='right' (primary rightmost).
    """

    def __init__(self, master=None, title='', subtitle=None, bg=None, **kw):
        bg = bg or surface_of(master)
        super().__init__(master, bg=bg, **kw)
        self.actions = tk.Frame(self, bg=bg)
        self.actions.pack(side='right', anchor='s')
        col = tk.Frame(self, bg=bg)
        col.pack(side='left', fill='x', expand=True)
        self.title_label = tk.Label(col, text=strip_glyphs(title), bg=bg,
                                    fg=C['fg_0'], font=FONTS['title'],
                                    anchor='w')
        self.title_label.pack(fill='x')
        self.subtitle_label = tk.Label(col, text=subtitle or '', bg=bg,
                                       fg=C['fg_3'], font=FONTS['body'],
                                       anchor='w', justify='left')
        if subtitle:
            self.subtitle_label.pack(fill='x', pady=(px(2), 0))
            _autowrap(self.subtitle_label)

    def set(self, title=None, subtitle=None):
        if title is not None:
            self.title_label.configure(text=strip_glyphs(title))
        if subtitle is not None:
            self.subtitle_label.configure(text=subtitle)
            if subtitle and not self.subtitle_label.winfo_ismapped():
                self.subtitle_label.pack(fill='x', pady=(px(2), 0))


class SectionTitle(tk.Frame):
    """Card/section heading: subtitle-style title, optional caption and a
    right-aligned `actions` slot."""

    def __init__(self, master=None, title='', caption=None, bg=None,
                 overline=False, **kw):
        bg = bg or surface_of(master)
        super().__init__(master, bg=bg, **kw)
        self.actions = tk.Frame(self, bg=bg)
        self.actions.pack(side='right', anchor='center')
        col = tk.Frame(self, bg=bg)
        col.pack(side='left', fill='x', expand=True)
        if overline:
            self.title_label = tk.Label(col, text=strip_glyphs(title).upper(),
                                        bg=bg, fg=C['fg_4'],
                                        font=FONTS['overline'], anchor='w')
        else:
            self.title_label = tk.Label(col, text=strip_glyphs(title), bg=bg,
                                        fg=C['fg_0'], font=FONTS['subtitle'],
                                        anchor='w')
        self.title_label.pack(fill='x')
        self.caption_label = None
        if caption:
            self.caption_label = tk.Label(col, text=caption, bg=bg,
                                          fg=C['fg_3'], font=FONTS['caption'],
                                          anchor='w', justify='left')
            self.caption_label.pack(fill='x', pady=(px(2), 0))
            _autowrap(self.caption_label)


def field_label(master, text, help_text=None, bg=None):
    """Label above a field (body, fg_2) with optional help to the right."""
    bg = bg or surface_of(master)
    row = tk.Frame(master, bg=bg)
    tk.Label(row, text=text, bg=bg, fg=C['fg_2'], font=FONTS['body'],
             anchor='w').pack(side='left')
    if help_text:
        tk.Label(row, text=help_text, bg=bg, fg=C['fg_4'],
                 font=FONTS['caption'], anchor='w').pack(side='left',
                                                         padx=(px(8), 0))
    return row


def help_text(master, text, bg=None, tone=None):
    bg = bg or surface_of(master)
    fg = {'error': C['danger_hi'], 'warn': C['warn_hi'],
          'success': C['success_hi']}.get(tone, C['fg_3'])
    lbl = tk.Label(master, text=text, bg=bg, fg=fg, font=FONTS['caption'],
                   anchor='w', justify='left')
    return _autowrap(lbl)


# ─────────────────────────────────────────────────────────────────────
# Tabs / Segmented
# ─────────────────────────────────────────────────────────────────────
class Tabs(tk.Frame):
    """Underline tabs. items: list of (key, label[, icon]).
    on_select(key) is called on click; select(key) updates the look."""

    def __init__(self, master=None, items=(), on_select=None, bg=None,
                 selected=None, **kw):
        bg = bg or surface_of(master)
        super().__init__(master, bg=bg, **kw)
        self._bg = bg
        self.on_select = on_select
        self._tabs = {}
        row = tk.Frame(self, bg=bg)
        row.pack(fill='x')
        Divider(self, color=C['border_2']).pack(fill='x')
        self._row = row
        for it in items:
            self.add(*it)
        if selected or items:
            self.select(selected or items[0][0])

    def add(self, key, label, icon=None):
        bg = self._bg
        f = tk.Frame(self._row, bg=bg, cursor='hand2')
        f.pack(side='left', padx=(0, px(4)))
        inner = tk.Frame(f, bg=bg)
        inner.pack(padx=px(12), pady=(px(9), px(7)))
        ic = None
        if icon:
            ic = tk.Label(inner, bg=bg, bd=0)
            ic.pack(side='left', padx=(0, px(8)))
        lbl = tk.Label(inner, text=strip_glyphs(label), bg=bg, fg=C['fg_3'],
                       font=FONTS['body'])
        lbl.pack(side='left')
        bar = tk.Frame(f, bg=bg, height=px(3))
        bar.pack(fill='x', padx=px(12), side='bottom')
        self._tabs[key] = dict(frame=f, label=lbl, bar=bar, icon=ic,
                               icon_name=icon, inner=inner, selected=False)

        def click(_e, k=key):
            self.select(k)
            if self.on_select:
                self.on_select(k)
        for w in (f, inner, lbl) + ((ic,) if ic else ()):
            w.bind('<Button-1>', click)
            w.bind('<Enter>', lambda e, k=key: self._hover(k, True))
            w.bind('<Leave>', lambda e, k=key: self._hover(k, False))
        self._paint(key)
        return f

    def set_label(self, key, label):
        t = self._tabs.get(key)
        if t:
            t['label'].configure(text=strip_glyphs(label))

    def _hover(self, key, on):
        t = self._tabs.get(key)
        if not t or t['selected']:
            return
        t['label'].configure(fg=C['fg_1'] if on else C['fg_3'])

    def _paint(self, key):
        t = self._tabs[key]
        sel = t['selected']
        t['label'].configure(fg=C['fg_0'] if sel else C['fg_3'],
                             font=FONTS['body_strong'] if sel else FONTS['body'])
        t['bar'].configure(bg=C['accent'] if sel else self._bg)
        if t['icon'] is not None:
            t['icon'].configure(image=icon_image(
                t['icon_name'], 16, C['accent_hi'] if sel else C['fg_3'], self._bg))

    def select(self, key):
        for k, t in self._tabs.items():
            t['selected'] = (k == key)
            self._paint(k)


class Segmented(Rounded):
    """Segmented toggle for 2–4 options. options: list of (value, label).
    variable: tk variable holding the value; command(value) on change."""

    def __init__(self, master=None, options=(), variable=None, command=None,
                 bg=None, **kw):
        outside = bg or surface_of(master)
        super().__init__(master, bg=C['bg_0'], border=C['border_2'], radius=4,
                         padding=px(2), surface=outside, **kw)
        self.var = variable if variable is not None else tk.StringVar()
        self.command = command
        self._btns = {}
        for value, label in options:
            b = tk.Label(self, text=strip_glyphs(label), bg=C['bg_0'],
                         fg=C['fg_3'], font=FONTS['body'], padx=px(12),
                         pady=px(4), cursor='hand2')
            b.pack(side='left', padx=(0, px(2)))
            b.bind('<Button-1>', lambda e, v=value: self._pick(v))
            b.bind('<Enter>', lambda e, v=value: self._hover(v, True))
            b.bind('<Leave>', lambda e, v=value: self._hover(v, False))
            self._btns[value] = b
        try:
            self.var.trace_add('write', lambda *a: self._paint())
        except Exception:
            pass
        self._paint()

    def _pick(self, v):
        self.var.set(v)
        if self.command:
            self.command(v)

    def _hover(self, v, on):
        if str(self.var.get()) == str(v):
            return
        self._btns[v].configure(fg=C['fg_1'] if on else C['fg_3'])

    def _paint(self):
        cur = str(self.var.get())
        for v, b in self._btns.items():
            sel = str(v) == cur
            b.configure(bg=C['bg_3'] if sel else C['bg_0'],
                        fg=C['fg_0'] if sel else C['fg_3'],
                        font=FONTS['body_strong'] if sel else FONTS['body'])


# ─────────────────────────────────────────────────────────────────────
# Empty state
# ─────────────────────────────────────────────────────────────────────
class EmptyState(tk.Frame):
    def __init__(self, master=None, icon='package', title='', text='',
                 action=None, bg=None, **kw):
        bg = bg or surface_of(master)
        super().__init__(master, bg=bg, **kw)
        inner = tk.Frame(self, bg=bg)
        inner.place(relx=0.5, rely=0.45, anchor='center')
        self.inner = inner
        tile = Rounded(inner, bg=C['bg_3'], radius=8, surface=bg)
        tile.pack()
        IconLabel(tile, icon, 24, C['fg_3'], C['bg_3']).pack(padx=px(14), pady=px(14))
        self.title_label = tk.Label(inner, text=title, bg=bg, fg=C['fg_0'],
                                    font=FONTS['subtitle'])
        self.title_label.pack(pady=(px(14), px(4)))
        self.text_label = tk.Label(inner, text=text, bg=bg, fg=C['fg_3'],
                                   font=FONTS['body'], justify='center',
                                   wraplength=px(420))
        if text:
            self.text_label.pack()
        if action:
            lbl, cmd = action[0], action[1]
            ic = action[2] if len(action) > 2 else None
            Button(inner, text=lbl, command=cmd, icon=ic,
                   surface=bg).pack(pady=(px(14), 0))
        self.configure(height=px(220))

    def set(self, title=None, text=None):
        if title is not None:
            self.title_label.configure(text=title)
        if text is not None:
            self.text_label.configure(text=text)


# ─────────────────────────────────────────────────────────────────────
# Progress
# ─────────────────────────────────────────────────────────────────────
class ProgressBar(ttk.Progressbar):
    """6px rounded progress bar. tone: accent|success|warn|danger."""

    def __init__(self, master=None, tone='accent', thickness=6, surface=None,
                 **kw):
        self._sf = surface or surface_of(master)
        self._tone = tone
        self._th = thickness
        kw = _filter(kw)
        kw.pop('style', None)
        kw.setdefault('mode', 'determinate')
        super().__init__(master, style=progress_style(self._sf, tone, thickness),
                         **kw)

    def set_tone(self, tone):
        if tone != self._tone:
            self._tone = tone
            self.configure(style=progress_style(self._sf, tone, self._th))


# ─────────────────────────────────────────────────────────────────────
# Toasts
# ─────────────────────────────────────────────────────────────────────
class Toasts:
    """Bottom-right transient notifications inside a root window."""

    def __init__(self, root):
        self.root = root
        self.items = []

    def show(self, title, message='', kind='info', timeout=6000, action=None):
        icn = {'info': ('info', 'accent_hi'), 'success': ('success', 'success'),
               'warn': ('warning', 'warn'), 'danger': ('error', 'danger')}
        name, col = icn.get(kind, icn['info'])
        t = Rounded(self.root, bg=C['bg_3'], border=C['border_3'], radius=8,
                    padding=(px(14), px(12)), surface=C['bg_1'])
        IconLabel(t, name, 16, C[col], C['bg_3']).pack(side='left', anchor='n')
        body = tk.Frame(t, bg=C['bg_3'])
        body.pack(side='left', fill='x', expand=True, padx=(px(12), px(8)))
        tk.Label(body, text=title, bg=C['bg_3'], fg=C['fg_0'],
                 font=FONTS['body_strong'], anchor='w', justify='left',
                 wraplength=px(260)).pack(fill='x')
        if message:
            tk.Label(body, text=message, bg=C['bg_3'], fg=C['fg_2'],
                     font=FONTS['caption'], anchor='w', justify='left',
                     wraplength=px(260)).pack(fill='x', pady=(px(2), 0))
        if action:
            Button(body, text=action[0], command=action[1], size='sm',
                   surface=C['bg_3']).pack(anchor='w', pady=(px(8), 0))
        IconButton(t, 'close', lambda: self.dismiss(t), size='sm',
                   surface=C['bg_3'], tooltip='Dismiss').pack(side='right',
                                                              anchor='n')
        self.items.insert(0, t)
        self._layout()
        if timeout and kind != 'danger':
            self.root.after(timeout, lambda: self.dismiss(t))
        return t

    def dismiss(self, t):
        if t in self.items:
            self.items.remove(t)
            try:
                t.destroy()
            except Exception:
                pass
            self._layout()

    def _layout(self):
        y = -px(16)
        for t in self.items[:3]:
            t.place(relx=1.0, rely=1.0, x=-px(16), y=y, anchor='se',
                    width=px(360))
            t.lift()
            t.update_idletasks()
            y -= t.winfo_reqheight() + px(8)
        for t in self.items[3:]:
            t.place_forget()


# ─────────────────────────────────────────────────────────────────────
# RoundedBar — canvas progress bar with a place()-compatible fill proxy
# ─────────────────────────────────────────────────────────────────────
class _FillProxy:
    """Stands in for a legacy `fill` Frame that code moved with
    place_configure(relx=…, relwidth=…)."""

    def __init__(self, bar):
        self.bar = bar

    def place_configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if 'relx' in kw:
            self.bar._rx = float(kw['relx'])
        if 'relwidth' in kw:
            self.bar._rw = float(kw['relwidth'])
        self.bar._redraw()

    place = place_configure

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        bg = kw.get('bg') or kw.get('background')
        if bg:
            self.bar.set_color(bg)

    config = configure


class RoundedBar(tk.Canvas):
    """6px rounded progress bar. `bar.fill` accepts place_configure(relx,
    relwidth) like the legacy Frame-based bars; `set(fraction)` too."""

    def __init__(self, master=None, height=6, color=None, track=None,
                 surface=None, **kw):
        self._sf = surface or surface_of(master)
        self._h = px(height)
        super().__init__(master, bg=self._sf, height=self._h,
                         highlightthickness=0, bd=0)
        self._color = color or C['accent']
        self._track = track or C['bg_5']
        self._rx, self._rw = 0.0, 0.0
        self.fill = _FillProxy(self)
        self.bind('<Configure>', lambda e: self._redraw())

    def set(self, frac, relx=0.0):
        self._rx, self._rw = float(relx), max(0.0, min(1.0, float(frac)))
        self._redraw()

    def set_color(self, color):
        self._color = color
        self._redraw()

    def _caps(self, color, under):
        h = self._h
        img = rounded_image(h, h, h // 2, color, under, key=('cap', h, color, under))
        return img

    def _seg(self, x0, x1, color, under):
        h = self._h
        r = h // 2
        if x1 - x0 < h:
            x1 = x0 + h
        cap = self._caps(color, under)
        self._keep.append(cap)
        self.create_rectangle(x0 + r, 0, x1 - r, h, fill=color, width=0)
        self.create_image(x0, 0, image=cap, anchor='nw')
        self.create_image(x1 - h, 0, image=cap, anchor='nw')

    def _redraw(self):
        self.delete('all')
        self._keep = []
        W = max(1, self.winfo_width())
        self._seg(0, W, self._track, self._sf)
        if self._rw > 0:
            x0 = int(W * self._rx)
            x1 = int(W * min(1.0, self._rx + self._rw))
            self._seg(x0, max(x0 + self._h, x1), self._color, self._track)


# ─────────────────────────────────────────────────────────────────────
# StepIcon — stepper dot driven by legacy glyph text
# ─────────────────────────────────────────────────────────────────────
_STEP_GLYPHS = {
    '✔': ('success', 'success'), '✓': ('success', 'success'),
    '▶': ('play_circle', 'accent_hi'), '⏳': ('circle', 'fg_6'),
    '✕': ('error', 'danger'), '✗': ('error', 'danger'),
    '⚠': ('warning', 'warn'),
}


class StepIcon(tk.Label):
    """A label that shows an icon for status glyphs (✔ ▶ ⏳ ✕ ⚠) so code
    that does .config(text='✔', fg=…) keeps working."""

    def __init__(self, master=None, glyph='⏳', size=16, bg=None, **kw):
        self._sz = size
        self._bg = bg or surface_of(master)
        super().__init__(master, bg=self._bg, bd=0, padx=0, pady=0)
        self.configure(text=glyph)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        t = kw.pop('text', None)
        kw.pop('fg', None)
        kw.pop('foreground', None)
        kw.pop('font', None)
        if 'bg' in kw:
            self._bg = kw['bg']
        if t is not None:
            name, col = _STEP_GLYPHS.get(t.strip()[:1] if t else '', ('circle', 'fg_6'))
            kw['image'] = icon_image(name, self._sz, C[col], self._bg)
        if kw:
            return super().configure(**kw)

    config = configure


# ─────────────────────────────────────────────────────────────────────
# CoverLabel — fixed-size cover art tile with an icon placeholder
# ─────────────────────────────────────────────────────────────────────
class CoverLabel(tk.Label):
    """Cover tile (box x box px). Accepts the legacy calls
    .config(image=photo, text='', width=…, height=…) and
    .config(image='', text='🎮', …) — the emoji becomes an icon."""

    def __init__(self, master=None, box=128, placeholder='game', **kw):
        self._box = px(box)
        self._ph = placeholder
        super().__init__(master, bg=C['bg_3'], bd=0, width=self._box,
                         height=self._box)
        self._show_placeholder(placeholder)

    def _show_placeholder(self, name):
        img = icon_image(name, 36, C['fg_4'], C['bg_3'])
        super().configure(image=img, text='', width=self._box, height=self._box)

    def configure(self, cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        img = kw.get('image', None)
        txt = kw.get('text', None)
        if img == '' or (img is None and txt):
            from ui.icons import leading_icon
            name = leading_icon(txt or '') or self._ph
            if name == 'ps5':
                name = 'game'
            self._show_placeholder(name)
            return
        if img:
            return super().configure(image=img, text='', width=self._box,
                                     height=self._box)
        kw = {k: v for k, v in kw.items() if k not in ('font', 'width', 'height', 'fg')}
        if kw:
            return super().configure(**kw)

    config = configure


# ─────────────────────────────────────────────────────────────────────
# Expander — collapsible section with chevron header
# ─────────────────────────────────────────────────────────────────────
class Expander(tk.Frame):
    """Header row (chevron · title · caption) toggling `body`.
    on_toggle(open) is called after each change."""

    def __init__(self, master=None, title='', caption=None, icon=None,
                 open=False, on_toggle=None, bg=None, **kw):
        bg = bg or surface_of(master)
        super().__init__(master, bg=bg, **kw)
        self._bg = bg
        self.is_open = bool(open)
        self.on_toggle = on_toggle
        self.head = Rounded(self, bg=C['bg_3'], border=C['border_2'], radius=4,
                            padding=(px(10), px(7)), surface=bg)
        self.head.pack(fill='x')
        self._chev = tk.Label(self.head, bg=C['bg_3'], bd=0)
        self._chev.pack(side='left')
        if icon:
            IconLabel(self.head, icon, 16, C['fg_3'], C['bg_3']).pack(
                side='left', padx=(px(8), 0))
        tk.Label(self.head, text=strip_glyphs(title), bg=C['bg_3'],
                 fg=C['fg_1'], font=FONTS['body_strong']).pack(
                     side='left', padx=(px(8), 0))
        if caption:
            tk.Label(self.head, text=caption, bg=C['bg_3'], fg=C['fg_4'],
                     font=FONTS['caption']).pack(side='left', padx=(px(10), 0))
        self.body = tk.Frame(self, bg=bg)
        for w in [self.head] + list(self.head.winfo_children()):
            w.bind('<Button-1>', lambda e: self.toggle())
            try:
                w.configure(cursor='hand2')
            except Exception:
                pass
        self._render()

    def toggle(self):
        self.is_open = not self.is_open
        self._render()
        if self.on_toggle:
            self.on_toggle(self.is_open)

    def _render(self):
        self._chev.configure(image=icon_image(
            'chevron_down' if self.is_open else 'chevron_right', 16,
            C['fg_2'], C['bg_3']))
        if self.is_open:
            self.body.pack(fill='x', pady=(px(8), 0))
        else:
            self.body.pack_forget()
