"""
ui/icons.py — the application's icon set.

One consistent line-icon family (Lucide, ISC licence — see
THIRD_PARTY_NOTICES.md) replaces the emoji the interface used to show.
Emoji render differently on every Windows version and font fallback; these
are rendered images, so every user sees exactly the same glyph.

    from ui.icons import icon_image
    img = icon_image('folder', 16, COLORS['fg_3'], COLORS['bg_2'])
    tk.Label(parent, image=img, bg=COLORS['bg_2'])

Images are opaque: the glyph is composited onto `bg`, the colour of the
surface it sits on, so it looks identical on Windows and Linux Tk builds.
Sizes are 96-dpi pixels and follow the display scale. Results are cached,
so asking again for the same icon is free.

``EMOJI_ICON`` maps the emoji used in the old interface to an icon name, and
``strip_glyphs()`` removes a leading emoji/symbol from a label so the text
can sit next to a real icon.
"""
import base64
import io
import re

from ui.icons_data import MASKS

_MASKS = {}
_CACHE = {}

# Emoji / symbol the legacy UI used -> icon name.
EMOJI_ICON = {
    '\U0001f3e0': 'home', '\U0001f6e0': 'build', '\U0001f528': 'build',
    '\U0001f4e4': 'extract', '✏': 'edit', '\U0001f4da': 'library',
    '\U0001f3ae': 'ps5', '\U0001f504': 'convert', '\U0001f9e9': 'backports',
    '⚙': 'settings', '\U0001f4cb': 'history', '\U0001f527': 'wrench',
    'ℹ': 'info', '\U0001f41e': 'bug', '\U0001f41b': 'bug',
    '\U0001f4c1': 'folder', '\U0001f4c2': 'folder_open', '\U0001f4be': 'drive',
    '\U0001f4e6': 'package', '\U0001f5d1': 'trash', '\U0001f50d': 'search',
    '\U0001f50e': 'search', '\U0001f4c4': 'file_text', '\U0001f4e1': 'wifi',
    '\U0001f5c2': 'folders', '\U0001f5dc': 'compress', '⏏': 'eject',
    '\U0001f4dd': 'klog', '\U0001f310': 'globe', '\U0001f4e5': 'download',
    '\U0001f512': 'lock', '\U0001f513': 'unlock', '\U0001f680': 'rocket',
    '\U0001f4bf': 'disc', '\U0001f4c0': 'disc', '\U0001f50c': 'plug',
    '\U0001f3ac': 'video', '❓': 'help', '❤': 'heart',
    '⚠': 'warning', '⚡': 'zap', '✓': 'check', '✔': 'check',
    '✅': 'success', '✗': 'close', '✕': 'close', '❌': 'error',
    '↻': 'refresh', '↺': 'undo', '▶': 'play', '⏸': 'pause',
    '⏹': 'stop', '■': 'stop', '⏻': 'power', '\U0001f4a4': 'sleep',
    '\U0001f514': 'bell', '\U0001f3a8': 'palette', '\U0001f4ca': 'chart',
    '\U0001f4cf': 'ruler', '✂': 'scissors', '\U0001f517': 'link',
    '\U0001f441': 'eye', '\U0001f4a1': 'lightbulb', '\U0001f6e1': 'shield',
    '\U0001f465': 'users', '\U0001f4e2': 'megaphone', '\U0001f4ac': 'message',
    '☕': 'coffee', '⌨': 'keyboard', '\U0001f52c': 'microscope',
    '\U0001f3af': 'target', '\U0001f4dc': 'klog', '⏱': 'timer',
    '⏳': 'loader', '\U0001f5bc': 'picture', '➕': 'plus',
    '⬇': 'download', '↓': 'arrow_down', '↑': 'arrow_up',
    '←': 'arrow_left', '→': 'arrow_right', '↗': 'external',
    '⋮': 'more_v', '⌂': 'home', '\U0001f4bd': 'disc',
    '\U0001f5c3': 'database', '⤓': 'to_line', '↩': 'undo',
    '⏎': 'arrow_left', '⏭': 'skip', '\U0001f30d': 'globe',
}

# A leading emoji / dingbat / arrow plus following spaces (and the variation
# selector U+FE0F) — used to clean legacy labels that carried their icon.
_LEAD = re.compile(
    '^[\\s]*(?:[\U0001F000-\U0001FAFF←-⇿⌀-⏿■-◿'
    '☀-➿⬀-⯿⤓⤵]️?)+[\\s]*')


def strip_glyphs(text):
    """Remove a leading emoji/symbol (and its spacing) from a label."""
    if not isinstance(text, str):
        return text
    return _LEAD.sub('', text)


def leading_icon(text):
    """Icon name for the emoji a legacy label starts with, or None."""
    if not isinstance(text, str):
        return None
    t = text.lstrip()
    if not t:
        return None
    ch = t[0]
    return EMOJI_ICON.get(ch)


def has_icon(name):
    return name in MASKS


def _mask(name):
    m = _MASKS.get(name)
    if m is None:
        from PIL import Image
        data = MASKS.get(name) or MASKS['circle']
        m = Image.open(io.BytesIO(base64.b64decode(''.join(data))))
        m.load()
        _MASKS[name] = m
    return m


def _rgb(c):
    c = c.lstrip('#')
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def icon_pil(name, size, fg, bg):
    """Return a PIL RGB image of the icon (size in device pixels)."""
    from PIL import Image
    m = _mask(name).resize((size, size), Image.LANCZOS)
    base = Image.new('RGB', (size, size), _rgb(bg))
    ink = Image.new('RGB', (size, size), _rgb(fg))
    base.paste(ink, (0, 0), m)
    return base


def icon_image(name, size=16, fg='#9da4ad', bg='#131519', master=None):
    """Return a cached PhotoImage of icon `name` (96-dpi px, scaled)."""
    try:
        from tkinter_theme import px, _ROOT
    except Exception:
        px = (lambda n: n)
        _ROOT = [None]
    dev = max(8, px(size))
    key = (name, dev, fg, bg)
    img = _CACHE.get(key)
    if img is not None:
        return img
    from PIL import ImageTk
    img = ImageTk.PhotoImage(icon_pil(name, dev, fg, bg),
                             master=master or _ROOT[0])
    _CACHE[key] = img
    return img
