"""
ui/shared/page_head.py — shared page helpers (design-system versions).

Public API (signatures unchanged; call sites across the app use them):

    make_themed_button(parent, text, command, kind='primary', icon=None,
                       font_size=10, padx=14, pady=7, state='normal')
        -> ui.shared.widgets.Button. kind maps onto the design-system
        variants: success/primary -> primary, danger -> danger,
        accent/ghost/warn -> secondary. A glyph `icon` becomes a real icon.

    info_banner(parent, text, on_click=None) -> inline info bar.

    page_head(parent, badge_emoji, title_text, subtitle_text)
        -> page header (title style + description). The emoji badge tile
        of the old design is no longer drawn.

    field_block(parent, label_text, var, on_browse=None,
                browse_text='Browse', hint=None)
        -> label (+ hint) above a rounded 32px field with an optional
        Browse button. Packs itself (fill x, 12px above) like before and
        returns the block frame.
"""
import tkinter as tk

from tkinter_theme import COLORS, FONTS, px, surface_of
from ui.shared.widgets import Button, TextField, InfoBar, PageHeader
from ui.icons import EMOJI_ICON, has_icon

_KIND = {
    'primary': 'primary', 'success': 'primary', 'success_done': 'secondary',
    'go': 'primary', 'danger': 'danger', 'accent': 'secondary',
    'ghost': 'secondary', 'warn': 'secondary', 'secondary': 'secondary',
    'subtle': 'subtle',
}
_GLYPH = {'+': 'plus', '▶': 'play', '✕': 'close', '✗': 'close',
          '✓': 'check', '↻': 'refresh', '→': 'arrow_right',
          '←': 'arrow_left', '⏸': 'pause', '…': 'more'}


def _icon_name(icon):
    if not icon:
        return None
    if has_icon(icon):
        return icon
    s = icon.strip()
    return _GLYPH.get(s) or EMOJI_ICON.get(s[:1])


def make_themed_button(parent, text, command, kind='primary',
                       icon=None, font_size=10, padx=14, pady=7,
                       state='normal'):
    size = 'sm' if (font_size <= 9 or pady <= 4) else 'md'
    return Button(parent, text=text, command=command,
                  variant=_KIND.get(kind, 'secondary'), size=size,
                  icon=_icon_name(icon), state=state)


def info_banner(parent, text, on_click=None):
    bar = InfoBar(parent, kind='info', text=text)
    if on_click:
        def _bind(w):
            try:
                w.bind('<Button-1>', lambda e: on_click(), add='+')
                w.configure(cursor='hand2')
            except Exception:
                pass
            for c in w.winfo_children():
                _bind(c)
        _bind(bar)
    return bar


def page_head(parent, badge_emoji, title_text, subtitle_text):
    return PageHeader(parent, title_text, subtitle_text)


def _tr(text):
    """Translate through the app's language table when it is loaded."""
    try:
        from exfat_builder import _ as tr
        return tr(text)
    except Exception:
        return text


def field_block(parent, label_text, var, on_browse=None,
                browse_text='Browse', hint=None):
    bg = surface_of(parent)
    browse_text = _tr(browse_text)
    block = tk.Frame(parent, bg=bg)
    block.pack(fill='x', pady=(px(12), 0))
    lbl_row = tk.Frame(block, bg=bg)
    lbl_row.pack(fill='x')
    tk.Label(lbl_row, text=label_text, font=FONTS['body'], bg=bg,
             fg=COLORS['fg_2'], anchor='w').pack(side='left')
    if hint:
        tk.Label(lbl_row, text=hint, font=FONTS['caption'], bg=bg,
                 fg=COLORS['fg_4'], anchor='w').pack(side='left',
                                                     padx=(px(8), 0))
    row = tk.Frame(block, bg=bg)
    row.pack(fill='x', pady=(px(4), 0))
    entry = TextField(row, textvariable=var, mono=True, surface=bg)
    entry.pack(side='left', fill='x', expand=True)
    block.entry = entry
    if on_browse:
        b = Button(row, text=browse_text + '…' if not browse_text.endswith('…') else browse_text,
                   icon='folder_open', command=on_browse, surface=bg)
        b.pack(side='left', padx=(px(6), 0))
        block.browse = b
    return block
