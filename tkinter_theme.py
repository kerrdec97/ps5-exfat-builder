"""
exFAT Image Builder — tkinter/ttk theme module
==============================================

Drop-in dark theme that mirrors the design system in colors_and_type.css.
The app already uses tkinter + ttk; this module gives every widget the
"professional tools" look without rewriting the rest of the codebase.

USAGE
-----
At the top of `exfat_builder.py`, after creating your root:

    import tkinter as tk
    from tkinter import ttk
    from tkinter_theme import apply_theme, COLORS, FONTS

    root = tk.Tk()
    apply_theme(root)                 # <-- single call themes everything
    root.configure(bg=COLORS["bg_1"])
    root.title("exFAT Image Builder")

After this:
    ttk.Button(root, text="Build All", style="Primary.TButton").pack()
    ttk.Button(root, text="Cancel",    style="Ghost.TButton").pack()
    ttk.Button(root, text="⚠ Backport", style="Warn.TButton").pack()
    ttk.Button(root, text="Delete",    style="Danger.TButton").pack()

STYLE NAMES (use as `style="..."` on a ttk widget)
--------------------------------------------------
Buttons        : Primary.TButton, Secondary.TButton, Ghost.TButton,
                 Warn.TButton, Danger.TButton, Success.TButton
Frames         : Card.TFrame, Panel.TFrame, Surface.TFrame
Labels         : H1.TLabel, H2.TLabel, H3.TLabel, Eyebrow.TLabel,
                 Body.TLabel, Muted.TLabel, Meta.TLabel, Mono.TLabel,
                 Accent.TLabel, Success.TLabel, Warn.TLabel, Danger.TLabel
Entry/Combo    : Field.TEntry, Field.TCombobox  (light-on-dark — matches app)
Notebook tabs  : (default — already themed)
Progress       : Build.Horizontal.TProgressbar
Treeview       : Library.Treeview  (for game grid / queue list)
Scrollbar      : (default — already themed)
LabelFrame     : Card.TLabelframe  (for sectioned cards in Settings/Advanced)
Separator      : Hairline.TSeparator

NON-TTK WIDGETS (tk.Listbox, tk.Text, tk.Canvas) get their defaults set via
root.option_add — colors are picked up automatically.
"""

import tkinter as tk
from tkinter import ttk
import platform

# ────────────────────────────────────────────────────────────────────
# COLOR TOKENS  (1:1 with colors_and_type.css)
# ────────────────────────────────────────────────────────────────────
COLORS = {
    # Backgrounds — purple-tinted dark per v1-bundle/backports-tab.html
    # (deepest is bg_0 = #0a0812; panels lighten through bg_5)
    "bg_0":     "#0a0812",
    "bg_1":     "#110d1c",
    "bg_2":     "#16121f",
    "bg_3":     "#1d1729",
    "bg_4":     "#251e34",
    "bg_5":     "#2d2540",

    # Borders — slight purple tint
    "border_1": "#1c1729",
    "border_2": "#25203a",
    "border_3": "#322a47",
    "border_4": "#3a3252",

    # Foregrounds — warm-leaning grays for contrast against purple-bg
    "fg_0":     "#f3f0fa",   # heading
    "fg_1":     "#e0dcec",
    "fg_2":     "#c5c0d2",
    "fg_3":     "#9d97ad",
    "fg_4":     "#7a7390",
    "fg_5":     "#5d5775",
    "fg_6":     "#423d57",

    # Dark fields (Step 35 / v2.2.16): the app's text inputs are now
    # dark — matches the rest of the theme and prevents white-bg
    # entries from screaming against the dark UI. The previous light
    # fields (field_bg=#f0eef5, field_fg=#000000) were a deliberate
    # Windows-form-style choice; user feedback rejected that.
    "field_bg": "#251e34",
    "field_fg": "#e0dcec",
    "field_select_bg": "#b07ad6",
    "field_select_fg": "#ffffff",

    # Accent — purple is now the global accent (was electric blue).
    # The CSS mock used `--purple` as the primary action color.
    "accent":     "#b07ad6",
    "accent_hi":  "#c993e8",
    "accent_lo":  "#2a1a3d",
    "accent_08":  "#1a0d2a",   # opaque equivalent of purple @ 8% over bg_1
    "accent_15":  "#241540",
    "accent_pressed": "#8e60b8",

    # Status — adjusted for the purple-tint background
    # (greens/ambers/reds shifted slightly cooler so they harmonize)
    "success":    "#4ec9a4",
    "success_hi": "#6fdcb8",
    "success_bg": "#0f2418",
    "success_ok": "#00ff41",   # terminal phosphor green — klog only;
                                # intentionally retina-burning since
                                # klog is meant to evoke a CRT terminal

    "warn":       "#d6a85a",
    "warn_hi":    "#e8c074",
    "warn_bg":    "#241a08",

    "danger":     "#d65a6a",
    "danger_hi":  "#e87a8a",
    "danger_bg":  "#240e14",

    "info_bg":    "#1a0d2a",

    # Confidence dots (Dump Rename)
    "conf_clean": "#4ec9a4",
    "conf_guess": "#d6a85a",
    "conf_fail":  "#d65a6a",

    # Purple — same as accent now (kept as separate tokens for clarity
    # when code semantically wants "this means backport" vs "this means
    # primary action")
    "purple":     "#b07ad6",
    "purple_hi":  "#c993e8",
    # Teal — kept distinct for ffpkg, shifted to harmonize with purple
    "teal":       "#5ac9b8",
    "teal_hi":    "#7adcca",
    "teal_bg":    "#0e221f",
    "teal_lo":    "#1f4a42",
}

# ────────────────────────────────────────────────────────────────────
# FONTS
# ────────────────────────────────────────────────────────────────────
# tkinter takes (family, size, weight) tuples. Sizes here mirror the CSS
# scale (px → tk pt; close enough on Windows where 1pt ≈ 1.33px).
# Font choice: per the v1-bundle/backports-tab.html mock, the design uses
# Inter (sans) and JetBrains Mono (mono). These are not bundled with
# Windows by default, so the user needs to install them — but we list
# Segoe UI / Consolas as fallback so tkinter renders something sensible
# if Inter/JetBrains Mono aren't on the system.
#
# tkinter picks the FIRST family from a comma-separated list at the
# Tk-level, NOT a CSS-style font-stack — so we pass the preferred name
# and rely on tk's automatic substitution if the family is missing.
# On Windows, font registration via tkinter.font.families() can confirm
# whether the install worked.
def _pick_font(preferred, fallback):
    """Return preferred if it's installed, else fallback. Run at theme
    apply time so we can probe the actual system font list."""
    try:
        from tkinter import font as _tkfont
        # Build a temporary root only if no default exists; this is
        # cheap and doesn't display anything.
        try:
            installed = set(_tkfont.families())
        except Exception:
            installed = set()
        if preferred in installed:
            return preferred
    except Exception:
        pass
    return fallback


# Set at module-load with platform-default fallbacks. apply_theme will
# later re-resolve via _pick_font() to use Inter/JetBrains Mono if they're
# actually installed.
_SANS_FALLBACK = "Segoe UI" if platform.system() == "Windows" else (
    ".AppleSystemUIFont" if platform.system() == "Darwin" else "Sans")
_MONO_FALLBACK = "Consolas" if platform.system() == "Windows" else (
    "Menlo" if platform.system() == "Darwin" else "DejaVu Sans Mono")

_SANS = _SANS_FALLBACK
_MONO = _MONO_FALLBACK

FONTS = {
    "h1":      (_SANS, 22, "bold"),
    "h2":      (_SANS, 15, "bold"),
    "h3":      (_SANS, 12, "bold"),
    "eyebrow": (_SANS,  9, "bold"),    # render uppercase manually
    "body":    (_SANS, 10, "normal"),
    "body_b":  (_SANS, 10, "bold"),
    "label":   (_SANS,  9, "normal"),
    "meta":    (_SANS,  8, "normal"),
    "button":  (_SANS, 10, "normal"),
    "tab":     (_SANS, 10, "normal"),
    "mono":    (_MONO, 10, "normal"),
    "mono_sm": (_MONO,  9, "normal"),
}


# ────────────────────────────────────────────────────────────────────
# THEME APPLICATION
# ────────────────────────────────────────────────────────────────────
def apply_theme(root: tk.Tk) -> ttk.Style:
    """Apply the dark theme to `root`. Returns the ttk.Style for further tweaks."""
    # ── Probe for Inter / JetBrains Mono now that we have a Tk root.
    # If installed, replace the family in every FONTS entry so the
    # design-system fonts are used; otherwise fall back to Segoe UI /
    # Consolas (already in place from module load).
    global _SANS, _MONO
    try:
        from tkinter import font as _tkfont
        installed = set(_tkfont.families())
        if "Inter" in installed:
            _SANS = "Inter"
        if "JetBrains Mono" in installed:
            _MONO = "JetBrains Mono"
        # Re-build FONTS with the resolved families
        for key, val in list(FONTS.items()):
            family, size, weight = val
            new_family = _SANS if family == _SANS_FALLBACK else (
                _MONO if family == _MONO_FALLBACK else family)
            FONTS[key] = (new_family, size, weight)
    except Exception:
        pass

    C = COLORS
    F = FONTS

    style = ttk.Style(root)
    # 'clam' is the only built-in theme that fully respects color overrides
    # on Windows. Without this, ttk falls back to the native Windows theme
    # and most of these configure() calls will be ignored.
    style.theme_use("clam")

    # ── tk option defaults (cover non-ttk widgets: Listbox, Text, Menu, Canvas) ──
    root.configure(bg=C["bg_1"])
    root.option_add("*background",        C["bg_1"])
    root.option_add("*foreground",        C["fg_1"])
    root.option_add("*Font",              F["body"])
    root.option_add("*selectBackground",  C["accent"])
    root.option_add("*selectForeground",  C["fg_0"])
    root.option_add("*insertBackground",  C["fg_0"])
    root.option_add("*highlightThickness", 0)
    root.option_add("*borderWidth",        0)

    root.option_add("*Listbox.background",         C["bg_2"])
    root.option_add("*Listbox.foreground",         C["fg_1"])
    root.option_add("*Listbox.selectBackground",   C["accent"])
    root.option_add("*Listbox.selectForeground",   C["fg_0"])
    root.option_add("*Listbox.borderWidth",        0)
    root.option_add("*Listbox.highlightThickness", 1)
    root.option_add("*Listbox.highlightBackground", C["border_2"])
    root.option_add("*Listbox.highlightColor",     C["accent"])
    root.option_add("*Listbox.activeStyle",        "none")

    root.option_add("*Text.background",      C["bg_0"])
    root.option_add("*Text.foreground",      C["fg_1"])
    root.option_add("*Text.insertBackground", C["accent"])
    root.option_add("*Text.borderWidth",     0)
    root.option_add("*Text.highlightThickness", 0)
    root.option_add("*Text.font",            F["mono"])
    root.option_add("*Text.padX",            8)
    root.option_add("*Text.padY",            8)

    root.option_add("*Menu.background",          C["bg_3"])
    root.option_add("*Menu.foreground",          C["fg_1"])
    root.option_add("*Menu.activeBackground",    C["accent_08"])
    root.option_add("*Menu.activeForeground",    C["accent"])
    root.option_add("*Menu.borderWidth",         1)
    root.option_add("*Menu.activeBorderWidth",   0)
    root.option_add("*Menu.relief",              "flat")

    root.option_add("*Canvas.background",       C["bg_1"])
    root.option_add("*Canvas.highlightThickness", 0)

    # ── ttk base ──
    style.configure(".",
                    background=C["bg_1"],
                    foreground=C["fg_1"],
                    fieldbackground=C["field_bg"],
                    bordercolor=C["border_2"],
                    lightcolor=C["border_2"],
                    darkcolor=C["border_2"],
                    troughcolor=C["bg_4"],
                    focuscolor=C["accent"],
                    selectbackground=C["accent"],
                    selectforeground=C["fg_0"],
                    insertcolor=C["fg_0"],
                    font=F["body"])

    # ── TFrame ──
    style.configure("TFrame",         background=C["bg_1"])
    style.configure("Surface.TFrame", background=C["bg_1"])
    style.configure("Card.TFrame",    background=C["bg_2"], relief="flat", borderwidth=1)
    style.configure("Panel.TFrame",   background=C["bg_3"], relief="flat", borderwidth=1)

    # ── TLabelframe (sectioned cards) ──
    style.configure("TLabelframe",
                    background=C["bg_2"],
                    bordercolor=C["border_2"],
                    relief="solid",
                    borderwidth=1)
    style.configure("TLabelframe.Label",
                    background=C["bg_2"],
                    foreground=C["accent"],
                    font=F["h3"])
    style.configure("Card.TLabelframe",
                    background=C["bg_2"],
                    bordercolor=C["border_3"],
                    borderwidth=1, relief="solid", padding=16)
    style.configure("Card.TLabelframe.Label",
                    background=C["bg_2"], foreground=C["accent"], font=F["h3"])

    # ── TLabel variants ──
    style.configure("TLabel",        background=C["bg_1"], foreground=C["fg_1"], font=F["body"])
    style.configure("H1.TLabel",     foreground=C["accent"], font=F["h1"])
    style.configure("H2.TLabel",     foreground=C["fg_0"],   font=F["h2"])
    style.configure("H3.TLabel",     foreground=C["accent"], font=F["h3"])
    style.configure("Eyebrow.TLabel", foreground=C["accent"], font=F["eyebrow"])
    style.configure("Body.TLabel",   foreground=C["fg_1"],   font=F["body"])
    style.configure("Muted.TLabel",  foreground=C["fg_4"],   font=F["body"])
    style.configure("Meta.TLabel",   foreground=C["fg_5"],   font=F["meta"])
    style.configure("Mono.TLabel",   foreground=C["accent"], font=F["mono"], background=C["bg_4"])
    style.configure("Accent.TLabel", foreground=C["accent"], font=F["body_b"])
    style.configure("Success.TLabel", foreground=C["success_hi"], font=F["body"])
    style.configure("Warn.TLabel",   foreground=C["warn_hi"], font=F["body"])
    style.configure("Danger.TLabel", foreground=C["danger_hi"], font=F["body"])
    style.configure("OnCard.TLabel", background=C["bg_2"], foreground=C["fg_1"])
    style.configure("OnCard.H3.TLabel", background=C["bg_2"], foreground=C["accent"], font=F["h3"])
    style.configure("OnCard.Muted.TLabel", background=C["bg_2"], foreground=C["fg_4"])

    # ── TButton — primary ──
    style.configure("TButton",
                    background=C["bg_4"],
                    foreground=C["fg_1"],
                    bordercolor=C["border_3"],
                    lightcolor=C["border_3"],
                    darkcolor=C["border_3"],
                    relief="flat",
                    padding=(14, 7),
                    font=F["button"])
    style.map("TButton",
              background=[("active", C["bg_5"]),
                          ("pressed", C["bg_3"]),
                          ("disabled", C["bg_2"])],
              foreground=[("disabled", C["fg_6"])],
              bordercolor=[("focus", C["accent"]),
                           ("active", C["border_4"])])

    style.configure("Primary.TButton",
                    background=C["accent"], foreground=C["fg_0"],
                    bordercolor=C["accent"], lightcolor=C["accent"], darkcolor=C["accent"],
                    padding=(16, 8), font=F["button"], relief="flat")
    style.map("Primary.TButton",
              background=[("active", C["accent_hi"]),
                          ("pressed", C["accent_pressed"]),
                          ("disabled", C["bg_3"])],
              foreground=[("disabled", C["fg_6"])])

    style.configure("Secondary.TButton",
                    background=C["bg_4"], foreground=C["accent"],
                    bordercolor=C["accent"], lightcolor=C["accent"], darkcolor=C["accent"],
                    padding=(14, 7), relief="flat")
    style.map("Secondary.TButton",
              background=[("active", C["accent_08"]), ("pressed", C["accent_15"])])

    style.configure("Ghost.TButton",
                    background=C["bg_1"], foreground=C["fg_3"],
                    bordercolor=C["bg_1"], lightcolor=C["bg_1"], darkcolor=C["bg_1"],
                    padding=(10, 6), relief="flat")
    style.map("Ghost.TButton",
              background=[("active", C["bg_3"]), ("pressed", C["bg_4"])],
              foreground=[("active", C["fg_1"])])

    style.configure("Warn.TButton",
                    background=C["warn"], foreground="#1a0e00",
                    bordercolor=C["warn"], lightcolor=C["warn"], darkcolor=C["warn"],
                    padding=(14, 7), font=F["body_b"], relief="flat")
    style.map("Warn.TButton",
              background=[("active", C["warn_hi"]), ("pressed", "#cc8800")])

    style.configure("Danger.TButton",
                    background=C["danger"], foreground=C["fg_0"],
                    bordercolor=C["danger"], lightcolor=C["danger"], darkcolor=C["danger"],
                    padding=(14, 7), font=F["body_b"], relief="flat")
    style.map("Danger.TButton",
              background=[("active", C["danger_hi"]), ("pressed", "#c62828")])

    style.configure("Success.TButton",
                    background=C["success"], foreground=C["fg_0"],
                    bordercolor=C["success"], lightcolor=C["success"], darkcolor=C["success"],
                    padding=(14, 7), font=F["body_b"], relief="flat")
    style.map("Success.TButton",
              background=[("active", C["success_hi"]), ("pressed", "#388e3c")])

    style.configure("Backport.TButton",
                    background=C["purple"], foreground=C["fg_0"],
                    bordercolor=C["purple"], lightcolor=C["purple"], darkcolor=C["purple"],
                    padding=(14, 7), font=F["body_b"], relief="flat")
    style.map("Backport.TButton",
              background=[("active", C["purple_hi"]), ("pressed", "#7d3c98")])

    # ── TCheckbutton / TRadiobutton ──
    style.configure("TCheckbutton",
                    background=C["bg_1"], foreground=C["fg_1"],
                    indicatorcolor=C["bg_4"], indicatorbackground=C["bg_4"],
                    focuscolor=C["accent"], font=F["body"])
    style.map("TCheckbutton",
              indicatorcolor=[("selected", C["accent"]), ("pressed", C["accent_lo"])],
              foreground=[("disabled", C["fg_6"])])
    style.configure("OnCard.TCheckbutton", background=C["bg_2"])

    style.configure("TRadiobutton",
                    background=C["bg_1"], foreground=C["fg_1"],
                    indicatorcolor=C["bg_4"], focuscolor=C["accent"])
    style.map("TRadiobutton", indicatorcolor=[("selected", C["accent"])])
    # On-card radio: same as TRadiobutton but with bg_2 background so it
    # blends into Card.TFrame surfaces.
    style.configure("OnCard.TRadiobutton",
                    background=C["bg_2"], foreground=C["fg_1"],
                    indicatorcolor=C["bg_4"], focuscolor=C["accent"])
    style.map("OnCard.TRadiobutton",
              indicatorcolor=[("selected", C["accent"])])

    # ── TEntry ──
    # Note: the app's entries are intentionally light-on-dark — matches Windows-form
    # style. If you want fully-dark entries instead, swap field_bg → bg_4, field_fg → fg_1.
    style.configure("TEntry",
                    fieldbackground=C["field_bg"], foreground=C["field_fg"],
                    bordercolor=C["border_3"], lightcolor=C["border_3"], darkcolor=C["border_3"],
                    insertcolor=C["accent"], padding=6, relief="flat")
    style.map("TEntry",
              bordercolor=[("focus", C["accent"])],
              lightcolor=[("focus", C["accent"])],
              darkcolor=[("focus", C["accent"])])

    style.configure("Dark.TEntry",
                    fieldbackground=C["bg_4"], foreground=C["fg_1"],
                    insertcolor=C["accent"], padding=6, relief="flat",
                    bordercolor=C["border_3"], lightcolor=C["border_3"], darkcolor=C["border_3"])
    style.map("Dark.TEntry", bordercolor=[("focus", C["accent"])])

    # ── TCombobox ──
    style.configure("TCombobox",
                    fieldbackground=C["field_bg"], foreground=C["field_fg"],
                    background=C["bg_4"], arrowcolor=C["fg_2"],
                    bordercolor=C["border_3"], padding=6, relief="flat")
    style.map("TCombobox",
              fieldbackground=[("readonly", C["bg_4"])],
              foreground=[("readonly", C["fg_1"])],
              arrowcolor=[("active", C["accent"])],
              bordercolor=[("focus", C["accent"])])
    # Dropdown listbox must be themed via option_add
    root.option_add("*TCombobox*Listbox.background",       C["bg_3"])
    root.option_add("*TCombobox*Listbox.foreground",       C["fg_1"])
    root.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
    root.option_add("*TCombobox*Listbox.selectForeground", C["fg_0"])
    root.option_add("*TCombobox*Listbox.borderWidth",      0)
    root.option_add("*TCombobox*Listbox.font",             F["body"])

    # ── TNotebook (the main tab strip) ──
    style.configure("TNotebook",
                    background=C["bg_1"], bordercolor=C["bg_1"],
                    borderwidth=0, tabmargins=(8, 8, 8, 0))
    style.configure("TNotebook.Tab",
                    background=C["bg_1"], foreground=C["fg_4"],
                    bordercolor=C["bg_1"], lightcolor=C["bg_1"], darkcolor=C["bg_1"],
                    padding=(16, 9), font=F["tab"])
    style.map("TNotebook.Tab",
              background=[("selected", C["bg_2"]), ("active", C["bg_3"])],
              foreground=[("selected", C["accent"]), ("active", C["fg_1"])],
              bordercolor=[("selected", C["accent"])],
              lightcolor=[("selected", C["accent"])])
    # Custom layout: thicker bottom border on selected tab (the blue underline)
    style.layout("TNotebook.Tab", [
        ("Notebook.tab", {"sticky": "nswe", "children": [
            ("Notebook.padding", {"side": "top", "sticky": "nswe", "children": [
                ("Notebook.label", {"side": "top", "sticky": ""})
            ]})
        ]})
    ])

    # ── Horizontal.TProgressbar ──
    style.configure("Horizontal.TProgressbar",
                    troughcolor=C["bg_4"], background=C["accent"],
                    bordercolor=C["bg_4"], lightcolor=C["accent"], darkcolor=C["accent"],
                    thickness=8)
    style.configure("Build.Horizontal.TProgressbar",
                    troughcolor=C["bg_4"], background=C["accent"],
                    bordercolor=C["bg_4"], lightcolor=C["accent"], darkcolor=C["accent"],
                    thickness=10)
    style.configure("Success.Horizontal.TProgressbar",
                    troughcolor=C["bg_4"], background=C["success"],
                    bordercolor=C["bg_4"], lightcolor=C["success"], darkcolor=C["success"])

    # ── Treeview (game library, queue list) ──
    style.configure("Treeview",
                    background=C["bg_2"], fieldbackground=C["bg_2"],
                    foreground=C["fg_1"], bordercolor=C["bg_2"],
                    rowheight=28, font=F["body"], borderwidth=0)
    style.map("Treeview",
              background=[("selected", C["accent_15"])],
              foreground=[("selected", C["fg_0"])])
    style.configure("Treeview.Heading",
                    background=C["bg_4"], foreground=C["fg_3"],
                    relief="flat", borderwidth=0,
                    font=(_SANS, 9, "bold"), padding=(10, 7))
    style.map("Treeview.Heading",
              background=[("active", C["bg_5"])],
              foreground=[("active", C["accent"])])
    style.layout("Treeview", [("Treeview.treearea", {"sticky": "nswe"})])  # no border

    # Library variant — taller rows for cover art
    style.configure("Library.Treeview", rowheight=72, background=C["bg_2"])

    # ── Scrollbars ──
    style.configure("TScrollbar",
                    background=C["bg_4"], troughcolor=C["bg_1"],
                    bordercolor=C["bg_1"], arrowcolor=C["fg_4"],
                    relief="flat", borderwidth=0)
    style.map("TScrollbar",
              background=[("active", C["bg_5"]), ("pressed", C["accent_lo"])],
              arrowcolor=[("active", C["fg_1"])])
    style.configure("Vertical.TScrollbar", arrowsize=14)
    style.configure("Horizontal.TScrollbar", arrowsize=14)

    # ── Separator ──
    style.configure("TSeparator", background=C["border_2"])
    style.configure("Hairline.TSeparator", background=C["border_2"])

    # ── Spinbox ──
    style.configure("TSpinbox",
                    fieldbackground=C["bg_4"], foreground=C["fg_1"],
                    background=C["bg_4"], arrowcolor=C["fg_3"],
                    bordercolor=C["border_3"], lightcolor=C["border_3"], darkcolor=C["border_3"],
                    insertcolor=C["accent"], padding=6, relief="flat")
    style.map("TSpinbox",
              bordercolor=[("focus", C["accent"])],
              arrowcolor=[("active", C["accent"])])

    # ── Scale (slider) ──
    style.configure("TScale",
                    background=C["bg_1"], troughcolor=C["bg_4"],
                    bordercolor=C["bg_4"], lightcolor=C["accent"], darkcolor=C["accent"])

    return style


# ────────────────────────────────────────────────────────────────────
# CONVENIENCE WIDGET FACTORIES
# Use these instead of raw tk.* widgets for consistent look without
# remembering style names.
# ────────────────────────────────────────────────────────────────────
def make_card(parent: tk.Widget, **kwargs) -> ttk.Frame:
    """A bordered card surface (bg_2)."""
    f = ttk.Frame(parent, style="Card.TFrame", padding=16, **kwargs)
    return f


def make_callout(parent: tk.Widget, kind: str = "info", text: str = "") -> tk.Frame:
    """
    A callout box (info / warn / danger / success) — left-bordered tinted strip.
    Uses raw tk.Frame because ttk doesn't expose a left-only border. Match the
    .info / .warn / .danger / .success classes in the design system.
    """
    palette = {
        "info":    (COLORS["info_bg"],    COLORS["accent"],     "#9ecdff"),
        "warn":    (COLORS["warn_bg"],    COLORS["warn"],       COLORS["warn_hi"]),
        "danger":  (COLORS["danger_bg"],  COLORS["danger"],     "#ff8080"),
        "success": (COLORS["success_bg"], COLORS["success"],    COLORS["success_hi"]),
    }
    bg, accent_color, fg = palette.get(kind, palette["info"])

    wrap = tk.Frame(parent, bg=accent_color)                 # left strip = accent
    inner = tk.Frame(wrap, bg=bg, padx=14, pady=10)
    inner.pack(side="right", fill="both", expand=True, padx=(3, 0))
    if text:
        tk.Label(inner, text=text, bg=bg, fg=fg,
                 font=FONTS["body"], anchor="w", justify="left",
                 wraplength=560).pack(anchor="w")
    return wrap


def make_status_pill(parent: tk.Widget, kind: str, text: str) -> tk.Label:
    """A rounded-feel status pill. tk doesn't do real rounded corners,
    so this is a subtly tinted label — looks correct in the dark UI."""
    palette = {
        "ok":      (COLORS["success_bg"], COLORS["success_hi"]),
        "active":  (COLORS["accent_08"],  COLORS["accent_hi"]),
        "wait":    (COLORS["bg_3"],       COLORS["fg_5"]),
        "warn":    (COLORS["warn_bg"],    COLORS["warn_hi"]),
        "fail":    (COLORS["danger_bg"],  COLORS["danger_hi"]),
    }
    bg, fg = palette.get(kind, palette["wait"])
    return tk.Label(parent, text=f" {text} ", bg=bg, fg=fg,
                    font=(_SANS, 9, "bold"), padx=8, pady=2, bd=0)


def make_titlebar(parent: tk.Widget, title: str, version: str = "") -> ttk.Frame:
    """The app's top header strip: brand + version, accent underline."""
    bar = ttk.Frame(parent, style="Card.TFrame", padding=(20, 14))
    bar.pack(fill="x")
    ttk.Label(bar, text="🎮", font=(_SANS, 18), style="OnCard.TLabel").pack(side="left")
    title_frame = ttk.Frame(bar, style="Card.TFrame")
    title_frame.pack(side="left", padx=(10, 0))
    ttk.Label(title_frame, text=title, style="OnCard.H3.TLabel",
              font=(_SANS, 13, "bold")).pack(anchor="w")
    if version:
        ttk.Label(title_frame, text=f"v{version}",
                  style="OnCard.Muted.TLabel",
                  font=FONTS["meta"]).pack(anchor="w")
    return bar


# ────────────────────────────────────────────────────────────────────
# DEMO — run this file directly to see every styled component
# ────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("980x720")
    root.title("exFAT Image Builder — theme preview")
    apply_theme(root)

    make_titlebar(root, "exFAT Image Builder", "1.9.0")

    main = ttk.Frame(root, style="Surface.TFrame", padding=20)
    main.pack(fill="both", expand=True)

    nb = ttk.Notebook(main)
    nb.pack(fill="both", expand=True)

    for label in ("📀 exFAT", "📦 ffpkg", "📚 Library", "💾 My Images",
                  "📋 Klog", "⚙ Settings"):
        page = ttk.Frame(nb, style="Surface.TFrame", padding=20)
        nb.add(page, text=label)

    page = nb.nametowidget(nb.tabs()[0])

    card = make_card(page); card.pack(fill="x", pady=(0, 12))
    ttk.Label(card, text="Build a new image", style="OnCard.H3.TLabel").pack(anchor="w")
    ttk.Label(card, text="Source folder must contain eboot.bin",
              style="OnCard.Muted.TLabel").pack(anchor="w", pady=(2, 12))

    row = ttk.Frame(card, style="Card.TFrame"); row.pack(fill="x", pady=4)
    ttk.Label(row, text="Game folder", style="OnCard.TLabel", width=14).pack(side="left")
    ttk.Entry(row).pack(side="left", fill="x", expand=True, padx=(0, 8))
    ttk.Button(row, text="Browse…", style="Secondary.TButton").pack(side="left")

    btnrow = ttk.Frame(card, style="Card.TFrame"); btnrow.pack(fill="x", pady=(16, 0))
    ttk.Button(btnrow, text="+ Add to Queue", style="Primary.TButton").pack(side="left", padx=(0, 8))
    ttk.Button(btnrow, text="Build All",      style="Success.TButton").pack(side="left", padx=(0, 8))
    ttk.Button(btnrow, text="⏸ Pause",        style="Secondary.TButton").pack(side="left", padx=(0, 8))
    ttk.Button(btnrow, text="⚠ Backport",     style="Backport.TButton").pack(side="left", padx=(0, 8))
    ttk.Button(btnrow, text="Cancel",         style="Ghost.TButton").pack(side="right")

    make_callout(page, "info",
        "Sector size is locked at 512 bytes — required for PS5 compatibility."
        ).pack(fill="x", pady=4)
    make_callout(page, "warn",
        "Output folder must NOT be inside the game folder."
        ).pack(fill="x", pady=4)
    make_callout(page, "danger",
        "Post-build deletion of source folder cannot be undone."
        ).pack(fill="x", pady=4)
    make_callout(page, "success",
        "Build complete · 12.4 GB · verified."
        ).pack(fill="x", pady=4)

    pb_card = make_card(page); pb_card.pack(fill="x", pady=(12, 0))
    ttk.Label(pb_card, text="Build progress", style="OnCard.H3.TLabel").pack(anchor="w")
    pb = ttk.Progressbar(pb_card, style="Build.Horizontal.TProgressbar",
                         mode="determinate", value=63)
    pb.pack(fill="x", pady=(8, 4))

    pillrow = ttk.Frame(pb_card, style="Card.TFrame"); pillrow.pack(anchor="w", pady=(8, 0))
    for kind, text in (("ok", "Mount"), ("ok", "Format"),
                       ("active", "Copy 63%"), ("wait", "Verify")):
        make_status_pill(pillrow, kind, text).pack(side="left", padx=(0, 6))

    root.mainloop()
