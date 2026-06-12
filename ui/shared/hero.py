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

_BADGE = {
    'ready':   (COLORS['success'], '#0a0a0a'),
    'wait':    (COLORS['bg_4'],    COLORS['fg_3']),
    'busy':    (COLORS['accent'],  '#0a0a0a'),
    'warn':    (COLORS['warn'],    '#0a0a0a'),
}


class GameHero(tk.Frame):
    """Build-style hero card.

    stats: ordered list of (label, key) pairs for the strip cells.
    """

    def __init__(self, parent, stats, cover_glyph='\U0001f3ae',
                 cover_size=150):
        super().__init__(parent, bg=COLORS['bg_2'],
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)
        self._cover_size = cover_size
        self._cover_glyph = cover_glyph
        self._cover_img = None
        self._cover_token = 0

        pad = tk.Frame(self, bg=COLORS['bg_2'])
        pad.pack(fill='both', expand=True, padx=20, pady=18)

        # ── cover tile ──
        self._cover_tile = tk.Frame(pad, bg=COLORS['bg_3'], width=cover_size,
                                    height=cover_size,
                                    highlightbackground=COLORS['border_3'],
                                    highlightthickness=1)
        self._cover_tile.pack(side='left')
        self._cover_tile.pack_propagate(False)
        self._cover_lbl = tk.Label(self._cover_tile, text=cover_glyph,
                                   bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                                   font=('Segoe UI', 52))
        self._cover_lbl.pack(expand=True)

        # ── right column: title row + stats strip ──
        right = tk.Frame(pad, bg=COLORS['bg_2'])
        right.pack(side='left', fill='both', expand=True, padx=(18, 0))

        title_row = tk.Frame(right, bg=COLORS['bg_2'])
        title_row.pack(fill='x')

        tcol = tk.Frame(title_row, bg=COLORS['bg_2'])
        tcol.pack(side='left', fill='x', expand=True)
        self.title_var = tk.StringVar(value='')
        tk.Label(tcol, textvariable=self.title_var,
                 font=(FONTS['h2'][0], 17, 'bold'), bg=COLORS['bg_2'],
                 fg=COLORS['fg_0'], anchor='w', justify='left').pack(
                     anchor='w', fill='x')
        self.id_var = tk.StringVar(value='')
        tk.Label(tcol, textvariable=self.id_var,
                 font=(FONTS['mono_sm'][0], 11), bg=COLORS['bg_2'],
                 fg=COLORS['fg_4'], anchor='w').pack(anchor='w')
        self.path_var = tk.StringVar(value='')
        self._path_lbl = tk.Label(tcol, textvariable=self.path_var,
                                  font=(FONTS['mono_sm'][0], 9),
                                  bg=COLORS['bg_2'], fg=COLORS['fg_5'],
                                  anchor='w')
        self._path_lbl.pack(anchor='w', pady=(2, 0))

        # status badge (top-right)
        self._badge = tk.Label(title_row, text='', font=(FONTS['mono_sm'][0],
                               9, 'bold'), bg=COLORS['bg_4'], fg=COLORS['fg_3'],
                               padx=10, pady=4)
        self._badge.pack(side='right', anchor='n')

        # stats strip
        strip = tk.Frame(right, bg=COLORS['bg_2'])
        strip.pack(fill='x', pady=(16, 0))
        self._cells = {}
        for i, (label, key) in enumerate(stats):
            cell = tk.Frame(strip, bg=COLORS['bg_2'])
            cell.pack(side='left', padx=(0 if i == 0 else 26, 0))
            tk.Label(cell, text=label.upper(),
                     font=(FONTS['mono_sm'][0], 8, 'bold'), bg=COLORS['bg_2'],
                     fg=COLORS['fg_5'], anchor='w').pack(anchor='w')
            v = tk.StringVar(value='\u2014')
            val = tk.Label(cell, textvariable=v,
                           font=(FONTS['mono_sm'][0], 11, 'bold'),
                           bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w')
            val.pack(anchor='w', pady=(2, 0))
            self._cells[key] = (v, val)

    # ── public API ──
    def set_title(self, title, game_id=''):
        self.title_var.set((title or '').upper() or '\u2014')
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
            val.config(fg=COLORS['warn'] if warn else COLORS['teal'])
        except Exception:
            pass

    def set_badge(self, text, kind='ready'):
        bg, fg = _BADGE.get(kind, _BADGE['wait'])
        prefix = '\u2713  ' if kind == 'ready' else ''
        try:
            self._badge.config(text=(prefix + text) if text else '',
                               bg=bg, fg=fg)
        except Exception:
            pass

    def reset_cover(self):
        self._cover_token += 1
        self._cover_img = None
        try:
            self._cover_lbl.config(image='', text=self._cover_glyph,
                                   font=('Segoe UI', 52))
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
