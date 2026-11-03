"""
ui/shared/modernize.py — replace legacy emoji on labels with real icons.

The interface used to draw its icons as emoji inside label text
("📂 Open folder", a 56pt "🎮" placeholder). Emoji render differently on
every Windows build and font fallback. `modernize(widget)` walks a widget
tree and, for every tk.Label (and leftover tk.Button / Checkbutton) whose
text starts with a pictographic emoji, shows the matching icon from
ui.icons instead — sized from the label's font and tinted with its
foreground on its background. Text symbols that render fine as type
(✓ ✗ ● arrows) are left alone.

Converted labels are patched so later `.config(text=…)` / fg / bg changes
re-render the right icon, so dynamic status labels stay correct.
"""
import tkinter as tk
import tkinter.font as tkfont

from ui.icons import EMOJI_ICON, icon_image, strip_glyphs

_PICTO_EXTRA = set('⚙✏⚠⚡⏏⏸⏹⏻ℹ'
                   '❓❤☕⌨⏱⏳✂➕⬇'
                   '⌂⏭⏎')


def _picto(text):
    """Icon name if `text` starts with a pictographic emoji, else None."""
    if not isinstance(text, str):
        return None
    t = text.lstrip()
    if not t:
        return None
    ch = t[0]
    if ord(ch) >= 0x1F000 or ch in _PICTO_EXTRA:
        return EMOJI_ICON.get(ch, 'circle')
    return None


def _font_px(w):
    try:
        f = tkfont.Font(font=w.cget('font'))
        size = f.actual('size')
        return max(12, min(48, int(abs(size) * (1.33 if size > 0 else 1))))
    except Exception:
        return 16


def _apply(w, text=None):
    if text is None:
        text = w._xb_orig_cget('text')
    name = _picto(text)
    if not name:
        return False
    rest = strip_glyphs(text)
    try:
        fg = w._xb_orig_cget('fg') or w._xb_orig_cget('foreground')
        bg = w._xb_orig_cget('bg') or w._xb_orig_cget('background')
    except Exception:
        return False
    if not (isinstance(fg, str) and fg.startswith('#')):
        from tkinter_theme import COLORS
        fg = COLORS['fg_3']
    if not (isinstance(bg, str) and bg.startswith('#')):
        from tkinter_theme import surface_of
        bg = surface_of(w)
    sz = _font_px(w)
    if rest.strip():
        sz = min(sz + 2, 20)
    img = icon_image(name, sz, fg, bg)
    w._xb_orig_config(image=img, compound='left' if rest.strip() else 'none',
                      text=(' ' + rest) if rest.strip() else '')
    return True


def _patch(w):
    if getattr(w, '_xb_modern', False):
        return
    w._xb_modern = True
    w._xb_orig_config = w.configure
    w._xb_orig_cget = w.cget

    def configure(cnf=None, **kw):
        if cnf:
            kw.update(cnf)
        if not kw:
            return w._xb_orig_config()
        txt = kw.get('text')
        res = w._xb_orig_config(**kw)
        if txt is not None or 'fg' in kw or 'bg' in kw or 'foreground' in kw \
                or 'background' in kw:
            cur = txt if txt is not None else getattr(w, '_xb_src', '')
            if _picto(cur):
                w._xb_src = cur
                _apply(w, cur)
            elif txt is not None:
                w._xb_src = txt
                try:
                    if w._xb_orig_cget('image'):
                        w._xb_orig_config(image='', compound='none')
                except Exception:
                    pass
        return res

    def cget(key):
        if key == 'text' and getattr(w, '_xb_src', None) is not None:
            return w._xb_src
        return w._xb_orig_cget(key)

    w.configure = configure
    w.config = configure
    w.cget = cget


def modernize(root):
    """Walk `root` and convert emoji-led labels to icon + text."""
    stack = [root]
    n = 0
    while stack:
        w = stack.pop()
        try:
            stack.extend(w.winfo_children())
        except Exception:
            continue
        if getattr(w, '_xb_modern', False):
            continue
        try:
            cls = w.winfo_class()
        except Exception:
            continue
        if cls not in ('Label', 'Button', 'Checkbutton', 'Radiobutton'):
            continue
        try:
            if w.cget('image') or w.cget('textvariable'):
                continue
            text = w.cget('text')
        except Exception:
            continue
        if not _picto(text):
            continue
        _patch(w)
        w._xb_src = text
        try:
            if _apply(w, text):
                n += 1
        except Exception:
            pass
    return n
