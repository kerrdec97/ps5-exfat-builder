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
    "bg_0":     "#090b0f",
    "bg_1":     "#0e1116",
    "bg_2":     "#151920",
    "bg_3":     "#1b212a",
    "bg_4":     "#232b36",
    "bg_5":     "#2d3744",

    # Borders — slight purple tint
    "border_1": "#171c23",
    "border_2": "#242c36",
    "border_3": "#35414f",
    "border_4": "#465568",

    # Foregrounds — warm-leaning grays for contrast against purple-bg
    "fg_0":     "#f5f7fa",
    "fg_1":     "#e3e8ef",
    "fg_2":     "#c8d0da",
    "fg_3":     "#a7b1bf",
    "fg_4":     "#8f9aa8",
    "fg_5":     "#75808e",
    "fg_6":     "#4e5967",

    # Dark fields (Step 35 / v2.2.16): the app's text inputs are now
    # dark — matches the rest of the theme and prevents white-bg
    # entries from screaming against the dark UI. The previous light
    # fields (field_bg=#f0eef5, field_fg=#000000) were a deliberate
    # Windows-form-style choice; user feedback rejected that.
    "field_bg": "#11161d",
    "field_fg": "#e3e8ef",
    "field_select_bg": "#6ea8fe",
    "field_select_fg": "#ffffff",

    # Accent — purple is now the global accent (was electric blue).
    # The CSS mock used `--purple` as the primary action color.
    "accent":     "#6ea8fe",
    "accent_hi":  "#9ac4ff",
    "accent_lo":  "#1b3554",
    "accent_08":  "#111d2c",
    "accent_15":  "#182a40",
    "accent_pressed": "#4f86d9",

    # Status — adjusted for the purple-tint background
    # (greens/ambers/reds shifted slightly cooler so they harmonize)
    "success":    "#45c7a5",
    "success_hi": "#72ddc0",
    "success_bg": "#102720",
    "success_ok": "#00ff41",   # terminal phosphor green — klog only;
                                # intentionally retina-burning since
                                # klog is meant to evoke a CRT terminal

    "warn":       "#e1ad56",
    "warn_hi":    "#f0c875",
    "warn_bg":    "#2b210f",

    "danger":     "#df6676",
    "danger_hi":  "#ef8b98",
    "danger_bg":  "#2b141a",

    "info_bg":    "#111d2c",

    # Confidence dots (Dump Rename)
    "conf_clean": "#45c7a5",
    "conf_guess": "#e1ad56",
    "conf_fail":  "#df6676",

    # Purple — same as accent now (kept as separate tokens for clarity
    # when code semantically wants "this means backport" vs "this means
    # primary action")
    "purple":     "#b58cff",
    "purple_hi":  "#c9adff",
    # Teal — kept distinct for ffpkg, shifted to harmonize with purple
    "teal":       "#50c5c7",
    "teal_hi":    "#7ddbdd",
    "teal_bg":    "#102526",
    "teal_lo":    "#204b4d",
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
    "h1":      (_SANS, 24, "bold"),
    "h2":      (_SANS, 16, "bold"),
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
    style.configure("Card.TFrame", background=C["bg_2"], relief="solid",
                    borderwidth=1, bordercolor=C["border_3"])
    style.configure("Panel.TFrame", background=C["bg_3"], relief="solid",
                    borderwidth=1, bordercolor=C["border_3"])

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
                    padding=(16, 9),
                    focuscolor=C["accent"],
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
                    padding=(18, 10), font=F["body_b"], relief="flat",
                    focuscolor=C["fg_0"])
    style.map("Primary.TButton",
              background=[("active", C["accent_hi"]),
                          ("pressed", C["accent_pressed"]),
                          ("disabled", C["bg_3"])],
              foreground=[("disabled", C["fg_6"])],
              bordercolor=[("focus", C["fg_0"])],
              lightcolor=[("focus", C["fg_0"])],
              darkcolor=[("focus", C["fg_0"])])

    style.configure("Secondary.TButton",
                    background=C["bg_4"], foreground=C["accent"],
                    bordercolor=C["accent"], lightcolor=C["accent"], darkcolor=C["accent"],
                    padding=(16, 9), relief="flat", focuscolor=C["accent_hi"])
    style.map("Secondary.TButton",
              background=[("active", C["accent_08"]), ("pressed", C["accent_15"])],
              bordercolor=[("focus", C["accent_hi"])],
              lightcolor=[("focus", C["accent_hi"])],
              darkcolor=[("focus", C["accent_hi"])])

    style.configure("Ghost.TButton",
                    background=C["bg_1"], foreground=C["fg_3"],
                    bordercolor=C["bg_1"], lightcolor=C["bg_1"], darkcolor=C["bg_1"],
                    padding=(13, 8), relief="flat", focuscolor=C["accent"])
    style.map("Ghost.TButton",
              background=[("active", C["bg_3"]), ("pressed", C["bg_4"])],
              foreground=[("active", C["fg_1"])],
              bordercolor=[("focus", C["accent"])],
              lightcolor=[("focus", C["accent"])],
              darkcolor=[("focus", C["accent"])])

    style.configure("Warn.TButton",
                    background=C["warn"], foreground="#1a0e00",
                    bordercolor=C["warn"], lightcolor=C["warn"], darkcolor=C["warn"],
                    padding=(16, 9), font=F["body_b"], relief="flat",
                    focuscolor=C["fg_0"])
    style.map("Warn.TButton",
              background=[("active", C["warn_hi"]), ("pressed", "#cc8800")],
              bordercolor=[("focus", C["fg_0"])],
              lightcolor=[("focus", C["fg_0"])],
              darkcolor=[("focus", C["fg_0"])])

    style.configure("Danger.TButton",
                    background=C["danger"], foreground=C["fg_0"],
                    bordercolor=C["danger"], lightcolor=C["danger"], darkcolor=C["danger"],
                    padding=(16, 9), font=F["body_b"], relief="flat",
                    focuscolor=C["fg_0"])
    style.map("Danger.TButton",
              background=[("active", C["danger_hi"]), ("pressed", "#c62828")],
              bordercolor=[("focus", C["fg_0"])],
              lightcolor=[("focus", C["fg_0"])],
              darkcolor=[("focus", C["fg_0"])])

    style.configure("Success.TButton",
                    background=C["success"], foreground=C["fg_0"],
                    bordercolor=C["success"], lightcolor=C["success"], darkcolor=C["success"],
                    padding=(16, 9), font=F["body_b"], relief="flat",
                    focuscolor=C["fg_0"])
    style.map("Success.TButton",
              background=[("active", C["success_hi"]), ("pressed", "#388e3c")],
              bordercolor=[("focus", C["fg_0"])],
              lightcolor=[("focus", C["fg_0"])],
              darkcolor=[("focus", C["fg_0"])])

    style.configure("Backport.TButton",
                    background=C["purple"], foreground=C["fg_0"],
                    bordercolor=C["purple"], lightcolor=C["purple"], darkcolor=C["purple"],
                    padding=(16, 9), font=F["body_b"], relief="flat",
                    focuscolor=C["fg_0"])
    style.map("Backport.TButton",
              background=[("active", C["purple_hi"]), ("pressed", "#7d3c98")],
              bordercolor=[("focus", C["fg_0"])],
              lightcolor=[("focus", C["fg_0"])],
              darkcolor=[("focus", C["fg_0"])])

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
                    insertcolor=C["accent"], padding=9, relief="flat")
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
                    bordercolor=C["border_3"], padding=9, relief="flat")
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
                    rowheight=32, font=F["body"], borderwidth=0)
    style.map("Treeview",
              background=[("selected", C["accent_15"])],
              foreground=[("selected", C["fg_0"])])
    style.configure("Treeview.Heading",
                    background=C["bg_4"], foreground=C["fg_3"],
                    relief="flat", borderwidth=0,
                    font=(_SANS, 9, "bold"), padding=(12, 9))
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


# ════════════════════════════════════════════════════════════════════
# STAGE 1 — DESIGN-TOKEN SYSTEM  (added 2026-05, UI refactor)
# ════════════════════════════════════════════════════════════════════
# Everything below this banner is ADDITIVE. No constant above this line
# is changed or removed. All existing imports
# (`apply_theme`, `COLORS`, `FONTS`, `make_card`, `make_callout`,
# `make_status_pill`, `make_titlebar`) keep working exactly as before.
#
# These tokens are NOT yet wired into any page. They exist so later
# stages can build reusable components against one source of truth.
# The app launches and behaves identically with this block present.
# ════════════════════════════════════════════════════════════════════

# ── SPACING ──────────────────────────────────────────────────────────
# A 4px base scale. Use SPACING["md"] etc. instead of magic numbers so
# padding stays consistent across the redesigned UI.
SPACING = {
    "none": 0,
    "xxs":  2,
    "xs":   4,
    "sm":   8,
    "md":   12,
    "lg":   16,
    "xl":   20,
    "xxl":  24,
    "xxxl": 32,
    "huge": 48,
}

# ── RADIUS ───────────────────────────────────────────────────────────
# Tkinter widgets cannot truly round their corners; these values are for
# Canvas-drawn rounded rectangles in later component work. Kept here so
# the "roundedness" of the UI is defined in one place.
RADIUS = {
    "none": 0,
    "sm":   4,
    "md":   8,
    "lg":   12,
    "xl":   16,
    "pill": 999,
}

# ── ELEVATION / BORDER STYLES ────────────────────────────────────────
# Tk has no drop-shadow; "elevation" here is expressed as a background +
# border pairing. Each level names a (background-token, border-token).
ELEVATION = {
    "flat":    {"bg": "bg_1", "border": "border_1"},
    "raised":  {"bg": "bg_2", "border": "border_2"},
    "overlay": {"bg": "bg_3", "border": "border_3"},
    "popover": {"bg": "bg_4", "border": "border_4"},
}

BORDERS = {
    "hairline":  {"color": "border_2", "width": 1},
    "card":      {"color": "border_3", "width": 1},
    "strong":    {"color": "border_4", "width": 1},
    "accent":    {"color": "accent",   "width": 1},
    "focus":     {"color": "accent",   "width": 2},
}

# ── SEMANTIC / STATUS COLORS ─────────────────────────────────────────
# Maps an app-level status word to a (text, background, border) trio of
# EXISTING color-token names. Used by apply_status_badge_style and by
# later badge components. Values are token names, resolved via get_color.
STATUS_COLORS = {
    "idle":     {"fg": "fg_5",       "bg": "bg_3",        "border": "border_3"},
    "waiting":  {"fg": "fg_5",       "bg": "bg_3",        "border": "border_3"},
    "queued":   {"fg": "fg_4",       "bg": "bg_3",        "border": "border_3"},
    "running":  {"fg": "accent_hi",  "bg": "accent_08",   "border": "accent"},
    "active":   {"fg": "accent_hi",  "bg": "accent_08",   "border": "accent"},
    "progress": {"fg": "accent_hi",  "bg": "accent_08",   "border": "accent"},
    "success":  {"fg": "success_hi", "bg": "success_bg",  "border": "success"},
    "done":     {"fg": "success_hi", "bg": "success_bg",  "border": "success"},
    "ok":       {"fg": "success_hi", "bg": "success_bg",  "border": "success"},
    "warn":     {"fg": "warn_hi",    "bg": "warn_bg",     "border": "warn"},
    "warning":  {"fg": "warn_hi",    "bg": "warn_bg",     "border": "warn"},
    "error":    {"fg": "danger_hi",  "bg": "danger_bg",   "border": "danger"},
    "failed":   {"fg": "danger_hi",  "bg": "danger_bg",   "border": "danger"},
    "fail":     {"fg": "danger_hi",  "bg": "danger_bg",   "border": "danger"},
}

# ── BUTTON STYLES ────────────────────────────────────────────────────
# Maps a variant name to the EXISTING ttk style string already defined
# in apply_theme(). Nothing new is registered with ttk — this is just a
# lookup so components can ask for "primary" without knowing the exact
# ttk style name.
BUTTON_STYLES = {
    "default":   "TButton",
    "primary":   "Primary.TButton",
    "secondary": "Secondary.TButton",
    "ghost":     "Ghost.TButton",
    "warn":      "Warn.TButton",
    "danger":    "Danger.TButton",
    "success":   "Success.TButton",
    "backport":  "Backport.TButton",
    "purple":    "Backport.TButton",   # alias — purple == backport accent
}

# ── CARD STYLES ──────────────────────────────────────────────────────
# Named card surfaces → (background-token, border-token, default padding).
CARD_STYLES = {
    "default":  {"bg": "bg_2", "border": "border_3", "pad": SPACING["lg"]},
    "flat":     {"bg": "bg_1", "border": "border_2", "pad": SPACING["md"]},
    "raised":   {"bg": "bg_3", "border": "border_3", "pad": SPACING["lg"]},
    "inset":    {"bg": "bg_0", "border": "border_2", "pad": SPACING["md"]},
    "accent":   {"bg": "bg_2", "border": "accent",   "pad": SPACING["lg"]},
}

# ── INPUT STYLES ─────────────────────────────────────────────────────
# Named input surfaces → token set + the matching ttk style for ttk
# entries/combos. Raw tk widgets use the color tokens directly.
INPUT_STYLES = {
    "default": {"bg": "field_bg", "fg": "field_fg", "border": "border_3",
                "focus": "accent", "ttk_entry": "TEntry"},
    "dark":    {"bg": "bg_4",     "fg": "fg_1",      "border": "border_3",
                "focus": "accent", "ttk_entry": "Dark.TEntry"},
}

# ── LOG COLORS ───────────────────────────────────────────────────────
# Colors for live-log / console text by line severity. Resolved to hex
# via get_color. Used by the log-view component in a later stage.
LOG_COLORS = {
    "bg":       "bg_0",
    "default":  "fg_1",
    "dim":      "fg_4",
    "info":     "accent_hi",
    "success":  "success_hi",
    "warn":     "warn_hi",
    "error":    "danger_hi",
    "command":  "teal_hi",
    "phosphor": "success_ok",   # klog CRT-green
}


# ── SAFE HELPER FUNCTIONS ────────────────────────────────────────────
# All helpers are defensive: bad input never raises, it returns a
# sensible fallback. Wiring them into a widget is harmless because the
# worst case is a no-op.

def get_color(name, fallback=None):
    """Resolve a color token to its hex value.

    `name` may be a key in COLORS or in STATUS_COLORS. Unknown names
    return `fallback` (or COLORS['fg_1'] if no fallback given). Never
    raises.
    """
    try:
        if name in COLORS:
            return COLORS[name]
    except Exception:
        pass
    if fallback is not None:
        # fallback may itself be a token name — resolve one level.
        return COLORS.get(fallback, fallback)
    return COLORS.get("fg_1", "#e0dcec")


def get_font(name, fallback=None):
    """Resolve a font token to its (family, size, weight) tuple.

    Unknown names return `fallback` if it is a known token, else the
    fallback tuple as-is, else FONTS['body']. Never raises.
    """
    try:
        if name in FONTS:
            return FONTS[name]
    except Exception:
        pass
    if fallback is not None:
        if isinstance(fallback, str) and fallback in FONTS:
            return FONTS[fallback]
        return fallback
    return FONTS.get("body", (_SANS, 10, "normal"))


def get_status_color(status, role="fg"):
    """Resolve a STATUS_COLORS entry to a hex value.

    `role` is one of 'fg', 'bg', 'border'. Unknown status falls back to
    'idle'. Never raises.
    """
    entry = STATUS_COLORS.get(str(status).lower(), STATUS_COLORS["idle"])
    token = entry.get(role, entry.get("fg"))
    return get_color(token)


def apply_card_style(widget, variant="default"):
    """Style an existing tk.Frame (or tk.Widget) as a card surface.

    Only applies to plain tk widgets that accept bg/highlight options;
    ttk widgets are left untouched (they should use the 'Card.TFrame'
    ttk style instead). Returns the widget for chaining. Never raises.
    """
    spec = CARD_STYLES.get(variant, CARD_STYLES["default"])
    try:
        widget.configure(
            bg=get_color(spec["bg"]),
            highlightbackground=get_color(spec["border"]),
            highlightcolor=get_color(spec["border"]),
            highlightthickness=1,
            bd=0,
        )
    except Exception:
        # ttk widget or option not supported — no-op, safe.
        pass
    return widget


def apply_button_style(widget, variant="default"):
    """Apply a button variant to a widget.

    For a ttk.Button, sets the matching ttk style string. For a plain
    tk.Button, applies a token-based color approximation. Returns the
    widget. Never raises.
    """
    ttk_style = BUTTON_STYLES.get(variant, BUTTON_STYLES["default"])
    # ttk path
    try:
        if isinstance(widget, ttk.Widget):
            widget.configure(style=ttk_style)
            return widget
    except Exception:
        pass
    # plain tk.Button path — approximate with token colors
    _tk_btn_palette = {
        "default":   ("bg_4",   "fg_1"),
        "primary":   ("accent", "fg_0"),
        "secondary": ("bg_4",   "accent"),
        "ghost":     ("bg_1",   "fg_3"),
        "warn":      ("warn",   "bg_0"),
        "danger":    ("danger", "fg_0"),
        "success":   ("success", "fg_0"),
        "backport":  ("purple", "fg_0"),
        "purple":    ("purple", "fg_0"),
    }
    bg_tok, fg_tok = _tk_btn_palette.get(variant, _tk_btn_palette["default"])
    try:
        widget.configure(
            bg=get_color(bg_tok), fg=get_color(fg_tok),
            activebackground=get_color(bg_tok),
            activeforeground=get_color(fg_tok),
            relief="flat", bd=0, font=get_font("button"),
            highlightthickness=0, cursor="hand2",
        )
    except Exception:
        pass
    return widget


def apply_input_style(widget, variant="default"):
    """Style an input widget (tk.Entry / ttk.Entry / tk.Text).

    ttk entries/combos get the matching ttk style; plain tk inputs get
    token colors. Returns the widget. Never raises.
    """
    spec = INPUT_STYLES.get(variant, INPUT_STYLES["default"])
    # ttk path
    try:
        if isinstance(widget, ttk.Widget):
            widget.configure(style=spec["ttk_entry"])
            return widget
    except Exception:
        pass
    # plain tk path
    try:
        widget.configure(
            bg=get_color(spec["bg"]),
            fg=get_color(spec["fg"]),
            insertbackground=get_color(spec["focus"]),
            highlightbackground=get_color(spec["border"]),
            highlightcolor=get_color(spec["focus"]),
            highlightthickness=1,
            relief="flat", bd=0,
        )
    except Exception:
        pass
    return widget


def apply_status_badge_style(widget, status):
    """Style a tk.Label (or similar) as a status badge.

    Uses STATUS_COLORS to set fg/bg. Intended for plain tk.Label badges.
    Returns the widget. Never raises.
    """
    try:
        widget.configure(
            bg=get_status_color(status, "bg"),
            fg=get_status_color(status, "fg"),
            font=get_font("eyebrow"),
            padx=SPACING["sm"], pady=SPACING["xxs"],
            bd=0, highlightthickness=0,
        )
    except Exception:
        pass
    return widget


# ── DESIGN-TOKEN SELF-TEST ───────────────────────────────────────────
# Optional sanity check; not run on import. Call tokens_selftest() in a
# REPL to confirm every token table resolves cleanly.
def tokens_selftest():
    """Return True if all token tables are internally consistent."""
    ok = True
    for tbl in (CARD_STYLES, INPUT_STYLES):
        for _name, spec in tbl.items():
            for key in ("bg", "border"):
                if key in spec and spec[key] not in COLORS:
                    print("MISSING color token:", spec[key]); ok = False
    for _s, spec in STATUS_COLORS.items():
        for role in ("fg", "bg", "border"):
            if spec[role] not in COLORS:
                print("MISSING status token:", spec[role]); ok = False
    for tok in LOG_COLORS.values():
        if tok not in COLORS:
            print("MISSING log token:", tok); ok = False
    for _v, st in BUTTON_STYLES.items():
        if not isinstance(st, str):
            print("BAD button style:", st); ok = False
    return ok


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
