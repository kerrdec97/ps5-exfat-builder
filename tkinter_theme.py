"""
exFAT Image Builder — tkinter/ttk theme module (Farewell Edition design system)
===============================================================================

The code side of the published design system "PS5 exFAT Image Builder —
Farewell Edition": graphite surfaces, one electric-blue accent, Windows 11
geometry (4px control radius, 8px containers, 32px controls).

Single call themes the whole application:

    root = tk.Tk()
    apply_theme(root)

After that every ttk widget picks up the design system, and
`ui.shared.widgets` builds the custom components (buttons, fields, cards,
badges, switches, progress bars) on top of the helpers defined here.

Token names are the keys the application already used (bg_0 … bg_5,
fg_0 … fg_6, accent, success …). Keys from earlier palettes are kept as
aliases so no call site breaks.

Rounded controls
----------------
Native Tk widgets are square. Buttons, fields, checkboxes, radios,
scrollbars and progress bars here are ttk image elements: small rounded
rectangles rendered with Pillow at 4x and downsampled. Every image is
composited onto the surface colour it sits on (no transparency), so it
renders identically on Windows and Linux. Styles are created lazily per
surface via ``button_style()``, ``entry_style()`` and friends.
"""

import tkinter as tk
from tkinter import ttk
import platform

# ────────────────────────────────────────────────────────────────────
# COLOR TOKENS
# ────────────────────────────────────────────────────────────────────
COLORS = {
    # Surfaces — graphite ladder, darkest = window chrome
    "bg_0":     "#0d0f12",   # sidebar, header, console wells, drop zones
    "bg_1":     "#131519",   # content canvas, root window
    "bg_2":     "#191c21",   # cards, panels, tables, dialogs
    "bg_3":     "#20242a",   # secondary button, hover row, active nav, menus
    "bg_4":     "#272b32",   # hover on bg_3, scrollbar thumb
    "bg_5":     "#30353c",   # pressed, progress track

    # Lines
    "border_1": "#1e2126",   # row dividers
    "border_2": "#262a30",   # card / header / sidebar hairlines
    "border_3": "#30353c",   # buttons, dialogs, menus
    "border_4": "#3c424a",   # hover borders
    "border_control": "#6a717b",  # input / checkbox / switch outline (3:1)

    # Text
    "fg_0":     "#f3f5f8",   # headings
    "fg_1":     "#dce0e5",   # body
    "fg_2":     "#bcc2ca",   # emphasised secondary
    "fg_3":     "#9da4ad",   # secondary text, idle icons
    "fg_4":     "#8a919b",   # meta, placeholders (not on bg_4)
    "fg_5":     "#6b727c",   # disabled only
    "fg_6":     "#454b53",   # decorative only

    # Fields
    "field_bg": "#1d2026",
    "field_fg": "#dce0e5",
    "field_select_bg": "#2a6de8",
    "field_select_fg": "#ffffff",

    # Accent — electric blue
    "accent":         "#2a6de8",
    "accent_hover":   "#2766d9",
    "accent_pressed": "#2259c0",
    "accent_hi":      "#6ea8ff",
    "accent_lo":      "#122340",
    "accent_08":      "#121a28",
    "on_accent":      "#ffffff",
    "focus":          "#6ea8ff",

    # Status
    "success":    "#34c48a", "success_hi": "#4fd39b", "success_bg": "#0f2a20",
    "warn":       "#e0a030", "warn_hi":    "#f0b443", "warn_bg":    "#2d220c",
    "danger":     "#e5484d", "danger_hi":  "#ff7a7f", "danger_bg":  "#33161a",
    "danger_fill": "#c62f39", "danger_fill_hover": "#b22a33",

    "scrim":      "#050608",
}
# Legacy keys from earlier palettes — kept as aliases so every existing
# call site keeps working. Purple and teal are retired: they now resolve
# to the single accent.
COLORS.update({
    "accent_15":  COLORS["accent_lo"],
    "info_bg":    COLORS["accent_lo"],
    "purple":     COLORS["accent"],
    "purple_hi":  COLORS["accent_hi"],
    "teal":       COLORS["accent"],
    "teal_hi":    COLORS["accent_hi"],
    "teal_bg":    COLORS["accent_lo"],
    "teal_lo":    COLORS["accent_lo"],
    "success_ok": COLORS["success_hi"],   # klog keeps mono, loses neon
    "conf_clean": COLORS["success"],
    "conf_guess": COLORS["warn"],
    "conf_fail":  COLORS["danger"],
})

# ────────────────────────────────────────────────────────────────────
# FONTS
# ────────────────────────────────────────────────────────────────────
# Windows 11 ships Segoe UI Variable; Windows 10 has Segoe UI. Tk only knows
# "normal"/"bold", so semibold comes from the named semibold family. The
# families are resolved against the installed fonts in apply_theme(); the
# values below are the Windows 10 fallbacks used until then.
_IS_WIN = platform.system() == "Windows"
_SANS = "Segoe UI"
_SEMI = "Segoe UI Semibold"
_DISP = "Segoe UI Semibold"
_MONO = "Consolas" if _IS_WIN else "Cascadia Mono"


def _build_fonts():
    return {
        # Design-system roles (sizes in points: 1pt = 1.33px at 96 dpi)
        "display":        (_DISP, 20, "normal"),   # 26px — hero title
        "title":          (_DISP, 15, "normal"),   # 20px — page / dialog title
        "stat":           (_DISP, 16, "normal"),   # 22px — stat numbers
        "subtitle":       (_SEMI, 11, "normal"),   # 15px — card titles
        "body_strong":    (_SEMI, 10, "normal"),   # 13px
        "body":           (_SANS, 10, "normal"),   # 13px
        "caption":        (_SANS, 9, "normal"),    # 12px
        "caption_strong": (_SEMI, 9, "normal"),
        "overline":       (_SEMI, 8, "normal"),    # 11px, upper-case text
        "code":           (_MONO, 9, "normal"),    # 12px paths, IDs, logs
        "code_sm":        (_MONO, 8, "normal"),    # 11px dense logs
        # Legacy keys. `mono_sm` was used as a general label font all over
        # the app; it is now the sans caption — real monospace is `code`.
        "h1":      (_DISP, 20, "normal"),
        "h2":      (_DISP, 15, "normal"),
        "h3":      (_SEMI, 11, "normal"),
        "eyebrow": (_SEMI, 8, "normal"),
        "body_b":  (_SEMI, 10, "normal"),
        "label":   (_SANS, 10, "normal"),
        "meta":    (_SANS, 9, "normal"),
        "button":  (_SANS, 10, "normal"),
        "tab":     (_SANS, 10, "normal"),
        "mono":    (_MONO, 9, "normal"),
        "mono_sm": (_SANS, 9, "normal"),
    }


FONTS = _build_fonts()


def _resolve_fonts(root):
    """Pick the best installed families and rebuild FONTS in place."""
    global _SANS, _SEMI, _DISP, _MONO
    try:
        from tkinter import font as tkfont
        fams = set(tkfont.families(root))
    except Exception:
        fams = set()

    def first(*names):
        for n in names:
            if n in fams:
                return n
        return names[-1]

    _SANS = first("Segoe UI Variable Text", "Segoe UI")
    if _SANS == "Segoe UI Variable Text":
        _SEMI = first("Segoe UI Variable Text Semibold",
                      "Segoe UI Variable Text Semib", "Segoe UI Semibold")
        _DISP = first("Segoe UI Variable Display Semibold",
                      "Segoe UI Variable Display Semib", _SEMI)
    else:
        _SEMI = _DISP = "Segoe UI Semibold"
    _MONO = first("Cascadia Mono", "Consolas")
    FONTS.clear()
    FONTS.update(_build_fonts())


def font(role, size=None):
    """Return the font tuple for a design-system role, optionally resized."""
    f = FONTS.get(role, FONTS["body"])
    return f if size is None else (f[0], size, f[2])


# ────────────────────────────────────────────────────────────────────
# SCALE — pixel sizes follow the Windows display scale
# ────────────────────────────────────────────────────────────────────
SCALE = 1.0


def px(n):
    """Scale a 96-dpi pixel length to the current display."""
    return int(round(n * SCALE))


# ────────────────────────────────────────────────────────────────────
# SPACING / RADIUS / SIZES
# ────────────────────────────────────────────────────────────────────
SPACING = {
    "none": 0, "xxs": 2, "xs": 4, "sm": 8, "md": 12, "lg": 16,
    "xl": 20, "xxl": 24, "xxxl": 32, "huge": 48,
}

RADIUS = {"none": 0, "xs": 2, "sm": 4, "md": 8, "lg": 8, "xl": 8, "pill": 999}

SIZES = {
    "header": 48, "sidebar": 232, "sidebar_compact": 56, "nav_item": 36,
    "control": 32, "control_sm": 28, "row": 36, "row_compact": 28,
    "icon": 16, "check": 18, "switch_w": 40, "progress": 6,
    "dialog": 480, "toast": 360,
}

# ────────────────────────────────────────────────────────────────────
# IMAGE FACTORY — rounded, anti-aliased, surface-composited
# ────────────────────────────────────────────────────────────────────
_IMAGES = {}      # cache: key -> PhotoImage (also keeps references alive)
_ROOT = [None]


def _rgb(c):
    c = c.lstrip("#")
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def rounded_image(w, h, radius, fill, surface, border=None, bw=1,
                  ring=None, ring_w=2, gap=1, margin=0, key=None):
    """Return a cached PhotoImage of a rounded rectangle.

    The control occupies the box inset by `margin`; when `ring` is given a
    focus ring of `ring_w` is drawn in the margin, `gap` px away from the
    control. Everything is composited on `surface` (fully opaque).
    """
    k = key or ("rr", w, h, radius, fill, surface, border, bw, ring,
                ring_w, gap, margin)
    img = _IMAGES.get(k)
    if img is not None:
        return img
    from PIL import Image, ImageDraw, ImageTk
    sc = 4
    W, H = w * sc, h * sc
    im = Image.new("RGB", (W, H), _rgb(surface))
    d = ImageDraw.Draw(im)
    m = margin * sc
    if ring:
        rr = (radius + margin) * sc
        d.rounded_rectangle((0, 0, W - 1, H - 1), rr, fill=_rgb(ring))
        inner = (ring_w) * sc
        d.rounded_rectangle((inner, inner, W - 1 - inner, H - 1 - inner),
                            max(0, rr - inner), fill=_rgb(surface))
    r = radius * sc
    box = (m, m, W - 1 - m, H - 1 - m)
    if border and border != fill:
        d.rounded_rectangle(box, r, fill=_rgb(border))
        b = bw * sc
        d.rounded_rectangle((box[0] + b, box[1] + b, box[2] - b, box[3] - b),
                            max(0, r - b), fill=_rgb(fill))
    else:
        d.rounded_rectangle(box, r, fill=_rgb(fill))
    im = im.resize((w, h), Image.LANCZOS)
    img = ImageTk.PhotoImage(im, master=_ROOT[0])
    _IMAGES[k] = img
    return img


def surface_of(widget, default=None):
    """Best-effort background colour of a widget (tk or ttk)."""
    w = widget
    for _ in range(6):
        if w is None:
            break
        try:
            bg = w.cget("bg")
            if bg:
                return bg if bg.startswith("#") else _named_to_hex(w, bg)
        except Exception:
            pass
        try:
            st = w.cget("style") or w.winfo_class()
            bg = ttk.Style(w).lookup(st, "background")
            if bg:
                return bg if bg.startswith("#") else _named_to_hex(w, bg)
        except Exception:
            pass
        try:
            w = w.master
        except Exception:
            break
    return default or COLORS["bg_1"]


def _named_to_hex(w, name):
    try:
        r, g, b = w.winfo_rgb(name)
        return "#%02x%02x%02x" % (r >> 8, g >> 8, b >> 8)
    except Exception:
        return COLORS["bg_1"]


def _skey(surface):
    return surface.lstrip("#").lower()


# ────────────────────────────────────────────────────────────────────
# BUTTON STYLES (lazy, per variant / size / surface)
# ────────────────────────────────────────────────────────────────────
# visual box = control height; the ttk widget adds a 3px margin on every
# side that carries the keyboard focus ring.
_FOCUS_MARGIN = 3

BUTTON_VARIANTS = {
    #           fill        border      hover_fill     hover_border    pressed        fg           font
    "primary":   ("accent",      "accent",      "accent_hover",      "accent_hover",      "accent_pressed",    "on_accent", "body_strong"),
    "secondary": ("bg_3",        "border_3",    "bg_4",              "border_4",          "bg_5",              "fg_1",      "body"),
    "subtle":    (None,          None,          "bg_3",              "bg_3",              "bg_4",              "fg_1",      "body"),
    "danger":    ("danger_fill", "danger_fill", "danger_fill_hover", "danger_fill_hover", "danger_fill_hover", "on_accent", "body_strong"),
    # Toggle "on" look for segmented / selected buttons
    "selected":  ("bg_5",        "border_4",    "bg_5",              "border_4",          "bg_5",              "fg_0",      "body_strong"),
    # Quiet accent text button (links in cards)
    "link":      (None,          None,          "bg_3",              "bg_3",              "bg_4",              "accent_hi", "body"),
}
BUTTON_ALIASES = {
    "ghost": "secondary", "default": "secondary", "success": "primary",
    "go": "primary", "warn": "secondary", "backport": "secondary",
    "purple": "secondary", "success_done": "secondary",
    "danger_ghost": "secondary", "danger_filled": "danger",
}
_BUTTON_SIZES = {"md": 32, "sm": 28, "lg": 36}


def _c(tok, surface):
    return surface if tok is None else COLORS.get(tok, tok)


def button_style(variant="secondary", size="md", surface=None, square=False):
    """Create (once) and return the ttk style name for a button."""
    variant = BUTTON_ALIASES.get(variant, variant)
    if variant not in BUTTON_VARIANTS:
        variant = "secondary"
    surface = surface or COLORS["bg_1"]
    name = "XB%s%s%s.%s.TButton" % (variant.capitalize(), size.capitalize(),
                                      "Sq" if square else "", _skey(surface))
    root = _ROOT[0]
    if root is None:
        return "TButton"
    st = ttk.Style(root)
    if name in _CREATED:
        return name
    fill, border, hfill, hborder, pfill, fg, ftok = BUTTON_VARIANTS[variant]
    hpx = px(_BUTTON_SIZES.get(size, 32))
    m = px(_FOCUS_MARGIN)
    r = px(4)
    w = h = hpx + 2 * m
    sf = surface
    ring = COLORS["focus"]

    def img(fl, bd, rg=None):
        return rounded_image(w, h, r, _c(fl, sf), sf, _c(bd, sf), bw=1,
                             ring=rg, ring_w=px(2), margin=m)

    dis_fill = None if variant in ("subtle", "link") else (
        "bg_2" if sf != COLORS["bg_2"] else "bg_3")
    dis_border = None if variant in ("subtle", "link") else "border_2"
    e = "XB.%s.%s%s.%s.border" % (variant, size, "sq" if square else "", _skey(sf))
    try:
        st.element_create(
            e, "image", img(fill, border),
            ("disabled", img(dis_fill, dis_border)),
            ("pressed", img(pfill, hborder)),
            ("focus", "active", img(hfill, hborder, ring)),
            ("active", img(hfill, hborder)),
            ("focus", img(fill, border, ring)),
            border=m + r + 1, padding=0, sticky="nsew")
    except tk.TclError:
        pass
    st.layout(name, [(e, {"sticky": "nsew", "children": [
        ("Button.padding", {"sticky": "nsew", "children": [
            ("Button.label", {"sticky": "nsew"})]})]})])
    try:
        from tkinter import font as tkfont
        line = tkfont.Font(root=root, font=FONTS[ftok]).metrics("linespace")
    except Exception:
        line = px(18)
    total = hpx + 2 * m
    if square:
        pad_v = pad_h = max(0, (total - px(16)) // 2)
    else:
        pad_v = max(0, (total - max(line, px(16))) // 2)
        pad_h = m + px(12 if size != "sm" else 10)
    st.configure(name, foreground=COLORS[fg], background=sf,
                 font=FONTS[ftok], padding=(pad_h, pad_v, pad_h,
                                            total - pad_v - max(line, px(16)) if not square else pad_v),
                 anchor="center", relief="flat", borderwidth=0,
                 focusthickness=0, width=0)
    st.map(name, foreground=[("disabled", COLORS["fg_5"])],
           background=[("!disabled", sf)])
    _CREATED.add(name)
    return name


_CREATED = set()


# ────────────────────────────────────────────────────────────────────
# FIELD STYLES (entry / combobox / spinbox), per surface
# ────────────────────────────────────────────────────────────────────
def entry_style(surface=None, kind="TEntry"):
    """ttk style for Entry/Combobox/Spinbox fields on `surface`."""
    surface = surface or COLORS["bg_1"]
    name = "XB.%s.%s" % (_skey(surface), kind)
    root = _ROOT[0]
    if root is None:
        return kind
    if name in _CREATED:
        return name
    st = ttk.Style(root)
    hpx = px(32)
    r = px(4)
    sf = surface
    C = COLORS

    def img(bd, fl="field_bg", bw=1):
        return rounded_image(hpx, hpx, r, C[fl], sf, C[bd], bw=bw)

    base = "Entry" if kind == "TEntry" else kind[1:]
    e = "XB.field.%s.%s" % (_skey(sf), base)
    try:
        st.element_create(
            e, "image", img("border_control"),
            ("disabled", img("border_3", "bg_2" if sf != C["bg_2"] else "bg_1")),
            ("invalid", img("danger", bw=px(2))),
            ("focus", img("focus", bw=px(2))),
            ("hover", img("fg_4")),
            border=r + px(2), padding=(px(12), px(6)), sticky="nsew")
    except tk.TclError:
        pass
    if kind == "TEntry":
        layout = [(e, {"sticky": "nsew", "children": [
            ("Entry.padding", {"sticky": "nsew", "children": [
                ("Entry.textarea", {"sticky": "nsew"})]})]})]
    elif kind == "TCombobox":
        arrow = "XB.arrow.%s" % _skey(sf)
        try:
            from ui.icons import icon_image
            st.element_create(
                arrow, "image",
                icon_image("chevron_down", 16, C["fg_3"], C["field_bg"]),
                ("disabled", icon_image("chevron_down", 16, C["fg_5"],
                                        C["bg_2"] if sf != C["bg_2"] else C["bg_1"])),
                sticky="", padding=(px(4), 0, 0, 0))
        except Exception:
            arrow = "Combobox.downarrow"
        layout = [(e, {"sticky": "nsew", "children": [
            (arrow, {"side": "right", "sticky": "ns"}),
            ("Combobox.padding", {"sticky": "nsew", "children": [
                ("Combobox.textarea", {"sticky": "nsew"})]})]})]
    else:  # TSpinbox
        layout = [(e, {"sticky": "nsew", "children": [
            ("Spinbox.uparrow", {"side": "top", "sticky": "e"}),
            ("Spinbox.downarrow", {"side": "bottom", "sticky": "e"}),
            ("Spinbox.padding", {"sticky": "nsew", "children": [
                ("Spinbox.textarea", {"sticky": "nsew"})]})]})]
    st.layout(name, layout)
    st.configure(name, foreground=C["field_fg"], fieldbackground=C["field_bg"],
                 background=sf, insertcolor=C["fg_0"],
                 selectbackground=C["field_select_bg"],
                 selectforeground=C["field_select_fg"],
                 font=FONTS["body"], padding=0, arrowcolor=C["fg_3"],
                 arrowsize=px(12))
    st.map(name,
           foreground=[("disabled", C["fg_5"])],
           fieldbackground=[("readonly", C["field_bg"]),
                            ("disabled", C["bg_2"])],
           selectbackground=[("readonly", "!focus", C["field_bg"]),
                             ("readonly", "focus", C["field_bg"])],
           selectforeground=[("readonly", C["field_fg"])])
    _CREATED.add(name)
    return name


# ────────────────────────────────────────────────────────────────────
# CHECKBOX / RADIO STYLES, per surface
# ────────────────────────────────────────────────────────────────────
def _check_images(sf, radio=False):
    from PIL import Image, ImageDraw, ImageTk
    C = COLORS
    box = px(18)
    m = px(3)
    size = box + 2 * m
    out = {}
    states = {
        "off":       ("field_bg", "border_control", None),
        "off_hover": ("bg_3",     "fg_4",           None),
        "on":        ("accent",   "accent",         "on_accent"),
        "on_hover":  ("accent_hover", "accent_hover", "on_accent"),
        "dis":       ("bg_2" if sf != C["bg_2"] else "bg_1", "border_3", None),
        "dis_on":    ("bg_5",     "bg_5",           "fg_5"),
        "alt":       ("accent",   "accent",         "on_accent"),
    }
    for nm, (fl, bd, mark) in states.items():
        for focus in (False, True):
            k = ("chk", radio, sf, nm, focus, box)
            if k in _IMAGES:
                out[(nm, focus)] = _IMAGES[k]
                continue
            sc = 4
            S = size * sc
            im = Image.new("RGB", (S, S), _rgb(sf))
            d = ImageDraw.Draw(im)
            M = m * sc
            if focus:
                if radio:
                    d.ellipse((0, 0, S - 1, S - 1), fill=_rgb(C["focus"]))
                    i = px(2) * sc
                    d.ellipse((i, i, S - 1 - i, S - 1 - i), fill=_rgb(sf))
                else:
                    d.rounded_rectangle((0, 0, S - 1, S - 1), (px(4) + m) * sc,
                                        fill=_rgb(C["focus"]))
                    i = px(2) * sc
                    d.rounded_rectangle((i, i, S - 1 - i, S - 1 - i),
                                        (px(4) + m) * sc - i, fill=_rgb(sf))
            bx = (M, M, S - 1 - M, S - 1 - M)
            b = max(1, px(1)) * sc
            if radio:
                d.ellipse(bx, fill=_rgb(C[bd]))
                d.ellipse((bx[0] + b, bx[1] + b, bx[2] - b, bx[3] - b),
                          fill=_rgb(C[fl]))
                if mark and nm != "alt":
                    c = S / 2
                    rr = box * sc * 0.22
                    d.ellipse((c - rr, c - rr, c + rr, c + rr), fill=_rgb(C[mark]))
            else:
                rad = px(4) * sc
                d.rounded_rectangle(bx, rad, fill=_rgb(C[bd]))
                d.rounded_rectangle((bx[0] + b, bx[1] + b, bx[2] - b, bx[3] - b),
                                    max(0, rad - b), fill=_rgb(C[fl]))
                if mark:
                    lw = max(2, int(px(2) * sc * 0.9))
                    if nm == "alt":
                        y = S / 2
                        d.line((S * 0.33, y, S * 0.67, y), fill=_rgb(C[mark]), width=lw)
                    else:
                        pts = [(S * 0.32, S * 0.52), (S * 0.45, S * 0.64),
                               (S * 0.69, S * 0.38)]
                        d.line(pts, fill=_rgb(C[mark]), width=lw, joint="curve")
            img = ImageTk.PhotoImage(im.resize((size, size), Image.LANCZOS),
                                     master=_ROOT[0])
            _IMAGES[k] = img
            out[(nm, focus)] = img
    return out


def check_style(surface=None, radio=False):
    """ttk style for a Checkbutton (or Radiobutton) on `surface`."""
    surface = surface or COLORS["bg_1"]
    kind = "TRadiobutton" if radio else "TCheckbutton"
    name = "XB.%s.%s" % (_skey(surface), kind)
    root = _ROOT[0]
    if root is None:
        return kind
    if name in _CREATED:
        return name
    st = ttk.Style(root)
    I = _check_images(surface, radio)
    e = "XB.ind.%s.%s" % (_skey(surface), "r" if radio else "c")
    try:
        st.element_create(
            e, "image", I[("off", False)],
            ("disabled", "selected", I[("dis_on", False)]),
            ("disabled", I[("dis", False)]),
            ("alternate", I[("alt", False)]),
            ("selected", "focus", I[("on", True)]),
            ("selected", "active", I[("on_hover", False)]),
            ("selected", I[("on", False)]),
            ("focus", I[("off", True)]),
            ("active", I[("off_hover", False)]),
            sticky="")
    except tk.TclError:
        pass
    base = "Radiobutton" if radio else "Checkbutton"
    st.layout(name, [(base + ".padding", {"sticky": "nsew", "children": [
        (e, {"side": "left", "sticky": ""}),
        (base + ".label", {"side": "left", "sticky": "w"})]})])
    st.configure(name, background=surface, foreground=COLORS["fg_1"],
                 font=FONTS["body"], padding=(0, px(2), px(4), px(2)),
                 focusthickness=0, indicatormargin=0)
    st.map(name, foreground=[("disabled", COLORS["fg_5"])],
           background=[("active", surface)])
    _CREATED.add(name)
    return name


# ────────────────────────────────────────────────────────────────────
# SCROLLBAR / PROGRESS STYLES, per surface
# ────────────────────────────────────────────────────────────────────
def scrollbar_style(surface=None, orient="vertical"):
    surface = surface or COLORS["bg_1"]
    O = "Vertical" if orient == "vertical" else "Horizontal"
    name = "XB.%s.%s.TScrollbar" % (_skey(surface), O)
    root = _ROOT[0]
    if root is None or name in _CREATED:
        return name if root else O + ".TScrollbar"
    st = ttk.Style(root)
    t = px(10)
    L = px(40)
    w, h = (t, L) if O == "Vertical" else (L, t)
    th = rounded_image(w, h, px(3), COLORS["bg_4"], surface, margin=px(2),
                       key=("sb", O, surface, "n"))
    tha = rounded_image(w, h, px(3), COLORS["bg_5"], surface, margin=px(2),
                        key=("sb", O, surface, "a"))
    tr = rounded_image(w, h, 0, surface, surface, key=("sbt", O, surface))
    e = "XB%s.%s.Scrollbar.thumb" % (_skey(surface), O)
    et = "XB%s.%s.Scrollbar.trough" % (_skey(surface), O)
    try:
        st.element_create(e, "image", th, ("pressed", tha), ("active", tha),
                          border=px(5), sticky="nsew")
        st.element_create(et, "image", tr, border=0, sticky="nsew")
    except tk.TclError:
        pass
    st.layout(name, [(et, {"sticky": "nsew", "children": [
        (e, {"expand": "1", "sticky": "nsew"})]})])
    st.configure(name, width=t, arrowsize=0)
    _CREATED.add(name)
    return name


def progress_style(surface=None, tone="accent", thickness=6):
    surface = surface or COLORS["bg_2"]
    name = "XB%s%d.%s.Horizontal.TProgressbar" % (tone.capitalize(), thickness,
                                                   _skey(surface))
    root = _ROOT[0]
    if root is None or name in _CREATED:
        return name if root else "Horizontal.TProgressbar"
    st = ttk.Style(root)
    t = px(thickness)
    fill = COLORS.get(tone, COLORS["accent"])
    tr = rounded_image(px(24), t, px(2), COLORS["bg_5"], surface,
                       key=("pbt", surface, t))
    pb = rounded_image(px(24), t, px(2), fill, COLORS["bg_5"],
                       key=("pbb", fill, t))
    e1 = "XB%s%d.Horizontal.Progressbar.trough" % (_skey(surface), t)
    e2 = "XB%s%d.Horizontal.Progressbar.pbar" % (fill.lstrip("#"), t)
    cap = px(3)
    try:
        st.element_create(e1, "image", tr, border=(cap, 0, cap, 0),
                          padding=0, sticky="nsew")
    except tk.TclError:
        pass
    try:
        st.element_create(e2, "image", pb, border=(cap, 0, cap, 0),
                          padding=0, sticky="nsew")
    except tk.TclError:
        pass
    st.layout(name, [(e1, {"sticky": "nsew", "children": [
        (e2, {"side": "left", "sticky": "ns"})]})])
    st.configure(name, thickness=t, background=surface)
    _CREATED.add(name)
    return name


# ────────────────────────────────────────────────────────────────────
# THEME APPLICATION
# ────────────────────────────────────────────────────────────────────
def apply_theme(root: tk.Tk) -> ttk.Style:
    """Apply the design system to `root`. Returns the ttk.Style."""
    global SCALE
    if _ROOT[0] is not None and _ROOT[0] is not root:
        # A new Tk interpreter: images and element styles made for the
        # previous one do not exist in it, so rebuild them on demand.
        _IMAGES.clear()
        _CREATED.clear()
        try:
            from ui import icons as _icons
            _icons._CACHE.clear()
        except Exception:
            pass
    _ROOT[0] = root
    try:
        SCALE = max(1.0, round(root.winfo_fpixels("1i") / 96.0 * 4) / 4)
    except Exception:
        SCALE = 1.0
    _resolve_fonts(root)
    C = COLORS
    F = FONTS

    style = ttk.Style(root)
    style.theme_use("clam")

    # ── tk option defaults (non-ttk widgets) ──
    root.configure(bg=C["bg_1"])
    root.option_add("*background",         C["bg_1"])
    root.option_add("*foreground",         C["fg_1"])
    root.option_add("*Font",               F["body"])
    root.option_add("*selectBackground",   C["field_select_bg"])
    root.option_add("*selectForeground",   C["field_select_fg"])
    root.option_add("*insertBackground",   C["fg_0"])
    root.option_add("*highlightThickness", 0)
    root.option_add("*borderWidth",        0)
    root.option_add("*activeBackground",   C["bg_3"])
    root.option_add("*activeForeground",   C["fg_0"])
    root.option_add("*disabledForeground", C["fg_5"])

    root.option_add("*Listbox.background",          C["bg_2"])
    root.option_add("*Listbox.foreground",          C["fg_1"])
    root.option_add("*Listbox.selectBackground",    C["accent_lo"])
    root.option_add("*Listbox.selectForeground",    C["fg_0"])
    root.option_add("*Listbox.borderWidth",         0)
    root.option_add("*Listbox.highlightThickness",  1)
    root.option_add("*Listbox.highlightBackground", C["border_2"])
    root.option_add("*Listbox.highlightColor",      C["focus"])
    root.option_add("*Listbox.activeStyle",         "none")

    root.option_add("*Text.background",       C["bg_0"])
    root.option_add("*Text.foreground",       C["fg_1"])
    root.option_add("*Text.insertBackground", C["fg_0"])
    root.option_add("*Text.borderWidth",      0)
    root.option_add("*Text.highlightThickness", 0)
    root.option_add("*Text.font",             F["code_sm"])
    root.option_add("*Text.padX",             px(10))
    root.option_add("*Text.padY",             px(8))
    root.option_add("*Text.selectBackground", C["accent_lo"])

    root.option_add("*Menu.background",          C["bg_3"])
    root.option_add("*Menu.foreground",          C["fg_1"])
    root.option_add("*Menu.activeBackground",    C["bg_4"])
    root.option_add("*Menu.activeForeground",    C["fg_0"])
    root.option_add("*Menu.borderWidth",         1)
    root.option_add("*Menu.activeBorderWidth",   0)
    root.option_add("*Menu.relief",              "flat")
    root.option_add("*Menu.font",                F["body"])

    root.option_add("*Canvas.background",         C["bg_1"])
    root.option_add("*Canvas.highlightThickness", 0)

    root.option_add("*TCombobox*Listbox.background",       C["bg_3"])
    root.option_add("*TCombobox*Listbox.foreground",       C["fg_1"])
    root.option_add("*TCombobox*Listbox.selectBackground", C["bg_5"])
    root.option_add("*TCombobox*Listbox.selectForeground", C["fg_0"])
    root.option_add("*TCombobox*Listbox.borderWidth",      0)
    root.option_add("*TCombobox*Listbox.font",             F["body"])

    # ── ttk base ──
    style.configure(".",
                    background=C["bg_1"], foreground=C["fg_1"],
                    fieldbackground=C["field_bg"],
                    bordercolor=C["border_2"], lightcolor=C["bg_1"],
                    darkcolor=C["bg_1"], troughcolor=C["bg_5"],
                    focuscolor=C["focus"],
                    selectbackground=C["field_select_bg"],
                    selectforeground=C["field_select_fg"],
                    insertcolor=C["fg_0"], font=F["body"])

    style.configure("TFrame",         background=C["bg_1"])
    style.configure("Surface.TFrame", background=C["bg_1"])
    style.configure("Card.TFrame", background=C["bg_2"], relief="solid",
                    borderwidth=1, bordercolor=C["border_2"])
    style.configure("Panel.TFrame", background=C["bg_0"], relief="solid",
                    borderwidth=1, bordercolor=C["border_2"])

    style.configure("TLabelframe", background=C["bg_2"],
                    bordercolor=C["border_2"], relief="solid", borderwidth=1)
    style.configure("TLabelframe.Label", background=C["bg_2"],
                    foreground=C["fg_0"], font=F["subtitle"])
    style.configure("Card.TLabelframe", background=C["bg_2"],
                    bordercolor=C["border_2"], borderwidth=1, relief="solid",
                    padding=px(16))
    style.configure("Card.TLabelframe.Label", background=C["bg_2"],
                    foreground=C["fg_0"], font=F["subtitle"])

    style.configure("TLabel", background=C["bg_1"], foreground=C["fg_1"],
                    font=F["body"])
    for nm, fg, f in (("H1", "fg_0", "display"), ("H2", "fg_0", "title"),
                      ("H3", "fg_0", "subtitle"), ("Eyebrow", "fg_4", "overline"),
                      ("Body", "fg_1", "body"), ("Muted", "fg_3", "body"),
                      ("Meta", "fg_4", "caption"), ("Mono", "fg_2", "code"),
                      ("Accent", "accent_hi", "body_strong"),
                      ("Success", "success_hi", "body"),
                      ("Warn", "warn_hi", "body"),
                      ("Danger", "danger_hi", "body")):
        style.configure(nm + ".TLabel", foreground=C[fg], font=F[f])
    style.configure("OnCard.TLabel", background=C["bg_2"], foreground=C["fg_1"])
    style.configure("OnCard.H3.TLabel", background=C["bg_2"],
                    foreground=C["fg_0"], font=F["subtitle"])
    style.configure("OnCard.Muted.TLabel", background=C["bg_2"],
                    foreground=C["fg_3"])

    # ── Buttons: legacy style names map onto the design-system variants ──
    for legacy, variant in (("TButton", "secondary"),
                            ("Primary.TButton", "primary"),
                            ("Secondary.TButton", "secondary"),
                            ("Ghost.TButton", "subtle"),
                            ("Warn.TButton", "secondary"),
                            ("Danger.TButton", "danger"),
                            ("Success.TButton", "primary"),
                            ("Backport.TButton", "secondary")):
        src = button_style(variant, "md", C["bg_1"])
        style.layout(legacy, style.layout(src))
        style.configure(legacy, **{k: v for k, v in
                                   style.configure(src).items()})
        style.map(legacy, foreground=[("disabled", C["fg_5"])])

    # ── Check / radio ──
    for legacy, radio, sf in (("TCheckbutton", False, C["bg_1"]),
                              ("OnCard.TCheckbutton", False, C["bg_2"]),
                              ("TRadiobutton", True, C["bg_1"]),
                              ("OnCard.TRadiobutton", True, C["bg_2"])):
        src = check_style(sf, radio)
        style.layout(legacy, style.layout(src))
        style.configure(legacy, **style.configure(src))
        style.map(legacy, foreground=[("disabled", C["fg_5"])],
                  background=[("active", sf)])

    # ── Fields ──
    for legacy, kind in (("TEntry", "TEntry"), ("Dark.TEntry", "TEntry"),
                         ("Field.TEntry", "TEntry"),
                         ("TCombobox", "TCombobox"),
                         ("Field.TCombobox", "TCombobox"),
                         ("TSpinbox", "TSpinbox")):
        src = entry_style(C["bg_1"], kind)
        style.layout(legacy, style.layout(src))
        style.configure(legacy, **style.configure(src))
        style.map(legacy, **{k: style.map(src, k) for k in
                             ("foreground", "fieldbackground",
                              "selectbackground", "selectforeground")})

    # ── Notebook (any ttk.Notebook still in use) ──
    style.configure("TNotebook", background=C["bg_1"], borderwidth=0,
                    tabmargins=(0, 0, 0, 0))
    style.configure("TNotebook.Tab", background=C["bg_1"],
                    foreground=C["fg_3"], bordercolor=C["bg_1"],
                    lightcolor=C["bg_1"], darkcolor=C["bg_1"],
                    padding=(px(12), px(8)), font=F["body"], borderwidth=0)
    style.map("TNotebook.Tab",
              background=[("selected", C["bg_1"]), ("active", C["bg_2"])],
              foreground=[("selected", C["fg_0"]), ("active", C["fg_1"])],
              font=[("selected", F["body_strong"])])
    style.layout("TNotebook.Tab", [
        ("Notebook.tab", {"sticky": "nswe", "children": [
            ("Notebook.padding", {"side": "top", "sticky": "nswe", "children": [
                ("Notebook.label", {"side": "top", "sticky": ""})]})]})])
    style.layout("Hidden.TNotebook.Tab", [])
    style.configure("Hidden.TNotebook", background=C["bg_1"], borderwidth=0)

    # ── Progress bars ──
    for legacy, tone, sf in (("Horizontal.TProgressbar", "accent", C["bg_2"]),
                             ("Build.Horizontal.TProgressbar", "accent", C["bg_2"]),
                             ("Success.Horizontal.TProgressbar", "success", C["bg_2"])):
        src = progress_style(sf, tone)
        style.layout(legacy, style.layout(src))
        style.configure(legacy, **style.configure(src))

    # ── Treeview ──
    style.configure("Treeview", background=C["bg_2"],
                    fieldbackground=C["bg_2"], foreground=C["fg_1"],
                    bordercolor=C["bg_2"], lightcolor=C["bg_2"],
                    darkcolor=C["bg_2"], rowheight=px(36), font=F["body"],
                    borderwidth=0)
    style.map("Treeview",
              background=[("selected", C["accent_lo"])],
              foreground=[("selected", C["fg_0"])])
    style.configure("Treeview.Heading", background=C["bg_2"],
                    foreground=C["fg_2"], relief="flat", borderwidth=0,
                    bordercolor=C["border_2"], lightcolor=C["bg_2"],
                    darkcolor=C["border_2"],
                    font=F["caption_strong"], padding=(px(12), px(8)))
    style.map("Treeview.Heading",
              background=[("active", C["bg_3"])],
              foreground=[("active", C["fg_0"])])
    style.layout("Treeview", [("Treeview.treearea", {"sticky": "nswe"})])
    style.configure("Library.Treeview", rowheight=px(72), background=C["bg_2"])
    style.configure("Compact.Treeview", rowheight=px(28))

    # ── Scrollbars ──
    for O in ("Vertical", "Horizontal"):
        src = scrollbar_style(C["bg_1"], O.lower())
        style.layout(O + ".TScrollbar", style.layout(src))
        style.configure(O + ".TScrollbar", **style.configure(src))
    style.configure("TScrollbar", troughcolor=C["bg_1"], background=C["bg_4"],
                    bordercolor=C["bg_1"], arrowcolor=C["fg_4"],
                    relief="flat", borderwidth=0)

    # ── Separator / scale ──
    style.configure("TSeparator", background=C["border_2"])
    style.configure("Hairline.TSeparator", background=C["border_2"])
    style.configure("TScale", background=C["bg_1"], troughcolor=C["bg_5"],
                    bordercolor=C["bg_1"], lightcolor=C["accent"],
                    darkcolor=C["accent"])

    # Windows: dark title bars for the root and every Toplevel.
    try:
        _dark_title_bar(root)
        root.bind_class("Toplevel", "<Map>",
                        lambda e: _dark_title_bar(e.widget), add="+")
    except Exception:
        pass
    return style


def _dark_title_bar(win):
    """Ask DWM for a dark caption (Windows 10 2004+ / 11). No-op elsewhere."""
    if not _IS_WIN:
        return
    try:
        if getattr(win, "_xb_dark_done", False):
            return
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        val = ctypes.c_int(1)
        for attr in (20, 19):   # DWMWA_USE_IMMERSIVE_DARK_MODE (+ pre-20H1)
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attr, ctypes.byref(val), ctypes.sizeof(val)) == 0:
                break
        # Windows 11: caption colour = window chrome (COLORREF is 0x00BBGGRR)
        r, g, b = _rgb(COLORS["bg_0"])
        cref = ctypes.c_int(r | (g << 8) | (b << 16))
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 35, ctypes.byref(cref), ctypes.sizeof(cref))
        win._xb_dark_done = True
    except Exception:
        pass


# ────────────────────────────────────────────────────────────────────
# CONVENIENCE FACTORIES (kept for compatibility)
# ────────────────────────────────────────────────────────────────────
def make_card(parent: tk.Widget, **kwargs) -> ttk.Frame:
    """A bordered card surface (bg_2)."""
    return ttk.Frame(parent, style="Card.TFrame", padding=16, **kwargs)


def make_callout(parent: tk.Widget, kind: str = "info", text: str = "") -> tk.Frame:
    """An inline info bar (info / warn / danger / success)."""
    try:
        from ui.shared.widgets import InfoBar
        return InfoBar(parent, kind=kind, text=text)
    except Exception:
        tone = {"info": "accent_lo", "warn": "warn_bg", "danger": "danger_bg",
                "success": "success_bg"}.get(kind, "accent_lo")
        f = tk.Frame(parent, bg=COLORS[tone], padx=16, pady=12)
        if text:
            tk.Label(f, text=text, bg=COLORS[tone], fg=COLORS["fg_1"],
                     font=FONTS["body"], anchor="w", justify="left",
                     wraplength=560).pack(anchor="w")
        return f


def make_status_pill(parent: tk.Widget, kind: str, text: str) -> tk.Label:
    """A status badge (ok / active / wait / warn / fail)."""
    palette = {
        "ok":     (COLORS["success_bg"], COLORS["success_hi"]),
        "active": (COLORS["accent_lo"],  COLORS["accent_hi"]),
        "wait":   (COLORS["bg_3"],       COLORS["fg_2"]),
        "warn":   (COLORS["warn_bg"],    COLORS["warn_hi"]),
        "fail":   (COLORS["danger_bg"],  COLORS["danger_hi"]),
    }
    bg, fg = palette.get(kind, palette["wait"])
    return tk.Label(parent, text=text, bg=bg, fg=fg,
                    font=FONTS["caption_strong"], padx=8, pady=1, bd=0)


def make_titlebar(parent: tk.Widget, title: str, version: str = "") -> ttk.Frame:
    """Legacy header strip: product name + version in type."""
    bar = tk.Frame(parent, bg=COLORS["bg_0"], padx=16, pady=12)
    bar.pack(fill="x")
    tk.Label(bar, text=title, bg=COLORS["bg_0"], fg=COLORS["fg_0"],
             font=FONTS["body_strong"]).pack(side="left")
    if version:
        tk.Label(bar, text="v" + version, bg=COLORS["bg_0"], fg=COLORS["fg_3"],
                 font=FONTS["caption"]).pack(side="left", padx=(8, 0))
    return bar


# ────────────────────────────────────────────────────────────────────
# TOKEN TABLES (used by ui/shared components)
# ────────────────────────────────────────────────────────────────────
ELEVATION = {
    "flat":    {"bg": "bg_1", "border": "border_1"},
    "raised":  {"bg": "bg_2", "border": "border_2"},
    "overlay": {"bg": "bg_3", "border": "border_3"},
    "popover": {"bg": "bg_3", "border": "border_3"},
}

BORDERS = {
    "hairline": {"color": "border_2", "width": 1},
    "card":     {"color": "border_2", "width": 1},
    "strong":   {"color": "border_3", "width": 1},
    "accent":   {"color": "accent",   "width": 1},
    "focus":    {"color": "focus",    "width": 2},
}

# Status word -> (text, background, border) token names.
STATUS_COLORS = {
    "idle":     {"fg": "fg_2",       "bg": "bg_3",       "border": "border_3"},
    "waiting":  {"fg": "fg_2",       "bg": "bg_3",       "border": "border_3"},
    "queued":   {"fg": "fg_2",       "bg": "bg_3",       "border": "border_3"},
    "running":  {"fg": "accent_hi",  "bg": "accent_lo",  "border": "accent"},
    "active":   {"fg": "accent_hi",  "bg": "accent_lo",  "border": "accent"},
    "progress": {"fg": "accent_hi",  "bg": "accent_lo",  "border": "accent"},
    "info":     {"fg": "accent_hi",  "bg": "accent_lo",  "border": "accent"},
    "success":  {"fg": "success_hi", "bg": "success_bg", "border": "success"},
    "done":     {"fg": "success_hi", "bg": "success_bg", "border": "success"},
    "ok":       {"fg": "success_hi", "bg": "success_bg", "border": "success"},
    "warn":     {"fg": "warn_hi",    "bg": "warn_bg",    "border": "warn"},
    "warning":  {"fg": "warn_hi",    "bg": "warn_bg",    "border": "warn"},
    "error":    {"fg": "danger_hi",  "bg": "danger_bg",  "border": "danger"},
    "failed":   {"fg": "danger_hi",  "bg": "danger_bg",  "border": "danger"},
    "fail":     {"fg": "danger_hi",  "bg": "danger_bg",  "border": "danger"},
}

BUTTON_STYLES = {
    "default":   "TButton",
    "primary":   "Primary.TButton",
    "secondary": "Secondary.TButton",
    "ghost":     "Ghost.TButton",
    "warn":      "Warn.TButton",
    "danger":    "Danger.TButton",
    "success":   "Success.TButton",
    "backport":  "Backport.TButton",
    "purple":    "Backport.TButton",
}

CARD_STYLES = {
    "default": {"bg": "bg_2", "border": "border_2", "pad": SPACING["lg"]},
    "flat":    {"bg": "bg_1", "border": "border_2", "pad": SPACING["md"]},
    "raised":  {"bg": "bg_3", "border": "border_3", "pad": SPACING["lg"]},
    "inset":   {"bg": "bg_0", "border": "border_2", "pad": SPACING["md"]},
    "accent":  {"bg": "accent_08", "border": "accent", "pad": SPACING["lg"]},
}

INPUT_STYLES = {
    "default": {"bg": "field_bg", "fg": "field_fg", "border": "border_control",
                "focus": "focus", "ttk_entry": "TEntry"},
    "dark":    {"bg": "field_bg", "fg": "field_fg", "border": "border_control",
                "focus": "focus", "ttk_entry": "TEntry"},
}

LOG_COLORS = {
    "bg":       "bg_0",
    "default":  "fg_1",
    "dim":      "fg_4",
    "info":     "accent_hi",
    "success":  "success_hi",
    "warn":     "warn_hi",
    "error":    "danger_hi",
    "command":  "accent_hi",
    "phosphor": "success_hi",
}


# ── SAFE HELPER FUNCTIONS ────────────────────────────────────────────
def get_color(name, fallback=None):
    """Resolve a color token to its hex value (never raises)."""
    try:
        if name in COLORS:
            return COLORS[name]
    except Exception:
        pass
    if fallback is not None:
        return COLORS.get(fallback, fallback)
    return COLORS["fg_1"]


def get_font(name, fallback=None):
    """Resolve a font token to its (family, size, weight) tuple."""
    try:
        if name in FONTS:
            return FONTS[name]
    except Exception:
        pass
    if fallback is not None:
        if isinstance(fallback, str) and fallback in FONTS:
            return FONTS[fallback]
        return fallback
    return FONTS["body"]


def get_status_color(status, role="fg"):
    """Resolve a STATUS_COLORS entry to a hex value."""
    entry = STATUS_COLORS.get(str(status).lower(), STATUS_COLORS["idle"])
    return get_color(entry.get(role, entry.get("fg")))


def apply_card_style(widget, variant="default"):
    """Style an existing tk.Frame as a card surface."""
    spec = CARD_STYLES.get(variant, CARD_STYLES["default"])
    try:
        widget.configure(bg=get_color(spec["bg"]),
                         highlightbackground=get_color(spec["border"]),
                         highlightcolor=get_color(spec["border"]),
                         highlightthickness=1, bd=0)
    except Exception:
        pass
    return widget


def apply_button_style(widget, variant="default"):
    """Apply a button variant to a ttk.Button (or approximate on tk.Button)."""
    try:
        if isinstance(widget, ttk.Widget):
            widget.configure(style=button_style(
                variant, "md", surface_of(widget.master)))
            return widget
    except Exception:
        pass
    v = BUTTON_VARIANTS.get(BUTTON_ALIASES.get(variant, variant),
                            BUTTON_VARIANTS["secondary"])
    sf = surface_of(widget.master)
    try:
        widget.configure(bg=_c(v[0], sf), fg=COLORS[v[5]],
                         activebackground=_c(v[2], sf),
                         activeforeground=COLORS[v[5]], relief="flat", bd=0,
                         font=FONTS[v[6]], highlightthickness=0)
    except Exception:
        pass
    return widget


def apply_input_style(widget, variant="default"):
    """Style an input widget (tk.Entry / ttk.Entry / tk.Text)."""
    try:
        if isinstance(widget, ttk.Widget):
            widget.configure(style=entry_style(surface_of(widget.master)))
            return widget
    except Exception:
        pass
    try:
        widget.configure(bg=COLORS["field_bg"], fg=COLORS["field_fg"],
                         insertbackground=COLORS["fg_0"],
                         highlightbackground=COLORS["border_control"],
                         highlightcolor=COLORS["focus"], highlightthickness=1,
                         relief="flat", bd=0)
    except Exception:
        pass
    return widget


def apply_status_badge_style(widget, status):
    """Style a tk.Label as a status badge."""
    try:
        widget.configure(bg=get_status_color(status, "bg"),
                         fg=get_status_color(status, "fg"),
                         font=get_font("caption_strong"),
                         padx=SPACING["sm"], pady=1, bd=0,
                         highlightthickness=0)
    except Exception:
        pass
    return widget


def tokens_selftest():
    """Return True if all token tables resolve to real colours."""
    ok = True
    for tbl in (CARD_STYLES, INPUT_STYLES):
        for spec in tbl.values():
            for key in ("bg", "border"):
                if key in spec and spec[key] not in COLORS:
                    print("MISSING color token:", spec[key]); ok = False
    for spec in STATUS_COLORS.values():
        for role in ("fg", "bg", "border"):
            if spec[role] not in COLORS:
                print("MISSING status token:", spec[role]); ok = False
    for tok in LOG_COLORS.values():
        if tok not in COLORS:
            print("MISSING log token:", tok); ok = False
    return ok
