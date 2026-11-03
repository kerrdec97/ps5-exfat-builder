"""
ui/panel_bp_auto.py — Auto Backport form panel.

The "Auto Backport" sub-tab of the Backports screen, composed on the
design system (ui/shared/widgets.py):

    PageHeader                    Auto Backport + description
    ┌ Game ───────────────┐  ┌ Backup zips ──────────┐
    │ cover · title · id  │  │ Originals zip         │
    │ game folder field   │  │ Backported zip        │
    │ Detected SDK /      │  └───────────────────────┘
    │ Target FW / Patches │  ┌ Progress ─────────────┐
    └─────────────────────┘  │ status · bar          │
    ┌ Patch options ──────┐  └───────────────────────┘
    │ Target SDK · Notes  │
    │ Fakelib (+ path)    │
    │ [Restore…] [Apply]  │
    └─────────────────────┘

Backwards compat: every `app._abp_*` attribute the existing callbacks
read or write is preserved with the same name. None of the worker logic
(`_abp_run`, `_abp_detect_sdk`, `_abp_browse_*`, etc.) is changed.

Layout contracts the logic relies on:
  * `_abp_fakelib_mode_change` packs `_abp_fakelib_path_row` with
    `before=app._abp_btn.master` — the path row is a child of the Patch
    options card and `_abp_btn` lives in that card's action footer, so the
    row appears right under the Fakelib setting.
  * `_abp_get_game_folder` falls back to the first Entry under
    `_bp_form_frame` — the game-folder field is created first.

The panel exposes a public helper:
    update_selected_banner(app, game) — re-paints the Game card header
    from a game dict.
"""

import os
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import (
    Button, TextField, Dropdown, Card, Badge, InfoBar, Segmented, Divider,
    CoverLabel, autowrap)
from tkinter import ttk

from tkinter_theme import COLORS, FONTS, px
from ui.icons import strip_glyphs

# Star-import for `_` (i18n) and the legacy theme constants used by some
# inner widgets that we still touch via `app.*` attributes.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.log_view import ConsoleView

C = COLORS


def _kit():
    # Shared Backports layout helpers (private to the Backports screen).
    from ui import tab_backports as tb
    return tb


# ─────────────────────────────────────────────────────────────────────────
# Public entry — build the whole form into `parent`
# ─────────────────────────────────────────────────────────────────────────
def build_bp_auto_panel(parent, app):
    """Build the Auto Backport form into `parent` (a tk.Frame).

    `app` is the ExFATBuilder instance. All form state and widgets are
    attached to `app` under `_abp_*` names matching the legacy contract.
    """
    parent.configure(bg=COLORS['bg_1'])
    tb = _kit()

    # ── State (StringVars + flags) ──
    app._abp_game_var      = tk.StringVar()
    app._abp_output_var    = tk.StringVar(
        value=app._settings.get('abp_output', ''))
    app._abp_sdk_var       = tk.StringVar(value='4')
    app._abp_status_var    = tk.StringVar(value='')
    app._abp_running       = False
    saved_fakelib          = app._settings.get('abp_fakelib', '')
    app._abp_fakelib_saved = saved_fakelib
    app._abp_zip_orig_var  = tk.StringVar(
        value=app._settings.get('abp_zip_orig_dest', ''))
    app._abp_zip_pat_var   = tk.StringVar(
        value=app._settings.get('abp_zip_pat_dest', ''))
    app._abp_notes_var     = tk.StringVar(
        value=app._settings.get('abp_notes', ''))
    app._abp_fakelib_var   = tk.StringVar(value='None')
    app._abp_fakelib_path  = tk.StringVar()
    # New StringVars introduced by the redesign — fed by detect_sdk.
    # Step 38 (v2.5.2): default to '—' instead of stale '4.51' so the
    # card doesn't lie about target FW before any detection has run.
    app._abp_detected_sdk_var = tk.StringVar(value='—')
    app._abp_target_fw_var    = tk.StringVar(value='—')
    app._abp_patches_var      = tk.StringVar(value='—')

    # Restore previously saved fakelib mode/path
    if saved_fakelib:
        app._abp_fakelib_path.set(saved_fakelib)
        if os.path.isfile(saved_fakelib):
            app._abp_fakelib_var.set('file')
        elif os.path.isdir(saved_fakelib):
            app._abp_fakelib_var.set('folder')

    # ── Page ──
    page = tb._scroll_page(parent)
    hdr = tb._page_header(
        page, _('Auto Backport'),
        _('Automatically patches PS5 game ELF executables to run on '
          'older firmware. Decrypts and re-signs .self/.sprx/.prx '
          'files with a lower SDK target. Original files are backed '
          'up before patching.'))
    app._abp_page_header = hdr
    left, right = tb._columns(page)
    hidden = tb._hidden_frame(page)

    # ── Build sections ──
    _build_game_card(left, app)                  # game folder + detection
    _build_options_card(left, app, hidden)       # SDK, notes, fakelib, actions
    _build_zip_fields(right, app)                # backup zips
    _build_status_and_progress(right, app)       # status + progress
    _build_output_log(hidden, app)               # legacy console (hidden)

    # ── Settings persistence ──
    # Same as legacy: trace these vars so changes save automatically.
    app._abp_zip_orig_var.trace_add('write', lambda *a: (
        app._settings.update({'abp_zip_orig_dest': app._abp_zip_orig_var.get()}),
        save_settings(app._settings)))
    app._abp_zip_pat_var.trace_add('write', lambda *a: (
        app._settings.update({'abp_zip_pat_dest': app._abp_zip_pat_var.get()}),
        save_settings(app._settings)))
    app._abp_notes_var.trace_add('write', lambda *a: (
        app._settings.update({'abp_notes': app._abp_notes_var.get()}),
        save_settings(app._settings)))


# ─────────────────────────────────────────────────────────────────────────
# Game card — cover, title, folder field, detection stats
# ─────────────────────────────────────────────────────────────────────────
class _GameHeader:
    """Top of the Game card. Keeps the legacy GameHero API that
    update_selected_banner() drives (set_title / set_path / set_stat /
    set_badge / set_cover_from_folder / reset_cover)."""

    def __init__(self, parent, app):
        bg = C['bg_2']
        self.app = app
        self.frame = tk.Frame(parent, bg=bg)
        self.cover = CoverLabel(self.frame, box=88, placeholder='game')
        self.cover.pack(side='left', anchor='n')
        col = tk.Frame(self.frame, bg=bg)
        col.pack(side='left', fill='both', expand=True, padx=(px(16), 0))
        top = tk.Frame(col, bg=bg)
        top.pack(fill='x')
        self.badge = Badge(top, 'No game selected', 'neutral', surface=bg)
        self.title = tk.Label(top, text='No game selected',
                              font=FONTS['subtitle'], bg=bg, fg=C['fg_0'],
                              anchor='w', justify='left')
        self.title.pack(side='left', fill='x', expand=True)
        autowrap(self.title)
        self.meta = tk.Label(col, text='Pick a game folder, or click a game '
                             'card on the Games tab.', font=FONTS['caption'],
                             bg=bg, fg=C['fg_3'], anchor='w', justify='left')
        self.meta.pack(fill='x', pady=(px(2), 0))
        self.body = col
        self._cover_token = 0
        self._cover_img = None

    # legacy GameHero API
    def set_title(self, title, game_id=''):
        self.title.configure(text=title or '—')
        self.meta.configure(text=game_id or '', font=FONTS['code'])

    def set_path(self, text):
        pass  # the game-folder field below shows the path

    def set_stat(self, key, value, warn=False):
        pass  # the stats row is bound to the detect StringVars

    def set_badge(self, text, kind='ready'):
        tone = {'ready': 'success', 'busy': 'info', 'warn': 'warn',
                'wait': 'neutral'}.get(kind, 'neutral')
        word = {'READY TO PATCH': 'Ready to patch', 'DETECTING': 'Detecting',
                'NO GAME SELECTED': 'No game selected'}.get(text, text)
        try:
            self.badge.set(word, tone)
            if kind == 'wait':
                self.badge.pack_forget()
            elif not self.badge.winfo_ismapped():
                self.badge.pack(side='right', anchor='n', pady=(px(2), 0),
                                before=self.title)
        except Exception:
            pass

    def reset_cover(self):
        self._cover_token += 1
        try:
            self.cover.configure(image='')
        except Exception:
            pass

    def set_cover_from_folder(self, app, folder):
        import threading
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
                    pil = _load_cover_image(cp, target=px(88))
            except Exception:
                pil = None

            def _apply():
                if token != self._cover_token:
                    return
                if pil is None:
                    self.reset_cover()
                    return
                try:
                    from PIL import Image, ImageTk
                    box = px(88)
                    sq = Image.new('RGB', (box, box), C['bg_3'])
                    sq.paste(pil, ((box - pil.width) // 2, 0))
                    self._cover_img = ImageTk.PhotoImage(sq)
                    self.cover.configure(image=self._cover_img)
                except Exception:
                    self.reset_cover()
            try:
                self.frame.after(0, _apply)
            except Exception:
                pass
        threading.Thread(target=_w, daemon=True).start()

    # geometry passthrough (legacy callers pack/forget the hero)
    def pack(self, *a, **kw):
        pass

    def pack_forget(self):
        pass


def _build_game_card(parent, app):
    tb = _kit()
    bg = C['bg_2']
    card = Card(parent, title=_('Game folder'),
                subtitle=_('eboot.bin + sce_sys must be present'))
    card.pack(fill='x')
    Button(card.head_actions, text='\U0001f50d  ' + _('Auto-detect SDK'),
           command=app._abp_detect_sdk, surface=bg).pack(side='right')

    hero = _GameHeader(card.body, app)
    hero.frame.pack(fill='x')
    app._abp_banner = hero
    # Legacy write targets — kept as live StringVars / hidden labels.
    app._abp_banner_title = tk.StringVar(value='No game selected')
    app._abp_banner_meta  = tk.StringVar(value='')
    app._abp_banner_cover = tk.Label(tb._hidden_frame(card))
    app._abp_banner_fw_badge = tk.Label(tb._hidden_frame(card))

    # Folder field — the first Entry of the form (see module docstring).
    _blk, _ent = tb._field(hero.body, None, app._abp_game_var,
                           readonly=True,
                           buttons=[(_('Browse'), 'folder_open',
                                     app._abp_browse_game)])
    _blk.pack_configure(pady=(px(12), 0))

    Divider(card.body, color=C['border_1']).pack(fill='x', pady=(px(16), px(12)))
    _build_detected_strip(card.body, app)

    # Title / cover follow the folder field; the badge follows detection.
    state = {'from_game': False, 'last': None}

    def _on_folder(*_a):
        folder = app._abp_game_var.get().strip()
        if folder == state['last']:
            return
        state['last'] = folder
        if state['from_game']:
            state['from_game'] = False
            return
        if not folder:
            hero.title.configure(text='No game selected')
            hero.meta.configure(text='', font=FONTS['caption'])
            hero.set_badge('NO GAME SELECTED', 'wait')
            hero.reset_cover()
            return
        title, tid = os.path.basename(folder.rstrip('\\/')), ''
        try:
            t, tid, _v = get_game_info(folder)
            title = t or title
        except Exception:
            pass
        hero.set_title(title, tid or '')
        hero.set_cover_from_folder(app, folder)
    app._abp_game_var.trace_add('write', _on_folder)
    app._abp_banner_from_game = state

    def _mirror(*_a):
        try:
            sdk = app._abp_detected_sdk_var.get().strip()
            game = app._abp_game_var.get().strip()
            if game and sdk not in ('', '—', '—'):
                hero.set_badge('READY TO PATCH', 'ready')
            elif game:
                hero.set_badge('DETECTING', 'busy')
            else:
                hero.set_badge('NO GAME SELECTED', 'wait')
        except Exception:
            pass
    for v in (app._abp_detected_sdk_var, app._abp_target_fw_var,
              app._abp_patches_var, app._abp_game_var):
        v.trace_add('write', _mirror)


def update_selected_banner(app, game):
    """Refresh the Game card header from a game dict.

    Same entry point as before (called from the games grid / queue
    runner). If `game` is None the header goes back to "No game selected".
    """
    hero = getattr(app, '_abp_banner', None)
    if hero is None:
        return
    if not game:
        try:
            hero.title.configure(text='No game selected')
            hero.meta.configure(text='', font=FONTS['caption'])
            hero.reset_cover()
        except Exception:
            pass
        return

    title = game.get('title', 'Unknown')
    ppsa = game.get('ppsa') or game.get('title_id', '')
    parts = []
    if game.get('version'):
        parts.append('v' + game['version'])
    if game.get('size', 0) > 0:
        sz = game['size']
        parts.append('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))
    elif game.get('size_bytes', 0) > 0:
        sz = game['size_bytes']
        parts.append('%.1f GB' % (sz / 1024**3) if sz >= 1024**3
                     else '%d MB' % (sz // 1024**2))

    try:
        st = getattr(app, '_abp_banner_from_game', None)
        if st is not None:
            st['from_game'] = (app._abp_game_var.get().strip()
                               != (game.get('folder', '') or '').strip())
    except Exception:
        pass
    hero.set_title(title, ' · '.join(
        [x for x in [ppsa] + parts if x]))
    hero.set_path(game.get('folder', ''))
    # Keep legacy vars in sync for any old reader
    try:
        app._abp_banner_title.set(title)
        app._abp_banner_meta.set(' · '.join(
            [x for x in [ppsa] + parts if x]))
    except Exception:
        pass

    # Cover via the shared async loader (uses app._load_cover_art)
    try:
        hero.set_cover_from_folder(app, game.get('folder', ''))
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Detected strip — 3 cells: Detected SDK / Target FW / Patches
# ─────────────────────────────────────────────────────────────────────────
def _build_detected_strip(parent, app):
    """3-cell row showing detection results. Replaces the legacy
    single-cell amber `_abp_sdk_banner` while preserving its name + var."""
    tb = _kit()
    bg = parent.cget('bg')
    strip = tk.Frame(parent, bg=bg)
    strip.pack(fill='x')
    for i in range(3):
        strip.grid_columnconfigure(i, weight=1, uniform='det')

    for col, (label_text, var) in enumerate((
            (_('Detected SDK'), app._abp_detected_sdk_var),
            (_('Target FW'), app._abp_target_fw_var),
            (_('Patches'), app._abp_patches_var))):
        cell, _val = tb._stat_cell(strip, label_text, var=var)
        cell.grid(row=0, column=col, sticky='w')

    # Backwards-compat aliases — the existing _abp_detect_sdk callback
    # writes to _abp_sdk_info_var and packs _abp_sdk_banner. Those
    # references must remain valid: the banner lives in a frame that is
    # never shown, the info var feeds the parser below.
    app._abp_sdk_banner = tk.Frame(tb._hidden_frame(parent), bg=bg)
    app._abp_sdk_info_var = tk.StringVar(value='')
    app._abp_sdk_banner_lbl = tk.Label(app._abp_sdk_banner,
                                       textvariable=app._abp_sdk_info_var)

    # When the legacy var changes, parse "SDK X" / "FW X.xx+" / "(N patches)"
    # out of the message and update the new card vars. Step 38 (v2.5.2):
    # rewrote regexes to match the ACTUAL message shape produced by
    # _abp_detect_sdk in exfat_builder.py:
    #   "🔍  Requires FW 9.xx+ — Recommended backport target: SDK 9 or lower"
    def _sync(*_args):
        msg = app._abp_sdk_info_var.get()
        import re
        # Detected SDK: "SDK 9" or "SDK 9.00" — accept either form
        m = re.search(r'SDK\s+(\d+(?:\.\d+)?)', msg)
        if m:
            app._abp_detected_sdk_var.set(m.group(1))
        # Required FW: "FW 9.xx+" or "FW 4.51" — accept "X.xx+" too
        m = re.search(r'FW\s+(\d+(?:\.\w+)?\+?)', msg)
        if m:
            app._abp_target_fw_var.set(m.group(1))
        # Patch count (only set if present in message)
        m = re.search(r'\((\d+)\s+patches?\)', msg)
        if m:
            app._abp_patches_var.set('✓ ' + m.group(1) + ' known')
        # Reset card to "—" on warning/error messages so stale values
        # from a previous successful detect don't persist after a
        # failed re-detect on a different game.
        if msg.startswith('⚠'):  # ⚠ warning prefix
            app._abp_detected_sdk_var.set('—')
            app._abp_target_fw_var.set('—')
            app._abp_patches_var.set('—')
    app._abp_sdk_info_var.trace_add('write', _sync)


# ─────────────────────────────────────────────────────────────────────────
# Originals zip + Backported zip fields
# ─────────────────────────────────────────────────────────────────────────
def _build_zip_fields(parent, app):
    tb = _kit()
    card = Card(parent, title='Backup zips',
                subtitle=_('Leave both blank to be asked each time'))
    card.pack(fill='x')

    def _clear(var, key):
        return lambda: (var.set(''), app._settings.update({key: ''}),
                        save_settings(app._settings))

    tb._field(card.body, _('Originals zip'), app._abp_zip_orig_var,
              hint=_('backup of pre-patch files'), first=True,
              buttons=[(_('Browse'), 'folder_open',
                        lambda: app._abp_browse_zip_dest('orig')),
                       (_('Clear'), None, _clear(app._abp_zip_orig_var,
                                                 'abp_zip_orig_dest'),
                        'subtle')])
    tb._field(card.body, _('Backported zip'), app._abp_zip_pat_var,
              hint=_('output zip after patching'),
              buttons=[(_('Browse'), 'folder_open',
                        lambda: app._abp_browse_zip_dest('patched')),
                       (_('Clear'), None, _clear(app._abp_zip_pat_var,
                                                 'abp_zip_pat_dest'),
                        'subtle')])


# ─────────────────────────────────────────────────────────────────────────
# Patch options — Target SDK, notes, fakelib, actions
# ─────────────────────────────────────────────────────────────────────────
class _FakelibSegmented(Segmented):
    """Segmented whose paint treats the legacy initial 'None' as 'none'."""

    def _paint(self):
        cur = str(self.var.get())
        if cur == 'None':
            cur = 'none'
        for v, b in self._btns.items():
            sel = str(v) == cur
            b.configure(bg=C['bg_3'] if sel else C['bg_0'],
                        fg=C['fg_0'] if sel else C['fg_3'],
                        font=FONTS['body_strong'] if sel else FONTS['body'])


def _build_options_card(parent, app, hidden):
    tb = _kit()
    bg = C['bg_2']
    card = Card(parent, title='Patch options',
                subtitle='Target firmware, notes and optional fakelib stubs.',
                actions=True)
    card.pack(fill='x', pady=(px(16), 0))
    body = card.body

    # Target SDK
    ctl = tb._setting_row(
        body, _('Target SDK'),
        _('4 = firmware 4.xx  ·  5 = firmware 5.xx  ·  '
          '7 = firmware 7.xx  etc.'), first=True)
    sdk_options = ['4', '5', '6', '7', '8', '9', '10', '11']
    Dropdown(ctl, textvariable=app._abp_sdk_var, values=sdk_options,
             width=6, surface=bg).pack()
    InfoBar(body, kind='info',
            text=tb._clean(_('ℹ  Set this to YOUR PS5’s firmware — not the '
                                'game’s detected SDK. Example: on firmware 5.50, '
                                'choose 5. Picking the wrong target can stop the game '
                                'booting.'))
            ).pack(fill='x', pady=(px(12), 0))

    # Notes
    ctl = tb._setting_row(body, _('Notes'),
                          _('e.g. Works on 4.51, tested OK'))
    TextField(ctl, textvariable=app._abp_notes_var, width=28,
              surface=bg).pack()

    # Fakelib
    ctl = tb._setting_row(
        body, _('Fakelib'),
        _('Most games: leave as None. Optional — only needed '
          'when a game requires .sprx/.prx stub libraries for the '
          'target firmware.'))
    seg = _FakelibSegmented(
        ctl, [('none', _('None')), ('folder', _('Select folder')),
              ('file', _('Select file'))],
        variable=app._abp_fakelib_var,
        command=lambda _v: app._abp_fakelib_mode_change(), bg=bg)
    seg.pack()
    app._abp_fakelib_seg = seg

    # Action footer — `_abp_btn.master` is the footer (see docstring).
    acts = card.actions
    app._abp_btn = Button(acts, text='⚡  ' + _('Apply Auto Backport'),
                          variant='primary', command=app._abp_run,
                          surface=bg)
    app._abp_btn.pack(side='right')
    Button(acts, text='↩  ' + _('Restore originals'),
           command=app._abp_restore, surface=bg).pack(side='left')
    Button(acts, text=_('Restore multiple…'), icon='history',
           command=app._abp_restore_multi, surface=bg
           ).pack(side='left', padx=(px(8), 0))

    # Fakelib path row — pack/forget controlled by the mode callback.
    # It is a child of the card so it can be packed before the footer.
    app._abp_fakelib_path_row = tk.Frame(card, bg=bg)
    _blk, _fe = tb._field(app._abp_fakelib_path_row, None,
                          app._abp_fakelib_path, readonly=True, first=True,
                          buttons=[(_('Browse'), 'folder_open',
                                    app._abp_browse_fakelib),
                                   (_('Clear'), None,
                                    lambda: (app._abp_fakelib_path.set(''),
                                             app._abp_fakelib_var.set('none'),
                                             app._abp_fakelib_path_row.pack_forget()),
                                    'subtle')])
    _blk.pack_configure(pady=(px(12), 0))

    # If a saved fakelib path was restored, pack the row immediately
    if app._abp_fakelib_var.get() in ('file', 'folder'):
        app._abp_fakelib_path_row.pack(fill='x', pady=(2, 0),
                                       before=app._abp_btn.master)


# ─────────────────────────────────────────────────────────────────────────
# Status text + progress bar
# ─────────────────────────────────────────────────────────────────────────
def _build_status_and_progress(parent, app):
    tb = _kit()
    bg = C['bg_2']
    card = Card(parent, title='Progress')
    card.pack(fill='x', pady=(px(16), 0))
    tb._status_line(card.body, app._abp_status_var,
                    idle_text='Ready when you are. Pick a game folder and '
                              'apply.').pack(fill='x')
    app._abp_prog_cv = tb._CanvasBar(card.body, surface=bg)
    app._abp_prog_cv.pack(fill='x', pady=(px(12), 0))
    app._abp_prog_rect = app._abp_prog_cv.create_rectangle(
        0, 0, 0, 8, fill=COLORS['accent'], outline='')
    autowrap(tk.Label(
        card.body,
        text=_('Detailed output appears in the OUTPUT LOG at the bottom of the window.'),
        font=FONTS['caption'], bg=bg, fg=C['fg_3'], anchor='w',
        justify='left')).pack(fill='x', pady=(px(12), 0))


# ─────────────────────────────────────────────────────────────────────────
# Output log — legacy ConsoleView, built into a hidden frame
# ─────────────────────────────────────────────────────────────────────────
def _build_output_log(parent, app):
    """The legacy per-panel console. The global OUTPUT LOG drawer is the
    visible log surface; this one is kept (hidden) for code that still
    inserts into `app._abp_log` directly."""
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x')
    tk.Label(head, text=_('OUTPUT LOG'),
             font=FONTS['overline'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='left')
    Button(head, text=_('Clear'), variant='subtle', size='sm',
           command=app._abp_clear_log).pack(side='right')

    cv = ConsoleView(parent, height=180)
    cv.pack(fill='both', expand=True, pady=(2, 0))
    app._abp_log = cv.text
    app._abp_log_console = cv

    # Legacy tag aliases — same colours as the existing _abp tags
    app._abp_log.tag_configure('ok',   foreground=COLORS['success_ok'])
    app._abp_log.tag_configure('err',  foreground=COLORS['danger_hi'])
    app._abp_log.tag_configure('warn', foreground=COLORS['warn_hi'])
    app._abp_log.tag_configure('info', foreground=COLORS['accent_hi'])


# ─────────────────────────────────────────────────────────────────────────
# Helpers kept for compatibility (other code may import them)
# ─────────────────────────────────────────────────────────────────────────
def _field_row(parent, label_text, hint=None):
    """Label above an input row. Returns the input row (Frame)."""
    bg = parent.cget('bg')
    block = tk.Frame(parent, bg=bg)
    block.pack(fill='x', pady=(0, px(12)))
    _kit()._label_row(block, label_text, hint).pack(fill='x')
    row = tk.Frame(block, bg=bg)
    row.pack(fill='x', pady=(px(4), 0))
    return row


def _dark_entry(parent, var, readonly=False, width=None):
    """A design-system text field in a wrapper Frame (legacy signature)."""
    wrap = tk.Frame(parent, bg=parent.cget('bg'))
    kw = dict(textvariable=var, mono=True,
              state='readonly' if readonly else 'normal')
    if width is not None:
        kw['width'] = width
    TextField(wrap, **kw).pack(fill='x')
    return wrap


def _ghost_btn(parent, text, command):
    return Button(parent, text=text, command=command, size='sm')
