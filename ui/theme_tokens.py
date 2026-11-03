"""
ui/theme_tokens.py — v4.0 Design System semantic layer
======================================================

This module is the code side of `exFAT_Image_Builder_v4_Design_System.md`. It
does NOT define new colours. It maps the design system's *role names*
(accent.primary, bg.card, text.secondary, the status-chip set, the button
variants, the spacing scale, the type roles) onto the palette the app already
ships in `tkinter_theme.COLORS` / `FONTS`.

Why a mapping and not a copy:
  * The live "Dark Purple" theme is the single source of truth for hex. Copying
    values here would let them drift. Referencing the same COLORS dict means
    these tokens follow any theme switch automatically.
  * Screens can start speaking the design-system vocabulary today
    (theme_tokens.PRIMARY, SPACE['lg'], chip('done'), button('go')) without
    anyone re-picking a colour.

This is ADDITIVE and imports nothing new — it reuses the app's own theme import,
so adding the file cannot change any existing screen. Wiring screens to use it
is done in later, individually testable batches (see the v4 release plan).

Role -> existing key mapping (verified against tkinter_theme.COLORS):
  bg.app        bg_1        bg.sunken   bg_0        bg.card   bg_3   bg.raised bg_4
  text.primary  fg_0        text.secondary fg_3     text.disabled fg_5
  border.subtle border_2    border.strong border_4
  accent.primary accent (#b07ad6 purple)   accent.go success (#4ec9a4)
  accent.danger  danger     accent.warning warn     accent.info teal
Chip fills reuse the theme's own *_bg slots (success_bg / warn_bg / danger_bg /
teal_bg / accent_15) so chips match the palette exactly.
"""

from tkinter_theme import COLORS, FONTS

# ---------------------------------------------------------------------------
# Colour roles
# ---------------------------------------------------------------------------

BG_APP = COLORS["bg_1"]
BG_SUNKEN = COLORS["bg_0"]
BG_CARD = COLORS["bg_3"]
BG_RAISED = COLORS["bg_4"]

TEXT_PRIMARY = COLORS["fg_0"]
TEXT_SECONDARY = COLORS["fg_3"]
TEXT_DISABLED = COLORS["fg_5"]
TEXT_ON_ACCENT = COLORS["field_select_fg"]

BORDER_SUBTLE = COLORS["border_2"]
BORDER_STRONG = COLORS["border_4"]
FOCUS_RING = COLORS["accent"]

# Accent roles
PRIMARY = COLORS["accent"]
PRIMARY_HOVER = COLORS["accent_hi"]
PRIMARY_ACTIVE = COLORS["accent_pressed"]
GO = COLORS["success"]
GO_HOVER = COLORS["success_hi"]
DANGER = COLORS["danger"]
DANGER_HOVER = COLORS["danger_hi"]
WARNING = COLORS["warn"]
INFO = COLORS["teal"]

# ---------------------------------------------------------------------------
# Status chips
# ---------------------------------------------------------------------------

CHIP = {
    "queued": {"fg": COLORS["fg_3"], "bg": COLORS["bg_4"]},
    "working": {"fg": COLORS["teal_hi"], "bg": COLORS["teal_bg"]},
    "done": {"fg": COLORS["success_hi"], "bg": COLORS["success_bg"]},
    "failed": {"fg": COLORS["danger_hi"], "bg": COLORS["danger_bg"]},
    "skipped": {"fg": COLORS["warn_hi"], "bg": COLORS["warn_bg"]},
    "warning": {"fg": COLORS["warn_hi"], "bg": COLORS["warn_bg"]},
}


def chip(state):
    """Return {'fg','bg'} for a status-chip state; falls back to 'queued'."""
    return CHIP.get(state, CHIP["queued"])


# ---------------------------------------------------------------------------
# Buttons
# ---------------------------------------------------------------------------

BUTTON = {
    "go": {"bg": GO, "fg": TEXT_ON_ACCENT,
           "hover": GO_HOVER, "active": GO_HOVER, "border": None},
    "primary": {"bg": PRIMARY, "fg": TEXT_ON_ACCENT,
                "hover": PRIMARY_HOVER, "active": PRIMARY_ACTIVE, "border": None},
    "ghost": {"bg": None, "fg": TEXT_PRIMARY,
              "hover": BG_RAISED, "active": BG_RAISED, "border": BORDER_SUBTLE},
    "danger_ghost": {"bg": None, "fg": DANGER,
                     "hover": COLORS["danger_bg"], "active": COLORS["danger_bg"],
                     "border": DANGER},
    "danger_filled": {"bg": DANGER, "fg": TEXT_ON_ACCENT,
                      "hover": DANGER_HOVER, "active": DANGER_HOVER, "border": None},
    "disabled": {"bg": BG_RAISED, "fg": TEXT_DISABLED,
                 "hover": BG_RAISED, "active": BG_RAISED, "border": BORDER_SUBTLE},
}

BUTTON_H_STANDARD = 36
BUTTON_H_LARGE = 44
BUTTON_H_SMALL = 28


def button(variant):
    """Return the token bundle for a button variant; falls back to 'ghost'."""
    return BUTTON.get(variant, BUTTON["ghost"])


# ---------------------------------------------------------------------------
# Spacing / radius
# ---------------------------------------------------------------------------

SPACE = {"xs": 4, "sm": 8, "md": 12, "lg": 16, "xl": 24, "2xl": 32}
CARD_PAD = SPACE["lg"]
PAGE_GUTTER = SPACE["xl"]
SECTION_GAP = SPACE["2xl"]

# Corner radii (px). Tk can't round rectangles natively; these are the
# design-system values for any canvas-drawn surfaces.
RADIUS = {"card": 10, "input": 6, "button": 6, "chip": 999}

# ---------------------------------------------------------------------------
# Type roles
# ---------------------------------------------------------------------------

FONT_DISPLAY = FONTS["h1"]
FONT_TITLE = FONTS["h2"]
FONT_SUBTITLE = FONTS["meta"]
FONT_SECTION = FONTS["eyebrow"]
FONT_BODY = FONTS["body"]
FONT_BODY_STRONG = FONTS["body_b"]
FONT_SMALL = FONTS["meta"]
FONT_MONO = FONTS["mono"]
FONT_MONO_SM = FONTS["mono_sm"]

FONT = {
    "display": FONT_DISPLAY, "title": FONT_TITLE, "subtitle": FONT_SUBTITLE,
    "section": FONT_SECTION, "body": FONT_BODY, "body_strong": FONT_BODY_STRONG,
    "small": FONT_SMALL, "mono": FONT_MONO, "mono_sm": FONT_MONO_SM,
}

# ---------------------------------------------------------------------------
# Vocabulary (canonical verbs / labels from the design system)
# ---------------------------------------------------------------------------

TERMS = {
    "build": "Build", "extract": "Extract", "convert": "Convert",
    "send": "Send to PS5", "scan": "Scan", "validate": "Validate",
    "cancel": "Cancel", "source_folder": "Source folder",
    "output_name": "Output name",
}


__all__ = [
    "BG_APP", "BG_SUNKEN", "BG_CARD", "BG_RAISED",
    "TEXT_PRIMARY", "TEXT_SECONDARY", "TEXT_DISABLED", "TEXT_ON_ACCENT",
    "BORDER_SUBTLE", "BORDER_STRONG", "FOCUS_RING",
    "PRIMARY", "PRIMARY_HOVER", "PRIMARY_ACTIVE", "GO", "GO_HOVER",
    "DANGER", "DANGER_HOVER", "WARNING", "INFO",
    "CHIP", "chip", "BUTTON", "button",
    "BUTTON_H_STANDARD", "BUTTON_H_LARGE", "BUTTON_H_SMALL",
    "SPACE", "CARD_PAD", "PAGE_GUTTER", "SECTION_GAP", "RADIUS",
    "FONT", "FONT_DISPLAY", "FONT_TITLE", "FONT_SUBTITLE", "FONT_SECTION",
    "FONT_BODY", "FONT_BODY_STRONG", "FONT_SMALL", "FONT_MONO", "FONT_MONO_SM",
    "TERMS",
]
