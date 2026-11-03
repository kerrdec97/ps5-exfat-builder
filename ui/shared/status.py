"""
ui/shared/status.py — status-bar text bound to an existing StringVar.

The app's status variables carry legacy decoration ("🎮 PS5  ●  Online
(192.168.1.40)"). StatusText follows such a variable and shows a coloured
8px dot plus clean text ("PS5  Online (192.168.1.40)"); the dot colour is
derived from the wording. The variable itself is never changed, so every
existing writer keeps working.
"""
import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS, px, rounded_image
from ui.icons import strip_glyphs

C = COLORS

_GOOD = ('online', 'connected', 'complete', 'ready', 'done', 'success', '✓')
_BUSY = ('connecting', 'uploading', 'sending', 'working', 'cancelling',
         'running', 'building')
_BAD = ('failed', 'error', 'offline', 'cancelled', 'refused', 'timeout',
        'timed out')


def tone_for(text):
    t = (text or '').lower()
    if 'not connected' in t or 'no ip' in t:
        return 'idle'
    if any(w in t for w in _BAD):
        return 'danger' if ('failed' in t or 'error' in t) else 'idle'
    if any(w in t for w in _GOOD):
        return 'success'
    if any(w in t for w in _BUSY):
        return 'info'
    return 'idle'


def clean(text):
    t = strip_glyphs(text or '')
    t = t.replace('●', ' ').replace('✓', '').replace('✗', '')
    t = strip_glyphs(t)
    return re.sub(r'\s{2,}', '  ', t).strip()


class StatusText(tk.Frame):
    def __init__(self, master, variable, bg=None, prefix=None, cursor='',
                 font=None):
        bg = bg or C['bg_0']
        super().__init__(master, bg=bg, cursor=cursor)
        self._bg = bg
        self.var = variable
        self.prefix = prefix
        self._dot = tk.Label(self, bg=bg, bd=0, cursor=cursor)
        self._dot.pack(side='left', padx=(0, px(6)))
        self._lbl = tk.Label(self, bg=bg, fg=C['fg_3'],
                             font=font or FONTS['caption'], cursor=cursor)
        self._lbl.pack(side='left')
        try:
            variable.trace_add('write', lambda *a: self._refresh())
        except Exception:
            pass
        self._refresh()

    def bind(self, seq=None, func=None, add=None):
        super().bind(seq, func, add)
        self._dot.bind(seq, func, add)
        return self._lbl.bind(seq, func, add)

    def config(self, cnf=None, **kw):
        # Legacy writers call .config(fg=...) on the old Label; the colour is
        # derived from the text now, so fg is accepted and ignored.
        if cnf:
            kw.update(cnf)
        kw.pop('fg', None)
        kw.pop('foreground', None)
        if kw:
            return super().configure(**kw)

    configure = config

    def _refresh(self):
        try:
            raw = self.var.get()
        except Exception:
            return
        tone = tone_for(raw)
        col = {'success': C['success'], 'info': C['accent_hi'],
               'danger': C['danger'], 'idle': C['fg_5']}[tone]
        text = clean(raw)
        if self.prefix and not text.upper().startswith(self.prefix.upper()):
            text = self.prefix + '  ' + text
        self._dot.configure(image=rounded_image(px(8), px(8), px(4), col,
                                                self._bg))
        self._lbl.configure(text=text,
                            fg=C['fg_2'] if tone != 'idle' else C['fg_3'])
