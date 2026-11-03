"""ui/shared/hero.py — shared "game hero" card.

A reusable hero header mirroring the polished Build-tab hero so the
Extract and Edit workflows read as the same application: cover-art tile
on the left, title + game id beside it, a Build-style stats strip, and a
status badge in the top-right.

Pure presentation. Cover art reuses the existing Build loaders
(`app._load_cover_art(folder)` + `_load_cover_image`) and falls back to a
themed placeholder glyph when no folder/art is available. No extraction,
mount, or packer logic is invoked here.
"""

import os
import threading
import tkinter as tk

from tkinter_theme import COLORS, FONTS

_BADGE_TONE = {'ready': 'success', 'wait': 'neutral', 'busy': 'info',
               'warn': 'warn', 'fail': 'danger', 'error': 'danger'}
_BADGE = _BADGE_TONE   # legacy name


class GameHero(tk.Frame):
    """Source / image summary card (design system › Card): cover tile,
    title, ID, path, a status badge and a row of facts.

    stats: ordered list of (label, key) pairs for the fact cells.
    Public API unchanged: title_var / id_var / path_var, set_title,
    set_path, set_stat, set_badge, reset_cover, set_cover_from_folder,
    set_cover_from_file.
    """

    def __init__(self, parent, stats, cover_glyph='\U0001f3ae',
                 cover_size=150):
        from tkinter_theme import px
        from ui.shared.widgets import Rounded, CoverLabel, Badge, autowrap
        from ui.icons import leading_icon
        super().__init__(parent, bg=COLORS['bg_1'], highlightthickness=0)
        self._cover_size = min(px(cover_size), px(128))
        self._cover_glyph = cover_glyph
        self._cover_img = None
        self._cover_token = 0
        card = Rounded(self, bg=COLORS['bg_2'], border=COLORS['border_2'],
                       radius=8, padding=px(16), surface=COLORS['bg_1'])
        card.pack(fill='both', expand=True)
        self._card = card
        ph = leading_icon(cover_glyph or '') or 'game'
        if ph == 'ps5':
            ph = 'game'
        self._ph = ph
        self._cover_lbl = CoverLabel(card, box=int(self._cover_size / max(1, px(1))),
                                     placeholder=ph)
        self._cover_lbl.pack(side='left', anchor='n')
        right = tk.Frame(card, bg=COLORS['bg_2'])
        right.pack(side='left', fill='both', expand=True, padx=(px(16), 0))
        title_row = tk.Frame(right, bg=COLORS['bg_2'])
        title_row.pack(fill='x')
        tcol = tk.Frame(title_row, bg=COLORS['bg_2'])
        tcol.pack(side='left', fill='x', expand=True)
        self.title_var = tk.StringVar(value='')
        autowrap(tk.Label(tcol, textvariable=self.title_var,
                          font=FONTS['subtitle'], bg=COLORS['bg_2'],
                          fg=COLORS['fg_0'], anchor='w', justify='left'))
        tcol.winfo_children()[-1].pack(fill='x')
        self.id_var = tk.StringVar(value='')
        tk.Label(tcol, textvariable=self.id_var, font=FONTS['code'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w').pack(
                     anchor='w', pady=(px(2), 0))
        self.path_var = tk.StringVar(value='')
        self._path_lbl = tk.Label(tcol, textvariable=self.path_var,
                                  font=FONTS['caption'], bg=COLORS['bg_2'],
                                  fg=COLORS['fg_4'], anchor='w')
        self._path_lbl.pack(anchor='w', pady=(px(2), 0))
        self._badge = Badge(title_row, '', 'neutral', surface=COLORS['bg_2'])
        self._badge_shown = False
        strip = tk.Frame(right, bg=COLORS['bg_2'])
        strip.pack(fill='x', pady=(px(14), 0))
        self._cells = {}
        for i, (label, key) in enumerate(stats):
            cell = tk.Frame(strip, bg=COLORS['bg_2'])
            cell.pack(side='left', padx=(0 if i == 0 else px(28), 0))
            lab = label if not label.isupper() else label.capitalize()
            tk.Label(cell, text=lab, font=FONTS['caption'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
            v = tk.StringVar(value='\u2014')
            val = tk.Label(cell, textvariable=v, font=FONTS['body_strong'],
                           bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w')
            val.pack(anchor='w', pady=(px(2), 0))
            self._cells[key] = (v, val)

    # ── public API ──
    def set_title(self, title, game_id=''):
        self.title_var.set((title or '') or '\u2014')
        self.id_var.set(game_id or '\u2014')

    def set_path(self, text):
        self.path_var.set(text or '')

    def set_stat(self, key, value, warn=False):
        cell = self._cells.get(key)
        if not cell:
            return
        v, val = cell
        v.set(value if value not in (None, '') else '\u2014')
        try:
            val.config(fg=COLORS['warn_hi'] if warn else COLORS['fg_0'])
        except Exception:
            pass

    def set_badge(self, text, kind='ready'):
        try:
            if not text:
                if self._badge_shown:
                    self._badge.pack_forget()
                    self._badge_shown = False
                return
            self._badge.set(text, _BADGE_TONE.get(kind, 'neutral'))
            if not self._badge_shown:
                self._badge.pack(side='right', anchor='n')
                self._badge_shown = True
        except Exception:
            pass

    def reset_cover(self):
        self._cover_token += 1
        self._cover_img = None
        try:
            self._cover_lbl.config(image='', text=self._cover_glyph)
        except Exception:
            pass

    def set_cover_from_folder(self, app, folder):
        """Load cover via the existing Build loaders if a folder with art
        is available; otherwise keep the placeholder. Never invokes
        extraction/mount logic."""
        self._cover_token += 1
        token = self._cover_token
        if not folder or not os.path.isdir(folder):
            self.reset_cover()
            return

        def _w():
            pil = None
            try:
                cp = app._load_cover_art(folder)
                if cp:
                    from exfat_builder import _load_cover_image
                    pil = _load_cover_image(cp, target=self._cover_size)
            except Exception:
                pil = None

            def _apply():
                if token != self._cover_token:
                    return
                if pil is not None:
                    try:
                        from PIL import ImageTk as _IT
                        self._cover_img = _IT.PhotoImage(pil)
                        self._cover_lbl.config(image=self._cover_img, text='',
                                               width=self._cover_size,
                                               height=self._cover_size)
                    except Exception:
                        self.reset_cover()
                else:
                    self.reset_cover()
            try:
                self.after(0, _apply)
            except Exception:
                pass
        threading.Thread(target=_w, daemon=True).start()

    def set_cover_from_file(self, app, path):
        """Load a cover directly from an image file path (e.g. a library
        entry's cached cover). Mirrors set_cover_from_folder's token-guarded
        async pattern, but skips the _load_cover_art folder scan and loads the
        given file directly. Never invokes extraction/mount logic. Falls back
        to the placeholder if the path is missing or fails to load."""
        self._cover_token += 1
        token = self._cover_token
        if not path or not os.path.isfile(path):
            self.reset_cover()
            return

        def _w():
            pil = None
            try:
                from exfat_builder import _load_cover_image
                pil = _load_cover_image(path, target=self._cover_size)
            except Exception:
                pil = None

            def _apply():
                if token != self._cover_token:
                    return
                if pil is not None:
                    try:
                        from PIL import ImageTk as _IT
                        self._cover_img = _IT.PhotoImage(pil)
                        self._cover_lbl.config(image=self._cover_img, text='',
                                               width=self._cover_size,
                                               height=self._cover_size)
                    except Exception:
                        self.reset_cover()
                else:
                    self.reset_cover()
            try:
                self.after(0, _apply)
            except Exception:
                pass
        threading.Thread(target=_w, daemon=True).start()
